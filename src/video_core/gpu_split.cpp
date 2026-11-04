// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "Synchronization.lib")
#elif defined(__linux__)
#include <climits>
#include <ctime>
#include <dlfcn.h>
#include <linux/futex.h>
#include <pthread.h>
#include <unwind.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <cstdio>

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon {

namespace {

void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

// Sleeps until `word` moves on from `seen`, for at most `timeout`: a lost wake-up costs one
// timeout instead of a hang. Raw futexes: libc++ skips the wake syscall when its own waiter count
// for the address is zero, so its notify can't pair with these waits.
void WaitWord(std::atomic<u32>& word, u32 seen, std::chrono::microseconds timeout) {
    static_assert(sizeof(std::atomic<u32>) == sizeof(u32));
#if defined(_WIN32)
    WaitOnAddress(&word, &seen, sizeof(seen),
                  static_cast<DWORD>(std::max<s64>(timeout.count() / 1000, 1)));
#elif defined(__linux__)
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(timeout.count() / 1'000'000);
    ts.tv_nsec = static_cast<long>(timeout.count() % 1'000'000) * 1000;
    syscall(SYS_futex, reinterpret_cast<u32*>(&word), FUTEX_WAIT_PRIVATE, seen, &ts, nullptr, 0);
#else
    (void)word;
    (void)seen;
    (void)timeout;
    std::this_thread::yield();
#endif
}

void WakeWord(std::atomic<u32>& word) {
#if defined(_WIN32)
    WakeByAddressAll(&word);
#elif defined(__linux__)
    syscall(SYS_futex, reinterpret_cast<u32*>(&word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr,
            0);
#else
    (void)word;
#endif
}

// Phones sleep in the waits below on futexes: spinning or yielding, the front kept a big core
// busy and the phone hot. The back parks on a raw futex too (libc++'s std::atomic::wait can miss
// a notify). Desktop keeps the spin, yield and std::atomic waits.
#ifdef __ANDROID__
constexpr bool SleepingWaits = true;
#else
constexpr bool SleepingWaits = false;
#endif

// Wakes the back thread parked on `wake`.
void WakeBack(std::atomic<u32>& wake) {
    if constexpr (SleepingWaits) {
        WakeWord(wake);
    } else {
        wake.notify_all();
    }
}

[[maybe_unused]] std::atomic<u32> g_back_thread_id{};
std::atomic<bool> g_back_busy{};

} // Anonymous namespace

void GpuSplit::CheckOwner(const char* where) {
#ifdef _WIN32
    if (!g_back_busy.load(std::memory_order_acquire)) {
        return;
    }
    const DWORD thread = GetCurrentThreadId();
    if (thread == g_back_thread_id.load(std::memory_order_relaxed)) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    std::fprintf(stderr, "GPU split owner check: %s on thread %lu while the back runs\n", where,
                 thread);
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module{};
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base_name = std::strrchr(name, '\\');
        std::fprintf(stderr, "  frame %s+%#llx\n", base_name ? base_name + 1 : name,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                     reinterpret_cast<uintptr_t>(module)));
    }
    std::fflush(stderr);
#elif defined(__linux__)
    if (!g_back_busy.load(std::memory_order_acquire)) {
        return;
    }
    const u32 thread = static_cast<u32>(gettid());
    if (thread == g_back_thread_id.load(std::memory_order_relaxed)) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    char name[16] = "?";
    pthread_getname_np(pthread_self(), name, sizeof(name));
    struct Frames {
        std::array<void*, 32> pcs{};
        size_t count{};
    } frames;
    _Unwind_Backtrace(
        [](_Unwind_Context* context, void* arg) {
            auto& f = *static_cast<Frames*>(arg);
            if (f.count == f.pcs.size()) {
                return _URC_END_OF_STACK;
            }
            f.pcs[f.count++] = reinterpret_cast<void*>(_Unwind_GetIP(context));
            return _URC_NO_REASON;
        },
        &frames);
    std::string stack;
    for (size_t i = 1; i < frames.count; ++i) {
        Dl_info info{};
        const auto pc = reinterpret_cast<uintptr_t>(frames.pcs[i]);
        if (dladdr(frames.pcs[i], &info) != 0 && info.dli_fname != nullptr) {
            const char* base = std::strrchr(info.dli_fname, '/');
            stack += fmt::format(" {}+{:#x}", base ? base + 1 : info.dli_fname,
                                 pc - reinterpret_cast<uintptr_t>(info.dli_fbase));
        } else {
            stack += fmt::format(" {:#x}", pc);
        }
    }
    LOG_ERROR(HW_GPU, "GPU split owner check: {} on thread {} ({}) while the back runs:{}", where,
              thread, name, stack);
#else
    (void)where;
#endif
}

GpuSplit GpuSplit::instance;

void GpuSplit::MaybeStart(Core::System& system_) {
    static const char mode = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT");
        return value != nullptr ? value[0] : '2';
    }();
    const bool wanted = mode == '1' || mode == '2';
    if (!wanted || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        return;
    }
    // The front reads command lists straight from guest memory; a read that flushes the caches
    // first (safe DMA, accurate GPU) would touch back-owned state. The settings can change
    // mid-game: hand the engines back to the front (and start again when they allow it).
    const bool safe_reads = Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh()
                                                          : Settings::IsDMALevelSafe();
    const bool allowed = !safe_reads && !Settings::IsGPULevelHigh() &&
                         Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
    if (active && !allowed) {
        Drain();
        Stop();
        LOG_INFO(HW_GPU, "GPU thread split: stopped, the accuracy settings need the caches on the front");
        return;
    }
    if (active || !allowed) {
        return;
    }
    system = &system_;
    check_owner = std::getenv("EDEN_NX2_SPLIT_CHECK") != nullptr;
    ring = std::make_unique<u64[]>(RingWords);
    back_thread = std::jthread([this](std::stop_token stop_token) { BackLoop(stop_token); });
    active = true;
#if defined(ARCHITECTURE_x86_64) || (defined(ARCHITECTURE_arm64) && defined(__ANDROID__))
    // Front macros run compiled (macro_turing_jit.h, macro_turing_jit_arm64.h), or interpreted
    // where the ARM64 code is not used.
    const char* jit = std::getenv("EDEN_NX2_MME_JIT");
    front_macros = mode == '2' && (jit == nullptr || jit[0] != '0');
#endif
    LOG_INFO(HW_GPU, "GPU thread split: engine methods run on a second thread{}",
             front_macros ? ", macros on the first" : "");
}

void GpuSplit::Stop() {
    if (!back_thread.joinable()) {
        return;
    }
    back_thread.request_stop();
    wake.fetch_add(1, std::memory_order_seq_cst);
    WakeBack(wake);
    back_thread.join();
    active = false;
    // The split is static and outlives the emulation session. The next session's back thread
    // starts reading at 0: with the old positions kept it skipped through the new, empty ring a
    // wrap at a time and never reached the old write position (the next game's first GPU work
    // never ran, it waited on its syncpoint forever), and the engines it held were freed.
    ring.reset();
    write_position = 0;
    last_writer_position = 0;
    published_local = 0;
    consumed_cache = 0;
    publish_every = PublishEvery;
    front_engine = nullptr;
    back_engine = nullptr;
    front_macros = false;
    published.store(0, std::memory_order_relaxed);
    consumed.store(0, std::memory_order_relaxed);
    back_waiting.store(false, std::memory_order_relaxed);
    front_waiting.store(false, std::memory_order_relaxed);
}

u64* GpuSplit::ReserveSlow(size_t words) {
    size_t index = write_position & RingMask;
    if (index + words > RingWords) {
        // Entries never wrap: skip to the start of the ring.
        const size_t rest = RingWords - index;
        WaitForSpace(rest);
        ring[index] = static_cast<u64>(Kind::Wrap);
        write_position += rest;
        index = 0;
    }
    WaitForSpace(words);
    return &ring[index];
}

void GpuSplit::WaitForSpace(size_t words) {
    if (write_position + words - consumed_cache <= RingWords) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (write_position + words - consumed_cache <= RingWords) {
            return;
        }
        if (spins < 4096) {
            Pause();
        } else {
            SleepOnBack();
        }
    }
}

void GpuSplit::RequestIdleHook() {
    hook_requested.store(true, std::memory_order_seq_cst);
    wake.fetch_add(1, std::memory_order_seq_cst);
    WakeBack(wake);
}

void GpuSplit::SleepOnBack() {
    // Re-wake a back thread whose wake-up was lost (it froze Mario Kart World on Android with the
    // front waiting here and the back parked).
    if (back_waiting.load(std::memory_order_seq_cst)) {
        wake.fetch_add(1, std::memory_order_seq_cst);
        WakeBack(wake);
    }
    if constexpr (!SleepingWaits) {
        std::this_thread::yield();
        return;
    }
    // The back bumps `drained` after storing `consumed` when it sees front_waiting: read it
    // before the last look at `consumed`, so a store in between ends the wait at once.
    front_waiting.store(true, std::memory_order_seq_cst);
    const u32 seen = drained.load(std::memory_order_seq_cst);
    if (consumed.load(std::memory_order_seq_cst) == consumed_cache) {
        WaitWord(drained, seen, std::chrono::milliseconds{2});
    }
    front_waiting.store(false, std::memory_order_relaxed);
}

void GpuSplit::Publish() {
    if (published_local == write_position) {
        return;
    }
    published_local = write_position;
    publish_every = std::min<size_t>(publish_every * 2, PublishEvery);
    published.store(write_position, std::memory_order_seq_cst);
    if (back_waiting.load(std::memory_order_seq_cst)) {
        wake.fetch_add(1, std::memory_order_seq_cst);
        WakeBack(wake);
    }
}

void GpuSplit::Drain() {
    Drain(Nx2Profile::Zone::SplitDrain);
}

void GpuSplit::Drain(Nx2Profile::Zone zone) {
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache == write_position) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{zone};
    auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache == write_position) {
            publish_every = 8;
            return;
        }
        if (spins < (SleepingWaits ? 8192U : 16384U)) {
            Pause();
        } else {
            // Sleep instead of spinning on: the front spent most of each frame here, keeping a big
            // core busy and the phone hot.
            SleepOnBack();
            if ((SleepingWaits || (spins & 1023) == 0) &&
                std::chrono::steady_clock::now() >= next_report) {
                next_report += std::chrono::seconds{5};
                LOG_WARNING(HW_GPU,
                            "GPU split drain waiting on the back thread: {} of {} words consumed, "
                            "back {} ({})",
                            consumed_cache, write_position,
                            back_waiting.load() ? "parked" : "running",
                            g_back_busy.load() ? "inside a command" : "between batches");
            }
        }
    }
}

bool GpuSplit::WaitForWriters() {
    const u64 target = std::min(last_writer_position, write_position);
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache >= target) {
        return false;
    }
    wait_target.store(target, std::memory_order_seq_cst);
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWriters};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache >= target) {
            wait_target.store(~u64{0}, std::memory_order_relaxed);
            return true;
        }
        if (spins < 16384) {
            Pause();
        } else {
            SleepOnBack();
        }
    }
}

void GpuSplit::HandBack(u64 position) {
    // Seq_cst store and load against SleepOnBack's: one of the two sides sees the other.
    consumed.store(position, std::memory_order_seq_cst);
    if (front_waiting.load(std::memory_order_seq_cst)) {
        drained.fetch_add(1, std::memory_order_seq_cst);
        WakeWord(drained);
    }
}

void GpuSplit::SwitchEngine(Tegra::Engines::EngineInterface* engine) {
    front_engine = engine;
    u64* const entry = Reserve(2);
    entry[0] = static_cast<u64>(Kind::Engine);
    entry[1] = reinterpret_cast<u64>(engine);
    Commit(2);
}

void GpuSplit::MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                           u32 amount, u32 methods_pending, const GPUVAddr* segment) {
    SelectEngine(engine);
    Nx2Profile::Add(Nx2Profile::Counter::MultiWords, amount);
    // Long runs (constant buffer and inline uploads) go in pieces, as when a method's data
    // continues in the next GPFIFO entry.
    while (amount > 0) {
        const u32 piece = std::min(amount, MaxMultiWords);
        const size_t head = segment != nullptr ? 3 : 2;
        const size_t size = head + (piece + 1) / 2;
        u64* const entry = Reserve(size);
        entry[0] = static_cast<u64>(Kind::Multi) | (segment != nullptr ? HasSegment : 0) |
                   (u64{method & 0xffff} << 8) | (u64{piece} << 32);
        entry[1] = methods_pending;
        if (segment != nullptr) {
            entry[2] = *segment;
        }
        entry[size - 1] = 0;
        std::memcpy(entry + head, words, size_t{piece} * sizeof(u32));
        Commit(size);
        words += piece;
        amount -= piece;
        methods_pending -= piece;
    }
}

void GpuSplit::DeferFunction(std::function<void()>&& func) {
    auto* const heap = new std::function<void()>(std::move(func));
    Defer([heap] {
        (*heap)();
        delete heap;
    });
}

size_t GpuSplit::Execute(const u64* entry) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        if ((head & Direct) != 0) {
            Nx2Profile::SampledScope<4> method_scope{Nx2Profile::Zone::Method};
            Nx2Profile::Add(Nx2Profile::Counter::BackMethodCalls, 1);
            engine->ConsumeSink(*system);
            engine->CallMethod(*system, method, argument, true);
        } else if (!engine->execution_mask[method]) {
            Nx2Profile::Add(Nx2Profile::Counter::BackSinkWrites, 1);
            engine->method_sink.emplace_back(method, argument);
        } else {
            Nx2Profile::SampledScope<4> method_scope{Nx2Profile::Zone::Method};
            Nx2Profile::Add(Nx2Profile::Counter::BackMethodCalls, 1);
            engine->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
        }
            Nx2Profile::CountMethod(method);
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        Nx2Profile::SampledScope<4> method_scope{Nx2Profile::Zone::Method};
            Nx2Profile::CountMethod(method);
        engine->ConsumeSink(*system);
        if ((head & HasSegment) != 0) {
            engine->current_dma_segment = entry[2];
        }
        engine->CallMultiMethod(*system, method, reinterpret_cast<const u32*>(entry + data),
                                amount, static_cast<u32>(entry[1]));
        return data + (amount + 1) / 2;
    }
    case Kind::Call: {
        const size_t payload = static_cast<size_t>((head >> 8) & 0xffffff);
        Nx2Profile::Add(Nx2Profile::Counter::BackDeferredCalls, 1);
        reinterpret_cast<void (*)(const u64*)>(entry[1])(entry + 2);
        Nx2Profile::CountMethod(method, amount);
        return 2 + payload;
    }
    }
    return 1;
}

void GpuSplit::BackLoop(std::stop_token stop_token) {
    Common::SetCurrentThreadName("GPUBack");
    on_back_thread = true;
    back_running.store(true, std::memory_order_release);
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    system->RegisterHostThread();
#ifdef _WIN32
    g_back_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
#elif defined(__linux__)
    g_back_thread_id.store(static_cast<u32>(gettid()), std::memory_order_relaxed);
#endif
    u64 read_position = 0;
    while (!stop_token.stop_requested()) {
        Nx2Profile::Report("GPU back thread");
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            // The front usually sends more within microseconds: spin briefly before sleeping.
            for (u32 spins = 0; spins < 8192 && available == read_position &&
                                !hook_requested.load(std::memory_order_relaxed);
                 ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position && idle_hook != nullptr) {
                hook_requested.store(false, std::memory_order_seq_cst);
                idle_hook(idle_argument);
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once.
                back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = wake.load(std::memory_order_seq_cst);
                available = published.load(std::memory_order_seq_cst);
                // A request made after the hook ran (RequestIdleHook) runs it before sleeping.
                if (available == read_position && !stop_token.stop_requested() &&
                    !hook_requested.load(std::memory_order_seq_cst)) {
                    if constexpr (SleepingWaits) {
                        // A futex, not std::atomic::wait: libc++ spins with sched_yield first,
                        // and parks waiters in a shared table where a notify could miss this one.
                        WaitWord(wake, seen, std::chrono::milliseconds{2});
                    } else {
                        wake.wait(seen, std::memory_order_seq_cst);
                    }
                }
                back_waiting.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        u64 last_store = read_position;
        g_back_busy.store(true, std::memory_order_relaxed);
        while (read_position != available) {
            const size_t index = read_position & RingMask;
            const size_t size = Execute(&ring[index]);
            read_position += size != 0 ? size : RingWords - index;
            if (hook_requested.load(std::memory_order_relaxed) &&
                idle_hook != nullptr) [[unlikely]] {
                hook_requested.store(false, std::memory_order_seq_cst);
                idle_hook(idle_argument);
            }
            // Hand ring space back now and then while working through a long batch, and at once
            // when the front waits for this position (WaitForWriters).
            if ((read_position - last_store >= 4096 ||
                 read_position >= wait_target.load(std::memory_order_relaxed)) &&
                read_position != available) {
                HandBack(read_position);
                last_store = read_position;
            }
        }
        g_back_busy.store(false, std::memory_order_release);
        HandBack(read_position);
    }
    back_running.store(false, std::memory_order_release);
}

} // namespace VideoCommon

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstring>
#include <string>

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_perf.h"
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

[[maybe_unused]] std::atomic<u32> g_back_thread_id{};

} // Anonymous namespace

void GpuSplit::CheckOwner(const char* where) {
#ifdef _WIN32
    // Also while the back is idle: its idle hook submits (Scheduler::FlushDeferred).
    if (!instance.back_running.load(std::memory_order_acquire)) {
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
    std::string stack;
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
        stack += fmt::format(" {}+{:#x}", base_name ? base_name + 1 : name,
                             reinterpret_cast<uintptr_t>(frames[i]) -
                                 reinterpret_cast<uintptr_t>(module));
    }
    LOG_ERROR(HW_GPU, "GPU split owner check: {} on thread {} while the back runs; stack:{}",
              where, thread, stack);
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
    verify_repeats = Nx2Perf::Verify() && Nx2Perf::On(Nx2Perf::Opt::DrawBeginCoalesce);
    back_thread = std::jthread([this](std::stop_token stop_token) { BackLoop(stop_token); });
    active = true;
    eager_publish = Nx2Perf::On(Nx2Perf::Opt::EagerPublish);
    release_before_hook = Nx2Perf::On(Nx2Perf::Opt::ReleaseBeforeHook);
    writes.SetSeparateKinds(Nx2Perf::On(Nx2Perf::Opt::MacroReadOverlayWriters) &&
                            Nx2Perf::On(Nx2Perf::Opt::MacroReadCbOverlay) &&
                            Nx2Perf::On(Nx2Perf::Opt::MacroReadWriterSet));
    const SplitRingMode ring_mode = !Nx2Perf::On(Nx2Perf::Opt::SplitRings) ? SplitRingMode::Off
                                    : Nx2Perf::Verify()                     ? SplitRingMode::Verify
                                                                            : SplitRingMode::On;
    writes.SetRings(ring_mode);
    cb_words.SetRings(ring_mode);
#ifdef ARCHITECTURE_x86_64
    // Front macros run compiled (macro_turing_jit.h) only.
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
    wake.notify_one();
    back_thread.join();
    active = false;
    // The split is static and outlives the emulation session. The next session's back thread
    // starts reading at 0: with the old positions kept it skipped through the new, empty ring a
    // wrap at a time and never reached the old write position (the next game's first GPU work
    // never ran, it waited on its syncpoint forever), and the engines it held were freed.
    ring.reset();
    write_position = 0;
    last_writer_position = 0;
    last_sync_position = 0;
    last_function_position = 0;
    writes.Reset();
    async_writes.clear();
    cb_words.Clear();
    cb_words_lost = 0;
    note_memory = nullptr;
    note_page = ~GPUVAddr{0};
    published_local = 0;
    consumed_cache = 0;
    repeat_end = ~u64{0};
    publish_every = PublishEvery;
    front_engine = nullptr;
    back_engine = nullptr;
    front_macros = false;
    published.store(0, std::memory_order_relaxed);
    consumed.store(0, std::memory_order_relaxed);
    back_waiting.store(false, std::memory_order_relaxed);
    back_idle.store(false, std::memory_order_relaxed);
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
            std::this_thread::yield();
        }
    }
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
        wake.notify_one();
    }
}

void GpuSplit::Drain(DrainSite site) {
    if (!active) {
        return;
    }
    if (Nx2Perf::Verify()) [[unlikely]] {
        CheckLazySkip("Drain");
    }
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache == write_position && BackQuiet()) {
        return;
    }
    Publish();
    hook_requested.store(true, std::memory_order_release);
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    const u64 start = Nx2Profile::Enabled() ? Nx2Profile::Now() : 0;
    // Everything consumed is not enough: the back submits deferred work (Scheduler::DeferFlush)
    // from its idle hook afterwards, and the caller may use the scheduler once this returns.
    bool hook_asked = false;
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache == write_position) {
            if (BackQuiet()) {
                publish_every = 8;
                Nx2Profile::AddDrain(site, start);
                return;
            }
            if (!hook_asked) {
                RequestIdleHook();
                hook_asked = true;
            }
        }
        if (consumed_cache == write_position && !hook_requested.load(std::memory_order_relaxed)) {
            hook_requested.store(true, std::memory_order_release);
        }
        Backoff(spins, write_position);
    }
}

bool GpuSplit::WaitForWriters() {
    return WaitFor(std::min(last_writer_position, write_position), DrainSite::ListWriters);
}

bool GpuSplit::WaitForSync() {
    return WaitFor(std::min(last_sync_position, write_position), DrainSite::ListSync);
}

bool GpuSplit::WaitFor(u64 target, DrainSite site) {
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache >= target) {
        return false;
    }
    // ReleaseBeforeHook: these callers only read guest memory, page flags and the front's write
    // tracker afterwards, never the scheduler, so they need not wait for the back's idle hook.
    const bool release_first =
        release_before_hook && (site == DrainSite::MacroDmaWrites ||
                                site == DrainSite::AcquireEarly || site == DrainSite::Map);
    if (release_first) {
        wait_release_first.store(target, std::memory_order_relaxed);
    }
    wait_site.store(static_cast<u32>(site), std::memory_order_relaxed);
    wait_target.store(target, std::memory_order_seq_cst);
    Publish();
    const u64 start = Nx2Profile::Enabled() ? Nx2Profile::Now() : 0;
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache >= target) {
            wait_target.store(~u64{0}, std::memory_order_relaxed);
            if (release_first) {
                wait_release_first.store(~u64{0}, std::memory_order_relaxed);
            }
            Nx2Profile::AddDrain(site, start);
            return true;
        }
        Backoff(spins, target);
    }
}

void GpuSplit::Backoff(u32 spins, u64 target) {
    if (spins < 16384) {
        Pause();
        return;
    }
    if (!Nx2Perf::On(Nx2Perf::Opt::FrontSleep) || !back_running.load(std::memory_order_acquire)) {
        std::this_thread::yield();
        return;
    }
    // A long wait sleeps until the back stores a new position (port of #161's front_wake): the
    // back checks front_waiting after each store, the front checks the position after setting it.
    front_waiting.store(true, std::memory_order_seq_cst);
    const u32 seen = front_wake.load(std::memory_order_seq_cst);
    if (consumed.load(std::memory_order_seq_cst) < target) {
        front_wake.wait(seen, std::memory_order_seq_cst);
    }
    front_waiting.store(false, std::memory_order_relaxed);
}

bool GpuSplit::ReleaseFirst(u64 target) {
    // `target` was read from wait_target (acquire) first: a wait that does not set the flag stored
    // its clear (from the wait before) ahead of its target, so a stale flag never matches it.
    const u64 tag = wait_release_first.load(std::memory_order_acquire);
    if (tag == ~u64{0}) [[likely]] {
        return false;
    }
    const bool release_first = tag == target;
    if (Nx2Perf::Verify()) {
        static std::atomic<u32> reports{};
        if (!release_first) {
            const u32 count = reports.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 64) {
                LOG_ERROR(HW_GPU,
                          "Nx2Perf release_before_hook: flag for target {:#x} seen with wait "
                          "target {:#x} ({} so far)",
                          tag, target, count);
            }
        }
        return false;
    }
    return release_first;
}

void GpuSplit::SwitchEngine(Tegra::Engines::EngineInterface* engine) {
    front_engine = engine;
    u64* const entry = Reserve(2);
    entry[0] = static_cast<u64>(Kind::Engine);
    entry[1] = reinterpret_cast<u64>(engine);
    Commit(2, Effect::None);
}

void GpuSplit::MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                           u32 amount, u32 methods_pending, const GPUVAddr* segment,
                           Effect effect) {
    SelectEngine(engine);
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
        Commit(size, effect);
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
    last_function_position = write_position;
}

bool GpuSplit::NoteWrite(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size,
                         WriteKind kind) {
    if (size == 0 || !Nx2Perf::On(Nx2Perf::Opt::MacroReadEarly)) {
        return false;
    }
    // While a queued function may still change the GPU mappings, the back may translate the
    // address differently. Sparse pages are written through their own backing.
    const bool sparse = address < Tegra::g_nx2_sparse_high.load(std::memory_order_relaxed) &&
                        Tegra::g_nx2_sparse_low.load(std::memory_order_relaxed) < address + size;
    if (last_function_position > consumed_cache || sparse) {
        writes.Entry(Effect::Unknown, write_position);
        return false;
    }
    constexpr GPUVAddr page_mask = ~GPUVAddr{0xfff};
    const bool exact = Nx2Perf::On(Nx2Perf::Opt::ExactNotedWrites);
    for (GPUVAddr page = address & page_mask; page < address + size; page += 0x1000) {
        if (&memory != note_memory || page != note_page || note_function != last_function_position) {
            const std::optional<DAddr> device = memory.GpuToCpuAddress(page);
            if (!device) {
                note_memory = nullptr;
                writes.Entry(Effect::Unknown, write_position);
                return false;
            }
            note_memory = &memory;
            note_page = page;
            note_device = *device & page_mask;
            note_function = last_function_position;
        }
        // ExactNotedWrites: only the bytes in this page. Exact ranges merge less, so past half
        // of MaxWrites they go back to whole pages before the overflow makes them unknown.
        if (exact && writes.PendingWrites() < SplitWriteTracker::MaxWrites / 2) {
            writes.Write(note_device + ((std::max)(page, address) - page),
                         note_device + ((std::min)(page + 0x1000, address + size) - page),
                         write_position, kind);
        } else {
            writes.Write(note_device, note_device + 0x1000, write_position, kind);
        }
    }
    return true;
}

u64 GpuSplit::VerifyExactRead(bool differed) {
    static std::atomic<u64> reads{};
    static std::atomic<u64> mismatches{};
    const u64 differed_so_far =
        differed ? mismatches.fetch_add(1, std::memory_order_relaxed) + 1
                 : mismatches.load(std::memory_order_relaxed);
    const u64 count = reads.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((count & 4095) == 0 || count <= 1 || (differed && differed_so_far <= 64)) {
        LOG_INFO(HW_GPU,
                 "Nx2Perf exact_noted_writes: {} reads with an exact target below the page "
                 "target, {} differed",
                 count, differed_so_far);
    }
    return differed_so_far;
}

void GpuSplit::NoteConstBufferWrite(const Tegra::MemoryManager& memory, GPUVAddr address,
                                    const u32* words, u32 count, u32 stored) {
    const bool noted =
        NoteWrite(memory, address, u64{count} * sizeof(u32), WriteKind::ConstantBuffer);
    // Not noted by range: an unknown write, which early reads wait for anyway.
    if (!noted || stored == 0 || !Nx2Perf::On(Nx2Perf::Opt::MacroReadCbOverlay)) {
        return;
    }
    if ((address & 3) != 0 || cb_words.Size() + stored > MaxConstBufferWords) {
        cb_words_lost = write_position;
        return;
    }
    // NoteWrite translated these pages with no queued function pending: as the back will.
    constexpr GPUVAddr page_mask = ~GPUVAddr{0xfff};
    GPUVAddr page = ~GPUVAddr{0};
    u64 device_page = 0;
    for (u32 i = 0; i < stored; ++i) {
        const GPUVAddr word_address = address + u64{i} * sizeof(u32);
        if ((word_address & page_mask) != page) {
            page = word_address & page_mask;
            if (&memory == note_memory && page == note_page) {
                device_page = note_device;
            } else if (const std::optional<DAddr> device = memory.GpuToCpuAddress(page)) {
                device_page = *device & page_mask;
            } else {
                Nx2Profile::Add(Nx2Profile::Counter::CbWordsNoted, i);
                cb_words_lost = write_position;
                return;
            }
        }
        cb_words.Push({device_page + (word_address & 0xfff), write_position, words[i]});
    }
    Nx2Profile::Add(Nx2Profile::Counter::CbWordsNoted, stored);
}

void GpuSplit::OverlayConstBufferWords(u64 consumed_position, u64 begin, u64 size,
                                       u8* out) const {
    cb_words.Overlay(consumed_position, begin, size, out);
}

u64 GpuSplit::SettleWrites() {
    WaitFor((std::min)(last_function_position, write_position), DrainSite::Map);
    return RetireWrites();
}

u64 GpuSplit::RetireWrites() {
    consumed_cache = consumed.load(std::memory_order_acquire);
    writes.Retire(consumed_cache);
    cb_words.Retire(consumed_cache);
    return consumed_cache;
}

void GpuSplit::ExecuteRepeated(u64 head) {
    // Only Maxwell3D's draw.begin is sent with a count (Maxwell3D::FrontWrite), without a
    // segment. Every path below would consume the sink and call it, once per entry.
    using Tegra::Engines::Maxwell3D;
    auto* const maxwell3d = static_cast<Maxwell3D*>(back_engine);
    Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
    maxwell3d->ConsumeSink(*system);
    maxwell3d->CallMethod(*system, static_cast<u32>((head >> 8) & 0xffff),
                          static_cast<u32>(head >> 32), (head & IsLast) != 0);
    maxwell3d->draw_manager.AddInstances(*maxwell3d,
                                         static_cast<u32>((head & RepeatMask) >> 24));
}

void GpuSplit::VerifyRepeat(u64 head) {
    const u64 word = head & ~Repeatable;
    if ((head & Repeatable) != 0) {
        ++verify_mergeable;
        if (verify_previous != word) {
            static std::atomic<u64> differences{};
            const u64 count = differences.fetch_add(1, std::memory_order_relaxed) + 1;
            ++verify_mismatches;
            if (count <= 64) {
                LOG_ERROR(HW_GPU,
                          "Nx2Perf draw_begin_coalesce: mergeable entry {:#x} after {:#x}, not "
                          "the same draw.begin ({} so far)",
                          head, verify_previous, count);
            }
        }
        if ((verify_mergeable & 1023) == 0) {
            const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
            if (verify_report_ns == 0) {
                verify_report_ns = now;
            } else if (now - verify_report_ns >= 1'000'000'000) {
                const double seconds = static_cast<double>(now - verify_report_ns) / 1e9;
                LOG_INFO(HW_GPU,
                         "Nx2Perf draw_begin_coalesce: {:.0f} mergeable draw.begin entries/s, {} "
                         "not after the same one",
                         static_cast<double>(verify_mergeable) / seconds, verify_mismatches);
                verify_report_ns = now;
                verify_mergeable = 0;
                verify_mismatches = 0;
            }
        }
    }
    verify_previous = word;
}

size_t GpuSplit::Execute(const u64* entry, size_t words) {
    const u64 head = entry[0];
    if (verify_repeats) [[unlikely]] {
        VerifyRepeat(head);
    }
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        if ((head & RepeatMask) != 0) [[unlikely]] {
            ExecuteRepeated(head);
            return 1;
        }
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        if (engine->fast_maxwell3d) {
            // Nx2Perf: as below without the profile zones (off with fast_maxwell3d) and the
            // virtual call.
            using Tegra::Engines::Maxwell3D;
            auto* const maxwell3d = static_cast<Maxwell3D*>(engine);
            constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
            if (method - cb_data < 16 && (head & HasSegment) == 0 &&
                Nx2Perf::On(Nx2Perf::Opt::CbBatch)) {
                // Single constant buffer words in a row (macros write them one by one) go up in
                // one write. They are executable: each entry would only consume the sink first.
                std::array<u32, 64> methods;
                std::array<u32, 64> arguments;
                methods[0] = method;
                arguments[0] = argument;
                u32 count = 1;
                while (count < methods.size() && count < words) {
                    const u64 next = entry[count];
                    const u32 next_method = static_cast<u32>((next >> 8) & 0xffff);
                    if (static_cast<Kind>(next & 0xf) != Kind::Method || (next & HasSegment) != 0 ||
                        next_method - cb_data >= 16) {
                        break;
                    }
                    methods[count] = next_method;
                    arguments[count] = static_cast<u32>(next >> 32);
                    ++count;
                }
                maxwell3d->ConsumeSink(*system);
                if (count == 1) {
                    maxwell3d->CallMethod(*system, method, argument, true);
                } else {
                    maxwell3d->CallConstBufferWords(*system, methods.data(), arguments.data(),
                                                    count);
                }
                return count;
            }
            if (!maxwell3d->execution_mask[method] && (head & HasSegment) == 0 &&
                method < Maxwell3D::Regs::NUM_REGS && !Nx2Perf::Verify()) {
                if (!maxwell3d->method_sink.empty()) [[unlikely]] {
                    maxwell3d->ConsumeSink(*system);
                }
                return maxwell3d->CallStateMethodRun(entry, words);
            }
            if ((head & Direct) != 0) {
                maxwell3d->ConsumeSink(*system);
                maxwell3d->CallMethod(*system, method, argument, true);
            } else if (!maxwell3d->execution_mask[method]) {
                maxwell3d->method_sink.emplace_back(method, argument);
            } else {
                maxwell3d->ConsumeSink(*system);
                if ((head & HasSegment) != 0) {
                    maxwell3d->current_dma_segment = entry[1];
                }
                maxwell3d->CallMethod(*system, method, argument, (head & IsLast) != 0);
            }
            return (head & HasSegment) != 0 ? 2 : 1;
        }
        if ((head & Direct) != 0) {
            static const bool method_fast = [] {
                const char* value = std::getenv("EDEN_NX2_METHOD_FAST");
                return value == nullptr || value[0] != '0';
            }();
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            if (!engine->method_sink.empty()) {
                engine->ConsumeSink(*system);
            }
            if (method_fast && !engine->execution_mask[method]) {
                engine->CallStateMethod(*system, method, argument);
            } else {
                engine->CallMethod(*system, method, argument, true);
            }
        } else if (!engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, argument);
        } else {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
        }
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
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
        reinterpret_cast<void (*)(const u64*)>(entry[1])(entry + 2);
        return 2 + payload;
    }
    }
    return 1;
}

void GpuSplit::RunIdleHook(IdleHookReason reason) {
    Nx2Profile::Scope scope{Nx2Profile::Zone::BackIdleHook};
    idle_hook(idle_argument, reason);
}

IdleHookReason GpuSplit::WaitReason() const {
    if (hook_requested.load(std::memory_order_relaxed)) {
        return IdleHookReason::Requested;
    }
    // wait_target was read with acquire: the site stored before it is visible. A stale target
    // (a wait already over) may pair with a newer site, which only picks the hook's timing.
    switch (static_cast<DrainSite>(wait_site.load(std::memory_order_relaxed))) {
    case DrainSite::MacroDmaWrites:
    case DrainSite::AcquireEarly:
    case DrainSite::Map:
        return IdleHookReason::WaitMemory;
    default:
        return IdleHookReason::WaitResults;
    }
}

s64 GpuSplit::HookFlushDeadlineNs() {
    static const s64 deadline_ns = [] {
        const char* value = std::getenv("EDEN_NX2_HOOK_FLUSH_US");
        const s64 us = value != nullptr ? static_cast<s64>(std::atoll(value)) : s64{1000};
        return std::max<s64>(us, 0) * 1000;
    }();
    return deadline_ns;
}

void GpuSplit::ReportLazySkip(const char* where) const {
    const s64 since = lazy_skip_since.load(std::memory_order_relaxed);
    if (since == 0) {
        return;
    }
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (now - since <= HookFlushDeadlineNs()) {
        return;
    }
    static std::atomic<u64> missed{};
    const u64 count = missed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 64 || (count & (count - 1)) == 0) {
        LOG_ERROR(HW_GPU,
                  "Nx2Perf hook_lazy_flush: {} found a skipped deferred submit pending for {} us "
                  "(deadline {} us; {} missed submits so far)",
                  where, (now - since) / 1000, HookFlushDeadlineNs() / 1000, count);
    }
}

void GpuSplit::BackLoop(std::stop_token stop_token) {
    Common::SetCurrentThreadName("GPUBack");
    on_back_thread = true;
    back_running.store(true, std::memory_order_release);
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    if (Nx2Perf::On(Nx2Perf::Opt::GpuCorePin)) {
        Common::PinCurrentThreadToGpuSlot(1);
    }
    system->RegisterHostThread();
#ifdef _WIN32
    g_back_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
#endif
    u64 read_position = 0;
    while (!stop_token.stop_requested()) {
        Nx2Profile::Report("GPU back thread");
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            if (Nx2Perf::FenceTimeline::On()) [[unlikely]] {
                Nx2Perf::FenceTimeline::BeginBackIdle();
            }
            back_idle.store(true, std::memory_order_relaxed);
            // The front usually sends more within microseconds: spin briefly before sleeping.
            for (u32 spins = 0; spins < 8192 && available == read_position &&
                                !hook_requested.load(std::memory_order_relaxed);
                 ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                hook_requested.store(false, std::memory_order_seq_cst);
                // The front may already see everything consumed: Drain() waits for back_quiet
                // too, so the deferred submit can't overlap whatever the front does next.
                if (!back_quiet.load(std::memory_order_relaxed)) {
                    if (idle_hook != nullptr) {
                        RunIdleHook(IdleHookReason::Sleep);
                    }
                    back_quiet.store(true, std::memory_order_release);
                }
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once.
                back_waiting.store(true, std::memory_order_seq_cst);
                while (available == read_position && !stop_token.stop_requested()) {
                    const u32 seen = wake.load(std::memory_order_seq_cst);
                    available = published.load(std::memory_order_seq_cst);
                    if (available == read_position && !stop_token.stop_requested()) {
                        wake.wait(seen, std::memory_order_seq_cst);
                        available = published.load(std::memory_order_acquire);
                    }
                }
                back_waiting.store(false, std::memory_order_seq_cst);
                back_idle.store(false, std::memory_order_relaxed);
                if (Nx2Perf::FenceTimeline::On()) [[unlikely]] {
                    Nx2Perf::FenceTimeline::EndBackIdle();
                }
                continue;
            }
            back_idle.store(false, std::memory_order_relaxed);
            if (Nx2Perf::FenceTimeline::On()) [[unlikely]] {
                Nx2Perf::FenceTimeline::EndBackIdle();
            }
        }
        u64 last_store = read_position;
        // Before any entry runs, so a front that sees these entries consumed also sees this.
        back_quiet.store(false, std::memory_order_seq_cst);
        while (read_position != available) {
            const size_t index = read_position & RingMask;
            size_t size;
            {
                Nx2Profile::Scope replay_scope{Nx2Profile::Zone::BackReplay};
                size = Execute(&ring[index],
                               std::min<u64>(available - read_position, RingWords - index));
            }
            read_position += size != 0 ? size : RingWords - index;
            if (hook_requested.load(std::memory_order_relaxed) &&
                idle_hook != nullptr) [[unlikely]] {
                hook_requested.store(false, std::memory_order_seq_cst);
                RunIdleHook(IdleHookReason::Requested);
            }
            // Hand ring space back now and then while working through a long batch, and at once
            // when the front waits for this position (WaitForWriters).
            if ((read_position - last_store >= 4096 ||
                 read_position >= wait_target.load(std::memory_order_relaxed)) &&
                read_position != available) {
                const u64 target = wait_target.load(std::memory_order_acquire);
                const bool release_first =
                    idle_hook != nullptr && read_position >= target && ReleaseFirst(target);
                if (idle_hook != nullptr && read_position >= target && !release_first) {
                    RunIdleHook(WaitReason());
                }
                consumed.store(read_position, std::memory_order_seq_cst);
                last_store = read_position;
                NotifyFront();
                if (release_first) {
                    RunIdleHook(WaitReason());
                }
            }
        }
        // A list wait (and a batch end nobody waits for) keeps the hook first: a front that sees
        // the position reads the list copies the hook records and submits.
        const u64 target = wait_target.load(std::memory_order_acquire);
        const bool release_first =
            idle_hook != nullptr && read_position >= target && ReleaseFirst(target);
        const IdleHookReason reason =
            read_position >= target ? WaitReason()
            : hook_requested.load(std::memory_order_relaxed) ? IdleHookReason::Requested
                                                             : IdleHookReason::BatchEnd;
        if (idle_hook != nullptr && !release_first) {
            RunIdleHook(reason);
        }
        consumed.store(read_position, std::memory_order_seq_cst);
        NotifyFront();
        if (release_first) {
            RunIdleHook(reason);
        }
    }
    back_running.store(false, std::memory_order_seq_cst);
    front_wake.fetch_add(1, std::memory_order_seq_cst);
    front_wake.notify_all();
}

} // namespace VideoCommon

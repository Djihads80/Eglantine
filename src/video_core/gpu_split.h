// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Tegra::Engines {
class EngineInterface;
}

namespace VideoCommon::Nx2Profile {
enum class Zone : u32;
}

namespace VideoCommon {

// Splits the GPU thread in two (NX2_GPU_THREAD_SPLIT.md). The GPU thread (the front) takes the
// CPU's commands, parses command lists and runs the puller; engine method calls and everything
// behind them (register state, macros, draws, the rasterizer and its caches) run in order on a
// second thread (the back), fed through a single-producer single-consumer ring. Work the front
// would do on back-owned state is deferred into the same ring; the front waits for the back to
// catch up (Drain) only where it needs a result from that state, e.g. a semaphore it acquires.
// On for Switch 2 titles with the 3D and compute engines' macros on the front, against copies of
// the registers they read (Maxwell3D and KeplerCompute Front*), which sends the writes they make.
// EDEN_NX2_SPLIT=1 keeps the macros on the back; EDEN_NX2_SPLIT=0 runs everything on one thread.
class GpuSplit {
public:
    static GpuSplit& Get() {
        return instance;
    }

    /// Whether the front sends work to the back (front thread only).
    [[nodiscard]] bool Active() const {
        return active;
    }
    /// Whether the front runs the engines' macros (EDEN_NX2_SPLIT=2).
    [[nodiscard]] bool FrontMacros() const {
        return front_macros;
    }

    /// Whether the calling thread is the back.
    [[nodiscard]] static bool OnBackThread() {
        return on_back_thread;
    }
    /// Called on the back before it sleeps waiting for work (the rasterizer submits deferred work).
    void SetIdleHook(void (*hook)(void*), void* argument) {
        idle_hook = hook;
        idle_argument = argument;
    }
    /// Any thread: whether the back thread runs (false before it starts and once it stopped).
    [[nodiscard]] bool BackRunning() const {
        return back_running.load(std::memory_order_acquire);
    }
    /// Any thread: has the back call the idle hook soon, between entries or at once if it sleeps.
    void RequestIdleHook();

    /// Starts the back thread once the split is wanted (EDEN_NX2_SPLIT=1, a Switch 2 title,
    /// commands read without cache flushes). Called by the front between commands.
    void MaybeStart(Core::System& system);
    void Stop();

    /// An engine method write, executed as DmaPusher would (method sink, ConsumeSink, call).
    /// `segment`: the engine's current_dma_segment for the call, when the engine reads it.
    void Method(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument, bool is_last,
                const GPUVAddr* segment) {
        SelectEngine(engine);
        const size_t words = segment != nullptr ? 2 : 1;
        u64* const entry = Reserve(words);
        entry[0] = static_cast<u64>(Kind::Method) | (is_last ? IsLast : 0) |
                   (segment != nullptr ? HasSegment : 0) | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        if (segment != nullptr) {
            entry[1] = *segment;
        }
        Commit(words);
    }
    void MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                     u32 amount, u32 methods_pending, const GPUVAddr* segment);
    /// A word of a run of one engine method (a macro's constant buffer data): goes with the
    /// words queued before it as one multi-method entry, sent before anything else enters the
    /// ring (Reserve, Publish, Drain and WaitForWriters flush it), so the order is kept.
    void QueueWord(Tegra::Engines::EngineInterface* engine, u32 method, u32 word) {
        if (pending_count != 0 && (engine != pending_engine || method != pending_method ||
                                   pending_count == PendingWords)) [[unlikely]] {
            FlushPending();
        }
        if (pending_count == 0) {
            pending_engine = engine;
            pending_method = method;
        }
        pending_words[pending_count++] = word;
    }
    void FlushPending();
    /// A pushbuffer run of `count` incrementing methods from `first_method` within the 3D
    /// engine's constant buffer offset and data registers (EngineInterface::LoadConstantRun).
    void ConstantRun(Tegra::Engines::EngineInterface* engine, u32 first_method, const u32* words,
                     u32 count);
    /// A write made by a macro: CallMethod without the method sink, as the engines' macros do.
    void MacroWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument) {
        SelectEngine(engine);
        u64* const entry = Reserve(1);
        entry[0] = static_cast<u64>(Kind::Method) | IsLast | Direct | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        Commit(1);
    }

    /// Runs `func` on the back after everything sent before it. Must be trivially copyable.
    template <typename Func>
    void Defer(const Func& func) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t payload = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        u64* const entry = Reserve(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        Commit(2 + payload);
    }
    void DeferFunction(std::function<void()>&& func);

    /// EDEN_NX2_SPLIT_CHECK=1 (debug): reports, with its stack, a thread other than the back
    /// using back-owned state (the Vulkan scheduler) while the back runs entries.
    static inline bool check_owner{};
    static void CheckOwner(const char* where);

    /// Makes what was sent visible to the back.
    void Publish();
    /// Waits until the back executed everything sent so far. `zone`: the profile zone the wait
    /// counts as (SplitDrain, or SplitDmaRead for a macro DMA read).
    void Drain();
    void Drain(Nx2Profile::Zone zone);

    /// Front thread: the ring position after everything sent so far (an entry sent now ends
    /// below the position returned after it is sent).
    [[nodiscard]] u64 WritePosition() const {
        return write_position;
    }
    /// Front thread: the ring position the back has executed up to (as last seen; refreshed).
    [[nodiscard]] u64 Consumed() {
        consumed_cache = consumed.load(std::memory_order_acquire);
        return consumed_cache;
    }

    /// Compute or copy work was just sent: what can write GPU-written command lists.
    void NoteWriter() {
        last_writer_position = write_position;
    }
    /// Waits until the back executed the compute and copy work sent so far (not what came after).
    /// Returns whether any of it was still queued.
    bool WaitForWriters();
    /// Waits until the back executed what was sent before `position`. Returns whether it waited.
    bool WaitForPosition(u64 position);
    /// Front thread: the ring position after the last compute or copy work sent.
    [[nodiscard]] u64 WriterPosition() const {
        return last_writer_position;
    }

    /// Front thread: the entry just sent writes this device range when the back runs it (a
    /// report, an inline upload), not through the GPU. MME DMA reads of it wait for it.
    void NotePendingWrite(u64 device_address, u64 size) {
        if (size == 0) {
            return;
        }
        for (u64 page = device_address >> PendingPageBits;
             page <= (device_address + size - 1) >> PendingPageBits; ++page) {
            u64& slot = pending_writes[PendingSlot(page)];
            slot = (std::max)(slot, write_position);
        }
    }
    /// Front thread: as NotePendingWrite, for a range too large to list: every read waits for it.
    void NotePendingWriteAnywhere() {
        pending_anywhere = write_position;
    }
    /// Front thread: the ring position after the last such write to the range sent (pages may
    /// share a slot: never less than the true one), 0 for none.
    [[nodiscard]] u64 PendingWrite(u64 device_address, u64 size) const {
        u64 position = pending_anywhere;
        for (u64 page = device_address >> PendingPageBits;
             size != 0 && page <= (device_address + size - 1) >> PendingPageBits; ++page) {
            position = (std::max)(position, pending_writes[PendingSlot(page)]);
        }
        return position;
    }

private:
    enum class Kind : u64 {
        Wrap,   ///< The rest of the ring is unused: continue at its start
        Engine, ///< The engine the next methods go to: word 1
        Method, ///< Bits 8-23 method, 32-63 argument; flags IsLast, HasSegment (word 1)
        Multi,  ///< Bits 8-23 method, 32-63 amount; word 1 methods pending, [segment], words
        Call,   ///< Bits 8-31 payload size in words; word 1 thunk, then the payload
        ConstantRun, ///< Bits 8-23 first method, 32-63 count; then the words (two per u64)
    };
    static constexpr u64 IsLast = 1 << 4;
    static constexpr u64 HasSegment = 1 << 5;
    static constexpr u64 Direct = 1 << 6; ///< Method: call it even if not executable (macros)

    static constexpr size_t RingWords = size_t{1} << 20; // 8 MiB
    static constexpr size_t RingMask = RingWords - 1;
    static constexpr u32 MaxMultiWords = 8192;
    static constexpr size_t PublishEvery = 256;

    template <typename Func>
    static void CallThunk(const u64* payload) {
        (*reinterpret_cast<const Func*>(payload))();
    }

    /// Room for an entry of `words` at the write position (never across the ring's end).
    u64* Reserve(size_t words) {
        if (pending_count != 0) [[unlikely]] {
            FlushPending();
        }
        const size_t index = write_position & RingMask;
        if (index + words <= RingWords && write_position + words - consumed_cache <= RingWords)
            [[likely]] {
            return &ring[index];
        }
        return ReserveSlow(words);
    }
    u64* ReserveSlow(size_t words);
    void Commit(size_t words) {
        write_position += words;
        if (write_position - published_local >= publish_every) {
            Publish();
        }
    }
    void WaitForSpace(size_t words);
    /// Sleeps a while on the back thread handing ring space back.
    void SleepOnBack();
    /// Back side: hands the ring up to `position` back, waking a sleeping front.
    void HandBack(u64 position);
    void SelectEngine(Tegra::Engines::EngineInterface* engine) {
        // Words queued (QueueWord) go first, before the engine is compared: their entry selects
        // their own engine, which may differ from this one, and from front_engine as it is now.
        if (pending_count != 0) [[unlikely]] {
            FlushPending();
        }
        if (engine != front_engine) [[unlikely]] {
            SwitchEngine(engine);
        }
    }
    void SwitchEngine(Tegra::Engines::EngineInterface* engine);

    void BackLoop(std::stop_token stop_token);
    size_t Execute(const u64* entry);

    Core::System* system{};
    bool active{};
    bool front_macros{};
    std::unique_ptr<u64[]> ring;

    // Front side.
    u64 write_position{};
    u64 last_writer_position{};
    static constexpr u64 PendingPageBits = 12;
    static constexpr size_t PendingSlots = 4096;
    [[nodiscard]] static size_t PendingSlot(u64 page) {
        return static_cast<size_t>((page * 0x9e3779b97f4a7c15ULL) >> 52);
    }
    std::array<u64, PendingSlots> pending_writes{};
    u64 pending_anywhere{};
    u64 published_local{};
    u64 consumed_cache{};
    /// Words between publishes: small right after a drain, while the back has nothing to do.
    size_t publish_every{PublishEvery};
    Tegra::Engines::EngineInterface* front_engine{};
    /// QueueWord: a run not yet sent.
    static constexpr u32 PendingWords = 256;
    std::array<u32, PendingWords> pending_words{};
    u32 pending_count{};
    u32 pending_method{};
    Tegra::Engines::EngineInterface* pending_engine{};

    // Back side.
    Tegra::Engines::EngineInterface* back_engine{};

    static GpuSplit instance;
    static inline thread_local bool on_back_thread{};
    void (*idle_hook)(void*){};
    void* idle_argument{};
    alignas(64) std::atomic<bool> hook_requested{};
    std::atomic<bool> back_running{};

    // 128 bytes: x86 spatial prefetchers pull 64-byte lines in pairs. back_waiting and wake are
    // always touched together by both threads, so they share a block.
    alignas(128) std::atomic<u64> published{};
    alignas(128) std::atomic<u64> consumed{};
    /// A position the front waits for (WaitForWriters): the back stores `consumed` once past it.
    alignas(128) std::atomic<u64> wait_target{~u64{0}};
    alignas(128) std::atomic<bool> back_waiting{};
    std::atomic<u32> wake{};
    /// Set while the front sleeps on the back thread; the back bumps `drained` when it hands ring
    /// space back then.
    alignas(64) std::atomic<bool> front_waiting{};
    std::atomic<u32> drained{};
    std::jthread back_thread;
};

} // namespace VideoCommon

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include "common/common_types.h"
#include "video_core/split_write_tracker.h"

namespace Core {
class System;
}

namespace Tegra {
class MemoryManager;
}

namespace Tegra::Engines {
class EngineInterface;
}

namespace VideoCommon {

/// Why the split's front waited for its back (EDEN_NX2_GPU_PROFILE counts each).
enum class DrainSite : u32 {
    Other,
    FetchDrain,     ///< EDEN_NX2_FETCH_DRAIN: before every entry
    Acquire,        ///< A semaphore acquire reads the value the back may release (other paths)
    AcquireTrigger, ///< The same, an Ampere semaphore trigger acquire checked when parsed
    AcquirePoll,    ///< The same, a pending acquire polled (Puller::CanResumeAcquire)
    AcquireEarly,   ///< Opt::AcquireEarly: only the queued possible writers of the word's page
    SemaphoreFlush, ///< The acquired semaphore is in the buffer cache
    FenceRelease,   ///< The puller's acquires release the pending fences
    ListWriters,    ///< GPU-written command lists: the dispatches and copies sent before them
    ListSync,       ///< Command lists on draw-written pages: the work before the last host sync
    ListFlush,      ///< Command lists on GPU-written pages: drain and flush the pages
    MacroDmaRead,   ///< The front's macros read memory (MME DMA)
    MacroDmaWrites, ///< The same, waiting only for the queued work that may write it
    MacroDmaFlush,  ///< Those words are in the buffer cache
    GpSyncFence,    ///< A GP entry's sync bit (sync_memory_operations)
    Map,            ///< GPU memory maps before later command reads
    FrontStart,     ///< The front takes an engine's macros over
    FrontVerify,    ///< EDEN_NX2_FRONT_VERIFY
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(DrainSite::Count)> drain_site_names{
    "other",       "fetch drain",     "acquire",    "acquire trigger", "acquire poll",
    "acquire early", "semaphore flush", "fence release",
    "list writers", "list sync",      "list flush", "macro dma read",  "macro dma writes",
    "macro dma flush",
    "gp sync fence", "map",           "front start", "front verify",
};

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
    /// Marks the calling thread as the GPU thread (the front, or the only one without the split).
    static void MarkGpuThread() {
        on_gpu_thread = true;
    }
    /// Whether the calling thread owns the Vulkan scheduler: the back while it runs, else the
    /// GPU thread. Other threads (CPU cores, services) must not record or submit work.
    [[nodiscard]] static bool OwnsScheduler() {
        return Get().BackRunning() ? on_back_thread : on_gpu_thread;
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
    /// Any thread: has the back call the idle hook soon, between entries or before it sleeps.
    void RequestIdleHook() {
        hook_requested.store(true, std::memory_order_seq_cst);
    }
    /// Any thread, a hint (relaxed loads): whether the front waits for the back now, for a ring
    /// position (WaitFor), a drain or a submission (hook_requested), or asleep in Backoff.
    [[nodiscard]] bool FrontWaitsForBack() const {
        return wait_target.load(std::memory_order_relaxed) != ~u64{0} ||
               hook_requested.load(std::memory_order_relaxed) ||
               front_waiting.load(std::memory_order_relaxed);
    }

    /// Starts the back thread once the split is wanted (EDEN_NX2_SPLIT=1, a Switch 2 title,
    /// commands read without cache flushes). Called by the front between commands.
    void MaybeStart(Core::System& system);
    void Stop();

    /// An engine method write, executed as DmaPusher would (method sink, ConsumeSink, call).
    /// `segment`: the engine's current_dma_segment for the call, when the engine reads it.
    /// `effect`: what the entry may write (SplitWriteTracker; unknown unless the caller knows).
    using Effect = SplitWriteTracker::Effect;
    using WriteKind = SplitWriteTracker::WriteKind;
    void Method(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument, bool is_last,
                const GPUVAddr* segment, Effect effect = Effect::Unknown) {
        SelectEngine(engine);
        const size_t words = segment != nullptr ? 2 : 1;
        u64* const entry = Reserve(words);
        entry[0] = static_cast<u64>(Kind::Method) | (is_last ? IsLast : 0) |
                   (segment != nullptr ? HasSegment : 0) | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        if (segment != nullptr) {
            entry[1] = *segment;
        }
        Commit(words, effect);
    }
    void MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                     u32 amount, u32 methods_pending, const GPUVAddr* segment,
                     Effect effect = Effect::Unknown);
    /// A write made by a macro: CallMethod without the method sink, as the engines' macros do.
    void MacroWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument,
                    Effect effect = Effect::Unknown) {
        SelectEngine(engine);
        u64* const entry = Reserve(1);
        entry[0] = static_cast<u64>(Kind::Method) | IsLast | Direct | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        Commit(1, effect);
    }

    /// Nx2Perf draw_begin_coalesce: Method (MacroWrite when `direct`) for an executable write
    /// whose immediate repeat the back can apply from a count (DrawManager::AddInstances). The
    /// write is counted into the last entry (bits 24-31) when that one is the same word, sent by
    /// this function, followed by nothing, still unpublished and below 255 repeats; otherwise it
    /// is sent. With `verify` it is always sent, flagged Repeatable when it would have been
    /// counted. Returns whether it was (or would have been) counted.
    bool RepeatableMethod(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument,
                          bool is_last, bool direct, Effect effect, bool verify) {
        const u64 word = static_cast<u64>(Kind::Method) |
                         (direct ? IsLast | Direct : (is_last ? IsLast : 0)) |
                         (u64{method & 0xffff} << 8) | (u64{argument} << 32);
        // The entry at write_position - 1 is unpublished while published_local is below it.
        const bool repeat =
            repeat_end == write_position && engine == front_engine &&
            published_local < write_position &&
            (ring[(write_position - 1) & RingMask] & ~(Repeatable | RepeatMask)) == word &&
            (ring[(write_position - 1) & RingMask] & RepeatMask) != RepeatMask;
        if (repeat && !verify) {
            ring[(write_position - 1) & RingMask] += RepeatOne;
            writes.Entry(effect, write_position);
            if (eager_publish && back_idle.load(std::memory_order_relaxed)) {
                Publish();
            }
            return true;
        }
        SelectEngine(engine);
        u64* const entry = Reserve(1);
        // Reserve may have wrapped: then the flagged entry would not follow the first one.
        const bool flagged = repeat && write_position == repeat_end;
        entry[0] = word | (flagged ? Repeatable : 0);
        Commit(1, effect);
        repeat_end = write_position;
        return flagged;
    }

    /// Runs `func` on the back after everything sent before it. Must be trivially copyable.
    template <typename Func>
    void Defer(const Func& func, Effect effect = Effect::Unknown) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t payload = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        u64* const entry = Reserve(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        Commit(2 + payload, effect);
    }
    void DeferFunction(std::function<void()>&& func);

    /// Runs `func` on the back and waits for it: for work on back-owned state, such as a buffer
    /// cache download that records into the Vulkan scheduler. A drained back still submits from
    /// its idle hook, so the front never touches that state itself.
    template <typename Func>
    void Run(const Func& func, DrainSite site = DrainSite::Other) {
        if (on_back_thread || !active) {
            func();
            return;
        }
        Defer(func);
        Drain(site);
    }

    /// EDEN_NX2_SPLIT_CHECK=1 (debug): reports, with its stack, a thread other than the back
    /// using back-owned state (the Vulkan scheduler) while the back runs entries.
    static inline bool check_owner{};
    static void CheckOwner(const char* where);

    /// Makes what was sent visible to the back.
    void Publish();
    /// Waits until the back executed everything sent so far.
    void Drain(DrainSite site = DrainSite::Other);

    /// Compute or copy work was just sent: what can write GPU-written command lists.
    void NoteWriter() {
        last_writer_position = write_position;
        if (eager_publish) {
            Publish(); // the fetch that waits for this entry usually follows within microseconds
        }
    }
    /// Waits until the back executed the compute and copy work sent so far (not what came after).
    /// Returns whether any of it was still queued.
    bool WaitForWriters();
    /// A host-level wait was just sent (a syncpoint acquire, a host WFI, a GP entry with its sync
    /// bit; semaphore acquires drain): the guest's GPU work before it is complete before the
    /// commands after it are fetched.
    void NoteSync() {
        last_sync_position = write_position;
    }
    /// Waits until the back executed the work sent before the last NoteSync. Returns whether any
    /// of it was still queued.
    bool WaitForSync();
    /// The entry just sent also writes [address, address + size) of `memory`, noted by device
    /// page (other mappings may alias it, and the caches keep state per page; with
    /// Nx2Perf ExactNotedWrites, by device byte in each page). `size` must cover every byte the
    /// back writes for the entry. Returns whether it was noted by range (not as an unknown
    /// write).
    bool NoteWrite(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size,
                   WriteKind kind = WriteKind::Other);
    /// EDEN_NX2_PERF_VERIFY with Nx2Perf ExactNotedWrites: an early read whose exact target was
    /// below its page target was compared with the old answer. Logs the totals; returns how many
    /// differed so far (callers cap their detail lines at 64).
    static u64 VerifyExactRead(bool differed);
    /// The entry just sent writes the constant buffer data `words` (`count` of them, the first
    /// `stored` land: the rest fall past the bank) at `address`. With MacroReadCbOverlay the
    /// stored words are kept by device address until the back ran the entry
    /// (OverlayConstBufferWords).
    void NoteConstBufferWrite(const Tegra::MemoryManager& memory, GPUVAddr address,
                              const u32* words, u32 count, u32 stored);
    /// Whether every constant buffer word queued after `consumed_position` is kept (none was
    /// dropped).
    [[nodiscard]] bool ConstBufferWordsKept(u64 consumed_position) const {
        return cb_words_lost <= consumed_position;
    }
    /// Copies the kept constant buffer words queued after `consumed_position` over the bytes
    /// `out` holds for the device range [begin, begin + size), oldest first: the newest wins.
    void OverlayConstBufferWords(u64 consumed_position, u64 begin, u64 size, u8* out) const;
    /// The entry just sent writes guest memory the front can't tell.
    void NoteUnknownWrite() {
        writes.Entry(Effect::Unknown, write_position);
    }
    /// The next entry's Effect::Draw was classified with this program epoch (MacroReadWriterSet).
    void DrawEpoch(u64 epoch) {
        writes.DrawEpoch(epoch);
    }
    /// The entry just sent invalidates the device range [begin, begin + size) of the caches
    /// (MacroReadWriterSet: early reads check it against proven programs, as a noted write).
    void NoteDeviceRange(u64 begin, u64 size) {
        if (size != 0) {
            writes.Write(begin, begin + size, write_position);
        }
    }
    /// Waits for the queued functions (GPU memory maps among them) before the front translates
    /// addresses, then drops the pending writes the back has run. Returns the back's position.
    u64 SettleWrites();
    /// Drops the pending writes the back has run. Returns the back's position.
    u64 RetireWrites();
    /// Front: a write that lands when a host query completes or a fence signals (counter
    /// reports, delayed releases), not when the back runs it (Tegra::Nx2AsyncReportsSettled).
    /// Call it after the entry that issues the write was sent.
    void NoteAsyncWrite(u64 begin, u64 size) {
        for (u64 slot = begin >> 4; slot <= (begin + size - 1) >> 4; ++slot) {
            async_writes[slot] = write_position;
        }
    }
    /// Front: the ring position of the newest async write noted to these bytes, or 0.
    [[nodiscard]] u64 HasAsyncWrite(u64 begin, u64 size) const {
        u64 newest = 0;
        if (async_writes.empty()) {
            return newest;
        }
        for (u64 slot = begin >> 4; slot <= (begin + size - 1) >> 4; ++slot) {
            if (const auto it = async_writes.find(slot); it != async_writes.end()) {
                newest = (std::max)(newest, it->second);
            }
        }
        return newest;
    }
    /// Ring position the back must pass before the front translates GPU addresses (the queued
    /// functions, GPU memory maps among them; SettleWrites waits for it).
    [[nodiscard]] u64 MapTarget() const {
        return (std::min)(last_function_position, write_position);
    }
    /// Front: the ring position after the last entry sent.
    [[nodiscard]] u64 WritePosition() const {
        return write_position;
    }
    /// The back's position right now.
    [[nodiscard]] u64 ConsumedNow() const {
        return consumed.load(std::memory_order_acquire);
    }
    /// Ring position the back must pass to execute possible writers of [begin, end).
    /// Deferred fence writes can still be pending afterwards.
    [[nodiscard]] u64 ReadTarget(u64 begin, u64 end) const {
        return writes.ReadTarget(begin, end);
    }
    /// The same for a command list fetch from the device range [begin, end) on pages no GPU
    /// work wrote yet: every unknown write, the compute and copy work (NoteWriter) before the
    /// last host-level wait (NoteSync; the fetch runs ahead of the engines) and the noted writes
    /// to the range.
    [[nodiscard]] u64 ListTarget(u64 begin, u64 end) const {
        return (std::max)({writes.LastUnknown(),
                           (std::min)(last_writer_position, last_sync_position),
                           writes.LastWriteTo(begin, end)});
    }
    [[nodiscard]] const SplitWriteTracker& Writes() const {
        return writes;
    }
    /// Waits until the back consumed `position` (not what came after). Returns whether it had
    /// not yet.
    bool WaitUntil(u64 position, DrainSite site) {
        return WaitFor((std::min)(position, write_position), site);
    }

private:
    friend struct GpuSplitTestAccess;

    enum class Kind : u64 {
        Wrap,   ///< The rest of the ring is unused: continue at its start
        Engine, ///< The engine the next methods go to: word 1
        Method, ///< Bits 8-23 method, 32-63 argument; flags IsLast, HasSegment (word 1)
        Multi,  ///< Bits 8-23 method, 32-63 amount; word 1 methods pending, [segment], words
        Call,   ///< Bits 8-31 payload size in words; word 1 thunk, then the payload
    };
    static constexpr u64 IsLast = 1 << 4;
    static constexpr u64 HasSegment = 1 << 5;
    static constexpr u64 Direct = 1 << 6; ///< Method: call it even if not executable (macros)
    /// Method, EDEN_NX2_PERF_VERIFY with draw_begin_coalesce: the front would have counted this
    /// entry into the one before it (RepeatableMethod)
    static constexpr u64 Repeatable = 1 << 7;
    /// Method: how many more times the back calls it, right after the first (RepeatableMethod)
    static constexpr u64 RepeatOne = u64{1} << 24;
    static constexpr u64 RepeatMask = u64{0xff} << 24;
    static_assert(((u64{0xf} | IsLast | HasSegment | Direct) & Repeatable) == 0 &&
                  ((u64{0xffff} << 8) & RepeatMask) == 0);

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
        const size_t index = write_position & RingMask;
        if (index + words <= RingWords && write_position + words - consumed_cache <= RingWords)
            [[likely]] {
            return &ring[index];
        }
        return ReserveSlow(words);
    }
    u64* ReserveSlow(size_t words);
    void Commit(size_t words, Effect effect) {
        write_position += words;
        writes.Entry(effect, write_position);
        // EagerPublish: while the back has nothing to do, every entry goes out at once instead of
        // waiting for the next 256-word batch (the back idled while the front filled it, then the
        // front waited for the back to catch up).
        if (write_position - published_local >= publish_every ||
            (eager_publish && back_idle.load(std::memory_order_relaxed))) {
            Publish();
        }
    }
    void WaitForSpace(size_t words);
    void SelectEngine(Tegra::Engines::EngineInterface* engine) {
        if (engine != front_engine) [[unlikely]] {
            SwitchEngine(engine);
        }
    }
    void SwitchEngine(Tegra::Engines::EngineInterface* engine);
    bool WaitFor(u64 target, DrainSite site);

    /// One step of a front wait for the back to consume `target`.
    void Backoff(u32 spins, u64 target);
    /// Back: whether the front's wait for `target` lets the back store `consumed` before its
    /// idle hook (ReleaseBeforeHook).
    bool ReleaseFirst(u64 target);
    /// The idle hook (deferred submits), timed as its own profiler zone.
    void RunIdleHook();
    /// After the back stored `consumed`: wakes a front sleeping in Backoff.
    void NotifyFront() {
        if (front_waiting.load(std::memory_order_seq_cst)) {
            front_wake.fetch_add(1, std::memory_order_seq_cst);
            front_wake.notify_one();
        }
    }

    void BackLoop(std::stop_token stop_token);
    /// Runs the entry at `entry` (of the `words` published before the ring's end), returns its size.
    size_t Execute(const u64* entry, size_t words);
    /// A Method entry with a repeat count (RepeatableMethod): the call, then the repeats.
    void ExecuteRepeated(u64 head);
    /// EDEN_NX2_PERF_VERIFY with draw_begin_coalesce: checks that a Repeatable entry directly
    /// follows the same word.
    void VerifyRepeat(u64 head);

    Core::System* system{};
    bool active{};
    bool eager_publish{}; ///< Nx2Perf::Opt::EagerPublish, read once at Start
    bool release_before_hook{}; ///< Nx2Perf::Opt::ReleaseBeforeHook, read once at Start
    /// Nx2Perf::Verify() with draw_begin_coalesce, read once at Start before the back starts
    bool verify_repeats{};
    bool front_macros{};
    std::unique_ptr<u64[]> ring;

    // Front side.
    u64 write_position{};
    u64 last_writer_position{};
    u64 last_sync_position{};
    u64 last_function_position{};
    SplitWriteTracker writes;
    /// MacroReadCbOverlay: queued constant buffer words, oldest first, popped by RetireWrites.
    static constexpr size_t MaxConstBufferWords = SplitConstBufferWords::MaxWords;
    SplitConstBufferWords cb_words;
    u64 cb_words_lost{}; ///< Newest position of a constant buffer word not kept
    /// NoteWrite's last translation: memory manager, GPU page, device page, the function
    /// position it was made at (a later function may remap).
    const Tegra::MemoryManager* note_memory{};
    GPUVAddr note_page{~GPUVAddr{0}};
    u64 note_device{};
    u64 note_function{};
    u64 published_local{};
    u64 consumed_cache{};
    /// RepeatableMethod: the ring position after the last entry it sent
    u64 repeat_end{~u64{0}};
    /// Words between publishes: small right after a drain, while the back has nothing to do.
    size_t publish_every{PublishEvery};
    Tegra::Engines::EngineInterface* front_engine{};

    // Back side.
    Tegra::Engines::EngineInterface* back_engine{};
    /// VerifyRepeat: the last entry's word, the Repeatable entries seen and those that did not
    /// follow the same word, since the last report
    u64 verify_previous{};
    u64 verify_mergeable{};
    u64 verify_mismatches{};
    u64 verify_report_ns{};

    /// Whether the back is done with the Vulkan scheduler until it gets more work: set after its
    /// idle hook ran, cleared before it executes entries.
    [[nodiscard]] bool BackQuiet() const {
        return back_quiet.load(std::memory_order_acquire) ||
               !back_running.load(std::memory_order_acquire);
    }

    static GpuSplit instance;
    static inline thread_local bool on_back_thread{};
    static inline thread_local bool on_gpu_thread{};
    void (*idle_hook)(void*){};
    void* idle_argument{};
    alignas(64) std::atomic<bool> hook_requested{};
    std::atomic<bool> back_running{};
    std::atomic<bool> back_quiet{true};

    // 128 bytes: x86 spatial prefetchers pull 64-byte lines in pairs. back_waiting and wake are
    // always touched together by both threads, so they share a block.
    alignas(128) std::atomic<u64> published{};
    alignas(128) std::atomic<u64> consumed{};
    std::unordered_map<u64, u64> async_writes; ///< Front: 16-byte slot -> ring position
    /// A position the front waits for (WaitFor): the back stores `consumed` once past it.
    alignas(128) std::atomic<u64> wait_target{~u64{0}};
    /// ReleaseBeforeHook: the wait_target of a wait whose caller never uses the scheduler (macro
    /// DMA writes, early acquire, map), stored before wait_target; ~0 otherwise.
    std::atomic<u64> wait_release_first{~u64{0}};
    alignas(128) std::atomic<bool> back_waiting{};
    std::atomic<bool> back_idle{}; ///< The back is spinning or sleeping for work (EagerPublish)
    std::atomic<u32> wake{};
    /// The front sleeps in Backoff (#161: instead of yielding).
    alignas(128) std::atomic<bool> front_waiting{};
    std::atomic<u32> front_wake{};
    std::jthread back_thread;
};

} // namespace VideoCommon

// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/gpu_split.h"
#include "video_core/engines/engine_interface.h"

namespace VideoCommon {

struct GpuSplitTestAccess {
    static void Start(GpuSplit& split) {
        split.ring = std::make_unique<u64[]>(GpuSplit::RingWords);
        split.active = true;
        split.back_thread = std::jthread([&split](std::stop_token stop_token) {
            GpuSplit::on_back_thread = true;
            split.back_running.store(true, std::memory_order_release);
            split.BackLoop(stop_token);
        });
    }

    static void EnableEarlyHookAck(GpuSplit& split) {
        split.early_hook_ack = true;
    }

    static bool HasPublished(const GpuSplit& split) {
        return split.published.load(std::memory_order_acquire) != 0;
    }

    static void PrepareMemoryQueue(GpuSplit& split, bool enabled = true) {
        split.ring = std::make_unique<u64[]>(GpuSplit::RingWords);
        split.memory_tail_wait = enabled;
        split.publish_every = GpuSplit::RingWords;
    }

    static void EnableMemoryWait(GpuSplit& split) {
        split.memory_tail_wait = true;
    }

    static void EnableSemaphoreWait(GpuSplit& split) {
        split.semaphore_tail_wait = true;
    }

    static u64 Position(const GpuSplit& split) {
        return split.write_position;
    }

    static u64 MemoryWriter(const GpuSplit& split) {
        return split.last_memory_writer_position;
    }

    static u64 WaitTarget(const GpuSplit& split) {
        return split.wait_target.load(std::memory_order_acquire);
    }

    static void Acknowledge(GpuSplit& split, u64 position) {
        split.consumed.store(position, std::memory_order_release);
    }

    static void PositionBeforeWrap(GpuSplit& split) {
        split.write_position = split.published_local = split.consumed_cache =
            GpuSplit::RingWords - 2;
        split.consumed.store(split.write_position, std::memory_order_relaxed);
        split.published.store(split.write_position, std::memory_order_relaxed);
    }

    static u64 NearWrap(GpuSplit& split, size_t remaining) {
        split.write_position = split.published_local = split.consumed_cache =
            GpuSplit::RingWords - remaining;
        split.consumed.store(split.write_position, std::memory_order_relaxed);
        split.published.store(split.write_position, std::memory_order_relaxed);
        return split.write_position;
    }

    static std::vector<u64> QueuedWords(const GpuSplit& split, u64 start = 0) {
        std::vector<u64> result;
        for (u64 position = start; position < split.write_position;) {
            const size_t index = position & GpuSplit::RingMask;
            const u64 head = split.ring[index];
            const auto kind = static_cast<GpuSplit::Kind>(head & 0xf);
            if (kind == GpuSplit::Kind::Wrap) {
                position += GpuSplit::RingWords - index;
                continue;
            }
            REQUIRE((kind == GpuSplit::Kind::Engine || kind == GpuSplit::Kind::Method));
            const size_t count = kind == GpuSplit::Kind::Engine ||
                                 (head & GpuSplit::HasSegment) != 0 ? 2 : 1;
            result.insert(result.end(), split.ring.get() + index, split.ring.get() + index + count);
            position += count;
        }
        return result;
    }

    static bool HasWrapped(const GpuSplit& split) {
        return split.write_position > GpuSplit::RingWords;
    }
};

namespace {

using namespace std::chrono_literals;

struct IdleGate {
    void Wait() {
        std::unique_lock lock{mutex};
        if (released) {
            return;
        }
        inside.store(true, std::memory_order_release);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [this] { return released; });
        inside.store(false, std::memory_order_release);
    }

    bool WaitUntilEntered() {
        std::unique_lock lock{mutex};
        return cv.wait_for(lock, 5s, [this] { return entered; });
    }

    void Release() {
        std::scoped_lock lock{mutex};
        released = true;
        cv.notify_all();
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool entered{};
    bool released{};
    std::atomic<bool> inside{};
};

struct RunningSplit {
    RunningSplit() {
        split.SetIdleHook([](void* argument) { static_cast<IdleGate*>(argument)->Wait(); }, &idle);
        GpuSplitTestAccess::Start(split);
    }

    ~RunningSplit() {
        idle.Release();
        split.Stop();
    }

    IdleGate idle;
    GpuSplit split;
};

// These tests enqueue real engine entries but control the consumer acknowledgement, so they
// can assert that a wait stops before the tail without starting a guest Core::System.
struct QueueEngine final : Tegra::Engines::EngineInterface {
    void CallMethod(Core::System&, u32, u32, bool) override {}
    void CallMultiMethod(Core::System&, u32, const u32*, u32, u32) override {}
};

} // namespace

TEST_CASE("GPU split Run executes inline when disabled", "[video_core][gpu_split]") {
    GpuSplit split;
    const auto caller = std::this_thread::get_id();
    std::thread::id callback;
    int result{};
    split.Run([&] {
        callback = std::this_thread::get_id();
        result = 42;
    });
    CHECK(callback == caller);
    CHECK(result == 42);
}

TEST_CASE("GPU split Run serializes with the real back queue and idle hook",
          "[video_core][gpu_split]") {
    RunningSplit running;
    auto& split = running.split;
    REQUIRE(running.idle.WaitUntilEntered());

    // A drained queue can still have its idle hook using the scheduler.
    split.Drain();
    REQUIRE(running.idle.inside.load(std::memory_order_acquire));

    int sequence{};
    bool on_back{};
    bool overlapped_idle{};
    bool nested_on_same_thread{};
    std::thread::id callback_thread;
    std::atomic<bool> returned{};
    split.Defer([&sequence] { sequence = 1; });
    std::jthread front([&] {
        split.Run([&] {
            callback_thread = std::this_thread::get_id();
            on_back = GpuSplit::OnBackThread();
            overlapped_idle = running.idle.inside.load(std::memory_order_acquire);
            if (sequence == 1) {
                sequence = 2;
            }
            split.Run([&] {
                nested_on_same_thread = std::this_thread::get_id() == callback_thread;
                if (sequence == 2) {
                    sequence = 3;
                }
            });
        });
        returned.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!GpuSplitTestAccess::HasPublished(split) &&
           !returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool published = GpuSplitTestAccess::HasPublished(split);
    const bool waited_for_idle = !returned.load(std::memory_order_acquire);
    running.idle.Release();
    front.join();

    CHECK(published);
    CHECK(waited_for_idle);
    CHECK(returned.load(std::memory_order_acquire));
    CHECK(sequence == 3);
    CHECK(on_back);
    CHECK_FALSE(overlapped_idle);
    CHECK(nested_on_same_thread);
    CHECK(callback_thread != std::this_thread::get_id());
}

TEST_CASE("GPU split completion waits outside the back queue", "[video_core][gpu_split]") {
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        GpuSplit::Completion* pending{};
        bool emitted_on_back{};
        bool completed_on_back{};
        bool returned{};
        int value{};

        void CompleteFromIdle() {
            std::scoped_lock lock{mutex};
            if (pending != nullptr) {
                completed_on_back = GpuSplit::OnBackThread();
                value = 42;
                pending->Signal();
                pending = nullptr;
            }
        }
    } state;
    GpuSplit split;
    split.SetIdleHook([](void* argument) { static_cast<State*>(argument)->CompleteFromIdle(); },
                      &state);
    GpuSplitTestAccess::Start(split);
    std::jthread front([&] {
        split.RunAndWait([&](GpuSplit::Completion& completion) {
            std::scoped_lock lock{state.mutex};
            state.emitted_on_back = GpuSplit::OnBackThread();
            state.pending = &completion;
        });
        std::scoped_lock lock{state.mutex};
        state.returned = true;
        state.cv.notify_all();
    });
    bool completed;
    {
        std::unique_lock lock{state.mutex};
        completed = state.cv.wait_for(lock, 5s, [&] { return state.returned; });
        // Release a wrongly blocked back so the test can report the failure instead of hanging.
        if (!completed && state.pending != nullptr) {
            state.pending->Signal();
            state.pending = nullptr;
        }
    }
    front.join();
    split.Stop();
    CHECK(completed);
    CHECK(state.emitted_on_back);
    CHECK(state.completed_on_back);
    CHECK(state.value == 42);
}

TEST_CASE("GPU split completion accepts immediate callbacks and nested back callers",
          "[video_core][gpu_split]") {
    GpuSplit disabled;
    int result{};
    disabled.RunAndWait([&](GpuSplit::Completion& completion) {
        result = 1;
        completion.Signal();
    });
    CHECK(result == 1);

    RunningSplit running;
    running.idle.Release();
    bool on_back{};
    running.split.Run([&] {
        running.split.RunAndWait([&](GpuSplit::Completion& completion) {
            on_back = GpuSplit::OnBackThread();
            result = 2;
            completion.Signal();
        });
    });
    CHECK(on_back);
    CHECK(result == 2);
}

TEST_CASE("Memory read waits retain uncertain and deferred draw writers",
          "[video_core][gpu_split][memory_tail]") {
    GpuSplit split;
    GpuSplitTestAccess::PrepareMemoryQueue(split);
    QueueEngine graphics;
    QueueEngine other;
    // Even a state write can emit a deferred draw until an explicit draw boundary was queued.
    split.Method(&graphics, 0x100, 1, true, nullptr, true);
    CHECK(GpuSplitTestAccess::MemoryWriter(split) == GpuSplitTestAccess::Position(split));

    split.DeferDrawBoundary(&graphics, [] {});
    const u64 boundary = GpuSplitTestAccess::MemoryWriter(split);
    split.Method(&graphics, 0x101, 2, true, nullptr, true);
    split.MacroWrite(&graphics, 0x102, 3, true);
    std::vector<u32> words(20000, 4); // Several real Multi entries.
    split.MultiMethod(&graphics, 0x103, words.data(), static_cast<u32>(words.size()),
                      static_cast<u32>(words.size()), nullptr, true);
    CHECK(GpuSplitTestAccess::MemoryWriter(split) == boundary);
    CHECK(GpuSplitTestAccess::Position(split) > boundary + words.size() / 2);

    // An executable operation may leave a draw deferred. Later state must not overtake it.
    split.MacroWrite(&graphics, 0x104, 5);
    const u64 draw = GpuSplitTestAccess::MemoryWriter(split);
    split.MacroWrite(&graphics, 0x105, 6, true);
    CHECK(GpuSplitTestAccess::MemoryWriter(split) > draw);
    CHECK(GpuSplitTestAccess::MemoryWriter(split) == GpuSplitTestAccess::Position(split));

    split.DeferDrawBoundary(&graphics, [] {});
    split.MultiMethod(&graphics, 0x106, words.data(), static_cast<u32>(words.size()),
                      static_cast<u32>(words.size()), nullptr);
    CHECK(GpuSplitTestAccess::MemoryWriter(split) == GpuSplitTestAccess::Position(split));
    split.DeferDrawBoundary(&graphics, [] {});
    split.Defer([] {}); // An unknown callback, e.g. map/puller/cache work, is always a writer.
    const u64 callback = GpuSplitTestAccess::MemoryWriter(split);
    split.Method(&graphics, 0x107, 7, true, nullptr, true);
    CHECK(GpuSplitTestAccess::MemoryWriter(split) > callback);

    split.DeferDrawBoundary(&graphics, [] {});
    split.Method(&other, 0x108, 8, true, nullptr, true);
    CHECK(GpuSplitTestAccess::MemoryWriter(split) == GpuSplitTestAccess::Position(split));
    split.Method(&graphics, 0x109, 9, true, nullptr, true);
    CHECK(GpuSplitTestAccess::MemoryWriter(split) == GpuSplitTestAccess::Position(split));
}

TEST_CASE("Memory read waits acknowledge the writer while leaving a state-only tail",
          "[video_core][gpu_split][memory_tail]") {
    GpuSplit split;
    GpuSplitTestAccess::PrepareMemoryQueue(split);
    QueueEngine graphics;
    split.Method(&graphics, 0x100, 1, true, nullptr);
    split.DeferDrawBoundary(&graphics, [] {});
    const u64 writer = GpuSplitTestAccess::MemoryWriter(split);
    for (u32 i = 0; i < 20; ++i) {
        split.MacroWrite(&graphics, 0x101, i, true);
    }
    const u64 end = GpuSplitTestAccess::Position(split);
    std::atomic<bool> returned{};
    u64 remaining{};
    std::atomic<int> guest_word{};
    int observed{};
    std::jthread front([&] {
        remaining = split.WaitForMemoryWriters();
        observed = guest_word.load(std::memory_order_relaxed);
        returned.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (GpuSplitTestAccess::WaitTarget(split) != writer &&
           !returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool waited_for_writer = GpuSplitTestAccess::WaitTarget(split) == writer &&
                                   !returned.load(std::memory_order_acquire);
    guest_word.store(42, std::memory_order_relaxed);
    GpuSplitTestAccess::Acknowledge(split, writer);
    const auto return_deadline = std::chrono::steady_clock::now() + 5s;
    while (!returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < return_deadline) {
        std::this_thread::yield();
    }
    const bool returned_before_tail = returned.load(std::memory_order_acquire);
    if (!returned_before_tail) {
        GpuSplitTestAccess::Acknowledge(split, end); // Let a bad full-drain implementation exit.
    }
    front.join();
    CHECK(waited_for_writer);
    CHECK(returned_before_tail);
    CHECK(remaining == end - writer);
    CHECK(observed == 42);
}

TEST_CASE("Memory read writer positions survive ring wrap and reset on Stop",
          "[video_core][gpu_split][memory_tail]") {
    GpuSplit split;
    GpuSplitTestAccess::PrepareMemoryQueue(split);
    QueueEngine graphics;
    split.Method(&graphics, 0x100, 1, true, nullptr);
    GpuSplitTestAccess::PositionBeforeWrap(split);
    split.DeferDrawBoundary(&graphics, [] {});
    REQUIRE(GpuSplitTestAccess::HasWrapped(split));
    const u64 writer = GpuSplitTestAccess::MemoryWriter(split);
    split.MacroWrite(&graphics, 0x101, 2, true);
    GpuSplitTestAccess::Acknowledge(split, writer);
    CHECK(split.WaitForMemoryWriters() == 1);
    split.NoteWriter(); // The existing compute/copy marker remains an additional dependency.
    GpuSplitTestAccess::Acknowledge(split, GpuSplitTestAccess::Position(split));
    CHECK(split.WaitForMemoryWriters() == 0);

    RunningSplit running;
    running.idle.Release();
    GpuSplitTestAccess::EnableMemoryWait(running.split);
    int result{};
    running.split.DeferDrawBoundary(&graphics, [&] { result = 1; });
    running.split.DeferFunction([&] { result = 2; });
    CHECK(GpuSplitTestAccess::MemoryWriter(running.split) ==
          GpuSplitTestAccess::Position(running.split));
    CHECK(running.split.WaitForMemoryWriters() == 0);
    CHECK(result == 2);
    running.split.Stop();
    CHECK_FALSE(running.split.MemoryTailWaitEnabled());
    CHECK(GpuSplitTestAccess::MemoryWriter(running.split) == 0);
    CHECK(GpuSplitTestAccess::Position(running.split) == 0);
}

TEST_CASE("Memory tail tracking is disabled by default", "[video_core][gpu_split][memory_tail]") {
    GpuSplit split;
    CHECK_FALSE(split.MemoryTailWaitEnabled());
    CHECK_FALSE(split.SemaphoreTailWaitEnabled());
    GpuSplitTestAccess::PrepareMemoryQueue(split, false);
    QueueEngine graphics;
    split.Method(&graphics, 0x100, 1, true, nullptr);
    split.DeferDrawBoundary(&graphics, [] {});
    split.MacroWrite(&graphics, 0x101, 2, true);
    CHECK(GpuSplitTestAccess::MemoryWriter(split) == 0);
}

TEST_CASE("Semaphore tail waits retain deferred releases without enabling macro waits",
          "[video_core][gpu_split][semaphore_tail]") {
    RunningSplit running;
    auto& split = running.split;
    GpuSplitTestAccess::EnableSemaphoreWait(split);
    REQUIRE(running.idle.WaitUntilEntered());
    CHECK(split.SemaphoreTailWaitEnabled());
    CHECK_FALSE(split.MemoryTailWaitEnabled());
    int released_word{};
    int observed{};
    bool released_on_back{};
    std::atomic<bool> returned{};
    split.Defer([&] {
        released_word = 42;
        released_on_back = GpuSplit::OnBackThread();
    });
    const u64 release_position = GpuSplitTestAccess::Position(split);
    CHECK(GpuSplitTestAccess::MemoryWriter(split) == release_position);
    std::jthread front([&] {
        split.WaitForMemoryWriters();
        observed = released_word;
        returned.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (GpuSplitTestAccess::WaitTarget(split) != release_position &&
           !returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool waited = GpuSplitTestAccess::WaitTarget(split) == release_position &&
                        !returned.load(std::memory_order_acquire);
    running.idle.Release();
    front.join();
    CHECK(waited);
    CHECK(released_on_back);
    CHECK(observed == 42);
    split.Stop();
    CHECK_FALSE(split.SemaphoreTailWaitEnabled());
}

TEST_CASE("Semaphore tail classification excludes only state after a draw boundary",
          "[video_core][gpu_split][semaphore_tail]") {
    GpuSplit split;
    GpuSplitTestAccess::PrepareMemoryQueue(split, false);
    GpuSplitTestAccess::EnableSemaphoreWait(split);
    QueueEngine graphics;
    split.Method(&graphics, 0x100, 0, true, nullptr);
    split.DeferDrawBoundary(&graphics, [] {});
    const u64 boundary = GpuSplitTestAccess::MemoryWriter(split);
    split.Method(&graphics, 0x100, 1, true, nullptr, true);
    split.MacroWrite(&graphics, 0x101, 2, true);
    GpuSplitTestAccess::Acknowledge(split, boundary);
    CHECK(split.WaitForMemoryWriters() == GpuSplitTestAccess::Position(split) - boundary);
    CHECK_FALSE(split.MemoryTailWaitEnabled());

    // A queued semaphore release must be seen, and invalidates the state-only classification.
    split.Defer([] {});
    CHECK(GpuSplitTestAccess::MemoryWriter(split) == GpuSplitTestAccess::Position(split));
    const u64 release = GpuSplitTestAccess::MemoryWriter(split);
    split.MacroWrite(&graphics, 0x102, 3, true);
    CHECK(GpuSplitTestAccess::MemoryWriter(split) > release);
    GpuSplitTestAccess::Acknowledge(split, GpuSplitTestAccess::Position(split));
    CHECK(split.WaitForMemoryWriters() == 0);
}

} // namespace VideoCommon

namespace VideoCommon {

TEST_CASE("Completed split callbacks acknowledge before an optional requested idle hook",
          "[video_core][gpu_split][early_hook_ack]") {
    GpuSplit split;
    IdleGate hook;
    std::atomic<bool> armed{}, prefix_returned{}, later_published{}, tail_ran{};
    std::atomic<int> prefix_value{};
    struct HookState {
        IdleGate* hook;
        std::atomic<bool>* armed;
    } hook_state{&hook, &armed};
    split.SetIdleHook([](void* p) {
        auto& state = *static_cast<HookState*>(p);
        if (state.armed->load(std::memory_order_acquire)) {
            state.hook->Wait();
        }
    }, &hook_state);
    GpuSplitTestAccess::EnableEarlyHookAck(split);
    GpuSplitTestAccess::Start(split);
    std::jthread front([&] {
        split.Run([&] {
            prefix_value.store(42, std::memory_order_release);
            armed.store(true, std::memory_order_release);
            split.RequestIdleHook();
        });
        prefix_returned.store(true, std::memory_order_release);
        split.Defer([&] { tail_ran.store(true, std::memory_order_release); });
        split.Publish();
        later_published.store(true, std::memory_order_release);
        split.Drain();
    });
    const bool entered = hook.WaitUntilEntered();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!later_published.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool acknowledged_while_hook_blocked = prefix_returned.load(std::memory_order_acquire);
    const bool queued_tail = later_published.load(std::memory_order_acquire);
    const bool tail_blocked = !tail_ran.load(std::memory_order_acquire);
    hook.Release(); // Always release before assertions/join, including a regression.
    front.join();
    split.Stop();
    CHECK(entered);
    CHECK(acknowledged_while_hook_blocked);
    CHECK(queued_tail);
    CHECK(tail_blocked);
    CHECK(prefix_value.load(std::memory_order_acquire) == 42);
    CHECK(tail_ran.load(std::memory_order_acquire));
}

TEST_CASE("Split writer waits acknowledge only the completed prefix before a requested hook",
          "[video_core][gpu_split][early_hook_ack]") {
    GpuSplit split;
    IdleGate hook;
    std::atomic<bool> armed{}, writer_returned{}, tail_ran{};
    std::atomic<int> prefix_value{};
    struct HookState {
        IdleGate* hook;
        std::atomic<bool>* armed;
    } hook_state{&hook, &armed};
    split.SetIdleHook([](void* p) {
        auto& state = *static_cast<HookState*>(p);
        if (state.armed->load(std::memory_order_acquire)) {
            state.hook->Wait();
        }
    }, &hook_state);
    GpuSplitTestAccess::EnableEarlyHookAck(split);
    GpuSplitTestAccess::Start(split);
    std::jthread front([&] {
        split.Defer([&] {
            prefix_value.store(84, std::memory_order_release);
            armed.store(true, std::memory_order_release);
            split.RequestIdleHook();
        });
        split.NoteWriter();
        split.Defer([&] { tail_ran.store(true, std::memory_order_release); });
        split.WaitForWriters();
        writer_returned.store(true, std::memory_order_release);
        split.Drain();
    });
    const bool entered = hook.WaitUntilEntered();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!writer_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool acknowledged_prefix = writer_returned.load(std::memory_order_acquire);
    const bool tail_blocked = !tail_ran.load(std::memory_order_acquire);
    hook.Release();
    front.join();
    split.Stop();
    CHECK(entered);
    CHECK(acknowledged_prefix);
    CHECK(tail_blocked);
    CHECK(prefix_value.load(std::memory_order_acquire) == 84);
    CHECK(tail_ran.load(std::memory_order_acquire));
}

} // namespace VideoCommon


namespace VideoCommon {

TEST_CASE("State batch emits exactly the scalar queue words across engine and execution boundaries",
          "[video_core][gpu_split][split_state_batch]") {
    using SplitStateBatch::Write;
    QueueEngine first;
    QueueEngine second;
    for (const bool wrap : {false, true}) {
        GpuSplit scalar;
        GpuSplit batched;
        GpuSplitTestAccess::PrepareMemoryQueue(scalar);
        GpuSplitTestAccess::PrepareMemoryQueue(batched);
        const u64 scalar_start = wrap ? GpuSplitTestAccess::NearWrap(scalar, 5) : 0;
        const u64 batch_start = wrap ? GpuSplitTestAccess::NearWrap(batched, 5) : 0;
        const std::array prefix{Write{0x80, 0x12345678, false}, Write{0x81, 0xffffffff, false},
                                Write{0x82, 0, true}, Write{0x83, 0x90abcdef, false}};
        for (const auto write : prefix)
            scalar.Method(&first, write.method, write.argument, write.is_last, nullptr, true);
        batched.StateMethods(&first, prefix);
        // Existing executable and macro calls retain their own flags/segment and exact position.
        const GPUVAddr segment = 0x1234567890;
        scalar.Method(&first, 0x500, 7, false, &segment);
        batched.Method(&first, 0x500, 7, false, &segment);
        scalar.MacroWrite(&first, 0x90, 9, true);
        batched.MacroWrite(&first, 0x90, 9, true);
        const std::array tail{Write{0x100, 1, false}, Write{0x101, 2, true}};
        for (const auto write : tail)
            scalar.Method(&second, write.method, write.argument, write.is_last, nullptr, true);
        batched.StateMethods(&second, tail);
        CHECK(GpuSplitTestAccess::QueuedWords(scalar, scalar_start) ==
              GpuSplitTestAccess::QueuedWords(batched, batch_start));
        CHECK(GpuSplitTestAccess::MemoryWriter(scalar) == GpuSplitTestAccess::Position(scalar));
        CHECK(GpuSplitTestAccess::MemoryWriter(batched) == GpuSplitTestAccess::Position(batched));
        if (wrap) CHECK(GpuSplitTestAccess::HasWrapped(batched));
    }
}

TEST_CASE("State batch bounded chunks preserve original last flags and empty batches do nothing",
          "[video_core][gpu_split][split_state_batch]") {
    QueueEngine engine;
    GpuSplit scalar;
    GpuSplit batched;
    GpuSplitTestAccess::PrepareMemoryQueue(scalar);
    GpuSplitTestAccess::PrepareMemoryQueue(batched);
    batched.StateMethods(&engine, {});
    CHECK(GpuSplitTestAccess::Position(batched) == 0);
    std::array<SplitStateBatch::Write, 131> writes;
    for (size_t i = 0; i < writes.size(); ++i) {
        writes[i] = {0x200 + static_cast<u32>(i), static_cast<u32>(i * 31), i + 1 == writes.size()};
        scalar.Method(&engine, writes[i].method, writes[i].argument, writes[i].is_last, nullptr, true);
    }
    batched.StateMethods(&engine, writes);
    CHECK(GpuSplitTestAccess::QueuedWords(scalar) == GpuSplitTestAccess::QueuedWords(batched));
}

TEST_CASE("Increasing state batches stop at parser and executable boundaries",
          "[video_core][gpu_split][split_state_batch]") {
    using SplitStateBatch::IncreasingPrefix;
    std::bitset<0x1000> execution;
    execution.set(0x88);
    CHECK(IncreasingPrefix(0x80, 20, 20, false, false, execution) == 8);
    CHECK(IncreasingPrefix(0x88, 20, 20, false, false, execution) == 0);
    CHECK(IncreasingPrefix(0x80, 3, 20, false, false, execution) == 3);
    CHECK(IncreasingPrefix(0x80, 20, 2, false, false, execution) == 2);
    CHECK(IncreasingPrefix(0x80, 0, 20, false, false, execution) == 0);
    CHECK(IncreasingPrefix(0x80, 20, 0, false, false, execution) == 0);
    CHECK(IncreasingPrefix(0x80, 20, 20, true, false, execution) == 0);
    CHECK(IncreasingPrefix(0x80, 20, 20, false, true, execution) == 0);
    CHECK(IncreasingPrefix(0, 100, 100, false, false, execution) == 0); // BindObject/rebind.
    CHECK(IncreasingPrefix(0x3f, 100, 100, false, false, execution) == 0); // Puller.
    CHECK(IncreasingPrefix(0xdfe, 8, 8, false, false, execution) == 2);
    CHECK(IncreasingPrefix(0xe00, 8, 8, false, false, execution) == 0); // Macro params.
    CHECK(IncreasingPrefix(0xffffffff, 8, 8, false, false, execution) == 0);
    CHECK(IncreasingPrefix(0x100, 1000, 1000, false, false, execution) == 64);
    // A smaller execution table must bound the planner too.
    const std::bitset<0x82> small;
    CHECK(IncreasingPrefix(0x80, 20, 20, false, false, small) == 2);
}

TEST_CASE("Front state preparation retains shadow replay and same-value write semantics",
          "[video_core][gpu_split][split_state_batch]") {
    struct Case { u32 reg, shadow, argument; bool track, replay, skip;
                  u32 expected_reg, expected_shadow; bool emit; };
    constexpr std::array cases{
        Case{1, 1, 1, true, false, true, 1, 1, false},
        Case{1, 0, 1, true, false, true, 1, 1, true},
        Case{1, 1, 2, true, false, true, 2, 2, true},
        Case{1, 1, 1, true, false, false, 1, 1, true},
        Case{1, 1, 2, false, true, true, 1, 1, false},
        Case{1, 2, 9, false, true, true, 2, 2, true},
        Case{1, 1, 9, false, true, false, 1, 1, true},
        Case{1, 7, 1, false, false, true, 1, 7, false},
        Case{1, 7, 2, false, false, true, 2, 7, true},
        Case{1, 7, 1, false, false, false, 1, 7, true}};
    for (const auto item : cases) {
        u32 reg = item.reg;
        u32 shadow = item.shadow;
        const auto prepared = SplitStateBatch::ApplyShadow(shadow, item.argument, item.track, item.replay);
        CHECK(SplitStateBatch::ApplyState(reg, prepared, item.skip) == item.emit);
        CHECK(reg == item.expected_reg);
        CHECK(shadow == item.expected_shadow);
    }
}

} // namespace VideoCommon

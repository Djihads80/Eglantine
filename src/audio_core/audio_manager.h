// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "common/polyfill_thread.h"

#include "audio_core/audio_event.h"

union Result;

namespace AudioCore {

/**
 * The AudioManager's main purpose is to wait for buffer events for the audio in and out managers,
 * and call an associated callback to release buffers.
 *
 * Execution pattern is:
 *     Buffers appended ->
 *     Buffers queued and played by the backend stream ->
 *     When consumed, set the corresponding manager event and signal the audio manager ->
 *     Consumed buffers are released, game is signalled ->
 *     Game appends more buffers.
 *
 * This is only used by audio in and audio out.
 */
class AudioManager {
    using BufferEventFunc = std::function<void()>;

public:
    explicit AudioManager();

    /**
     * Shutdown the audio manager.
     */
    void Shutdown();

    /**
     * Register an out manager, keeping a function to be called when the out event is signalled.
     * Every registered out manager is called.
     *
     * @param owner       - The registering manager, kept to tell its registration apart.
     * @param buffer_func - Function to be called on signal.
     * @return Result code.
     */
    Result SetOutManager(const void* owner, BufferEventFunc buffer_func);

    /**
     * Register the in manager, keeping a function to be called when the in event is signalled.
     *
     * @param owner       - The registering manager, kept to tell its registration apart.
     * @param buffer_func - Function to be called on signal.
     * @return Result code.
     */
    Result SetInManager(const void* owner, BufferEventFunc buffer_func);

    /**
     * Unregister a manager before it is destroyed. Waits for any callback to it in progress, so the
     * thread never calls into a destroyed manager (services shut down before the audio core).
     * Only removes the registration made by `owner`: another manager of the same type that holds
     * the slot keeps its callback.
     */
    void ClearManager(Event::Type type, const void* owner);

    /**
     * Set an event to signalled, and signal the thread.
     *
     * @param type      - Manager type to set.
     * @param signalled - Set the event to true or false?
     */
    void SetEvent(Event::Type type, bool signalled);

private:
    /// Unused
    bool needs_update{};
    /// Events to be set and signalled
    Event events{};
    /// A manager's callback and the manager that registered it
    struct Registration {
        const void* owner{};
        BufferEventFunc func{};
    };
    /// Callbacks for each manager type, one per registered manager (several output managers can
    /// exist at once). Read by the manager thread with the event lock held, so they are only
    /// written with that lock held too.
    std::array<std::vector<Registration>, 3> buffer_events{};
    /// General lock
    std::mutex lock{};
    /// Main thread for waiting and callbacks
    std::jthread thread;
};

} // namespace AudioCore

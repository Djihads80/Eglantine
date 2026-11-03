// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Mcp {

/// Category of a timeline event, used for filtering.
enum class EventKind : u8 {
    Log,
    State,
    Input,
    Mark,
    Ipc,
    Svc,
    Unimplemented,
    Watch,
    Crash,
    Frame,
    Break, ///< Debugger breakpoint/watchpoint hits and other debugger stops.
};

/// Short lowercase name of a kind ("log", "unimpl", ...), or "unknown".
std::string_view EventKindName(EventKind kind);
/// Inverse of EventKindName. Returns nullopt for an unrecognised name.
std::optional<EventKind> EventKindFromName(std::string_view name);

/// One recorded timeline entry.
struct Event {
    u64 seq = 0;
    u64 time_us = 0; ///< Since the timeline was created.
    u64 frame = 0;   ///< Guest present count when the event was recorded.
    EventKind kind = EventKind::Log;
    u8 level = 0;       ///< Common::Log::Level for logs, 0 otherwise.
    std::string source; ///< Log class, service name, etc.
    std::string message;
};

/// Bounded, sequence-numbered record of everything the emulator did, readable with a cursor so
/// a turn-based agent misses nothing between calls. Producers may run on any thread.
class Timeline {
public:
    /// Returns the current guest frame number; called on every Push.
    using FrameSource = std::function<u64()>;

    /// capacity is the ring size (at least 1); older events are overwritten once it is full.
    explicit Timeline(size_t capacity = 65536, FrameSource frame_source = {});

    /// Records an event and returns its sequence number (starting at 1).
    u64 Push(EventKind kind, u8 level, std::string source, std::string message);

    /// Event filter for Read and WaitFor. Empty fields match everything.
    struct Query {
        u64 since_seq = 0; ///< Return events with seq > since_seq.
        std::vector<EventKind> kinds; ///< Empty means all kinds.
        /// Minimum level for Log events (other kinds always pass); -1 means any.
        int min_level = -1;
        std::string source_prefix;
        /// Substring the message must contain.
        std::string contains;
        size_t limit = 200;
    };
    /// Result of Read: matching events in sequence order plus the cursor to continue from.
    struct QueryResult {
        std::vector<Event> events;
        u64 next_seq = 0; ///< Pass back as since_seq to continue.
        u64 dropped = 0;  ///< Events after since_seq that were overwritten before this read.
        bool truncated = false; ///< More matching events remain; call again with next_seq.
    };

    /// Returns up to query.limit matching events after since_seq. Never blocks beyond the lock.
    [[nodiscard]] QueryResult Read(const Query& query) const;

    /// Blocks until an event after since_seq matches the query (ignoring limit), or the timeout
    /// passes. Returns the first matching event.
    std::optional<Event> WaitFor(const Query& query, std::chrono::milliseconds timeout) const;

    /// Sequence number of the newest event, 0 if none yet.
    [[nodiscard]] u64 LastSeq() const;

    /// Wakes every WaitFor and makes later ones return at once (server shutting down).
    void Close();
    /// Undoes Close when a server starts again.
    void Reopen();
    [[nodiscard]] bool IsClosed() const;
    [[nodiscard]] size_t Capacity() const {
        return ring.size();
    }

    /// Whether event passes the query's filters. Ignores since_seq and limit.
    [[nodiscard]] static bool Matches(const Event& event, const Query& query);

private:
    FrameSource frame_source;
    std::chrono::steady_clock::time_point start;
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    mutable std::atomic<u32> waiters{0};
    std::vector<Event> ring;
    u64 last_seq = 0;
    bool closed = false;
};

} // namespace Mcp

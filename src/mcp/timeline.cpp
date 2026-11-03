// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <utility>

#include "mcp/timeline.h"

namespace Mcp {

namespace {
constexpr std::array<std::pair<EventKind, std::string_view>, 11> kKindNames{{
    {EventKind::Log, "log"},
    {EventKind::State, "state"},
    {EventKind::Input, "input"},
    {EventKind::Mark, "mark"},
    {EventKind::Ipc, "ipc"},
    {EventKind::Svc, "svc"},
    {EventKind::Unimplemented, "unimpl"},
    {EventKind::Watch, "watch"},
    {EventKind::Crash, "crash"},
    {EventKind::Frame, "frame"},
    {EventKind::Break, "break"},
}};
} // namespace

/// Short name of an event kind ("log", "crash", ...), or "unknown".
std::string_view EventKindName(EventKind kind) {
    for (const auto& [k, name] : kKindNames) {
        if (k == kind) {
            return name;
        }
    }
    return "unknown";
}

/// Event kind for a short name, or nullopt if the name is unknown.
std::optional<EventKind> EventKindFromName(std::string_view name) {
    for (const auto& [k, n] : kKindNames) {
        if (n == name) {
            return k;
        }
    }
    return std::nullopt;
}

/// Creates a ring holding `capacity` events (at least one).
Timeline::Timeline(size_t capacity, FrameSource frame_source_)
    : frame_source{std::move(frame_source_)}, start{std::chrono::steady_clock::now()},
      ring(capacity == 0 ? 1 : capacity) {}

/// Stores an event stamped with time and frame, wakes waiters, and returns its sequence number.
u64 Timeline::Push(EventKind kind, u8 level, std::string source, std::string message) {
    const u64 time_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count());
    const u64 frame = frame_source ? frame_source() : 0;
    u64 seq;
    {
        std::scoped_lock lock{mutex};
        seq = ++last_seq;
        Event& slot = ring[seq % ring.size()];
        slot.seq = seq;
        slot.time_us = time_us;
        slot.frame = frame;
        slot.kind = kind;
        slot.level = level;
        slot.source = std::move(source);
        slot.message = std::move(message);
    }
    if (waiters.load(std::memory_order_acquire) != 0) {
        cv.notify_all();
    }
    return seq;
}

/// True if the event passes the query's kind, level, source and text filters.
bool Timeline::Matches(const Event& event, const Query& query) {
    if (!query.kinds.empty() &&
        std::find(query.kinds.begin(), query.kinds.end(), event.kind) == query.kinds.end()) {
        return false;
    }
    if (query.min_level >= 0 && event.kind == EventKind::Log && event.level < query.min_level) {
        return false;
    }
    if (!query.source_prefix.empty() && !event.source.starts_with(query.source_prefix)) {
        return false;
    }
    if (!query.contains.empty() && event.message.find(query.contains) == std::string::npos) {
        return false;
    }
    return true;
}

/// Matching events after `since_seq`, up to the limit, with dropped and truncated counts.
Timeline::QueryResult Timeline::Read(const Query& query) const {
    QueryResult result;
    std::scoped_lock lock{mutex};
    const u64 capacity = ring.size();
    const u64 oldest = last_seq >= capacity ? last_seq - capacity + 1 : 1;
    u64 seq = query.since_seq + 1;
    if (seq < oldest) {
        result.dropped = oldest - seq;
        seq = oldest;
    }
    result.next_seq = query.since_seq;
    for (; seq <= last_seq; ++seq) {
        const Event& event = ring[seq % capacity];
        result.next_seq = seq;
        if (!Matches(event, query)) {
            continue;
        }
        if (result.events.size() >= query.limit) {
            result.truncated = true;
            result.next_seq = seq - 1;
            break;
        }
        result.events.push_back(event);
    }
    if (result.next_seq < query.since_seq) {
        result.next_seq = query.since_seq;
    }
    return result;
}

/// Blocks until an event after `since_seq` matches the query, or the timeout passes.
std::optional<Event> Timeline::WaitFor(const Query& query, std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    u64 cursor = query.since_seq;
    std::unique_lock lock{mutex};
    // Scans events after the cursor (the lock is held) and advances it.
    const auto scan = [&]() -> std::optional<Event> {
        const u64 capacity = ring.size();
        const u64 oldest = last_seq >= capacity ? last_seq - capacity + 1 : 1;
        for (u64 seq = std::max(cursor + 1, oldest); seq <= last_seq; ++seq) {
            if (Matches(ring[seq % capacity], query)) {
                return ring[seq % capacity];
            }
        }
        cursor = last_seq;
        return std::nullopt;
    };
    waiters.fetch_add(1, std::memory_order_acq_rel);
    std::optional<Event> found = scan();
    while (!found && !closed) {
        if (cv.wait_until(lock, deadline) == std::cv_status::timeout) {
            found = scan(); // anything that arrived with the timeout
            break;
        }
        found = scan();
    }
    waiters.fetch_sub(1, std::memory_order_acq_rel);
    return found;
}

void Timeline::Close() {
    {
        std::scoped_lock lock{mutex};
        closed = true;
    }
    cv.notify_all();
}

void Timeline::Reopen() {
    std::scoped_lock lock{mutex};
    closed = false;
}

bool Timeline::IsClosed() const {
    std::scoped_lock lock{mutex};
    return closed;
}

u64 Timeline::LastSeq() const {
    std::scoped_lock lock{mutex};
    return last_seq;
}

} // namespace Mcp

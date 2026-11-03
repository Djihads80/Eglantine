// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mcp/control.h"

namespace Mcp {

/// Drops the holder if its lease has expired by `now`. Caller holds the mutex.
void ControlLease::ExpireLocked(Clock::time_point now) {
    if (holder && now >= holder->expires) {
        holder.reset();
    }
}

/// Takes or extends the lease for `session`; fails, reporting the holder, if another holds it.
bool ControlLease::Acquire(const std::string& session, const std::string& label,
                           std::optional<std::chrono::seconds> ttl, Holder& out) {
    std::scoped_lock lock{mutex};
    const auto now = Clock::now();
    ExpireLocked(now);
    if (holder && holder->session_id != session) {
        out = *holder;
        return false;
    }
    current_ttl = ttl.value_or(default_ttl);
    if (!holder) {
        holder = Holder{session, label, now, now + current_ttl};
    } else {
        holder->expires = now + current_ttl;
        if (!label.empty()) {
            holder->label = label;
        }
    }
    out = *holder;
    return true;
}

/// Implicitly takes a free lease or extends our own; fails if another session holds it.
bool ControlLease::Touch(const std::string& session, Holder& out) {
    std::scoped_lock lock{mutex};
    const auto now = Clock::now();
    ExpireLocked(now);
    if (!holder) {
        current_ttl = default_ttl;
        holder = Holder{session, std::string{}, now, now + current_ttl};
    } else if (holder->session_id != session) {
        out = *holder;
        return false;
    } else {
        holder->expires = now + (current_ttl.count() > 0 ? current_ttl : default_ttl);
    }
    out = *holder;
    return true;
}

/// Extends the lease if `session` holds it; never takes a free one.
bool ControlLease::Renew(const std::string& session) {
    std::scoped_lock lock{mutex};
    const auto now = Clock::now();
    ExpireLocked(now);
    if (!holder || holder->session_id != session) {
        return false;
    }
    holder->expires = now + (current_ttl.count() > 0 ? current_ttl : default_ttl);
    return true;
}

/// Gives up the lease if `session` holds it.
bool ControlLease::Release(const std::string& session) {
    std::scoped_lock lock{mutex};
    if (holder && holder->session_id == session) {
        holder.reset();
        return true;
    }
    return false;
}

void ControlLease::Reset() {
    std::scoped_lock lock{mutex};
    holder.reset();
}

std::optional<ControlLease::Holder> ControlLease::Current() {
    std::scoped_lock lock{mutex};
    ExpireLocked(Clock::now());
    return holder;
}

} // namespace Mcp

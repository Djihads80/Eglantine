// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace Mcp {

/// Exclusive control of the emulator for one MCP session at a time, so several agents can share
/// one emulator: whoever holds the lease may change state (boot, input, stop...); everyone may
/// observe. The lease expires when its holder stops using it.
class ControlLease {
public:
    using Clock = std::chrono::steady_clock;

    /// Snapshot of who holds the lease and until when.
    struct Holder {
        std::string session_id;
        std::string label; ///< Optional name the agent gave itself.
        Clock::time_point acquired;
        Clock::time_point expires;
    };

    /// default_ttl is how long the lease lasts after its last use unless Acquire sets another.
    explicit ControlLease(std::chrono::seconds default_ttl = std::chrono::seconds{120})
        : default_ttl{default_ttl} {}

    /// Takes (or renews) the lease for session. Fails if another live session holds it, setting
    /// `holder` to the current holder.
    bool Acquire(const std::string& session, const std::string& label,
                 std::optional<std::chrono::seconds> ttl, Holder& holder);

    /// For controlled tools: renews the caller's lease, or grants it if free. Fails (and sets
    /// `holder`) if someone else holds it.
    bool Touch(const std::string& session, Holder& holder);

    /// For work that outlasts a call (background routes): extends the lease only if session still
    /// holds it. Unlike Touch it never takes a free lease, so a released lease stays released.
    bool Renew(const std::string& session);

    /// Releases if session holds it. Returns whether it did.
    bool Release(const std::string& session);

    /// Forcibly frees the lease (e.g. from the emulator's UI).
    void Reset();

    /// The live holder, or nullopt if the lease is free or has expired.
    [[nodiscard]] std::optional<Holder> Current();

private:
    /// Drops the holder if its lease has expired. Caller must hold mutex.
    void ExpireLocked(Clock::time_point now);

    std::chrono::seconds default_ttl;
    /// TTL chosen by the last Acquire or grant; Touch renews with it.
    std::chrono::seconds current_ttl{0};
    std::mutex mutex;
    std::optional<Holder> holder;
};

} // namespace Mcp

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

    struct Holder {
        std::string session_id;
        std::string label; ///< Optional name the agent gave itself.
        Clock::time_point acquired;
        Clock::time_point expires;
    };

    explicit ControlLease(std::chrono::seconds default_ttl = std::chrono::seconds{120})
        : default_ttl{default_ttl} {}

    /// Takes (or renews) the lease for session. Fails if another live session holds it, setting
    /// `holder` to the current holder.
    bool Acquire(const std::string& session, const std::string& label,
                 std::optional<std::chrono::seconds> ttl, Holder& holder);

    /// For controlled tools: renews the caller's lease, or grants it if free. Fails (and sets
    /// `holder`) if someone else holds it.
    bool Touch(const std::string& session, Holder& holder);

    /// Releases if session holds it. Returns whether it did.
    bool Release(const std::string& session);

    /// Forcibly frees the lease (e.g. from the emulator's UI).
    void Reset();

    [[nodiscard]] std::optional<Holder> Current();

private:
    void ExpireLocked(Clock::time_point now);

    std::chrono::seconds default_ttl;
    std::chrono::seconds current_ttl{0};
    std::mutex mutex;
    std::optional<Holder> holder;
};

} // namespace Mcp

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Kernel {
class KThread;
}

namespace Service {

/// One unimplemented (or auto-stubbed) IPC command the guest called, aggregated.
struct UnimplementedCommandRecord {
    std::string service;
    u32 command{};
    std::string function; ///< Handler name if the command is listed without a handler.
    u64 count{};          ///< Every call, including repeats that are no longer logged.
    bool auto_stubbed{};  ///< Answered with an empty success instead of failing.
    u64 first_seen_us{};  ///< Steady-clock microseconds since the first record.
    std::string first_request; ///< Command words of the first call ("[0]=0x.., [1]=..").
    std::string first_backtrace; ///< Caller's symbolized backtrace at the first call, one frame a line.
};

/// Records a call (thread-safe). Called by ServiceFrameworkBase::ReportUnimplementedFunction.
/// The first call of each command also records the caller's backtrace and notifies the hook.
void RecordUnimplementedCommand(const std::string& service, u32 command,
                                const std::string& function, bool auto_stubbed,
                                const std::string& request_words, const Kernel::KThread* caller);

/// Called with each newly seen command (from the service thread that saw it).
using UnimplementedHook = void (*)(const UnimplementedCommandRecord&);
inline std::atomic<UnimplementedHook> g_unimplemented_hook{nullptr};

/// Snapshot, in first-seen order.
std::vector<UnimplementedCommandRecord> GetUnimplementedCommands();
/// Drops all records (thread-safe), so the next call of each command counts as first seen again.
void ClearUnimplementedCommands();

} // namespace Service

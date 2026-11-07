// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Optional observers of IPC dispatch and SVCs (e.g. the MCP server's tracing). Unset, each costs
// one relaxed atomic load on its path.

#include <array>
#include <atomic>
#include <span>
#include <string_view>

#include "common/common_types.h"

namespace Kernel {
class KThread;
}

namespace Core::Trace {

/// One completed HLE IPC request. The views and span point into the dispatcher's storage and are
/// only valid during the hook call.
struct IpcCall {
    std::string_view service;
    std::string_view function; ///< Handler name, empty if unknown
    u32 command;
    u32 type;   ///< IPC::CommandType (>= 16: TIPC)
    u32 result; ///< Result word of the reply (0 = success)
    u64 thread_id;
    std::span<const u32> request; ///< First words of the request
};

/// One completed SVC: its number, the calling guest thread, the first argument registers on
/// entry and the first result registers on return.
struct SvcCall {
    u32 imm;
    u64 thread_id;
    std::array<u64, 4> in;
    std::array<u64, 3> out;
};

/// Called after each HLE service request is handled, on the thread that handled it.
using IpcHook = void (*)(const IpcCall&);
/// Called after each SVC returns, on the emulated CPU thread that made it.
using SvcHook = void (*)(const SvcCall&);
/// The guest crashed or broke into the debugger (svcBreak, fatal service). `thread` may be null.
using CrashHook = void (*)(std::string_view kind, std::string_view detail,
                           const Kernel::KThread* thread);

/// Installed hooks, nullptr when unset. A hook may still run briefly after it is cleared, so
/// the function it points to must stay valid.
inline std::atomic<IpcHook> g_ipc_hook{nullptr};
inline std::atomic<SvcHook> g_svc_hook{nullptr};
inline std::atomic<CrashHook> g_crash_hook{nullptr};

/// Calls the crash hook, if set, on the current thread. Costs one atomic load when unset.
inline void NotifyCrash(std::string_view kind, std::string_view detail,
                        const Kernel::KThread* thread) {
    if (const auto hook = g_crash_hook.load(std::memory_order_acquire)) {
        hook(kind, detail, thread);
    }
}

} // namespace Core::Trace

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/scope_exit.h"
#include <vector>
#include <tuple>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <fmt/ranges.h>
#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/service.h"
#include "core/hle/service/nx2_stub_interface.h"
#include "core/hle/service/sm/sm.h"
#include "core/reporter.h"

namespace Service {

/// @brief Creates a function string for logging, complete with the name (or header code, depending
/// on what's passed in) the port name, and all the cmd_buf arguments.
[[maybe_unused]] static std::string MakeFunctionString(std::string_view name, std::string_view port_name, const u32* cmd_buf) {
    // Number of params == bits 0-5 + bits 6-11
    int num_params = (cmd_buf[0] & 0x3F) + ((cmd_buf[0] >> 6) & 0x3F);
    std::string function_string = fmt::format("fn '{}': port={}", name, port_name);
    for (int i = 1; i <= num_params; ++i)
        function_string += fmt::format(", cmd_buf[{}]={:#x}", i, cmd_buf[i]);
    return function_string;
}

ServiceFrameworkBase::ServiceFrameworkBase(Core::System& system_, const char* service_name_, u32 max_sessions_, InvokerFn* handler_invoker_)
    : SessionRequestHandler(system_.Kernel(), service_name_)
    , system{system_}
    , service_name{service_name_}
    , handler_invoker{handler_invoker_}
    , max_sessions{max_sessions_}
{}

ServiceFrameworkBase::~ServiceFrameworkBase() {
    // Wait for other threads to release access before destroying
    const auto guard = ServiceFrameworkBase::LockService();
}

void ServiceFrameworkBase::RegisterHandlersBase(const FunctionInfoBase* functions, std::size_t n) {
    // Usually this array is sorted by id already, so hint to insert at the end
    handlers.reserve(handlers.size() + n);
    for (std::size_t i = 0; i < n; ++i)
        handlers.emplace_hint(handlers.cend(), functions[i].expected_header, functions[i]);
}

void ServiceFrameworkBase::RegisterHandlersBaseTipc(const FunctionInfoBase* functions, std::size_t n) {
    // Usually this array is sorted by id already, so hint to insert at the end
    handlers_tipc.reserve(handlers_tipc.size() + n);
    for (std::size_t i = 0; i < n; ++i)
        handlers_tipc.emplace_hint(handlers_tipc.cend(), functions[i].expected_header, functions[i]);
}

void ServiceFrameworkBase::ReportUnimplementedFunction(HLERequestContext& ctx,
                                                       const FunctionInfoBase* info) {
    // EDEN_NX2_AUTO_STUB=1 auto-stubs every unknown command without touching the config. Switch 2
    // programs are always auto-stubbed: they still call many commands nobody has implemented.
    static const bool env_auto_stub = std::getenv("EDEN_NX2_AUTO_STUB") != nullptr;
    const auto* const caller = ctx.GetThread().GetOwnerProcess();
    const bool auto_stub = Settings::values.use_auto_stub || env_auto_stub ||
                           std::string_view{service_name}.starts_with(Nx2StubPrefix) ||
                           (caller != nullptr && caller->IsSwitch2());
    // Auto-stubbed commands are reported once each: title 1 polls nx2:camera 29 every frame
    // (~47 calls a second), and every report wrote a Critical line, which flushes the log file.
    if (auto_stub) {
        static std::mutex reported_mutex;
        static std::set<std::pair<std::string, u32>> reported;
        std::scoped_lock lk{reported_mutex};
        if (!reported.emplace(service_name, ctx.GetCommand()).second) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultSuccess);
            return;
        }
    }

    auto cmd_buf = ctx.CommandBuffer();
    std::string function_name = info == nullptr ? "<unknown>" : info->name;

    fmt::memory_buffer buf;
    fmt::format_to(std::back_inserter(buf), "function '{}({})': port='{}' cmd_buf={{[0]={:#x}", ctx.GetCommand(), function_name, service_name, cmd_buf[0]);
    // The Switch 2 video decoder sessions are being reverse engineered: dump whole requests.
    const int last_word = std::string_view{service_name}.starts_with("nx2:vdec") ? 47 : 8;
    for (int i = 1; i <= last_word; ++i)
        fmt::format_to(std::back_inserter(buf), ", [{}]={:#x}", i, cmd_buf[i]);
    buf.push_back('}');

    system.GetReporter().SaveUnimplementedFunctionReport(ctx, ctx.GetCommand(), function_name, service_name);
    UNIMPLEMENTED_MSG("Unknown / unimplemented {}", fmt::to_string(buf));
    if (auto_stub) {
        LOG_WARNING(Service, "Using auto stub fallback (repeats of this command are not logged)");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
}

void ServiceFrameworkBase::InvokeRequest(HLERequestContext& ctx) {
    auto it = handlers.find(ctx.GetCommand());
    FunctionInfoBase const* info = it == handlers.end() ? nullptr : &it->second;
    if (info == nullptr || info->handler_callback == nullptr)
        return ReportUnimplementedFunction(ctx, info);

    LOG_TRACE(Service, "{}", MakeFunctionString(info->name, GetServiceName(), ctx.CommandBuffer()));
    // EDEN_NX2_IPC_TRACE=1 logs every dispatched command and the result word it returned.
    static const bool ipc_trace = std::getenv("EDEN_NX2_IPC_TRACE") != nullptr;
    if (ipc_trace) {
        LOG_INFO(Service, "ipc {}::{} ({})", service_name, ctx.GetCommand(), info->name);
    }
    handler_invoker(this, info->handler_callback, ctx);
    if (ipc_trace) {
        // Response layout: [0..1] header, then 'SFCO', version, result.
        const auto* cmd_buf = ctx.CommandBuffer();
        for (int i = 0; i < 12; ++i) {
            if (cmd_buf[i] == 0x4F434653) {  // "SFCO"
                if (cmd_buf[i + 2] != 0) {
                    LOG_INFO(Service, "ipc {}::{} -> result {:#x}", service_name, ctx.GetCommand(),
                             cmd_buf[i + 2]);
                }
                break;
            }
        }
    }
}

void ServiceFrameworkBase::InvokeRequestTipc(HLERequestContext& ctx) {
    auto it = handlers_tipc.find(ctx.GetCommand());
    FunctionInfoBase const* info = it == handlers_tipc.end() ? nullptr : &it->second;
    if (info == nullptr || info->handler_callback == nullptr)
        return ReportUnimplementedFunction(ctx, info);

    LOG_TRACE(Service, "{}", MakeFunctionString(info->name, GetServiceName(), ctx.CommandBuffer()));
    static const bool ipc_trace = std::getenv("EDEN_NX2_IPC_TRACE") != nullptr;
    if (ipc_trace) {
        const auto* w = ctx.CommandBuffer();
        LOG_INFO(Service, "tipc {}::{} ({}) [2]={:#x} [3]={:#x} [4]={:#x}", service_name,
                 ctx.GetCommand(), info->name, w[2], w[3], w[4]);
    }
    handler_invoker(this, info->handler_callback, ctx);
}

namespace {
// EDEN_NX2_IPC_SUMMARY: every 5 s, log how often each service command ran and with which result
// since the last summary (finds a game polling something that never becomes ready).
void CountRequest(const std::string& service, u32 command, u32 type, u32 result) {
    static const bool enabled = std::getenv("EDEN_NX2_IPC_SUMMARY") != nullptr;
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::map<std::tuple<std::string, u32, u32, u32>, u32> counts;
    static auto last = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    ++counts[{service, command, type, result}];
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds{5}) {
        return;
    }
    last = now;
    std::vector<std::pair<u32, std::string>> lines;
    for (const auto& [key, count] : counts) {
        lines.emplace_back(count, fmt::format("{}::{}{} -> {:#x}", std::get<0>(key), std::get<1>(key),
                                              std::get<2>(key) >= 16 ? " (tipc)" : "",
                                              std::get<3>(key)));
    }
    std::sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string text;
    for (size_t i = 0; i < std::min<size_t>(lines.size(), 24); ++i) {
        text += fmt::format(" | {}x {}", lines[i].first, lines[i].second);
    }
    LOG_INFO(Service, "ipc summary ({} kinds){}", lines.size(), text);
    counts.clear();
}
} // Anonymous namespace

Result ServiceFrameworkBase::HandleSyncRequest(Kernel::KServerSession& session,
                                               HLERequestContext& ctx) {
    const auto guard = LockService();
    SCOPE_EXIT {
        const auto* cmd_buf = ctx.CommandBuffer();
        u32 result_word = 0;
        const u32 type = static_cast<u32>(ctx.GetCommandType());
        if (type >= 16) {
            result_word = cmd_buf[2];
        } else {
            for (int i = 0; i < 12; ++i) {
                if (cmd_buf[i] == 0x4F434653) { // "SFCO"
                    result_word = cmd_buf[i + 2];
                    break;
                }
            }
        }
        CountRequest(service_name, ctx.GetCommand(), type, result_word);
    };

    Result result = ResultSuccess;

    switch (ctx.GetCommandType()) {
    case IPC::CommandType::Close:
    case IPC::CommandType::TIPC_Close: {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        result = IPC::ResultSessionClosed;
        break;
    }
    case IPC::CommandType::ControlWithContext:
    case IPC::CommandType::Control: {
        system.ServiceManager().InvokeControlRequest(ctx);
        break;
    }
    case IPC::CommandType::RequestWithContext:
    case IPC::CommandType::Request: {
        InvokeRequest(ctx);
        break;
    }
    default:
        if (ctx.IsTipc()) {
            InvokeRequestTipc(ctx);
            break;
        }

        UNIMPLEMENTED_MSG("command_type={}", ctx.GetCommandType());
        break;
    }

    // If emulation was shutdown, we are closing service threads, do not write the response back to
    // memory that may be shutting down as well.
    if (system.IsPoweredOn()) {
        ctx.WriteToOutgoingCommandBuffer();
    }

    return result;
}

} // namespace Service

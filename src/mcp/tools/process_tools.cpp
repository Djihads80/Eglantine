// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The running application's process: layout, modules, memory map, threads and backtraces
// (JSON versions of the GDB stub's monitor commands).

#include <limits>

#include <fmt/format.h>

#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_light_lock.h"
#include "core/hle/kernel/k_thread.h"
#include "mcp/context.h"
#include "mcp/host.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

std::string Hex(u64 value) {
    return fmt::format("{:#x}", value);
}

/// Name of a memory state as used in memory_map output and its "state" filter.
const char* MemoryStateName(Kernel::Svc::MemoryState state) {
    using S = Kernel::Svc::MemoryState;
    switch (state) {
    case S::Free: return "Free";
    case S::Io: return "Io";
    case S::Static: return "Static";
    case S::Code: return "Code";
    case S::CodeData: return "CodeData";
    case S::Normal: return "Normal";
    case S::Shared: return "Shared";
    case S::AliasCode: return "AliasCode";
    case S::AliasCodeData: return "AliasCodeData";
    case S::Ipc: return "Ipc";
    case S::Stack: return "Stack";
    case S::ThreadLocal: return "ThreadLocal";
    case S::Transferred: return "Transferred";
    case S::SharedTransferred: return "SharedTransferred";
    case S::SharedCode: return "SharedCode";
    case S::Inaccessible: return "Inaccessible";
    case S::NonSecureIpc: return "NonSecureIpc";
    case S::NonDeviceIpc: return "NonDeviceIpc";
    case S::Kernel: return "Kernel";
    case S::GeneratedCode: return "GeneratedCode";
    case S::CodeOut: return "CodeOut";
    case S::Coverage: return "Coverage";
    default: return "Unknown";
    }
}

/// "rwx"-style permission string. Free regions and any permission other than r-x, r-- or rw-
/// show as "---".
const char* PermissionString(const Kernel::Svc::MemoryInfo& info) {
    if (info.state == Kernel::Svc::MemoryState::Free) {
        return "---";
    }
    switch (info.permission) {
    case Kernel::Svc::MemoryPermission::ReadExecute: return "r-x";
    case Kernel::Svc::MemoryPermission::Read: return "r--";
    case Kernel::Svc::MemoryPermission::ReadWrite: return "rw-";
    default: return "---";
    }
}

/// The application process, or null when there is no system or no application running.
Kernel::KProcess* Process(ToolContext& ctx) {
    return ctx.system ? ctx.system->ApplicationProcess() : nullptr;
}

/// Loaded modules of the process with name, base, end and size.
json ModulesJson(Kernel::KProcess* process) {
    json modules = json::array();
    for (const auto& [base, name] : Core::FindModules(process)) {
        const u64 end = GetInteger(Core::GetModuleEnd(process, base));
        modules.push_back({{"name", name}, {"base", Hex(base)}, {"end", Hex(end)},
                           {"size", end - base}});
    }
    return modules;
}

/// process_info: pid, name, program id, Switch 2 flag, address-space regions and modules.
ToolResult ProcessInfo(ToolContext& ctx, const json&) {
    auto* process = Process(ctx);
    if (!process) {
        return ToolResult::Error("no application process");
    }
    auto& pt = process->GetPageTable();
    const auto range = [](auto start, u64 size) {
        return json{{"start", Hex(GetInteger(start))}, {"end", Hex(GetInteger(start) + size)},
                    {"size", size}};
    };
    return ToolResult::Structured(json{
        {"pid", process->GetProcessId()},
        {"name", process->GetName()},
        {"program_id", fmt::format("{:016X}", process->GetProgramId())},
        {"switch2", process->IsSwitch2()},
        {"layout",
         {{"alias", range(pt.GetAliasRegionStart(), pt.GetAliasRegionSize())},
          {"heap", range(pt.GetHeapRegionStart(), pt.GetHeapRegionSize())},
          {"aslr", range(pt.GetAliasCodeRegionStart(), pt.GetAliasCodeRegionSize())},
          {"stack", range(pt.GetStackRegionStart(), pt.GetStackRegionSize())}}},
        {"modules", ModulesJson(process)},
    });
}

/// memory_map: walks the page table from address 0 with QueryInfo. Free and Inaccessible regions
/// are skipped unless include_free, the trailing Inaccessible region always is, and the walk stops
/// once more than 20000 regions are collected.
ToolResult MemoryMap(ToolContext& ctx, const json& args) {
    auto* process = Process(ctx);
    if (!process) {
        return ToolResult::Error("no application process");
    }
    const bool include_free = args.value("include_free", false);
    const std::string state_filter = args.value("state", "");
    auto& pt = process->GetPageTable();
    json regions = json::array();
    u64 addr = 0;
    while (true) {
        Kernel::KMemoryInfo mem_info{};
        Kernel::Svc::PageInfo page_info{};
        if (pt.QueryInfo(std::addressof(mem_info), std::addressof(page_info), addr) !=
            ResultSuccess) {
            break;
        }
        const auto info = mem_info.GetSvcMemoryInfo();
        const bool last = info.base_address + info.size - 1 == (std::numeric_limits<u64>::max)();
        const bool is_free = info.state == Kernel::Svc::MemoryState::Free ||
                             info.state == Kernel::Svc::MemoryState::Inaccessible;
        const char* state = MemoryStateName(info.state);
        if ((include_free || !is_free) && (state_filter.empty() || state_filter == state) &&
            !(last && info.state == Kernel::Svc::MemoryState::Inaccessible)) {
            regions.push_back({{"base", Hex(info.base_address)},
                               {"end", Hex(info.base_address + info.size)},
                               {"size", info.size},
                               {"perm", PermissionString(info)},
                               {"state", state}});
        }
        const u64 next = info.base_address + info.size;
        if (next <= addr || regions.size() > 20000) {
            break;
        }
        addr = next;
    }
    return ToolResult::Structured(json{{"regions", std::move(regions)}});
}

/// One guest thread's id, name, state, wait reason, priority, core and CPU time.
json ThreadJson(const Kernel::KThread& thread) {
    return json{{"id", thread.GetThreadId()},
                {"name", Core::GetThreadName(&thread).value_or("")},
                {"state", Core::GetThreadState(&thread)},
                {"wait_reason", std::string{Core::GetThreadWaitReason(&thread)}},
                {"priority", thread.GetPriority()},
                {"core", thread.GetActiveCore()},
                {"cpu_time_ticks", thread.GetCpuTime()}};
}

/// list_threads: snapshot of the process's threads, taken under the process list lock.
ToolResult ListThreads(ToolContext& ctx, const json&) {
    auto* process = Process(ctx);
    if (!process) {
        return ToolResult::Error("no application process");
    }
    json threads = json::array();
    {
        Kernel::KScopedLightLock lock{process->GetListLock()};
        for (const auto& thread : process->GetThreadList()) {
            threads.push_back(ThreadJson(thread));
        }
    }
    return ToolResult::Structured(
        json{{"threads", std::move(threads)},
             {"note", "cpu_time_ticks grows with CPU use: call twice and diff for a per-thread "
                      "CPU profile."}});
}

/// backtrace: symbolized frames of one thread (thread_id) or all, read under the process list
/// lock. Pauses the game through the host first if it is running and resumes it afterwards.
/// Fails if pausing fails or no thread has the requested id.
ToolResult Backtrace(ToolContext& ctx, const json& args) {
    auto* process = Process(ctx);
    if (!process || !ctx.host) {
        return ToolResult::Error("no application process");
    }
    // Contexts of running threads are stale: pause unless the game already is.
    const bool was_paused = ctx.system->IsPaused();
    if (!was_paused) {
        if (std::string error = ctx.host->Pause(); !error.empty()) {
            return ToolResult::Error("could not pause for the backtrace: " + error);
        }
    }
    const bool all = !args.contains("thread_id");
    const u64 wanted = args.value("thread_id", u64{0});
    json threads = json::array();
    {
        Kernel::KScopedLightLock lock{process->GetListLock()};
        for (const auto& thread : process->GetThreadList()) {
            if (!all && thread.GetThreadId() != wanted) {
                continue;
            }
            json frames = json::array();
            for (const auto& f : Core::GetBacktrace(&thread)) {
                // address = IDA-style (0x7100000000 + module offset); runtime = real VA.
                frames.push_back({{"address", Hex(f.address)},
                                  {"runtime", Hex(f.original_address)},
                                  {"module", f.module},
                                  {"offset", Hex(f.offset)},
                                  {"symbol", f.name}});
            }
            json t = ThreadJson(thread);
            t["frames"] = std::move(frames);
            threads.push_back(std::move(t));
        }
    }
    if (!was_paused) {
        ctx.host->Resume();
    }
    if (threads.empty()) {
        return ToolResult::Error("no thread with that id (list_threads)");
    }
    return ToolResult::Structured(
        json{{"threads", std::move(threads)}, {"paused_for_capture", !was_paused}});
}

} // namespace

/// Registers process_info, memory_map, list_threads and backtrace.
void RegisterProcessTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "process_info",
        .title = "Process info",
        .description = "The running application's process: pid, name, program id, Switch 2 flag, "
                       "address-space layout (alias/heap/aslr/stack) and loaded modules with base "
                       "and end addresses.",
        .annotations = {.read_only = true, .idempotent = true},
        .requires_game = true,
        .handler = ProcessInfo,
    });
    registry.Add(Tool{
        .name = "memory_map",
        .title = "Memory map",
        .description = "The process's mapped regions (base, end, size, permissions, state such as "
                       "Code/CodeData/Normal/Stack/Shared). Optionally filter by state.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"include_free", {{"type", "boolean"}}},
                           {"state", {{"type", "string"}, {"description", "e.g. Normal, Code, Stack"}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .requires_game = true,
        .handler = MemoryMap,
    });
    registry.Add(Tool{
        .name = "list_threads",
        .title = "List guest threads",
        .description = "The application's threads: id, name, state, wait reason, priority, core "
                       "and CPU time.",
        .annotations = {.read_only = true},
        .requires_game = true,
        .handler = ListThreads,
    });
    registry.Add(Tool{
        .name = "backtrace",
        .title = "Thread backtraces",
        .description = "Symbolized backtraces (module, offset, symbol) of one thread or all. "
                       "Pauses the game briefly if it is running (contexts of running threads are "
                       "stale) and resumes it afterwards.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"thread_id", {{"type", "integer"}, {"minimum", 0}}}}},
                         {"additionalProperties", false}},
        // Pausing the game is a state change: it needs the lease (like pause), not writes.
        .annotations = {},
        .requires_game = true,
        .requires_control = true,
        .handler = Backtrace,
    });
}

} // namespace Mcp

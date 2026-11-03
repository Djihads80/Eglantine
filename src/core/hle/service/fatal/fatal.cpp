// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <ctime>
#include <fmt/chrono.h>
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/swap.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/debugger/trace_hooks.h"
#include "core/hle/service/fatal/fatal.h"
#include "core/hle/service/fatal/fatal_p.h"
#include "core/hle/service/fatal/fatal_u.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/reporter.h"

namespace Service::Fatal {

Module::Interface::Interface(std::shared_ptr<Module> module_, Core::System& system_,
                             const char* name)
    : ServiceFramework{system_, name}, module{std::move(module_)} {}

Module::Interface::~Interface() = default;

struct FatalInfo {
    enum class Architecture : s32 {
        AArch64,
        AArch32,
    };

    const char* ArchAsString() const {
        return arch == Architecture::AArch64 ? "AArch64" : "AArch32";
    }

    std::array<u64_le, 31> registers{};
    u64_le sp{};
    u64_le pc{};
    u64_le pstate{};
    u64_le afsr0{};
    u64_le afsr1{};
    u64_le esr{};
    u64_le far{};

    std::array<u64_le, 32> backtrace{};
    u64_le program_entry_point{};

    // Bit flags that indicate which registers have been set with values
    // for this context. The service itself uses these to determine which
    // registers to specifically print out.
    u64_le set_flags{};

    u32_le backtrace_size{};
    Architecture arch{};
    u32_le unk10{}; // TODO(ogniK): Is this even used or is it just padding?
};
static_assert(sizeof(FatalInfo) == 0x250, "FatalInfo is an invalid size");

enum class FatalType : u32 {
    ErrorReportAndScreen = 0,
    ErrorReport = 1,
    ErrorScreen = 2,
};

static void GenerateErrorReport(Core::System& system, Result error_code, const FatalInfo& info) {
    const auto title_id = system.GetApplicationProcessProgramID();
    std::string crash_report = fmt::format(
        "Eden {}-{} crash report\n"
        "Title ID:                        {:016x}\n"
        "Result:                          {:#x} ({:04}-{:04d})\n"
        "Set flags:                       {:#16X}\n"
        "Program entry point:             {:#16X}\n"
        "\n",
        Common::g_scm_branch, Common::g_scm_desc, title_id, error_code.raw,
        2000 + static_cast<u32>(error_code.GetModule()),
        static_cast<u32>(error_code.GetDescription()), info.set_flags, info.program_entry_point);
    if (info.backtrace_size != 0x0) {
        crash_report += "Registers:\n";
        for (size_t i = 0; i < info.registers.size(); i++) {
            crash_report +=
                fmt::format("    X[{:02d}]:                       {:016x}\n", i, info.registers[i]);
        }
        crash_report += fmt::format("    SP:                          {:016x}\n", info.sp);
        crash_report += fmt::format("    PC:                          {:016x}\n", info.pc);
        crash_report += fmt::format("    PSTATE:                      {:016x}\n", info.pstate);
        crash_report += fmt::format("    AFSR0:                       {:016x}\n", info.afsr0);
        crash_report += fmt::format("    AFSR1:                       {:016x}\n", info.afsr1);
        crash_report += fmt::format("    ESR:                         {:016x}\n", info.esr);
        crash_report += fmt::format("    FAR:                         {:016x}\n", info.far);
        crash_report += "\nBacktrace:\n";
        for (u32 i = 0; i < std::min<u32>(info.backtrace_size, 32); i++) {
            crash_report +=
                fmt::format("    Backtrace[{:02d}]:               {:016x}\n", i, info.backtrace[i]);
        }

        crash_report += fmt::format("Architecture:                    {}\n", info.ArchAsString());
        crash_report += fmt::format("Unknown 10:                      {:#016x}\n", info.unk10);
    }

    LOG_ERROR(Service_Fatal, "{}", crash_report);

    system.GetReporter().SaveCrashReport(
        title_id, error_code, info.set_flags, info.program_entry_point, info.sp, info.pc,
        info.pstate, info.afsr0, info.afsr1, info.esr, info.far, info.registers, info.backtrace,
        info.backtrace_size, info.ArchAsString(), info.unk10);
}

/// The AArch32 layout of the CPU context buffer (FatalInfo describes the AArch64 one).
struct FatalAarch32Context {
    std::array<u32_le, 16> r{}; ///< r11 = fp, r13 = sp, r14 = lr, r15 = pc
    u32_le pstate{};
    u32_le afsr0{};
    u32_le afsr1{};
    u32_le esr{};
    u32_le far{};
    std::array<u32_le, 32> stack_trace{};
    u32_le stack_trace_size{};
    u32_le start_address{};
    u32_le register_set_flags{};
};
static_assert(sizeof(FatalAarch32Context) <= sizeof(FatalInfo));

/// Posts a crash event for a fatal error to the crash observer. Everything comes from the CPU
/// context the game sent (ThrowFatalWithCpuContext), decoded by architecture, not from the calling
/// thread: this runs on the service thread, where the caller's saved context may be stale. The
/// game's own stack trace is listed first, then a frame-pointer walk from its registers.
static void NotifyFatalCrash(Core::System& system, Result error_code, FatalType fatal_type,
                             const FatalInfo& info) {
    std::string detail =
        fmt::format("fatal type {} result {:#x} ({:04}-{:04d})", static_cast<u32>(fatal_type),
                    error_code.raw, 2000 + static_cast<u32>(error_code.GetModule()),
                    static_cast<u32>(error_code.GetDescription()));
    auto* const process = system.ApplicationProcess();
    if (process == nullptr) {
        Core::Trace::NotifyCrash("fatal", detail, nullptr);
        return;
    }

    Kernel::Svc::ThreadContext context{};
    std::vector<u64> trace;
    if (info.arch == FatalInfo::Architecture::AArch32) {
        FatalAarch32Context ctx32{};
        std::memcpy(&ctx32, &info, sizeof(ctx32));
        for (size_t i = 0; i < ctx32.r.size(); ++i) {
            context.r[i] = ctx32.r[i];
        }
        context.fp = ctx32.r[11];
        context.sp = ctx32.r[13];
        context.lr = ctx32.r[14];
        context.pc = ctx32.r[15];
        const size_t count = (std::min<size_t>)(ctx32.stack_trace_size, ctx32.stack_trace.size());
        trace.assign(ctx32.stack_trace.begin(), ctx32.stack_trace.begin() + count);
    } else {
        for (size_t i = 0; i < context.r.size(); ++i) {
            context.r[i] = info.registers[i];
        }
        context.fp = info.registers[29];
        context.lr = info.registers[30];
        context.sp = info.sp;
        context.pc = info.pc;
        const size_t count = (std::min<size_t>)(info.backtrace_size, info.backtrace.size());
        trace.assign(info.backtrace.begin(), info.backtrace.begin() + count);
    }

    // Module of an address, for the game's own stack trace (raw addresses, not symbolized here).
    const auto modules = Core::FindModules(process);
    const auto locate = [&](u64 address) {
        auto it = modules.upper_bound(address);
        if (it == modules.begin()) {
            return fmt::format("{:#x}", address);
        }
        --it;
        return fmt::format("{}+{:#x}", it->second, address - it->first);
    };
    detail += fmt::format("\npc {}", locate(context.pc));
    if (!trace.empty()) {
        detail += "\nstack trace sent by the game:";
        for (const u64 address : trace) {
            detail += "\n" + locate(address);
        }
    }
    if (context.pc != 0) {
        detail += "\nframe walk from the game's registers:";
        for (const auto& frame : Core::GetBacktraceFromContext(process, context)) {
            detail += fmt::format("\n{}+{:#x} {}", frame.module, frame.offset, frame.name);
        }
    }
    Core::Trace::NotifyCrash("fatal", detail, nullptr);
}

static void ThrowFatalError(Core::System& system, Result error_code, FatalType fatal_type,
                            const FatalInfo& info) {
    LOG_ERROR(Service_Fatal, "Threw fatal error type {} with error code {:#x}", fatal_type,
              error_code.raw);
    if (Core::Trace::g_crash_hook.load(std::memory_order_relaxed)) {
        NotifyFatalCrash(system, error_code, fatal_type, info);
    }

    switch (fatal_type) {
    case FatalType::ErrorReportAndScreen:
        GenerateErrorReport(system, error_code, info);
        [[fallthrough]];
    case FatalType::ErrorScreen:
        // Since we have no fatal:u error screen. We should just kill execution instead
        ASSERT(false);
        break;
        // Should not throw a fatal screen but should generate an error report
    case FatalType::ErrorReport:
        GenerateErrorReport(system, error_code, info);
        break;
    }
}

void Module::Interface::ThrowFatal(HLERequestContext& ctx) {
    LOG_ERROR(Service_Fatal, "called");
    IPC::RequestParser rp{ctx};
    const auto error_code = rp.Pop<Result>();

    ThrowFatalError(system, error_code, FatalType::ErrorScreen, {});
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::ThrowFatalWithPolicy(HLERequestContext& ctx) {
    LOG_ERROR(Service_Fatal, "called");
    IPC::RequestParser rp(ctx);
    const auto error_code = rp.Pop<Result>();
    const auto fatal_type = rp.PopEnum<FatalType>();

    ThrowFatalError(system, error_code, fatal_type,
                    {}); // No info is passed with ThrowFatalWithPolicy
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::ThrowFatalWithCpuContext(HLERequestContext& ctx) {
    LOG_ERROR(Service_Fatal, "called");
    IPC::RequestParser rp(ctx);
    const auto error_code = rp.Pop<Result>();
    const auto fatal_type = rp.PopEnum<FatalType>();
    const auto fatal_info = ctx.ReadBuffer();
    FatalInfo info{};

    ASSERT_MSG(fatal_info.size() == sizeof(FatalInfo), "Invalid fatal info buffer size!");
    std::memcpy(&info, fatal_info.data(), sizeof(FatalInfo));

    ThrowFatalError(system, error_code, fatal_type, info);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    auto module = std::make_shared<Module>();

    server_manager->RegisterNamedService("fatal:p", std::make_shared<Fatal_P>(module, system));
    server_manager->RegisterNamedService("fatal:u", std::make_shared<Fatal_U>(module, system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::Fatal

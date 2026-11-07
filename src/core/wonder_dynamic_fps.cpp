// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>

#include "common/fs/path_util.h"
#include "common/game_fps.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include "core/wonder_dynamic_fps.h"

namespace Core {
bool TryHandleWonderTimingSvc(System& system, u32 imm, std::span<u64, 8> args) {
    if ((imm != 0xf001 && imm != 0xf002) ||
        system.GetApplicationProcessProgramID() != Settings::WonderSwitch2ProgramId ||
        !Settings::wonder_dynamic_fps_active.load(std::memory_order_acquire)) {
        return false;
    }
    auto& kernel = system.Kernel();
    auto& process = Kernel::GetCurrentProcess(kernel);
    if (!process.Is64Bit()) {
        return false;
    }
    const auto presents = g_nx2_present_count.load(std::memory_order_relaxed);
    // Private instruction installed by the build-checked loader patch. Preserve every
    // register except W1. The fallback additionally updates controller rate data;
    // never return an SVC result in X0. No executable writes during gameplay.
    static std::mutex timing_mutex;
    static Core::WonderDynamicFps timing;
    static Core::WonderSpeedAudit audit;
    static std::ofstream audit_csv;
    static u64 csv_present = ~u64{};
    static bool csv_attempted{};
    static u64 generation{};
    std::scoped_lock lock{timing_mutex};
    const auto current_generation =
        Settings::wonder_dynamic_fps_generation.load(std::memory_order_relaxed);
    if (generation != current_generation) {
        timing = {};
        audit = {};
        audit_csv.close();
        audit_csv.clear();
        csv_attempted = false;
        csv_present = ~u64{};
        generation = current_generation;
    }
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    const auto target = Settings::wonder_fps_active.load(std::memory_order_relaxed);
    const u32 speed = Settings::values.use_speed_limit.GetValue() ? Settings::SpeedLimit() : 0;
    static const bool audit_enabled = [] {
        const char* value = std::getenv("EDEN_WONDER_SPEED_AUDIT");
        return value && std::strcmp(value, "1") == 0;
    }();
    Kernel::Svc::ThreadContext context{};
    process.GetArmInterface(kernel.CurrentPhysicalCore().CoreIndex())->GetContext(context);
    auto& memory = process.GetMemory();
    constexpr u32 primary_offset = 0x1bbfb0;
    const u32 hook_offset = imm == 0xf001 ? primary_offset : 0x1bc04c;
    const u32 hook = 0xd4000001U | (imm << 5);
    u64 site = context.pc;
    if (memory.IsValidVirtualAddressRange(site - 4, 8) && memory.Read32(site - 4) == hook)
        site -= 4;
    float scale = std::numeric_limits<float>::quiet_NaN();
    if (memory.IsValidVirtualAddressRange(site, 4) && memory.Read32(site) == hook) {
        const u64 scale_address = site - hook_offset + 0x3aac460;
        if (memory.IsValidVirtualAddressRange(scale_address, 4))
            scale = std::bit_cast<float>(memory.Read32(scale_address));
    }
    if (audit_enabled && std::getenv("EDEN_WONDER_SPEED_CSV") && csv_present != presents) {
        csv_present = presents;
        if (!csv_attempted) {
            csv_attempted = true;
            const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                              fmt::format("wonder-speed-{}.csv", now);
            audit_csv.open(path);
            if (audit_csv) {
                audit_csv << "present,now_ns,scale,target_fps,speed_percent,hook\n";
                LOG_INFO(Kernel_SVC, "Wonder speed CSV: {}", path.string());
            }
        }
        if (audit_csv) {
            audit_csv << presents << ',' << now << ',' << scale << ',' << target << ',' << speed
                      << ',' << imm << '\n';
            if (presents % 120 == 0)
                audit_csv.flush();
        }
    }
    if (audit_enabled && audit.Observe(presents, now, scale)) {
        LOG_INFO(Kernel_SVC,
                 "Wonder speed audit: wall_s={:.6f} game_s={:.6f} speed_pct={:.3f} "
                 "frames={} uncovered_s={:.6f} discontinuities={} scale={:.6f} "
                 "above30_wall_s={:.6f} above30_game_s={:.6f} below30_wall_s={:.6f} "
                 "below30_game_s={:.6f} steady30_wall_s={:.6f} steady30_game_s={:.6f}",
                 audit.wall_seconds, audit.game_seconds,
                 audit.wall_seconds > 0 ? 100.0 * audit.game_seconds / audit.wall_seconds : 0,
                 audit.frames, audit.uncovered_seconds, audit.discontinuities, scale,
                 audit.above30_wall_seconds, audit.above30_game_seconds, audit.below30_wall_seconds,
                 audit.below30_game_seconds, audit.steady30_wall_seconds,
                 audit.steady30_game_seconds);
    }
    args[1] =
        timing.Sample(presents, now, target, speed,
                      scale > 0 ? double(scale) / 60.0 : std::numeric_limits<double>::quiet_NaN());
    if (imm == 0xf002) {
        // Unlike the primary path, the original fallback LDR has no following STR.
        // Update the verified controller field so the caller's 60/Hz conversion sees
        // the new rate. This is data, never executable memory.
        const u64 controller_rate = context.r[20] + 0x50;
        if (memory.IsValidVirtualAddressRange(controller_rate, sizeof(u32)))
            memory.Write32(controller_rate, static_cast<u32>(args[1]));
    }
    static u64 logged_present = ~u64{};
    if (presents != logged_present && (presents < 8 || presents % 120 == 0)) {
        logged_present = presents;
        LOG_INFO(Kernel_SVC, "Wonder dynamic timing: present {} simulation {} Hz target {}",
                 presents, args[1], target);
    }
    return true;
}
} // namespace Core

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "common/logging.h"
#include "core/hle/service/nvdrv/devices/nvhost_prof_gpu.h"

namespace Service::Nvidia::Devices {

nvhost_prof_gpu::nvhost_prof_gpu(Core::System& system_) : nvdevice{system_} {}
nvhost_prof_gpu::~nvhost_prof_gpu() = default;

NvResult nvhost_prof_gpu::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                                 std::span<u8> output) {
    return Acknowledge(command, output);
}

NvResult nvhost_prof_gpu::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                                 std::span<const u8> inline_input, std::span<u8> output) {
    return Acknowledge(command, output);
}

NvResult nvhost_prof_gpu::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                                 std::span<u8> output, std::span<u8> inline_output) {
    std::ranges::fill(inline_output, u8{0});
    return Acknowledge(command, output);
}

void nvhost_prof_gpu::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {
    LOG_INFO(Service_NVDRV, "GPU profiler device opened, fd={}", fd);
}

void nvhost_prof_gpu::OnClose(DeviceFD fd) {}

NvResult nvhost_prof_gpu::Acknowledge(Ioctl command, std::span<u8> output) {
    // No profiling data is produced; report success with zeroed output so callers proceed.
    std::ranges::fill(output, u8{0});
    std::scoped_lock lock{mutex};
    if (logged_commands.insert(command.raw).second) {
        LOG_WARNING(Service_NVDRV, "(STUBBED) GPU profiler ioctl={:08X}, output={} bytes", command.raw,
                    output.size());
    }
    return NvResult::Success;
}

} // namespace Service::Nvidia::Devices

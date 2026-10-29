// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include <set>
#include "common/common_types.h"
#include "core/hle/service/nvdrv/devices/nvdevice.h"

namespace Service::Nvidia::Devices {

/// GPU profiler device. Newer native SDKs open it during GPU initialisation and tear the whole
/// device down when the open fails, so it opens successfully and acknowledges every request.
class nvhost_prof_gpu final : public nvdevice {
public:
    explicit nvhost_prof_gpu(Core::System& system_);
    ~nvhost_prof_gpu() override;

    NvResult Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                    std::span<u8> output) override;
    NvResult Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                    std::span<const u8> inline_input, std::span<u8> output) override;
    NvResult Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                    std::span<u8> inline_output) override;

    void OnOpen(NvCore::SessionId session_id, DeviceFD fd) override;
    void OnClose(DeviceFD fd) override;

private:
    NvResult Acknowledge(Ioctl command, std::span<u8> output);

    std::mutex mutex;
    std::set<u32> logged_commands;
};

} // namespace Service::Nvidia::Devices

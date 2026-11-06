// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include "common/common_types.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "hid_core/frontend/emulated_console.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/frontend/emulated_devices.h"
#include "hid_core/hid_core.h"
#include "hid_core/resources/six_axis/seven_six_axis.h"

namespace Service::HID {
SevenSixAxis::SevenSixAxis(Core::System& system_)
    : ControllerBase{system_.HIDCore()}, system{system_} {
    console = hid_core.GetEmulatedConsole();
}

SevenSixAxis::~SevenSixAxis() = default;

void SevenSixAxis::OnInit() {}
void SevenSixAxis::OnRelease() {}

void SevenSixAxis::OnUpdate(const Core::Timing::CoreTiming& core_timing) {
    if (!IsControllerActivated() || transfer_memory == 0 || transfer_memory_owner == nullptr) {
        seven_sixaxis_lifo.buffer_count = 0;
        seven_sixaxis_lifo.buffer_tail = 0;
        return;
    }

    const auto& last_entry = seven_sixaxis_lifo.ReadCurrentEntry().state;
    next_seven_sixaxis_state.sampling_number = last_entry.sampling_number + 1;

    const auto motion_status = console->GetMotion();
    last_global_timestamp = core_timing.GetGlobalTimeNs().count();

    // This value increments every time the switch goes to sleep
    next_seven_sixaxis_state.unknown = 1;
    next_seven_sixaxis_state.timestamp = last_global_timestamp - last_saved_timestamp;
    next_seven_sixaxis_state.accel = motion_status.accel;
    next_seven_sixaxis_state.gyro = motion_status.gyro;
    next_seven_sixaxis_state.quaternion = {
        motion_status.quaternion[1],
        motion_status.quaternion[0],
        -motion_status.quaternion[3],
        -motion_status.quaternion[2],
    };
    seven_sixaxis_lifo.WriteNextEntry(next_seven_sixaxis_state);
    transfer_memory_owner->GetMemory().WriteBlock(transfer_memory, &seven_sixaxis_lifo,
                                                  sizeof(seven_sixaxis_lifo));
}

void SevenSixAxis::SetTransferMemoryAddress(Common::ProcessAddress t_mem, Kernel::KProcess* owner) {
    transfer_memory = t_mem;
    transfer_memory_owner = owner;
}

void SevenSixAxis::ResetTimestamp() {
    last_saved_timestamp = last_global_timestamp;
}

namespace {
constexpr u64 ImuRingSize = 0x288;
constexpr u64 ImuRingWriteIndex = 0x280;
constexpr u64 ImuRingSamples = 16;
constexpr u64 ImuRightRings = 0x1950;
constexpr u64 ImuConsoleRing = 0x32a0;
constexpr u64 ImuSampleInterval = 5'000'000;
constexpr f32 MetersPerSecondSquaredPerG = 9.80665f;
constexpr f32 RadiansPerTurn = 6.2831855f;
} // namespace

NpadImu::NpadImu(Core::System& system_) : system{system_} {}

NpadImu::~NpadImu() = default;

void NpadImu::Enable(Common::ProcessAddress address, Kernel::KProcess* owner) {
    std::scoped_lock lock{mutex};
    left_rings = {};
    right_rings = {};
    console_ring = {};
    buffer = address;
    buffer_owner = owner;
}

void NpadImu::Disable() {
    std::scoped_lock lock{mutex};
    buffer = {};
    buffer_owner = nullptr;
}

void NpadImu::WriteSample(Common::ProcessAddress ring_address, Ring& ring,
                          const Core::HID::ControllerMotion& motion) {
    const Sample sample{
        .sampling_number = ++ring.sampling_number,
        .delta_time = ImuSampleInterval,
        .accel = motion.accel * MetersPerSecondSquaredPerG,
        .gyro = motion.gyro * RadiansPerTurn,
    };
    auto& memory = buffer_owner->GetMemory();
    memory.WriteBlock(ring_address + ring.write_index * sizeof(Sample), &sample, sizeof(sample));
    ring.write_index = (ring.write_index + 1) % ImuRingSamples;
    memory.Write64(ring_address + ImuRingWriteIndex, ring.write_index);
}

void NpadImu::OnUpdate() {
    std::scoped_lock lock{mutex};
    if (buffer_owner == nullptr || !Settings::values.motion_enabled.GetValue()) {
        return;
    }
    auto& hid_core = system.HIDCore();
    for (u64 index = 0; index < left_rings.size(); ++index) {
        const auto* const controller =
            hid_core.GetEmulatedController(static_cast<Core::HID::NpadIdType>(index));
        if (controller == nullptr || !controller->IsConnected()) {
            continue;
        }
        const auto motions = controller->GetMotions();
        const auto left = buffer + index * ImuRingSize;
        const auto right = buffer + ImuRightRings + index * ImuRingSize;
        switch (controller->GetNpadStyleIndex()) {
        case Core::HID::NpadStyleIndex::Fullkey:
        case Core::HID::NpadStyleIndex::JoyconLeft:
            WriteSample(left, left_rings[index], motions[0]);
            break;
        case Core::HID::NpadStyleIndex::JoyconRight:
            WriteSample(right, right_rings[index], motions[1]);
            break;
        case Core::HID::NpadStyleIndex::JoyconDual:
            WriteSample(left, left_rings[index], motions[0]);
            WriteSample(right, right_rings[index], motions[1]);
            break;
        default:
            break;
        }
    }
    const auto* const handheld = hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    if (handheld != nullptr && handheld->IsConnected()) {
        WriteSample(buffer + ImuConsoleRing, console_ring, handheld->GetMotions()[0]);
    }
}

} // namespace Service::HID

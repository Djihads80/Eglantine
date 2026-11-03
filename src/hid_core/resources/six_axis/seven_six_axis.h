// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <mutex>

#include "common/common_types.h"
#include "common/vector_math.h"
#include "common/typed_address.h"
#include "hid_core/resources/controller_base.h"
#include "hid_core/resources/ring_lifo.h"

namespace Kernel {
class KProcess;
}

namespace Core {
class System;
} // namespace Core

namespace Core::HID {
class EmulatedConsole;
struct ControllerMotion;
} // namespace Core::HID

namespace Service::HID {
class SevenSixAxis final : public ControllerBase {
public:
    explicit SevenSixAxis(Core::System& system_);
    ~SevenSixAxis() override;

    // Called when the controller is initialized
    void OnInit() override;

    // When the controller is released
    void OnRelease() override;

    // When the controller is requesting an update for the shared memory
    void OnUpdate(const Core::Timing::CoreTiming& core_timing) override;

    // Called on InitializeSevenSixAxisSensor
    void SetTransferMemoryAddress(Common::ProcessAddress t_mem, Kernel::KProcess* owner);

    // Called on ResetSevenSixAxisSensorTimestamp
    void ResetTimestamp();

private:
    struct SevenSixAxisState {
        INSERT_PADDING_WORDS(2); // unused
        u64 timestamp{};
        u64 sampling_number{};
        u64 unknown{};
        Common::Vec<f32, 3> accel{};
        Common::Vec<f32, 3> gyro{};
        Common::Vec<f32, 4> quaternion{};
    };
    static_assert(sizeof(SevenSixAxisState) == 0x48, "SevenSixAxisState is an invalid size");

    Lifo<SevenSixAxisState, 0x21> seven_sixaxis_lifo{};
    static_assert(sizeof(seven_sixaxis_lifo) == 0xA70, "SevenSixAxisState is an invalid size");

    u64 last_saved_timestamp{};
    u64 last_global_timestamp{};

    SevenSixAxisState next_seven_sixaxis_state{};
    Common::ProcessAddress transfer_memory{};
    Kernel::KProcess* transfer_memory_owner{};
    Core::HID::EmulatedConsole* console = nullptr;

    Core::System& system;
};

class NpadImu final {
public:
    explicit NpadImu(Core::System& system_);
    ~NpadImu();

    void Enable(Common::ProcessAddress address, Kernel::KProcess* owner);
    void Disable();
    void OnUpdate();

private:
    struct Sample {
        u64 sampling_number{};
        u64 delta_time{};
        Common::Vec<f32, 3> accel{};
        Common::Vec<f32, 3> gyro{};
    };
    static_assert(sizeof(Sample) == 0x28, "Sample is an invalid size");

    struct Ring {
        u64 sampling_number{};
        u64 write_index{};
    };

    void WriteSample(Common::ProcessAddress ring_address, Ring& ring,
                     const Core::HID::ControllerMotion& motion);

    std::mutex mutex;
    std::array<Ring, 8> left_rings{};
    std::array<Ring, 8> right_rings{};
    Ring console_ring{};
    Common::ProcessAddress buffer{};
    Kernel::KProcess* buffer_owner{};

    Core::System& system;
};
} // namespace Service::HID

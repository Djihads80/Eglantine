// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include "core/hle/service/audio/nx2_hardware_audio_codec_types.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Service::Audio {

class INx2HardwareAudioCodecManager final
    : public ServiceFramework<INx2HardwareAudioCodecManager> {
public:
    explicit INx2HardwareAudioCodecManager(Core::System& system_, Kernel::KProcess* process_,
                                          const Nx2HardwareAudioCodecSharedMemory& params_,
                                          u64 shared_address_, u32 manager_id_);
    ~INx2HardwareAudioCodecManager() override;

    Kernel::KReadableEvent& GetCompletionEvent();

private:
    struct Task {
        u16 manager_id;
        u16 instance_id;
        u16 io_id;
        u16 sequence;
        u32 input_size;
        u8 reset;
        u8 end_of_batch;
        u16 reserved;
    };
    static_assert(sizeof(Task) == 0x10);

    struct Instance;

    Result InitializeInstance(
        u32 instance_id,
        InLargeData<Nx2HardwareAudioCodecParameter, BufferAttr_HipcPointer> parameter);
    Result FinalizeInstance(u32 instance_id);
    Result Execute(InArray<Task, BufferAttr_HipcAutoSelect> tasks);
    Result Decode(const Task& task, u32& output_size);

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* completion_event;
    Kernel::KProcess* process;
    Nx2HardwareAudioCodecSharedMemory params;
    u64 shared_address;
    u32 manager_id;
    u32 write_index{};
    bool logged_first_decode{};
    std::vector<Instance> instances;
    std::vector<u8> input_buffer;
    std::vector<s16> output_buffer;
};

} // namespace Service::Audio

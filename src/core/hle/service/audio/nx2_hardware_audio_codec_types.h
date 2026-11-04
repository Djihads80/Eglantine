// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include "common/common_types.h"

namespace Service::Audio {

struct Nx2HardwareAudioCodecParameter {
    u64 codec;
    u32 sample_rate;
    u32 channel_count;
    u64 frame_duration_ns;
    u32 stream_count;
    u32 stereo_stream_count;
    std::array<u8, 256> channel_mapping;
};
static_assert(sizeof(Nx2HardwareAudioCodecParameter) == 0x120);

struct Nx2HardwareAudioCodecSharedMemory {
    u32 version;
    u32 manager_size;
    u32 manager_offset;
    u32 instances_size;
    u32 instances_offset;
    u32 dsp_ring_size;
    u32 dsp_ring_offset;
    u32 transfer_memory_size;
    u32 io_data_size;
    u32 io_data_offset;
    u32 completion_ring_size;
    u32 completion_ring_offset;
    u32 shared_memory_size;
    u32 instance_size;
    u32 instance_count;
    u32 input_capacity;
    u32 output_capacity;
    u32 io_count;
    std::array<u32, 6> reserved;
};
static_assert(sizeof(Nx2HardwareAudioCodecSharedMemory) == 0x60);

} // namespace Service::Audio

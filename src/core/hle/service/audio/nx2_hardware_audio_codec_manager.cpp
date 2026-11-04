// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>

#include <opus.h>
#include <opus_multistream.h>

#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/audio/errors.h"
#include "core/hle/service/audio/nx2_hardware_audio_codec_manager.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/memory.h"

namespace Service::Audio {
namespace {

constexpr u64 RingEntriesOffset = 0x80;
constexpr u64 RingReadIndexOffset = 0x44;
constexpr u64 RingWriteIndexOffset = 4;

struct Completion {
    u16 manager_id;
    u16 instance_id;
    u16 io_id;
    u16 sequence;
    u64 elapsed_ns;
    u32 output_size;
    u32 result;
    std::array<u8, 0x28> reserved;
};
static_assert(sizeof(Completion) == 0x40);

Result OpusResult(int result) {
    switch (result) {
    case OPUS_OK:
        return ResultSuccess;
    case OPUS_BAD_ARG:
        return ResultLibOpusBadArg;
    case OPUS_BUFFER_TOO_SMALL:
        return ResultBufferTooSmall;
    case OPUS_INTERNAL_ERROR:
        return ResultLibOpusInternalError;
    case OPUS_INVALID_PACKET:
        return ResultLibOpusInvalidPacket;
    case OPUS_UNIMPLEMENTED:
        return ResultLibOpusUnimplemented;
    case OPUS_INVALID_STATE:
        return ResultLibOpusInvalidState;
    case OPUS_ALLOC_FAIL:
        return ResultLibOpusAllocFail;
    default:
        return ResultInvalidOpusDSPReturnCode;
    }
}

struct OpusDeleter {
    void operator()(::OpusDecoder* decoder) const {
        opus_decoder_destroy(decoder);
    }
    void operator()(::OpusMSDecoder* decoder) const {
        opus_multistream_decoder_destroy(decoder);
    }
};

} // namespace

struct INx2HardwareAudioCodecManager::Instance {
    u32 sample_rate{};
    u32 channel_count{};
    std::unique_ptr<::OpusDecoder, OpusDeleter> decoder;
    std::unique_ptr<::OpusMSDecoder, OpusDeleter> multistream_decoder;
};

INx2HardwareAudioCodecManager::INx2HardwareAudioCodecManager(
    Core::System& system_, Kernel::KProcess* process_,
    const Nx2HardwareAudioCodecSharedMemory& params_, u64 shared_address_, u32 manager_id_)
    : ServiceFramework{system_, "INx2HardwareAudioCodecManager"},
      service_context{system_, "INx2HardwareAudioCodecManager"},
      completion_event{service_context.CreateEvent("HardwareAudioCodecCompletion")},
      process{process_}, params{params_}, shared_address{shared_address_}, manager_id{manager_id_},
      instances(params.instance_count) {
    static const FunctionInfo functions[] = {
        {0, D<&INx2HardwareAudioCodecManager::InitializeInstance>, "InitializeInstance"},
        {1, D<&INx2HardwareAudioCodecManager::FinalizeInstance>, "FinalizeInstance"},
        {10, D<&INx2HardwareAudioCodecManager::Execute>, "Execute"},
    };
    RegisterHandlers(functions);
    process->Open(system.Kernel());

    auto& memory = process->GetMemory();
    const u64 ring_address = shared_address + params.completion_ring_offset;
    memory.Write32(ring_address, 1);
    memory.Write32(ring_address + RingWriteIndexOffset, write_index);
}

INx2HardwareAudioCodecManager::~INx2HardwareAudioCodecManager() {
    service_context.CloseEvent(completion_event);
    process->Close(system.Kernel());
}

Kernel::KReadableEvent& INx2HardwareAudioCodecManager::GetCompletionEvent() {
    return completion_event->GetReadableEvent();
}

Result INx2HardwareAudioCodecManager::InitializeInstance(
    u32 instance_id,
    InLargeData<Nx2HardwareAudioCodecParameter, BufferAttr_HipcPointer> parameter) {
    R_UNLESS(instance_id < instances.size(), ResultInvalidArgument);
    const auto& p = *parameter;
    R_UNLESS(p.sample_rate == 8000 || p.sample_rate == 12000 || p.sample_rate == 16000 ||
                 p.sample_rate == 24000 || p.sample_rate == 48000,
             ResultInvalidOpusSampleRate);
    R_UNLESS(p.codec == 1 || p.codec == 2, ResultNotSupported);
    R_UNLESS(p.channel_count >= 1 && p.channel_count <= (p.codec == 1 ? 2U : 255U),
             ResultInvalidOpusChannelCount);
    auto& instance = instances[instance_id];
    R_UNLESS(!instance.decoder && !instance.multistream_decoder, ResultLibOpusInvalidState);

    int error{};
    if (p.codec == 1) {
        instance.decoder.reset(opus_decoder_create(p.sample_rate, p.channel_count, &error));
    } else {
        R_UNLESS(p.stream_count >= 1 && p.stream_count <= 255 &&
                     p.stereo_stream_count <= p.stream_count &&
                     p.stream_count + p.stereo_stream_count <= 255,
                 ResultInvalidOpusChannelCount);
        instance.multistream_decoder.reset(opus_multistream_decoder_create(
            p.sample_rate, p.channel_count, p.stream_count, p.stereo_stream_count,
            p.channel_mapping.data(), &error));
    }
    R_TRY(OpusResult(error));
    instance.sample_rate = p.sample_rate;
    instance.channel_count = p.channel_count;
    LOG_DEBUG(Service_Audio, "Initialized codec instance {}: codec={} rate={} channels={}",
              instance_id, p.codec, p.sample_rate, p.channel_count);
    R_SUCCEED();
}

Result INx2HardwareAudioCodecManager::FinalizeInstance(u32 instance_id) {
    R_UNLESS(instance_id < instances.size(), ResultInvalidArgument);
    instances[instance_id] = {};
    R_SUCCEED();
}

Result INx2HardwareAudioCodecManager::Decode(const Task& task, u32& output_size) {
    output_size = 0;
    auto& instance = instances[task.instance_id];
    R_UNLESS(instance.decoder || instance.multistream_decoder, ResultLibOpusInvalidState);
    R_UNLESS(task.input_size > 8, ResultInputDataTooSmall);
    R_UNLESS(task.input_size <= params.input_capacity, ResultBufferTooSmall);

    const u64 stride = u64{params.input_capacity} + params.output_capacity;
    const u64 input_address = shared_address + params.io_data_offset + stride * task.io_id;
    auto& memory = process->GetMemory();
    input_buffer.resize(task.input_size);
    R_UNLESS(memory.ReadBlock(input_address, input_buffer.data(), input_buffer.size()),
             ResultInvalidAddressInfo);
    const u32 packet_size = (u32{input_buffer[0]} << 24) | (u32{input_buffer[1]} << 16) |
                            (u32{input_buffer[2]} << 8) | input_buffer[3];
    R_UNLESS(packet_size > 0 && packet_size <= task.input_size - 8 &&
                 packet_size <= std::numeric_limits<opus_int32>::max(),
             ResultLibOpusInvalidPacket);

    const u32 frame_capacity = std::min(params.output_capacity / (instance.channel_count * 2),
                                       instance.sample_rate * 120 / 1000);
    R_UNLESS(frame_capacity > 0, ResultBufferTooSmall);
    output_buffer.resize(frame_capacity * instance.channel_count);

    int samples;
    if (instance.decoder) {
        if (task.reset) {
            R_TRY(OpusResult(opus_decoder_ctl(instance.decoder.get(), OPUS_RESET_STATE)));
        }
        samples = opus_decode(instance.decoder.get(), input_buffer.data() + 8, packet_size,
                              output_buffer.data(), frame_capacity, 0);
    } else {
        if (task.reset) {
            R_TRY(OpusResult(
                opus_multistream_decoder_ctl(instance.multistream_decoder.get(), OPUS_RESET_STATE)));
        }
        samples = opus_multistream_decode(instance.multistream_decoder.get(), input_buffer.data() + 8,
                                         packet_size, output_buffer.data(), frame_capacity, 0);
    }
    if (samples < 0) {
        R_RETURN(OpusResult(samples));
    }
    const u32 bytes = samples * instance.channel_count * sizeof(s16);
    R_UNLESS(memory.WriteBlock(input_address + params.input_capacity, output_buffer.data(), bytes),
             ResultInvalidAddressInfo);
    output_size = bytes;
    if (!logged_first_decode) {
        s32 peak{};
        for (u32 i = 0; i < bytes / sizeof(s16); ++i) {
            const s32 sample = output_buffer[i];
            peak = std::max(peak, sample < 0 ? -sample : sample);
        }
        LOG_INFO(Service_Audio,
                 "hwacod first decode: instance={} input={} output={} samples={} PCM peak={}",
                 task.instance_id, task.input_size, bytes, samples, peak);
        logged_first_decode = true;
    }
    R_SUCCEED();
}

Result INx2HardwareAudioCodecManager::Execute(InArray<Task, BufferAttr_HipcAutoSelect> tasks) {
    R_SUCCEED_IF(tasks.empty());
    auto& memory = process->GetMemory();
    R_UNLESS(memory.IsValidVirtualAddressRange(shared_address, params.shared_memory_size),
             ResultInvalidAddressInfo);
    const u64 ring_address = shared_address + params.completion_ring_offset;
    const u32 capacity = params.io_count + 1;
    const u32 read_index = memory.Read32(ring_address + RingReadIndexOffset);
    R_UNLESS(read_index < capacity, ResultInvalidArgument);
    const u32 available = (read_index + capacity - write_index - 1) % capacity;
    R_UNLESS(tasks.size() <= available, ResultBufferCountReached);
    for (const auto& task : tasks) {
        R_UNLESS(task.manager_id == manager_id && task.instance_id < instances.size() &&
                     task.io_id < params.io_count && task.input_size <= params.input_capacity,
                 ResultInvalidArgument);
    }

    for (const auto& task : tasks) {
        Completion completion{};
        completion.manager_id = task.manager_id;
        completion.instance_id = task.instance_id;
        completion.io_id = task.io_id;
        completion.sequence = task.sequence;
        const auto start = std::chrono::steady_clock::now();
        const Result result = Decode(task, completion.output_size);
        completion.result = result.raw;
        completion.elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
        if (result.IsError()) {
            LOG_WARNING(Service_Audio, "Codec instance {} task {} failed: {:08X}", task.instance_id,
                        task.sequence, result.raw);
        }
        R_UNLESS(memory.WriteBlock(ring_address + RingEntriesOffset + write_index * sizeof(Completion),
                                   &completion, sizeof(completion)),
                 ResultInvalidAddressInfo);
        write_index = (write_index + 1) % capacity;
        memory.Write32(ring_address + RingWriteIndexOffset, write_index);
    }
    completion_event->Signal(system.Kernel());
    LOG_TRACE(Service_Audio, "Completed {} codec tasks", tasks.size());
    R_SUCCEED();
}

} // namespace Service::Audio

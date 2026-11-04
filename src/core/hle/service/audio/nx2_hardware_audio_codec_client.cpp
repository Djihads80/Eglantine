// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio_core/adsp/apps/opus/opus_decode_object.h"
#include "audio_core/adsp/apps/opus/opus_multistream_decode_object.h"
#include "common/alignment.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/service/audio/errors.h"
#include "core/hle/service/audio/nx2_hardware_audio_codec_client.h"
#include "core/hle/service/audio/nx2_hardware_audio_codec_manager.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/memory.h"

namespace Service::Audio {

INx2HardwareAudioCodecClient::INx2HardwareAudioCodecClient(Core::System& system_)
    : ServiceFramework{system_, "nx2:hwacod:client"} {
    static const FunctionInfo functions[] = {
        {0, D<&INx2HardwareAudioCodecClient::GetInstanceWorkBufferSize>,
         "GetInstanceWorkBufferSize"},
        {1, D<&INx2HardwareAudioCodecClient::GetManagerWorkBufferSize>, "GetManagerWorkBufferSize"},
        {10, D<&INx2HardwareAudioCodecClient::OpenManager>, "OpenManager"},
    };
    RegisterHandlers(functions);
}

Result INx2HardwareAudioCodecClient::GetInstanceWorkBufferSize(
    Out<u32> out_size,
    InLargeData<Nx2HardwareAudioCodecParameter, BufferAttr_HipcPointer> parameter) {
    using namespace AudioCore::ADSP::OpusDecoder;
    const auto& p = *parameter;
    R_UNLESS(p.sample_rate == 8000 || p.sample_rate == 12000 || p.sample_rate == 16000 ||
                 p.sample_rate == 24000 || p.sample_rate == 48000,
             ResultInvalidOpusSampleRate);
    u32 size{};
    switch (p.codec) {
    case 1:
        R_UNLESS(p.channel_count == 1 || p.channel_count == 2, ResultInvalidOpusChannelCount);
        size = OpusDecodeObject::GetWorkBufferSize(p.channel_count);
        break;
    case 2:
        R_UNLESS(p.channel_count > 0 && p.channel_count <= 255 && p.stream_count > 0 &&
                     p.stream_count <= 255 && p.stereo_stream_count <= p.stream_count &&
                     p.stream_count + p.stereo_stream_count <= 255,
                 ResultInvalidOpusChannelCount);
        size =
            OpusMultiStreamDecodeObject::GetWorkBufferSize(p.stream_count, p.stereo_stream_count);
        break;
    default:
        R_THROW(ResultNotSupported);
    }
    // Keep guest allocations independent of optional neural features in the host libopus.
    *out_size = Common::AlignUp(size, 64);
    LOG_DEBUG(Service_Audio, "hwacod codec={} rate={} channels={} instance size={:#x}", p.codec,
              p.sample_rate, p.channel_count, *out_size);
    R_SUCCEED();
}

Result INx2HardwareAudioCodecClient::GetManagerWorkBufferSize(Out<u32> out_size, u32 io_count) {
    R_UNLESS(io_count > 0 && io_count <= 0xffff, ResultInvalidArgument);
    // DSP state is host-owned; reserve one guest page. The SDK sizes the task rings separately.
    *out_size = 0x1000;
    R_SUCCEED();
}

Result INx2HardwareAudioCodecClient::OpenManager(
    Out<u32> out_id, Out<SharedPointer<INx2HardwareAudioCodecManager>> out_manager,
    OutCopyHandle<Kernel::KReadableEvent> out_event,
    Nx2HardwareAudioCodecSharedMemory p, InCopyHandle<Kernel::KTransferMemory> transfer_memory,
    InCopyHandle<Kernel::KProcess> process, u64 transfer_size, u64 shared_address, u64 shared_size,
    u64 properties) {
    R_UNLESS(transfer_memory && process && transfer_memory->GetOwner() == process.Get(),
             ResultInvalidHandle);
    R_UNLESS(p.version == 1 && p.instance_count > 0 && p.instance_count <= 0xffff &&
                 p.io_count > 0 && p.io_count <= 0xffff && next_manager_id <= 0xffff,
             ResultInvalidArgument);
    R_UNLESS(transfer_size <= transfer_memory->GetSize() &&
                 p.transfer_memory_size <= transfer_size && p.shared_memory_size <= shared_size,
             ResultInsufficientBuffer);
    const auto fits = [](u64 offset, u64 size, u64 total) {
        return offset <= total && size <= total - offset;
    };
    const u64 io_size = u64{p.io_count} * (u64{p.input_capacity} + p.output_capacity);
    const u64 ring_size = 0x80 + (u64{p.io_count} + 1) * 0x40;
    R_UNLESS(p.input_capacity > 0 && p.input_capacity <= 1024 * 1024 && p.output_capacity > 0 &&
                 p.output_capacity <= 4 * 1024 * 1024 &&
                 fits(p.io_data_offset, p.io_data_size, p.shared_memory_size) &&
                 io_size <= p.io_data_size &&
                 fits(p.completion_ring_offset, p.completion_ring_size, p.shared_memory_size) &&
                 ring_size <= p.completion_ring_size,
             ResultInvalidAddressInfo);
    R_UNLESS(process->GetMemory().IsValidVirtualAddressRange(shared_address, shared_size),
             ResultInvalidAddressInfo);
    auto manager = std::make_shared<INx2HardwareAudioCodecManager>(system, process.Get(), p,
                                                                   shared_address, next_manager_id);
    *out_id = next_manager_id++;
    *out_event = &manager->GetCompletionEvent();
    *out_manager = std::move(manager);
    LOG_INFO(Service_Audio,
             "hwacod manager {} instances={} buffers={} input={:#x} output={:#x} "
             "shared={:#x}+{:#x} ring={:#x} properties={:#x}",
             *out_id, p.instance_count, p.io_count, p.input_capacity, p.output_capacity,
             shared_address, shared_size, p.completion_ring_offset, properties);
    R_SUCCEED();
}

} // namespace Service::Audio

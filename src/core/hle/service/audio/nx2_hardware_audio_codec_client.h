// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/audio/nx2_hardware_audio_codec_types.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Kernel {
class KProcess;
class KReadableEvent;
class KTransferMemory;
} // namespace Kernel

namespace Service::Audio {

class INx2HardwareAudioCodecManager;

class INx2HardwareAudioCodecClient final : public ServiceFramework<INx2HardwareAudioCodecClient> {
public:
    explicit INx2HardwareAudioCodecClient(Core::System& system);

private:
    Result GetInstanceWorkBufferSize(
        Out<u32> out_size,
        InLargeData<Nx2HardwareAudioCodecParameter, BufferAttr_HipcPointer> parameter);
    Result GetManagerWorkBufferSize(Out<u32> out_size, u32 io_count);
    Result OpenManager(Out<u32> out_id,
                       Out<SharedPointer<INx2HardwareAudioCodecManager>> out_manager,
                       OutCopyHandle<Kernel::KReadableEvent> out_event,
                       Nx2HardwareAudioCodecSharedMemory parameter,
                       InCopyHandle<Kernel::KTransferMemory> transfer_memory,
                       InCopyHandle<Kernel::KProcess> process, u64 transfer_size,
                       u64 shared_address, u64 shared_size, u64 properties);

    u32 next_manager_id{1};
};

} // namespace Service::Audio

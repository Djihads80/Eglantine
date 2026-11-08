// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <opus.h>
#include "audio_core/opus/decoder_manager.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/service/audio/errors.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/memory.h"

namespace Service::Audio {
using HwacodParameter = std::array<u32, 72>;
using HwacodSharedParameter = std::array<u32, 24>;

class IHwacodManager final : public ServiceFramework<IHwacodManager> {
public:
    IHwacodManager(Core::System& system_, HwacodSharedParameter layout_,
                   Kernel::KTransferMemory* transfer_, u64 io_, u64 io_size_)
        : ServiceFramework{system_, "IHwacodManager"}, layout{layout_}, transfer{transfer_},
          memory{transfer_->GetOwner()->GetMemory()}, io{io_}, io_size{io_size_},
          context{system_, "IHwacodManager"} {
        transfer->Open(system.Kernel());
        event = context.CreateEvent("HwacodCompletion");
        static const FunctionInfo functions[] = {
            {0, &IHwacodManager::InitializeInstance, "InitializeInstance"},
            {1, &IHwacodManager::FinalizeInstance, "FinalizeInstance"},
            {10, &IHwacodManager::Execute, "Execute"},
        };
        RegisterHandlers(functions);
    }
    ~IHwacodManager() override {
        for (auto& [id, state] : instances)
            opus_decoder_destroy(state.decoder);
        context.CloseEvent(event);
        transfer->Close(system.Kernel());
    }
    Kernel::KReadableEvent& CompletionEvent() {
        return event->GetReadableEvent();
    }

private:
    struct Instance {
        ::OpusDecoder* decoder;
        u32 rate;
        u32 channels;
    };
    void Reply(HLERequestContext& ctx, Result result) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result);
    }
    void InitializeInstance(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 id = rp.Pop<u32>();
        HwacodParameter p{};
        const auto bytes = ctx.ReadBuffer();
        if (bytes.size() != sizeof(p)) {
            Reply(ctx, ResultInvalidArgument);
            return;
        }
        std::memcpy(p.data(), bytes.data(), sizeof(p));
        LOG_INFO(Service_Audio, "hwacod initialize id={} kind={} rate={} channels={}", id, p[0],
                 p[2], p[3]);
        if (id >= layout[14] || p[0] != 1 || p[1] != 0) {
            Reply(ctx, ResultNotSupported);
            return;
        }
        int error{};
        auto* decoder = opus_decoder_create(static_cast<int>(p[2]), static_cast<int>(p[3]), &error);
        if (!decoder || error != OPUS_OK) {
            Reply(ctx, ResultLibOpusBadArg);
            return;
        }
        if (auto it = instances.find(id); it != instances.end())
            opus_decoder_destroy(it->second.decoder);
        instances[id] = {decoder, p[2], p[3]};
        Reply(ctx, ResultSuccess);
    }
    void FinalizeInstance(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto id = rp.Pop<u32>();
        if (auto it = instances.find(id); it != instances.end()) {
            opus_decoder_destroy(it->second.decoder);
            instances.erase(it);
        }
        Reply(ctx, ResultSuccess);
    }
    void Execute(HLERequestContext& ctx) {
        const auto tasks = ctx.ReadBuffer();
        const u32 capacity = layout[17] + 1;
        const u64 ring_offset = layout[11];
        const u64 data_offset = layout[9];
        const u32 input_stride = layout[15], output_stride = layout[16];
        if (tasks.size() % 16 || capacity < 2 || capacity > 4096 || ring_offset > io_size ||
            128ULL + capacity * 64ULL > io_size - ring_offset) {
            Reply(ctx, ResultInvalidArgument);
            return;
        }
        const u64 ring = io + ring_offset;
        u32 write = memory.Read32(ring + 4);
        const u32 read = memory.Read32(ring + 0x44);
        if (write >= capacity || read >= capacity ||
            tasks.size() / 16 > (read + capacity - write - 1) % capacity) {
            Reply(ctx, ResultBufferCountReached);
            return;
        }
        for (size_t offset = 0; offset < tasks.size(); offset += 16) {
            u64 header{};
            u32 length{};
            std::memcpy(&header, tasks.data() + offset, 8);
            std::memcpy(&length, tasks.data() + offset + 8, 4);
            const u32 id = (header >> 16) & 0xffff, slot = (header >> 32) & 0xffff;
            Result result = ResultSuccess;
            u32 output_bytes{};
            const auto it = instances.find(id);
            const u64 slot_offset = data_offset + u64{slot} * (input_stride + u64{output_stride});
            if (it == instances.end() || slot >= layout[17] || length > input_stride ||
                slot_offset > io_size ||
                input_stride + u64{output_stride} > io_size - slot_offset) {
                result = ResultInvalidArgument;
            } else {
                auto& state = it->second;
                const u32 max_frames = std::min(5760U, output_stride / (2 * state.channels));
                if (!max_frames || length > 0x10000) {
                    result = ResultInvalidArgument;
                } else {
                    std::vector<u8> packet(length);
                    memory.ReadBlock(io + slot_offset, packet.data(), packet.size());
                    std::vector<s16> pcm(max_frames * state.channels);
                    // Hardware Opus packets carry an eight-byte big-endian size/range header.
                    const u8* data = packet.data();
                    u32 count = length;
                    if (length >= 8) {
                        const u32 framed = (u32{data[0]} << 24) | (u32{data[1]} << 16) |
                                           (u32{data[2]} << 8) | data[3];
                        if (framed == length - 8) {
                            data += 8;
                            count -= 8;
                        }
                    }
                    if (tasks[offset + 12])
                        opus_decoder_ctl(state.decoder, OPUS_RESET_STATE);
                    const int frames = opus_decode(state.decoder, data, static_cast<int>(count),
                                                   pcm.data(), static_cast<int>(max_frames), 0);
                    if (frames < 0)
                        result = ResultLibOpusInvalidPacket;
                    else {
                        output_bytes = static_cast<u32>(frames) * state.channels * 2;
                        memory.WriteBlock(io + slot_offset + input_stride, pcm.data(),
                                          output_bytes);
                    }
                }
            }
            if (result != ResultSuccess) {
                LOG_ERROR(Service_Audio,
                          "hwacod task failed instance={} slot={} input_bytes={} result={:#x}", id,
                          slot, length, result.raw);
            }
            std::array<u8, 64> completion{};
            std::memcpy(completion.data(), &header, 8);
            std::memcpy(completion.data() + 16, &output_bytes, 4);
            const u32 raw = result.raw;
            std::memcpy(completion.data() + 20, &raw, 4);
            memory.WriteBlock(ring + 128 + u64{write} * 64, completion.data(), completion.size());
            write = (write + 1) % capacity;
        }
        memory.Write32(ring + 4, write);
        event->Signal(system.Kernel());
        Reply(ctx, ResultSuccess);
    }
    HwacodSharedParameter layout;
    Kernel::KTransferMemory* transfer;
    Core::Memory::Memory& memory;
    u64 io, io_size;
    KernelHelpers::ServiceContext context;
    Kernel::KEvent* event{};
    std::map<u32, Instance> instances;
};

class IHwacodClient final : public ServiceFramework<IHwacodClient> {
public:
    explicit IHwacodClient(Core::System& system_)
        : ServiceFramework{system_, "IHwacodClient"}, impl{system_} {
        static const FunctionInfo functions[] = {
            {0, &IHwacodClient::InstanceSize, "GetInstanceSize"},
            {1, &IHwacodClient::TaskSize, "GetTaskBufferSize"},
            {10, &IHwacodClient::OpenManager, "OpenManager"},
        };
        RegisterHandlers(functions);
    }

private:
    void InstanceSize(HLERequestContext& ctx) {
        HwacodParameter p{};
        const auto bytes = ctx.ReadBuffer();
        if (bytes.size() != sizeof(p)) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultInvalidArgument);
            return;
        }
        std::memcpy(p.data(), bytes.data(), sizeof(p));
        u32 size{};
        Result result = ResultNotSupported;
        if (p[0] == 1 && !p[1]) {
            AudioCore::OpusDecoder::OpusParametersEx params{};
            params.sample_rate = p[2];
            params.channel_count = p[3];
            params.use_large_frame_size = true;
            result = impl.GetWorkBufferSizeEx(params, size);
        }
        LOG_INFO(Service_Audio, "hwacod instance size kind={} rate={} channels={} size={}", p[0],
                 p[2], p[3], size);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(result);
        rb.Push(size);
    }
    void TaskSize(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 count = rp.Pop<u32>();
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(count > 0 && count < 4096 ? ResultSuccess : ResultInvalidArgument);
        rb.Push(count > 0 && count < 4096 ? 128 + (count + 1) * 16 : 0U);
    }
    void OpenManager(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto layout = rp.PopRaw<HwacodSharedParameter>();
        const u64 transfer_size = rp.Pop<u64>(), io = rp.Pop<u64>(), io_size = rp.Pop<u64>();
        const u32 flags = rp.Pop<u32>();
        auto transfer = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(0));
        LOG_INFO(Service_Audio,
                 "hwacod open transfer={:#x} io={:#x} size={:#x} flags={} instances={} tasks={} "
                 "ring={:#x} input={} output={}",
                 transfer_size, io, io_size, flags, layout[14], layout[17], layout[11], layout[15],
                 layout[16]);
        if (transfer.IsNull() || transfer_size > transfer->GetSize() || layout[0] != 1 ||
            !layout[14] || layout[14] > 4096 || !layout[17] || layout[17] >= 4096 || !io ||
            io_size == 0 || io_size > 0x10000000 || io + io_size < io || layout[12] > io_size) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultInvalidArgument);
            return;
        }
        auto& memory = transfer->GetOwner()->GetMemory();
        for (u64 offset = 0; offset < io_size; offset += 0x1000) {
            if (!memory.IsValidVirtualAddress(io + offset)) {
                IPC::ResponseBuilder rb{ctx, 2};
                rb.Push(ResultInvalidAddressInfo);
                return;
            }
        }
        if (!memory.IsValidVirtualAddress(io + io_size - 1)) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultInvalidAddressInfo);
            return;
        }
        auto manager = std::make_shared<IHwacodManager>(system, layout, transfer.GetPointerUnsafe(),
                                                        io, io_size);
        IPC::ResponseBuilder rb{ctx, 3, 1, 1};
        rb.Push(ResultSuccess);
        rb.Push<u32>(0);
        rb.PushCopyObjects(ctx, manager->CompletionEvent());
        rb.PushIpcInterface(ctx, manager);
    }
    AudioCore::OpusDecoder::OpusDecoderManager impl;
};
} // namespace Service::Audio

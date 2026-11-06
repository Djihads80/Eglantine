// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include <fmt/ranges.h>

#include "common/string_util.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"
#include "core/memory.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Service::Audio {

// Switch 2 sound mixer streams (nn::audio::sysmix, behind audsmx:u). A stream is a ring buffer in
// the game's own memory: CreateStream hands over its address and size (4 KiB aligned), OpenStream
// its format and length in frames, and once the streams are engaged the system takes frames out of
// the ring at the stream's sample rate. Eden doesn't mix these into its output yet; it drains the
// rings in real time so the game sees its audio being played (Switch Sports' menu movie feeds one).
// The ring's shared indices, in frames modulo (frame count + 1): the system's read index at
// buffer + 0x44, the game's write index at buffer + 0x1004.
struct Nx2SysmixStream {
    u32 id{};
    u32 owner{}; ///< the user session whose timer drains it
    u64 buffer{};
    u64 size{};
    u32 sample_type{};
    u32 channels{};
    u32 sample_rate{};
    u32 frames{};
    bool open{};
    bool engaged{};
    double owed_frames{};
    u32 data_mode{};      ///< command 7's mode; 0 leaves the data event alone
    s32 data_threshold{}; ///< free frames that signal the data event
    Kernel::KEvent* state_event{};
    Kernel::KEvent* data_event{};
};

struct Nx2SysmixStreams {
    std::mutex mutex;
    std::map<u32, std::shared_ptr<Nx2SysmixStream>> streams;
    u32 next_id{1};
};

// Streams are created through audsmx:u but started and stopped through auddmg:u (AttachStream and
// DetachStream), so both services share one list.
inline std::shared_ptr<Nx2SysmixStreams> Nx2SysmixStreamList() {
    static const auto list = std::make_shared<Nx2SysmixStreams>();
    return list;
}

// Switch 2 virtual output devices (nn::audio::ListVirtualOutputDevices and friends), reached through
// the object auddmg:u command 0 returns. Eden plays everything through one stereo 48 kHz output, so
// there is one virtual device. The nx2: name keeps the other commands auto-stubbed and logged.
// - 0(u32): u32 count, the device names in the out buffer (0x100 bytes each).
// - 100(u32): the default device's name in the out buffer.
// - 300(16-byte stream format, name buffer): whether the format is supported (result only).
// - 401(u32, name buffer): the device's 0x228-byte property block in the out buffer. The sdk hands
//   the game the name, the channel count (0x108) and the u64 at 0x218; with the zeroed placeholder
//   reply it saw 0 channels and Switch Sports failed to set up audio (2153-1604).
// - 500(16 bytes, name buffer): an event signalled when the device's state changes.
// - 700(u32 stream id, u32 device functionality, name buffer): attach a sysmix stream to the
//   device, which starts it (nn::audio::devmgr::AttachStream, behind OutputStream Start).
// - 800(u32 stream id): detach it again (Stop).
class INx2AudioVirtualDeviceManager final : public ServiceFramework<INx2AudioVirtualDeviceManager> {
public:
    explicit INx2AudioVirtualDeviceManager(Core::System& system_)
        : ServiceFramework{system_, "nx2:auddmg:u:manager"},
          service_context{system_, "nx2:auddmg:u:manager"},
          state_event{service_context.CreateEvent("nx2:auddmg:u:StateEvent")} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INx2AudioVirtualDeviceManager::ListVirtualOutputDevices, "ListVirtualOutputDevices"},
            {100, &INx2AudioVirtualDeviceManager::GetDefaultVirtualOutputDevice, "GetDefaultVirtualOutputDevice"},
            {300, &INx2AudioVirtualDeviceManager::IsStreamFormatSupported, "IsVirtualOutputDeviceStreamFormatSupported"},
            {401, &INx2AudioVirtualDeviceManager::GetVirtualOutputDeviceProperties, "GetVirtualOutputDeviceProperties"},
            {500, &INx2AudioVirtualDeviceManager::AcquireStateChangeEvent, "AcquireVirtualOutputDeviceStateEvent (guessed)"},
            {700, &INx2AudioVirtualDeviceManager::AttachStream, "AttachStream"},
            {800, &INx2AudioVirtualDeviceManager::DetachStream, "DetachStream"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

    ~INx2AudioVirtualDeviceManager() override {
        service_context.CloseEvent(state_event);
    }

private:
    static constexpr std::size_t NameSize = 0x100;
    static constexpr std::string_view DeviceName = "AudioVirtualOutput0";
    static constexpr std::string_view PhysicalDeviceName = "AudioTvOutput";

    static void CopyName(u8* out, std::string_view name) {
        std::memcpy(out, name.data(), std::min(name.size(), NameSize - 1));
    }

    void ListVirtualOutputDevices(HLERequestContext& ctx) {
        std::array<u8, NameSize> names{};
        CopyName(names.data(), DeviceName);
        const u32 count = ctx.GetWriteBufferSize() >= NameSize ? 1 : 0;
        if (count) {
            ctx.WriteBuffer(names);
        }
        LOG_DEBUG(Service_Audio, "called, {} device", count);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(count);
    }

    void GetDefaultVirtualOutputDevice(HLERequestContext& ctx) {
        std::array<u8, NameSize> name{};
        CopyName(name.data(), DeviceName);
        ctx.WriteBuffer(name);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void IsStreamFormatSupported(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Audio, "called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Property block: name (0x000), u32 type (0x100, 3), u32 (0x104, 2), u32 channel count
    // (0x108), u32 sample rate (0x10c), physical output name (0x110), its id (0x210), u64 latency
    // in ns (0x218), u32 device id (0x220). The sdk's output streams pass the device id to
    // sysmix CreateStream, which refuses 0 (Switch Sports failed with 2153-1604 on a zero there).
    void GetVirtualOutputDeviceProperties(HLERequestContext& ctx) {
        std::array<u8, 0x228> block{};
        const auto name = ctx.ReadBuffer(0);
        std::memcpy(block.data(), name.data(), std::min(name.size(), NameSize - 1));
        const auto put32 = [&](std::size_t offset, u32 value) {
            std::memcpy(block.data() + offset, &value, sizeof(value));
        };
        put32(0x100, 3);
        put32(0x104, 2);
        put32(0x108, 2);     // stereo
        put32(0x10c, 48000); // Hz
        CopyName(block.data() + 0x110, PhysicalDeviceName);
        const u64 latency_ns = 10'000'000;
        std::memcpy(block.data() + 0x218, &latency_ns, sizeof(latency_ns));
        put32(0x220, 1);
        ctx.WriteBuffer(block);
        LOG_DEBUG(Service_Audio, "called for {}", Common::StringFromBuffer(name));
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void AcquireStateChangeEvent(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, state_event->GetReadableEvent());
    }

    void AttachStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto stream_id = rp.Pop<u32>();
        const auto functionality = rp.Pop<u32>();
        LOG_INFO(Service_Audio, "attach stream {} to {} (functionality {})", stream_id,
                 Common::StringFromBuffer(ctx.ReadBuffer(0)), functionality);
        SetStreamEngaged(stream_id, true);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void DetachStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto stream_id = rp.Pop<u32>();
        LOG_INFO(Service_Audio, "detach stream {}", stream_id);
        SetStreamEngaged(stream_id, false);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetStreamEngaged(u32 stream_id, bool engaged) {
        const auto list = Nx2SysmixStreamList();
        std::scoped_lock lk{list->mutex};
        if (const auto it = list->streams.find(stream_id); it != list->streams.end()) {
            it->second->engaged = engaged;
            it->second->owed_frames = 0;
            if (it->second->state_event) {
                it->second->state_event->Signal(system.Kernel());
            }
        }
    }

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* state_event;
};

// Switch 2 "auddmg:u": command 0 (16 bytes and the pid) returns the virtual device manager.
class INx2AudioDeviceManagerService final : public ServiceFramework<INx2AudioDeviceManagerService> {
public:
    explicit INx2AudioDeviceManagerService(Core::System& system_)
        : ServiceFramework{system_, "nx2:auddmg:u"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INx2AudioDeviceManagerService::OpenVirtualDeviceManager, "OpenVirtualDeviceManager (guessed)"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    void OpenVirtualDeviceManager(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Audio, "called");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<INx2AudioVirtualDeviceManager>(ctx, system);
    }
};

// The stream object IUserSession command 1 returns. Commands (ids, request -> reply):
// - 0(u32 sample type, u32 channels, u32 sample rate, u32 frames, u32 prefill, u32 flags): open.
// - 1: close. 2: u32 state. 3: f32 gain. 4(f32 gain, s64 ramp ns): set the gain.
// - 5 / 6: acquire (copy handle) / release the state-change event.
// - 7(u32 mode, s32 threshold) / 8: acquire (copy handle) / release the data event.
// - 9(s64 ns): clock the stream. 10(s32 frames) -> u32: prime. 11(s32 frames) -> u32: purge.
// - 12(name buffer): rename.
class INx2SysmixStreamSession final : public ServiceFramework<INx2SysmixStreamSession> {
public:
    explicit INx2SysmixStreamSession(Core::System& system_, std::shared_ptr<Nx2SysmixStreams> streams_,
                                     std::shared_ptr<Nx2SysmixStream> stream_)
        : ServiceFramework{system_, "nx2:audsmx:u:stream"}, service_context{system_, "nx2:audsmx:u:stream"},
          streams{std::move(streams_)}, stream{std::move(stream_)} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INx2SysmixStreamSession::OpenStream, "OpenStream"},
            {1, &INx2SysmixStreamSession::CloseStream, "CloseStream"},
            {2, &INx2SysmixStreamSession::GetStreamState, "GetStreamState"},
            {3, &INx2SysmixStreamSession::GetStreamGain, "GetStreamGain"},
            {4, &INx2SysmixStreamSession::SetStreamGain, "SetStreamGain"},
            {5, &INx2SysmixStreamSession::AcquireStateChangeEvent, "AcquireStreamStateChangeEvent"},
            {6, &INx2SysmixStreamSession::Succeed, "ReleaseStreamStateChangeEvent"},
            {7, &INx2SysmixStreamSession::AcquireDataEvent, "AcquireStreamDataEvent"},
            {8, &INx2SysmixStreamSession::Succeed, "ReleaseStreamDataEvent"},
            {9, &INx2SysmixStreamSession::Succeed, "ClockStream"},
            {10, &INx2SysmixStreamSession::EchoFrames, "PrimeStream"},
            {11, &INx2SysmixStreamSession::EchoFrames, "PurgeStream"},
            {12, &INx2SysmixStreamSession::Succeed, "RenameStream"},
        };
        // clang-format on
        RegisterHandlers(functions);
        std::scoped_lock lk{streams->mutex};
        stream->state_event = service_context.CreateEvent("nx2:audsmx:u:StreamStateEvent");
        stream->data_event = service_context.CreateEvent("nx2:audsmx:u:StreamDataEvent");
    }

    ~INx2SysmixStreamSession() override {
        // The drain timer signals these events under the lock: unlist the stream first.
        {
            std::scoped_lock lk{streams->mutex};
            streams->streams.erase(stream->id);
            service_context.CloseEvent(stream->state_event);
            service_context.CloseEvent(stream->data_event);
            stream->state_event = nullptr;
            stream->data_event = nullptr;
        }
    }

private:
    // States the session reports: 0 created, 1 open, 2 open and engaged.
    void OpenStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto sample_type = rp.Pop<u32>();
        const auto channels = rp.Pop<u32>();
        const auto sample_rate = rp.Pop<u32>();
        const auto frames = rp.Pop<u32>();
        const auto prefill = rp.Pop<u32>();
        const auto flags = rp.Pop<u32>();
        LOG_INFO(Service_Audio,
                 "stream {} open: sample type {}, {} channels, {} Hz, {} frames, prefill {}, flags {:#x}",
                 stream->id, sample_type, channels, sample_rate, frames, prefill, flags);
        {
            std::scoped_lock lk{streams->mutex};
            stream->sample_type = sample_type;
            stream->channels = channels;
            stream->sample_rate = sample_rate;
            stream->frames = frames;
            stream->owed_frames = 0;
            stream->open = true;
            stream->state_event->Signal(system.Kernel());
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CloseStream(HLERequestContext& ctx) {
        {
            std::scoped_lock lk{streams->mutex};
            stream->open = false;
            stream->state_event->Signal(system.Kernel());
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetStreamState(HLERequestContext& ctx) {
        u32 state{};
        {
            std::scoped_lock lk{streams->mutex};
            state = stream->open ? (stream->engaged ? 2 : 1) : 0;
        }
        LOG_DEBUG(Service_Audio, "stream {} state {}", stream->id, state);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(state);
    }

    void GetStreamGain(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(gain);
    }

    void SetStreamGain(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        gain = rp.Pop<f32>();
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void AcquireStateChangeEvent(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, stream->state_event->GetReadableEvent());
    }

    void AcquireDataEvent(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto mode = rp.Pop<u32>();
        const auto threshold = rp.Pop<s32>();
        LOG_INFO(Service_Audio, "stream {} data event: mode {}, threshold {}", stream->id, mode, threshold);
        {
            // The game waits on this before its first write, so signal it at once when the ring
            // already has room (it is empty right after OpenStream); the drain timer keeps it up.
            std::scoped_lock lk{streams->mutex};
            stream->data_mode = mode;
            stream->data_threshold = threshold;
            if (mode != 0 && stream->open && static_cast<s64>(stream->frames) >= threshold) {
                stream->data_event->Signal(system.Kernel());
            }
        }
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, stream->data_event->GetReadableEvent());
    }

    void EchoFrames(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto frames = rp.Pop<s32>();
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(static_cast<u32>((std::max)(frames, 0)));
    }

    void Succeed(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    KernelHelpers::ServiceContext service_context;
    std::shared_ptr<Nx2SysmixStreams> streams;
    std::shared_ptr<Nx2SysmixStream> stream;
    f32 gain{1.0f};
};

// The session audsmx:u command 0 returns. Commands:
// - 1(u32, u32 creation flags, u64 pid, u64 buffer address, u64 buffer size, name buffer):
//   u32 stream id and the stream object.
// - 2 / 3(buffer of u32 stream ids): engage / disengage those streams.
class INx2SysmixUserSession final : public ServiceFramework<INx2SysmixUserSession> {
public:
    explicit INx2SysmixUserSession(Core::System& system_) : ServiceFramework{system_, "nx2:audsmx:u:session"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {1, &INx2SysmixUserSession::CreateStream, "CreateStream"},
            {2, &INx2SysmixUserSession::EngageStreams, "EngageStreams"},
            {3, &INx2SysmixUserSession::DisengageStreams, "DisengageStreams"},
        };
        // clang-format on
        RegisterHandlers(functions);
        drain_timer = Core::Timing::CreateEvent(
            "nx2:audsmx:u drain",
            [streams = streams, &system = system, owner = session_id](s64, std::chrono::nanoseconds)
                -> std::optional<std::chrono::nanoseconds> {
                Drain(system, *streams, owner);
                return std::nullopt;
            });
        system.CoreTiming().ScheduleLoopingEvent(DrainInterval, DrainInterval, drain_timer);
    }

    ~INx2SysmixUserSession() override {
        system.CoreTiming().UnscheduleEvent(drain_timer);
    }

private:
    static constexpr std::chrono::nanoseconds DrainInterval{std::chrono::milliseconds{5}};
    static constexpr u64 ReadIndexOffset = 0x44;
    static constexpr u64 WriteIndexOffset = 0x1004;

    // Takes up to one interval's worth of frames out of each engaged ring (frames owed while a ring
    // is empty are dropped, as an underrun plays silence), and signals the data event of every open
    // stream with at least its threshold of free frames, engaged or not: the game fills the ring
    // before it engages the stream.
    static void Drain(Core::System& system, Nx2SysmixStreams& state, u32 owner) {
        std::scoped_lock lk{state.mutex};
        auto& memory = system.ApplicationMemory();
        for (auto& [id, stream] : state.streams) {
            if (stream->owner != owner || !stream->open || stream->frames == 0 || !stream->buffer) {
                continue;
            }
            const u32 capacity = stream->frames + 1;
            const u32 write = memory.Read32(stream->buffer + WriteIndexOffset);
            const u32 read = memory.Read32(stream->buffer + ReadIndexOffset);
            if (write >= capacity || read >= capacity) {
                continue;
            }
            u32 filled = (write + capacity - read) % capacity;
            if (stream->engaged) {
                stream->owed_frames += stream->sample_rate *
                                       std::chrono::duration<double>(DrainInterval).count();
                const auto owed = static_cast<u32>(stream->owed_frames);
                stream->owed_frames -= owed;
                const u32 taken = (std::min)(filled, owed);
                if (taken != 0) {
                    memory.Write32(stream->buffer + ReadIndexOffset, (read + taken) % capacity);
                    filled -= taken;
                }
            }
            const u32 free_frames = capacity - 1 - filled;
            if (stream->data_event && stream->data_mode != 0 &&
                static_cast<s64>(free_frames) >= stream->data_threshold) {
                stream->data_event->Signal(system.Kernel());
            }
        }
    }

    void CreateStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto kind = rp.Pop<u32>();
        const auto flags = rp.Pop<u32>();
        [[maybe_unused]] const auto pid = rp.Pop<u64>();
        const auto buffer = rp.Pop<u64>();
        const auto size = rp.Pop<u64>();
        const auto name = Common::StringFromBuffer(ctx.ReadBuffer(0));
        auto stream = std::make_shared<Nx2SysmixStream>();
        stream->buffer = buffer;
        stream->size = size;
        {
            std::scoped_lock lk{streams->mutex};
            stream->id = streams->next_id++;
            stream->owner = session_id;
            streams->streams.emplace(stream->id, stream);
        }
        LOG_INFO(Service_Audio, "stream {} '{}': kind {}, flags {:#x}, buffer {:#x} size {:#x}",
                 stream->id, name, kind, flags, buffer, size);
        IPC::ResponseBuilder rb{ctx, 3, 0, 1};
        rb.Push(ResultSuccess);
        rb.Push(stream->id);
        rb.PushIpcInterface<INx2SysmixStreamSession>(ctx, system, streams, stream);
    }

    void SetEngaged(HLERequestContext& ctx, bool engaged) {
        const auto buffer = ctx.ReadBuffer(0);
        std::vector<u32> ids(buffer.size() / sizeof(u32));
        std::memcpy(ids.data(), buffer.data(), ids.size() * sizeof(u32));
        {
            std::scoped_lock lk{streams->mutex};
            for (const u32 id : ids) {
                if (const auto it = streams->streams.find(id); it != streams->streams.end()) {
                    it->second->engaged = engaged;
                    it->second->owed_frames = 0;
                    if (it->second->state_event) {
                        it->second->state_event->Signal(system.Kernel());
                    }
                }
            }
        }
        LOG_INFO(Service_Audio, "{} streams {}", engaged ? "engage" : "disengage", fmt::join(ids, ","));
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void EngageStreams(HLERequestContext& ctx) {
        SetEngaged(ctx, true);
    }

    void DisengageStreams(HLERequestContext& ctx) {
        SetEngaged(ctx, false);
    }

    static inline std::atomic<u32> next_session_id{1};
    const u32 session_id = next_session_id++;
    std::shared_ptr<Nx2SysmixStreams> streams = Nx2SysmixStreamList();
    std::shared_ptr<Core::Timing::EventType> drain_timer;
};

// Switch 2 "audsmx:u": command 0 (16 bytes, the pid and a copy handle) returns the user session.
class INx2AudioSoundMixerService final : public ServiceFramework<INx2AudioSoundMixerService> {
public:
    explicit INx2AudioSoundMixerService(Core::System& system_)
        : ServiceFramework{system_, "nx2:audsmx:u"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INx2AudioSoundMixerService::OpenUserSession, "OpenUserSession"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    void OpenUserSession(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Audio, "called");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<INx2SysmixUserSession>(ctx, system);
    }
};

} // namespace Service::Audio

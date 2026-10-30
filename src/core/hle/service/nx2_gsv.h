// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <initializer_list>
#include <atomic>
#include <deque>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <vector>

#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/perf_stats.h"
#include "video_core/gpu.h"
#include "video_core/framebuffer_config.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_system_resource.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_page_group.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/memory.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/nx2_video_decoder.h"
#include "video_core/nx2.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nx2_stub_interface.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"

namespace Service {

// Creates a session served by `iface` and returns its client end. Unlike PushIpcInterface this
// works from a domain too, for callers that want a raw session handle.
template <class T>
inline Kernel::KClientSession* CreateRawSession(Core::System& system, HLERequestContext& ctx,
                                                std::shared_ptr<T> iface) {
    auto& kernel = system.Kernel();
    ASSERT(Kernel::GetCurrentProcess(kernel).GetResourceLimit()->Reserve(
        kernel, Kernel::LimitableResource::SessionCountMax, 1));

    auto* session = Kernel::KSession::Create(kernel);
    session->Initialize(kernel, nullptr, 0);
    Kernel::KSession::Register(kernel, session);

    auto manager =
        std::make_shared<SessionRequestManager>(kernel, ctx.GetManager()->GetServerManager());
    manager->SetSessionHandler(std::move(iface));
    ctx.GetManager()->GetServerManager().RegisterSession(&session->GetServerSession(), manager);
    return &session->GetClientSession();
}

// Replies to a TIPC request with success and raw u32 data words. ResponseBuilder::Push(Result)
// writes the CMIF two-word result, which shifts TIPC data one word past where the client reads
// it (and past the copied size); TIPC has a one-word result followed directly by the data.
inline void TipcReplyWords(HLERequestContext& ctx, std::span<const u32> words) {
    IPC::ResponseBuilder rb{ctx, static_cast<u32>(2 + words.size())};
    rb.PushRaw<u32>(ResultSuccess.raw);
    for (const u32 w : words) {
        rb.Push<u32>(w);
    }
}

inline void TipcReplyWords(HLERequestContext& ctx, std::initializer_list<u32> words) {
    TipcReplyWords(ctx, std::span<const u32>{words.begin(), words.size()});
}

// Replies with success and a copied handle to a new session served by `iface`.
template <class T>
inline void ReturnRawSession(Core::System& system, HLERequestContext& ctx, std::shared_ptr<T> iface) {
    auto* client = CreateRawSession(system, ctx, std::move(iface));
    IPC::ResponseBuilder rb{ctx, 2, 1, 0};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(ctx, *client);
}

// Placeholder for the Switch 2 NVIDIA driver server (nn::nvsrv). It has no handlers, so every
// command (CMIF or TIPC) gets an empty success and is logged through the nx2: prefix.
class INx2NvsrvSession final : public ServiceFramework<INx2NvsrvSession> {
public:
    explicit INx2NvsrvSession(Core::System& system_) : ServiceFramework{system_, "nx2:nvsrv"} {}
};

// Placeholder for the Switch 2 syncpoint server (nn::syncpt), handed out by gsv 202. TIPC 1
// returns a u32 the SDK requires to be < 0x400 and then passes min(value, 63) to TIPC 21.
// Everything else is auto-stubbed through the nx2: prefix.
// - TIPC 0(u64): handshake, echo the token (newer SDKs).
// - TIPC 22(u32 slot): an event handle for that slot (copy handle at word 3, result at word 4).
//   NVN's NvOsEventPool asks for slots and multi-waits on them.
// - TIPC 201(slot, counter, u64 value, mode): signal the slot's event once the counter reaches
//   value. The counter is a SyncCounterId, the bitwise NOT of the host1x syncpoint id; mode 0
//   compares 32-bit wrapping, mode 1 64-bit. Eden's guest syncpoints back it.
// - TIPC 202(slot): cancel the slot's request.
class INx2SyncptSession final : public ServiceFramework<INx2SyncptSession> {
public:
    explicit INx2SyncptSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:syncpt"}, m_context{system_, "nx2:syncpt"} {
        RegisterHandlersTipc({
            {0, &INx2SyncptSession::Handshake, "Handshake"},
            {1, &INx2SyncptSession::GetCount, "GetCount (guessed)"},
            {22, &INx2SyncptSession::GetSlotEvent, "GetSlotEvent (guessed)"},
            {103, &INx2SyncptSession::QueryCounter, "QueryCounter"},
            {201, &INx2SyncptSession::RequestSlotEvent, "RequestSlotEvent (guessed)"},
            {202, &INx2SyncptSession::CancelSlotEvent, "CancelSlotEvent (guessed)"},
        });
    }

    ~INx2SyncptSession() override {
        // A registered action may be running on the GPU thread right now: mark the session dead
        // under the lock it signals under, so it can't touch a request or event closed below.
        {
            std::scoped_lock lk{m_shared->mutex};
            m_shared->alive = false;
        }
        for (auto& [slot, request] : m_requests) {
            Cancel(request);
        }
        for (auto& [slot, event] : m_events) {
            m_context.CloseEvent(event);
        }
    }

private:
    // Newer SDKs (title 1) open with TIPC 0(u64 token) and abort unless the reply echoes it.
    void Handshake(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_INFO(Service, "nx2:syncpt TIPC 0 handshake {:#x}", u64{w[2]} | (u64{w[3]} << 32));
        TipcReplyWords(ctx, {w[2], w[3]});
    }

    void GetCount(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:syncpt TIPC 1, returning 32");
        TipcReplyWords(ctx, {32});
    }

    // From changes.patch: the counter is the bitwise NOT of the host1x syncpoint id.
    void QueryCounter(HLERequestContext& ctx) {
        const u32 syncpoint = ~ctx.CommandBuffer()[2];
        const u64 value = syncpoint < HostSyncpoints
                              ? system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(syncpoint)
                              : 0;
        LOG_DEBUG(Service, "nx2:syncpt 103 syncpoint={} value={:#x}", syncpoint, value);
        TipcReplyWords(ctx, {static_cast<u32>(value), static_cast<u32>(value >> 32), 0});
    }

    void GetSlotEvent(HLERequestContext& ctx) {
        const u32 slot = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:syncpt TIPC 22 slot={:#x}, returning an event", slot);
        auto*& event = m_events[slot];
        if (event == nullptr) {
            event = m_context.CreateEvent(fmt::format("nx2:syncpt:Slot{:x}", slot));
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    // Map nodes are stable, so a registered action can hold a reference to its request.
    struct SlotRequest {
        u32 syncpoint{};
        std::atomic<bool> pending{};
        Tegra::Host1x::SyncpointManager::ActionHandle handle{};
    };

    // Outlives the session (actions hold a reference): signalling happens under `mutex` and only
    // while `alive`.
    struct SharedState {
        std::mutex mutex;
        bool alive{true};
    };

    void RequestSlotEvent(HLERequestContext& ctx) {
        const auto* cmd_buf = ctx.CommandBuffer();
        const u32 slot = cmd_buf[2];
        const u32 syncpoint = ~cmd_buf[3];
        const u64 value = u64{cmd_buf[4]} | (u64{cmd_buf[5]} << 32);
        const u32 mode = cmd_buf[6];
        LOG_DEBUG(Service, "nx2:syncpt TIPC 201 slot={:#x} syncpoint={} value={:#x} mode={}", slot,
                  syncpoint, value, mode);
        auto*& event = m_events[slot];
        if (event == nullptr) {
            event = m_context.CreateEvent(fmt::format("nx2:syncpt:Slot{:x}", slot));
        }
        if (syncpoint >= HostSyncpoints) {
            LOG_WARNING(Service, "nx2:syncpt TIPC 201 syncpoint {} out of range, signalling",
                        syncpoint);
            event->Signal(system.Kernel());
            TipcReplyWords(ctx, {});
            return;
        }
        auto& request = m_requests[slot];
        Cancel(request);
        // A new request starts unsignalled. Slots are reused by different client objects: title 1's
        // movie player got slot 0 after NVN's fence waits had left it signalled, took that as a
        // decoded frame and aborted when the decoder had none.
        event->Clear(system.Kernel());
        request.syncpoint = syncpoint;
        request.pending = true;
        auto& manager = system.Host1x().GetSyncpointManager();
        // The action runs on the GPU thread under the manager's lock, or inline if the value is
        // already reached.
        request.handle = manager.RegisterGuestAction(
            syncpoint, static_cast<u32>(value),
            [shared = m_shared, &kernel = system.Kernel(), &request, event] {
                std::scoped_lock lk{shared->mutex};
                if (!shared->alive) {
                    return;
                }
                request.pending = false;
                event->Signal(kernel);
            });
        TipcReplyWords(ctx, {});
    }

    void CancelSlotEvent(HLERequestContext& ctx) {
        const u32 slot = ctx.CommandBuffer()[2];
        LOG_DEBUG(Service, "nx2:syncpt TIPC 202 slot={:#x}", slot);
        if (auto it = m_requests.find(slot); it != m_requests.end()) {
            Cancel(it->second);
        }
        // nn::syncpt::sdk's cancel always waits on the slot's event after this reply, so a cancel
        // signals it like a completion, also when nothing was pending; the client rechecks the
        // counter (TIPC 103) itself. Title 1 hung forever there when the GPU thread ran a frame
        // behind (EDEN_NX2_FRAMES_AHEAD) and a mid-frame wait outlasted NvOsEventPool's 100 ms,
        // and its movie player cancels each slot before its first request: signalled only for
        // pending requests, the player never got to start a movie.
        auto*& event = m_events[slot];
        if (event == nullptr) {
            event = m_context.CreateEvent(fmt::format("nx2:syncpt:Slot{:x}", slot));
        }
        event->Signal(system.Kernel());
        TipcReplyWords(ctx, {});
    }

    /// Returns true if the request was still pending.
    bool Cancel(SlotRequest& request) {
        if (request.pending.exchange(false)) {
            system.Host1x().GetSyncpointManager().DeregisterGuestAction(request.syncpoint,
                                                                        request.handle);
            return true;
        }
        return false;
    }

    static constexpr u32 HostSyncpoints = 192; // Tegra::Host1x::SyncpointManager

    KernelHelpers::ServiceContext m_context;
    std::map<u32, Kernel::KEvent*> m_events;
    std::map<u32, SlotRequest> m_requests;
    std::shared_ptr<SharedState> m_shared = std::make_shared<SharedState>();
};

// Placeholder for the Switch 2 GPU session (nn::gpu::sdk), handed out by gsv 497.
// - TIPC 11: [first, last] id range (first <= min(last, 64)); ids outside it are masked out.
// - TIPC 12(id): event handle for that id (handle at word 3, result at word 4); the SDK attaches
//   it as a SystemEvent.
// - TIPC 21(id): starts an operation; the SDK then waits on the id's event. Nothing runs here, so
//   the event is signalled at once.
// - TIPC 22: another handle the SDK keeps like a session; a placeholder session is returned.
class INx2GpuSession final : public ServiceFramework<INx2GpuSession> {
public:
    explicit INx2GpuSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:gpu"}, m_context{system_, "nx2:gpu"} {
        RegisterHandlersTipc({
            {11, &INx2GpuSession::GetIdRange, "GetIdRange (guessed)"},
            {12, &INx2GpuSession::GetIdEvent, "GetIdEvent (guessed)"},
            {21, &INx2GpuSession::Start, "Start (guessed)"},
            {22, &INx2GpuSession::OpenSubSession, "OpenSubSession (guessed)"},
            {33, &INx2GpuSession::GetCount33, "Unknown33 (u32 count)"},
            {201, &INx2GpuSession::GetUserPartition, "GetUserPartition (newer SDKs)"},
            {202, &INx2GpuSession::GetGsvPartition, "GetGsvPartition (newer SDKs)"},
            {203, &INx2GpuSession::GetVaRange, "GetVaRange (guessed)"},
            // Sub-session commands (gpu 22 hands out a second session to this same object).
            {101, &INx2GpuSession::SubRegisterMemory, "CreateGpuAccessibleMemory"},
            {102, &INx2GpuSession::SubDestroyMemory, "DestroyGpuAccessibleMemory (guessed)"},
            {211, &INx2GpuSession::SubSubmitJob, "NormalMapGpuMemory4K"},
            {212, &INx2GpuSession::SubModifyKind, "NormalModifyMemoryKind4K"},
            {213, &INx2GpuSession::SubUnmapJob, "NormalUnmapGpuMemory4K"},
            {214, &INx2GpuSession::SubCreateGpuOnlyRegion, "CreateSparseRegion"},
            {215, &INx2GpuSession::SubDestroySparseRegion, "DestroySparseRegion"},
        });
        if (LazySparseRegions()) {
            Tegra::Nx2SetSparseBacking(
                [this](u64 gpu_address, u64 size) { BackSparseWrite(gpu_address, size); });
        }
    }

    ~INx2GpuSession() override {
        // Waits for a backing the GPU thread may be doing.
        Tegra::Nx2SetSparseBacking({});
        for (auto* event : m_events) {
            if (event != nullptr) {
                m_context.CloseEvent(event);
            }
        }
    }

private:
    static constexpr u32 NumIds = 32;

    Kernel::KEvent* EventFor(u32 id) {
        id %= NumIds;
        if (m_events[id] == nullptr) {
            m_events[id] = m_context.CreateEvent(fmt::format("nx2:gpu:Event{}", id));
        }
        return m_events[id];
    }

    void GetIdRange(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 11, returning [0, {}]", NumIds - 1);
        TipcReplyWords(ctx, {0, NumIds - 1});
    }

    void GetIdEvent(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 12 id={}", id);
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, EventFor(id)->GetReadableEvent());
    }

    void Start(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 21 id={}, signalling completion", id);
        EventFor(id)->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetCount33(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 33 [2]={:#x}, returning 1", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {1});
    }

    // Used by nn::gpu::sdk::CreateGpuAddressSpace: three u64s after the result, a kind (< 5, 0 =
    // none) and the GPU VA range of the in-process VirtualMachine that serves AllocSpace
    // (nn::gpu::sdk::util::ExecuteVirtualMachineAllocSpace). NVN reserves fixed VA from 0x8000000
    // up to 512 GiB, so report the same range NvRm gives AllocAsEx: [0x8000000, 0xC000000000).
    void GetVaRange(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 203 [2]={:#x}", ctx.CommandBuffer()[2]);
        constexpr u64 kind = 1;
        constexpr u64 start = 0x8000000ULL;
        constexpr u64 size = 0xC000000000ULL - start;
        TipcReplyWords(ctx, {static_cast<u32>(kind), static_cast<u32>(kind >> 32),
                             static_cast<u32>(start), static_cast<u32>(start >> 32),
                             static_cast<u32>(size), static_cast<u32>(size >> 32)});
    }

    // Newer SDKs (title 1) query the address space's user and gsv partitions with 201/202(id):
    // two u64s each, stored as the partition's min/max. The user value works whether it is read
    // as (start, size) or (min, max). Title 4 never needed the gsv partition, so it stays empty.
    void GetUserPartition(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 201 [2]={:#x}", ctx.CommandBuffer()[2]);
        constexpr u64 first = 0x8000000ULL;
        constexpr u64 second = 0xC000000000ULL - first;
        TipcReplyWords(ctx, {static_cast<u32>(first), static_cast<u32>(first >> 32),
                             static_cast<u32>(second), static_cast<u32>(second >> 32)});
    }

    void GetGsvPartition(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 202 [2]={:#x}, empty", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {0, 0, 0, 0});
    }

    void OpenSubSession(HLERequestContext& ctx) {
        // Served by this object so sub-session jobs can signal the per-id events.
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 22, returning a sub-session on nx2:gpu");
        ReturnRawSession(system, ctx, std::static_pointer_cast<INx2GpuSession>(shared_from_this()));
    }

    // Sub 101: (start u64, last u64, flags...) after CreateGpuAccessibleMemory's svc 0x80.
    void SubRegisterMemory(HLERequestContext& ctx) {
        const auto* b = ctx.CommandBuffer();
        const u64 start = u64{b[2]} | (u64{b[3]} << 32);
        const u64 last = u64{b[4]} | (u64{b[5]} << 32);
        LOG_DEBUG(Service, "nx2:gpu sub 101 start={:#x} last={:#x} [6]={:#x} [7]={:#x}", start,
                    last, b[6], b[7]);
        if (last > start) {
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Register(start, last - start + 1);
            // Two-page regions hold queue semaphores: GPU releases go to the second page, waits
            // read the first (see video_core/nx2.h).
            if (last - start + 1 == 0x2000) {
                Tegra::Nx2AddSemaphoreRegion(start);
                LOG_DEBUG(Service, "nx2:gpu semaphore page {:#x} mirrored to {:#x}", start + 0x1000,
                         start);
            }
        }
        // nn::gpu::sdk::CreateGpuAccessibleMemory stores word 3 of the reply as the
        // GpuMemoryHandle that later map jobs (211) name.
        const u32 handle = m_next_memory_handle++;
        m_memory[handle] = {start, last - start + 1};
        TipcReplyWords(ctx, {handle});
    }

    // Jobs that complete through a job-slot event (word Slot of the request), like 211.
    template <size_t Slot>
    void SubSlotJob(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_DEBUG(Service, "(STUBBED) nx2:gpu {} words {:08x} slot={}", ctx.GetCommand(),
                 fmt::join(std::span<const u32>{w + 2, 18}, " "), w[Slot]);
        EventFor(w[Slot])->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 211: job submission (nn::gpu::sdk job tracking). Word 2 is a constant id; word 17 is
    // the job slot whose event (gpu TIPC 12) the SDK then waits on. Nothing runs here, so
    // complete it at once.
    void SubSubmitJob(HLERequestContext& ctx) {
        const auto* cmd_buf = ctx.CommandBuffer();
        const u32 slot = cmd_buf[17];
        const u64 a = u64{cmd_buf[3]} | (u64{cmd_buf[4]} << 32);
        const u64 b = u64{cmd_buf[5]} | (u64{cmd_buf[6]} << 32);
        const u64 c = u64{cmd_buf[8]} | (u64{cmd_buf[9]} << 32);
        // Words 3-4: first GPU address, 5-6: last, 7: memory handle, 8-9: offset into it.
        const u32 handle = cmd_buf[7];
        LOG_DEBUG(Service, "nx2:gpu 211 map [{:#x}, {:#x}] handle={} offset={:#x} slot={}", a, b,
                 handle, c, slot);
        if (const auto it = m_memory.find(handle); it != m_memory.end() && b > a) {
            // Also at the memory's own address: an earlier 213 may have unmapped the mapping 101
            // made there.
            const u64 cpu = it->second.address + c;
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().MapAt(a, cpu, b - a + 1);
        } else {
            LOG_WARNING(Service, "nx2:gpu 211 map of unknown handle {}", handle);
        }
        EventFor(slot)->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 212 (nn::gpu::sdk::NormalModifyMemoryKind4K, behind NvRmGpuMappingModify): word 2 the
    // address space, 3-4 first GPU address, 5-6 last, 7 the wanted (compressible) kind, 8 the
    // fallback kind, 9 a sync flag, 10 the job slot. The reply's word 3 is the kind applied. Kinds
    // do not matter to the emulated GPU, so keep the mapping and report the wanted kind.
    void SubModifyKind(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 first = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        const u32 kind = w[7] & 0xff;
        LOG_DEBUG(Service, "nx2:gpu 212 modify [{:#x}, {:#x}] kind={:#x} fallback={:#x} slot={}",
                  first, last, kind, w[8] & 0xff, w[10]);
        EventFor(w[10])->Signal(system.Kernel());
        TipcReplyWords(ctx, {kind});
    }

    // Sub 213: words 3-4 first GPU address, 5-6 last, job slot in word 8.
    void SubUnmapJob(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 first = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        LOG_DEBUG(Service, "nx2:gpu 213 unmap [{:#x}, {:#x}] slot={}", first, last, w[8]);
        if (last > first) {
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Unmap(first, last - first + 1);
        }
        SignalAfterGpu(w[8]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Signals a job slot's event once the GPU thread has run everything queued so far, as the
    // console's graphics server completes unmap jobs behind the submitted work.
    void SignalAfterGpu(u32 slot) {
        auto* const event = EventFor(slot);
        system.GPU().RunInOrder([this, event] { event->Signal(system.Kernel()); });
    }

    // Sub 102: word 2 is the memory handle 101 returned, word 3 the job slot. The game frees the
    // memory (svc 0x80 clears GPU access) and reuses it as ordinary heap right after, so nothing
    // on the GPU side may keep it: drop the mapping 101 made at its own address.
    void SubDestroyMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u32 handle = w[2];
        if (const auto it = m_memory.find(handle); it != m_memory.end()) {
            const auto [address, size] = it->second;
            LOG_DEBUG(Service, "nx2:gpu 102 destroy handle={} [{:#x}, +{:#x}] slot={}", handle,
                      address, size, w[3]);
            if (size != 0) {
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Unmap(address, size);
            }
            if (size == 0x2000) {
                Tegra::Nx2RemoveSemaphoreRegion(address);
            }
            m_memory.erase(it);
        } else {
            LOG_WARNING(Service, "nx2:gpu 102 destroy of unknown handle {}", handle);
        }
        SignalAfterGpu(w[3]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 214: GPU-only memory [start, last] (words 3-6, job slot in word 11). The graphics
    // server backs it itself: the client never maps anything there but renders into it (title 4
    // keeps its render targets in six 300 MB regions at 0x81_0000_0000). Back each region with
    // kernel memory placed in the process's alias code region and map that for the GPU.
    void SubCreateGpuOnlyRegion(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 start = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        const u32 slot = w[11];
        LOG_DEBUG(Service, "nx2:gpu 214 words {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}",
                 w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12]);
        if (last > start && ((last + 1 - start) % Core::Memory::YUZU_PAGESIZE) == 0) {
            const u64 size = last + 1 - start;
            std::scoped_lock lk{m_region_mutex};
            FreeReleasedRegions();
            if ((w[7] & SparseRegionFlag) != 0 && LazySparseRegions()) {
                // Title 1's terrain texture arrays and layer pools (170 MB-5.8 GB, mostly empty):
                // backed whole, they ran the pool dry and the later ones got no memory at all.
                // The region gets a device range of its own, unbacked pages read zero, and
                // BackSparseWrite gives a block memory before the GPU writes into it.
                m_sparse_regions[start] = {size, std::vector<u8>(Common::DivCeil(size, SparseBlockSize))};
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().CreateSparse(start, size);
                Tegra::Nx2SparseRegionCreated(start, size, SparseBlockSize);
                Tegra::Nx2AddSparseSpan(start, size);
                LOG_INFO(Service, "nx2:gpu 214 sparse region [{:#x}, {:#x}] backed as the GPU writes it",
                         start, last);
            } else if (const auto address = BackGpuOnlyRegion(start, size)) {
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().MapAt(start, *address, size);
                LOG_INFO(Service, "nx2:gpu 214 GPU-only region [{:#x}, {:#x}] backed at {:#x} "
                                  "({} MiB of GPU-only memory in use, peak {} MiB)",
                         start, last, *address, m_gpu_only_bytes >> 20, m_gpu_only_peak >> 20);
            }
        } else {
            LOG_WARNING(Service, "nx2:gpu 214 unexpected region [{:#x}, {:#x}]", start, last);
        }
        EventFor(slot)->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 215 (nn::gpu::sdk::DestroySparseRegion): words 3-6 the range, word 8 the job slot, like
    // 213. Drop the GPU mapping, then the backing 214 added: its SMMU mapping (in GPU-thread
    // order) and, once the GPU thread is past that, its pages. Kept allocated, title 1 ran the
    // pool dry within minutes and later regions had no memory at all (GPU writes to them lost).
    void SubDestroySparseRegion(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 first = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        LOG_DEBUG(Service, "nx2:gpu 215 destroy sparse region [{:#x}, {:#x}] words {:#x} {:#x} {:#x} {:#x}",
                 first, last, w[1], w[2], w[7], w[8]);
        std::scoped_lock lk{m_region_mutex};
        FreeReleasedRegions();
        if (last > first) {
            if (m_sparse_regions.erase(first) != 0) {
                Tegra::Nx2SparseRegionDestroyed(first);
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DestroySparse(first, last - first + 1);
            } else {
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Unmap(first, last - first + 1);
            }
            // The region's backing: all of it, or the blocks a sparse region got as it was written.
            std::shared_ptr<std::atomic<bool>> done;
            for (auto region = m_gpu_only_regions.lower_bound(first);
                 region != m_gpu_only_regions.end() && region->first <= last;) {
                if (region->first + region->second.size - 1 > last) {
                    ++region;
                    continue;
                }
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().ReleaseBacking(
                    region->second.cpu_address, region->second.size);
                if (!done) {
                    done = std::make_shared<std::atomic<bool>>(false);
                    system.GPU().RunInOrder([done] { done->store(true, std::memory_order_release); });
                }
                m_released_regions.push_back({std::move(region->second), done});
                region = m_gpu_only_regions.erase(region);
            }
        }
        SignalAfterGpu(w[8]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Returns the pages of destroyed regions the GPU thread is done with to the kernel.
    void FreeReleasedRegions() {
        auto* const process = system.ApplicationProcess();
        std::erase_if(m_released_regions, [&](ReleasedRegion& released) {
            if (!released.done->load(std::memory_order_acquire)) {
                return false;
            }
            auto& region = released.region;
            if (process != nullptr &&
                R_FAILED(process->GetPageTable().UnmapPageGroup(
                    region.cpu_address, *region.pages, Kernel::KMemoryState::IoMemory))) {
                LOG_ERROR(Service, "nx2:gpu could not unmap GPU-only backing {:#x}+{:#x}",
                          region.cpu_address, region.size);
                return true; // still mapped: keep its pages
            }
            region.pages->Close(system.Kernel());
            m_gpu_only_bytes -= region.size;
            return true;
        });
    }

    std::optional<u64> BackGpuOnlyRegion(u64 gpu_address, u64 size, bool log_failure = true,
                                         bool clear = false) {
        using Core::Memory::YUZU_PAGESIZE;
        auto& kernel_core = system.Kernel();
        auto* const process = system.ApplicationProcess();
        if (process == nullptr) {
            return std::nullopt;
        }
        auto pg = std::make_unique<Kernel::KPageGroup>(
            kernel_core, std::addressof(kernel_core.GetSystemSystemResource().GetBlockInfoManager()));
        // The graphics server's own memory, not the application's: taken from the application
        // pool, title 4's six regions left it 1.8 GB less than the kernel reports to it. Not
        // cleared: the GPU writes these before reading them, and touching 300 MB would commit it
        // on the host.
        if (R_FAILED(kernel_core.MemoryManager().AllocateAndOpen(
                pg.get(), size / YUZU_PAGESIZE,
                Kernel::KMemoryManager::EncodeOption(Kernel::KMemoryManager::Pool::SystemNonSecure,
                                                     Kernel::KMemoryManager::Direction::FromBack)))) {
            if (log_failure) {
                LOG_ERROR(Service, "nx2:gpu 214 could not allocate {:#x} bytes", size);
            }
            return std::nullopt;
        }
        if (clear) {
            // Cheap for memory that's still zero (most of it): only reused pages get cleared.
            for (const auto& block : *pg) {
                system.DeviceMemory().buffer.ClearBackingRegion(
                    GetInteger(block.GetAddress()) - Core::DramMemoryMap::Base, block.GetSize(), 0);
            }
        }
        auto& page_table = process->GetPageTable();
        const u64 region_start = GetInteger(page_table.GetAliasCodeRegionStart());
        const u64 region_size = page_table.GetAliasCodeRegionSize();
        constexpr u64 Alignment = 0x200000; // 2 MiB
        // From where the last backing went (sparse regions back hundreds of blocks), then wrapping.
        const u64 slots = region_size / Alignment;
        for (u64 i = 0; i < slots; ++i) {
            const u64 offset = ((m_alias_hint + i) % slots) * Alignment;
            if (offset + size > region_size) {
                continue;
            }
            const Common::ProcessAddress address{region_start + offset};
            if (R_SUCCEEDED(page_table.MapPageGroup(address, *pg, Kernel::KMemoryState::IoMemory,
                                                    Kernel::KMemoryPermission::UserReadWrite))) {
                m_alias_hint = (offset + Common::AlignUp(size, Alignment)) / Alignment;
                m_gpu_only_regions[gpu_address] = {GetInteger(address), size, std::move(pg)};
                m_gpu_only_bytes += size;
                m_gpu_only_peak = (std::max)(m_gpu_only_peak, m_gpu_only_bytes);
                return GetInteger(address);
            }
        }
        LOG_ERROR(Service, "nx2:gpu 214 no room for {:#x} bytes in the alias code region "
                           "[{:#x}, +{:#x})", size, region_start, region_size);
        pg->Close(kernel_core);
        return std::nullopt;
    }

    // Runs before a GPU-side write that may land in a sparse region (Tegra::Nx2BackSparseWrite,
    // usually on the GPU thread): the blocks the write touches get memory, cleared, behind the
    // region's device pages. The GPU mapping stays as it is, so the images over the region keep
    // their storage and only see the write.
    void BackSparseWrite(u64 gpu_address, u64 size) {
        std::scoped_lock lk{m_region_mutex};
        const u64 end = gpu_address + size;
        auto it = m_sparse_regions.upper_bound(gpu_address);
        if (it != m_sparse_regions.begin()) {
            --it;
        }
        for (; it != m_sparse_regions.end() && it->first < end; ++it) {
            auto& [start, region] = *it;
            if (start + region.size <= gpu_address) {
                continue;
            }
            const u64 first_block = ((std::max)(gpu_address, start) - start) / SparseBlockSize;
            const u64 last_block = ((std::min)(end, start + region.size) - 1 - start) / SparseBlockSize;
            for (u64 block = first_block; block <= last_block; ++block) {
                if (region.blocks[block] != BlockEmpty) {
                    continue;
                }
                const u64 block_address = start + block * SparseBlockSize;
                const u64 block_size = (std::min)(SparseBlockSize, region.size - block * SparseBlockSize);
                const auto address = BackGpuOnlyRegion(block_address, block_size, false, true);
                if (!address) {
                    region.blocks[block] = BlockFailed; // out of memory: not again for every write
                    if (!region.warned) {
                        region.warned = true;
                        LOG_WARNING(Service, "nx2:gpu sparse region at {:#x}: pool exhausted, GPU writes "
                                             "to its unbacked blocks are dropped",
                                    start);
                    }
                    continue;
                }
                region.blocks[block] = BlockBacked;
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().BackSparse(block_address, *address,
                                                                        block_size);
                Tegra::Nx2SparseBlockWritten(start, block);
                m_sparse_backed_bytes += block_size;
                if (++m_sparse_blocks == 1 || (m_sparse_blocks % 64) == 0) {
                    LOG_INFO(Service, "nx2:gpu {} sparse blocks backed ({} MiB); {} MiB of GPU-only "
                                      "memory in use, peak {} MiB",
                             m_sparse_blocks, m_sparse_backed_bytes >> 20, m_gpu_only_bytes >> 20,
                             m_gpu_only_peak >> 20);
                }
            }
        }
    }

    // EDEN_NX2_SPARSE_LAZY=0 backs sparse regions whole, as before.
    static bool LazySparseRegions() {
        static const bool lazy = [] {
            const char* value = std::getenv("EDEN_NX2_SPARSE_LAZY");
            return value == nullptr || value[0] != '0';
        }();
        return lazy;
    }

    static constexpr u32 SparseRegionFlag = 0x200; ///< Word 7 of 214 (0x60c arrays, 0xa0c pools)
    static constexpr u64 SparseBlockSize = 4ULL << 20;
    static constexpr u8 BlockEmpty = 0;
    static constexpr u8 BlockBacked = 1;
    static constexpr u8 BlockFailed = 2;
    struct SparseRegion {
        u64 size;
        std::vector<u8> blocks; ///< BlockEmpty / BlockBacked / BlockFailed per SparseBlockSize
        bool warned{};          ///< Logged that the pool ran out for its blocks
    };
    std::map<u64, SparseRegion> m_sparse_regions; ///< By GPU address
    u64 m_sparse_blocks{};       ///< Sparse blocks backed so far
    u64 m_sparse_backed_bytes{}; ///< Sparse region memory given out so far
    u64 m_gpu_only_bytes{};      ///< Backing of GPU-only regions and sparse blocks right now
    u64 m_gpu_only_peak{};
    u64 m_alias_hint{}; ///< 2 MiB slot after the last backing in the alias code region
    /// Regions are created and destroyed on the service thread and backed on the GPU thread.
    std::recursive_mutex m_region_mutex;

    struct GpuOnlyRegion {
        u64 cpu_address;
        u64 size;
        std::unique_ptr<Kernel::KPageGroup> pages;
    };
    struct ReleasedRegion {
        GpuOnlyRegion region;
        std::shared_ptr<std::atomic<bool>> done; ///< Set by the GPU thread past the region's last use
    };
    std::map<u64, GpuOnlyRegion> m_gpu_only_regions; ///< By GPU address
    std::vector<ReleasedRegion> m_released_regions;

    struct GpuMemory {
        u64 address;
        u64 size;
    };

    KernelHelpers::ServiceContext m_context;
    std::array<Kernel::KEvent*, NumIds> m_events{};
    std::map<u32, GpuMemory> m_memory;
    u32 m_next_memory_handle{1};
};

// Native memory and images handed out by the Switch 2 graphics server (gsv 111/114/423/429/315),
// shared with the vi2 session so presents can find an image's memory. Command meanings follow
// the shared changes.patch (IGraphicsSupervisor), re-checked against title 4's SDK.
struct Nx2NativeObjects {
    struct Memory {
        u64 size{};
        u64 address{}; // CPU (== GPU) address once bound by 114/423
    };
    struct Image {
        u32 memory{};
        std::array<u32, 16> preference{};
        u64 size{};
        u32 count{1}; // images in the pool; handle id + i is image i
        u64 offset{}; // into its memory (images of a decoder pool share one)
        bool window_pool{};
        bool decoder{}; // a video decoder output image (gsv 911)
    };
    std::mutex mutex;
    std::map<u32, Memory> memories;
    std::map<u32, Image> images;
    u32 next_memory{1};
    u32 next_image{1};
};

// Block-linear layout of an InterModuleUserImagePreference (16 words): word 2 holds
// (width - 1) | (height - 1) << 16, word 4 the block height (log2 of GOB rows), word 5 the row
// pitch in 64-byte GOBs. Formula from changes.patch (native_image_layout.h); preferences outside
// it fall back to 4 bytes per pixel and 128-row blocks.
struct Nx2ImageLayout {
    u32 width;
    u32 height;
    u64 pitch;
    u32 block_height_log2;
    u64 size;
    u64 alignment;
};

inline Nx2ImageLayout Nx2CalculateImageLayout(const u32* preference) {
    Nx2ImageLayout layout{};
    layout.width = (preference[2] & 0xFFFF) + 1;
    layout.height = (preference[2] >> 16) + 1;
    const u64 pitch_gobs = preference[5];
    const u32 block_height_log2 = preference[4];
    if (block_height_log2 <= 8 && pitch_gobs >= (u64{layout.width} * 4 + 63) / 64 &&
        pitch_gobs <= 4096) {
        layout.pitch = pitch_gobs * 64;
        layout.block_height_log2 = block_height_log2;
    } else {
        layout.pitch = Common::AlignUp(u64{layout.width} * 4, 64);
        layout.block_height_log2 = 7;
    }
    const u64 block_rows = u64{1} << layout.block_height_log2;
    layout.size = layout.pitch * Common::AlignUp(u64{layout.height}, block_rows);
    layout.alignment = std::max<u64>(64 * block_rows, 0x1000);
    return layout;
}

// GameChat's graphics side (nn::chat::gsv), handed out by gsv 1202. Read from the SDK's
// nn::chat::gsv::Initialize: TIPC 1() returns a u32 id at word 3 (it aborts with 2162-0001 if
// the id is over 0x3ff), then TIPC 11(u32 id) returns an event (copy handle at word 3, result at
// word 4) that it attaches as a system event. Everything else falls through to the nx2 auto stub.
class INx2ChatGsvSession final : public ServiceFramework<INx2ChatGsvSession> {
public:
    explicit INx2ChatGsvSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:chat:gsv"}, m_context{system_, "nx2:chat:gsv"} {
        RegisterHandlersTipc({
            {1, &INx2ChatGsvSession::GetEventId, "GetEventId (guessed)"},
            {11, &INx2ChatGsvSession::GetEvent, "GetEvent (guessed)"},
        });
    }

    ~INx2ChatGsvSession() override {
        for (auto& [id, event] : m_events) {
            m_context.CloseEvent(event);
        }
    }

private:
    void GetEventId(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:chat:gsv TIPC 1, returning id 0");
        TipcReplyWords(ctx, {0});
    }

    void GetEvent(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:chat:gsv TIPC 11 id={}, returning an event", id);
        auto*& event = m_events[id];
        if (event == nullptr) {
            event = m_context.CreateEvent(fmt::format("nx2:chat:gsv:Event{}", id));
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    KernelHelpers::ServiceContext m_context;
    std::map<u32, Kernel::KEvent*> m_events;
};

// Video decoder output images (nn::codec image pools, gsv 991/911/961). The preference
// (MakeDecodeImagePreference) holds an output layout code at word 1 and (width - 1) |
// (height - 1) << 16 at word 2. Codes come in pairs, one per CodecMode: 1/4 NV12 (format 3, what
// the movie players use), 2/5 BGRA, 3/6 RGBA; the HDR codes (10-13) are written as RGBA. The
// server picks the layout and describes it to the client with gsv 961: here pitch-linear rows,
// NV12's half-height chroma plane below the luma plane, each plane padded to 128 rows.
struct Nx2DecoderImageLayout {
    u32 code;
    u32 width;
    u32 height;
    Tegra::Host1x::Nx2PictureFormat format;
    u64 pitch;
    u64 chroma_offset;
    u64 size;
};

inline Nx2DecoderImageLayout Nx2CalculateDecoderImageLayout(const u32* preference) {
    using Tegra::Host1x::Nx2PictureFormat;
    Nx2DecoderImageLayout layout{};
    layout.code = preference[1];
    layout.width = (preference[2] & 0xFFFF) + 1;
    layout.height = (preference[2] >> 16) + 1;
    switch (layout.code) {
    case 1:
    case 4:
        layout.format = Nx2PictureFormat::Nv12;
        break;
    case 2:
    case 5:
        layout.format = Nx2PictureFormat::Bgra8;
        break;
    default:
        layout.format = Nx2PictureFormat::Rgba8;
        break;
    }
    const bool nv12 = layout.format == Nx2PictureFormat::Nv12;
    constexpr u64 BlockRows = 128;
    layout.pitch = Common::AlignUp(u64{layout.width} * (nv12 ? 1 : 4), 64);
    layout.chroma_offset = layout.pitch * Common::AlignUp(u64{layout.height}, BlockRows);
    layout.size = layout.chroma_offset +
                  (nv12 ? layout.pitch * Common::AlignUp((u64{layout.height} + 1) / 2, BlockRows) : 0);
    return layout;
}

// Placeholder for the Switch 2 display session (nn::vi2::sdk), handed out by gsv 305. Reply
// shapes read from the SDK: TIPC 1 -> u32 < 0x400, TIPC 3 -> u32 < 8, TIPC 11/31(u32) and
// 32(handle, u32) -> one handle at word 3 then the result. Handles returned here are events (a
// guess); everything else is auto-stubbed through the nx2: prefix.
class INx2Vi2Session final : public ServiceFramework<INx2Vi2Session> {
public:
    explicit INx2Vi2Session(Core::System& system_, std::shared_ptr<Nx2NativeObjects> objects_)
        : ServiceFramework{system_, "nx2:vi2"}, m_context{system_, "nx2:vi2"},
          m_objects{std::move(objects_)} {
        RegisterHandlersTipc({
            {1, &INx2Vi2Session::ReturnOne, "Unknown1"},
            {3, &INx2Vi2Session::ReturnOne, "GetDisplayCount (guessed)"},
            {5, &INx2Vi2Session::GetTimestampRecorderCount, "GetTimestampRecorderCount (guessed)"},
            {11, &INx2Vi2Session::ReturnEvent, "Unknown11"},
            {31, &INx2Vi2Session::GetVsyncEvent, "GetVsyncEvent (guessed)"},
            {32, &INx2Vi2Session::GetVsyncEvent, "GetDisplayVsyncEvent (guessed)"},
            // Title 1 calls these ~75 times a second with the display name "default"; the
            // auto stub's empty success reply works, but logged two lines per call (a 50 MB log
            // in an 18 minute session).
            {104, &INx2Vi2Session::SucceedQuietly, "Unknown104 (display \"default\")"},
            {105, &INx2Vi2Session::SucceedQuietly, "Unknown105 (display \"default\")"},
            {228, &INx2Vi2Session::CancelClientBuffer, "CancelClientBuffer"},
            {229, &INx2Vi2Session::PresentClientBuffer, "PresentClientBuffer"},
        });
    }

    ~INx2Vi2Session() override {
        // The vsync callback may be running on the HostTiming thread: empty its event list under
        // its lock first (it holds the state, not this session), then close the events.
        {
            std::scoped_lock lk{m_vsync->mutex};
            m_vsync->events.clear();
        }
        if (m_vsync_timer) {
            system.CoreTiming().UnscheduleEvent(m_vsync_timer);
        }
        for (auto* event : m_events) {
            m_context.CloseEvent(event);
        }
    }

private:
    // 229(u64 client token, u32 slot, u32 image, ...fences): result, u64 mask of buffer slots the
    // client may acquire next, four zero words. Nothing is shown yet; every slot is handed back
    // at once (changes.patch's default mask is 0x7fff).
    void PresentClientBuffer(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        ++m_presents;
        Core::g_nx2_present_count.fetch_add(1, std::memory_order_relaxed);
        if (m_presents <= 4 || (m_presents % 600) == 0) {
            LOG_INFO(Service, "nx2:vi2 229 present #{} token={:#x} slot={} image={}", m_presents,
                     u64{w[2]} | (u64{w[3]} << 32), w[4], w[5]);
        }
        // Wait for the previous frame's fence instead of this one, so the game prepares frame N+1
        // while the GPU thread runs frame N (the console keeps several frames in flight). Waiting
        // for this frame serialized the two: 8-56 ms of every 34-105 ms frame on title 1.
        // EDEN_NX2_FRAMES_AHEAD=0 restores the wait for the current frame.
        static const bool frames_ahead = [] {
            const char* value = std::getenv("EDEN_NX2_FRAMES_AHEAD");
            return value == nullptr || value[0] != '0';
        }();
        const auto entered = std::chrono::steady_clock::now();
        if (frames_ahead) {
            LOG_DEBUG(Service, "nx2:vi2 present #{} fence ({}, {:#x}), waiting for ({}, {:#x}); now {:#x}",
                      m_presents, ~w[22], w[24], m_previous_fence_id, m_previous_fence_value,
                      ~w[22] < 192 ? system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(~w[22]) : 0U);
            WaitForFrame(m_previous_fence_id, m_previous_fence_value);
            m_previous_fence_id = ~w[22];
            m_previous_fence_value = w[24];
        } else {
            WaitForFrame(~w[22], w[24]);
        }
        const auto waited = std::chrono::steady_clock::now();
        Composite(w[5], ~w[22], w[24]);
        const auto composited = std::chrono::steady_clock::now();
        PaceToVblank();
        const auto paced = std::chrono::steady_clock::now();
        // Where each frame's time goes on the present thread, averaged per 600 presents: the
        // game is blocked here while Eden's GPU thread catches up (fence) and composites.
        {
            using us = std::chrono::microseconds;
            // long long like the stats: microseconds::rep is long on Linux (std::max needs one type).
            const auto to_us = [](auto d) -> long long {
                return std::chrono::duration_cast<us>(d).count();
            };
            m_stats.fence_us += to_us(waited - entered);
            m_stats.composite_us += to_us(composited - waited);
            m_stats.pace_us += to_us(paced - composited);
            m_stats.max_fence_us = (std::max<long long>)(m_stats.max_fence_us, static_cast<long long>(to_us(waited - entered)));
            if (m_stats.last_present != std::chrono::steady_clock::time_point{}) {
                const long long frame = to_us(entered - m_stats.last_present);
                m_stats.frame_us += frame;
                m_stats.max_frame_us = (std::max)(m_stats.max_frame_us, frame);
                m_stats.hitches += frame > 100'000 ? 1 : 0;
            }
            m_stats.last_present = entered;
            if (++m_stats.presents == 600) {
                LOG_INFO(Service,
                         "nx2:vi2 last 600 presents: frame {} us (max {}, {} over 100 ms), "
                         "blocked on the GPU fence {} us (max {}), composite {} us, vblank "
                         "pacing {} us",
                         m_stats.frame_us / 599, m_stats.max_frame_us, m_stats.hitches,
                         m_stats.fence_us / 600, m_stats.max_fence_us, m_stats.composite_us / 600,
                         m_stats.pace_us / 600);
                m_stats = {.last_present = entered};
            }
        }
        constexpr u64 AvailableSlots = 0x7fff;
        TipcReplyWords(ctx, {static_cast<u32>(AvailableSlots),
                             static_cast<u32>(AvailableSlots >> 32), 0, 0, 0, 0});
    }

    // The console's compositor frees a buffer only once its frame's GPU fence has passed, which
    // is what keeps a game from running ahead of the GPU. This reply always reports every buffer
    // free, so pace the game here instead: wait (bounded, in case the GPU waits on the CPU) until
    // Eden's GPU thread has processed the frame. Otherwise the game laps the GPU thread and
    // rewrites command memory it hasn't read yet.
    u32 m_wait_timeouts{};
    struct PresentStats {
        long long frame_us{}, fence_us{}, composite_us{}, pace_us{}, max_fence_us{};
        long long max_frame_us{}, hitches{};
        u32 presents{};
        std::chrono::steady_clock::time_point last_present{};
    } m_stats{};
    u32 m_previous_fence_id{~0U}; // none yet (WaitForFrame ignores ids >= 192)
    u32 m_previous_fence_value{};

    // The console flips one frame per 60 Hz vblank and keeps the rest queued, so a game can't
    // present faster than that. Every slot is reported free here: hold each present until the
    // next vblank instead (title 4 ran at up to 160 fps, its timing and audio with it).
    std::chrono::steady_clock::time_point m_next_vblank{};

    void PaceToVblank() {
        if (!Settings::values.use_speed_limit.GetValue() || Settings::SpeedLimit() == 0) {
            return;
        }
        const auto interval = std::chrono::nanoseconds{100'000'000'000LL / 60 / Settings::SpeedLimit()};
        const auto now = std::chrono::steady_clock::now();
        if (now > m_next_vblank + interval) {
            // Running slower than 60 fps: show at once and don't bank the lost time.
            m_next_vblank = now;
        } else if (now < m_next_vblank) {
            std::this_thread::sleep_until(m_next_vblank);
        }
        m_next_vblank += interval;
    }

    void WaitForFrame(u32 fence_id, u32 fence_value) {
        if (fence_id >= 192) {
            return;
        }
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        const auto start = std::chrono::steady_clock::now();
        // Long enough to cover shader/pipeline compile stalls: a game that gets more than a few
        // command buffers ahead misses the equal-value gates its other channels acquire.
        const auto deadline = start + std::chrono::milliseconds{4000};
        if (!syncpoints.IsReadyGuest(fence_id, fence_value)) {
            // Woken by the GPU thread as it reaches the fence. Polling with 200 us sleeps slept
            // about 1 ms per check on Windows, added to every frame. An action left registered
            // after a timeout fires later on a state nobody waits on.
            struct Wake {
                std::mutex mutex;
                std::condition_variable cv;
                bool done{};
            };
            auto wake = std::make_shared<Wake>();
            syncpoints.RegisterGuestAction(fence_id, fence_value, [wake] {
                {
                    std::scoped_lock lk{wake->mutex};
                    wake->done = true;
                }
                wake->cv.notify_all();
            });
            std::unique_lock lk{wake->mutex};
            while (!wake->done && system.IsPoweredOn() &&
                   std::chrono::steady_clock::now() < deadline) {
                wake->cv.wait_for(lk, std::chrono::milliseconds{50});
            }
        }
        if (!syncpoints.IsReadyGuest(fence_id, fence_value) && m_wait_timeouts++ < 64) {
            LOG_WARNING(Service, "nx2:vi2 present #{} fence ({}, {}) not reached in 4 s (now {})",
                        m_presents, fence_id, fence_value, syncpoints.GetGuestSyncpointValue(fence_id));
        }
        if (m_presents <= 8 || (m_presents % 600) == 0) {
            LOG_INFO(Service, "nx2:vi2 present #{} fence ({}, {}) now {}, waited {} us",
                     m_presents, fence_id, fence_value, syncpoints.GetGuestSyncpointValue(fence_id),
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count());
        }
    }

    // Shows a presented native image: word 5 is the image id, ~word 22 / word 24 the GPU fence
    // (syncpoint, value) to wait for (layout from changes.patch). The image is a block-linear
    // RGBA8 surface in the console's native GOB layout at GPU address == CPU address.
    void Composite(u32 image_id, u32 fence_id, u32 fence_value) {
        u64 address{};
        Nx2ImageLayout layout{};
        {
            std::scoped_lock lk{m_objects->mutex};
            const auto image = m_objects->images.find(image_id);
            if (image == m_objects->images.end()) {
                return;
            }
            const auto memory = m_objects->memories.find(image->second.memory);
            if (memory == m_objects->memories.end() || memory->second.address == 0) {
                return;
            }
            address = memory->second.address;
            layout = Nx2CalculateImageLayout(image->second.preference.data());
        }
        const auto device = Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DeviceAddressOf(address);
        if (!device) {
            LOG_WARNING(Service, "nx2:vi2 present: image {} at {:#x} has no device mapping",
                        image_id, address);
            return;
        }
        u32 crop_width = layout.width;
        u32 crop_height = layout.height;
        // Mario Kart World draws into the top-left corner of its present images: 2560x1440 docked
        // (in 3840x2160 images), 1920x1080 handheld. It keeps its docked images after a switch to
        // handheld, and showing all of them put the frame in a corner with stale docked frames
        // around it.
        if (system.GetApplicationProcessProgramID() == 0x0400C3F00006E000ULL) {
            if (!Settings::IsDockedMode()) {
                crop_width = (std::min)(crop_width, 1920U);
                crop_height = (std::min)(crop_height, 1080U);
            } else if (layout.width == 3840 && layout.height == 2160) {
                crop_width = 2560;
                crop_height = 1440;
            }
        }
        static const u32 env_crop_w = [] {
            const char* v = std::getenv("EDEN_NX2_CROP_WIDTH");
            return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0U;
        }();
        static const u32 env_crop_h = [] {
            const char* v = std::getenv("EDEN_NX2_CROP_HEIGHT");
            return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0U;
        }();
        if (env_crop_w != 0 && env_crop_h != 0) {
            crop_width = env_crop_w;
            crop_height = env_crop_h;
        }
        const u64 shown = (u64{layout.width} << 48) | (u64{layout.height} << 32) |
                          (u64{crop_width} << 16) | crop_height;
        if (shown != m_shown_layout) {
            m_shown_layout = shown;
            LOG_INFO(Service, "nx2:vi2 composite image {} layout {}x{}, crop {}x{}",
                     image_id, layout.width, layout.height, crop_width, crop_height);
        }
        std::vector<Tegra::FramebufferConfig> layers{Tegra::FramebufferConfig{
            .address = *device,
            .offset = 0,
            .width = layout.width,
            .height = layout.height,
            .stride = static_cast<u32>(layout.pitch / 4),
            .pixel_format = android::PixelFormat::Rgba8888,
            .transform_flags = android::BufferTransformFlags::Unset,
            .crop_rect = {0, 0, static_cast<int>(crop_width), static_cast<int>(crop_height)},
            .blending = Tegra::BlendMode::Opaque,
            .native_tiling = true,
        }};
        std::vector<Nvidia::NvFence> fences;
        if (fence_id < 192) {
            fences.push_back({static_cast<s32>(fence_id), fence_value});
        }
        system.GPU().RequestComposite(std::move(layers), std::move(fences));
        CaptureFrame();
        DumpRanges();
        system.SpeedLimiter().DoSpeedLimiting(system.CoreTiming().GetGlobalTimeUs());
        system.GetPerfStats().EndSystemFrame();
        system.GetPerfStats().BeginSystemFrame();
    }

    // EDEN_NX2_FRAME_CAPTURE=<dir>: every 120th present, save what the renderer shows as
    // nx2-frame-<n>.tga in <dir> (lets hidden test runs check the output; idea from changes.patch).
    void CaptureFrame() {
        static const char* const directory = std::getenv("EDEN_NX2_FRAME_CAPTURE");
        if (directory == nullptr || (m_presents % 120) != 1 || m_capture_pending->exchange(true)) {
            return;
        }
        const auto layout = Layout::DefaultFrameLayout(1280, 720);
        auto pixels = std::make_shared<std::vector<u8>>(layout.width * layout.height * 4);
        const std::string path =
            fmt::format("{}/nx2-frame-{:05}.tga", directory, m_presents);
        auto pending = m_capture_pending; // the screenshot callback can outlive the session
        system.GPU().Renderer().RequestScreenshot(
            pixels->data(),
            [pixels, layout, path, pending](bool invert_y) {
                std::array<u8, 18> header{};
                header[2] = 2; // uncompressed true colour
                header[12] = static_cast<u8>(layout.width);
                header[13] = static_cast<u8>(layout.width >> 8);
                header[14] = static_cast<u8>(layout.height);
                header[15] = static_cast<u8>(layout.height >> 8);
                header[16] = 32;
                // Descriptor bit 5: rows top to bottom. invert_y means the data is bottom-up.
                header[17] = invert_y ? 0x08 : 0x28;
                if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(header.data(), 1, header.size(), file);
                    std::fwrite(pixels->data(), 1, pixels->size(), file);
                    std::fclose(file);
                }
                pending->store(false);
            },
            layout);
    }

    // EDEN_NX2_DUMP=<present>:<hex address>:<hex size>[,<hex address>:<hex size>...]: at that
    // present, flush those GPU ranges back from the host GPU and save their bytes as
    // nx2-dump-<address>.bin in the EDEN_NX2_FRAME_CAPTURE directory (to inspect what a pass
    // rendered, e.g. a 3D volume, offline).
    void DumpRanges() {
        static const std::string spec = [] {
            const char* text = std::getenv("EDEN_NX2_DUMP");
            return text ? std::string{text} : std::string{};
        }();
        if (spec.empty() || m_presents != std::strtoull(spec.c_str(), nullptr, 10)) {
            return;
        }
        const char* const directory = std::getenv("EDEN_NX2_FRAME_CAPTURE");
        size_t position = spec.find(':');
        while (position != std::string::npos) {
            char* end{};
            const u64 address = std::strtoull(spec.c_str() + position + 1, &end, 16);
            const u64 size = *end == ':' ? std::strtoull(end + 1, &end, 16) : 0;
            position = spec.find(',', position + 1); // the next "address:size"
            const std::string path = fmt::format("{}/nx2-dump-{:x}.bin", directory ? directory : ".", address);
            // On the GPU thread: waiting for it here can deadlock with a GPU wait on this present.
            static_cast<void>(system.GPU().RunOnGpuThread([&system = system, address, size, path] {
                std::vector<u8> bytes(size);
                for (u64 current = address; current < address + size;) {
                    const u64 chunk = std::min((current | 0xfff) + 1, address + size) - current;
                    if (const auto device = Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DeviceAddressOf(current)) {
                        system.GPU().Renderer().ReadRasterizer()->FlushRegion(*device, chunk);
                        system.Host1x().MemoryManager().ReadBlock(*device, bytes.data() + (current - address), chunk);
                    }
                    current += chunk;
                }
                if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(bytes.data(), 1, bytes.size(), file);
                    std::fclose(file);
                }
                LOG_INFO(Service, "nx2: dumped {:#x} bytes at {:#x} to {}", size, address, path);
            }));
        }
    }

    // 228(u64 token, u32 slot): return a buffer without presenting it.
    void CancelClientBuffer(HLERequestContext& ctx) {
        constexpr u64 AvailableSlots = 0x7fff;
        TipcReplyWords(ctx, {static_cast<u32>(AvailableSlots),
                             static_cast<u32>(AvailableSlots >> 32), 0, 0, 0, 0});
    }

    void ReturnOne(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC {}, returning 1", ctx.GetCommand());
        TipcReplyWords(ctx, {1});
    }

    // The same reply as the auto stub (success, no data), without its log lines.
    void SucceedQuietly(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Must be < 8; nn::vi::detail::LayerHistory creates 7 timestamp recorders per layer.
    void GetTimestampRecorderCount(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC 5, returning 7");
        TipcReplyWords(ctx, {7});
    }

    void ReturnEvent(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC {} [2]={:#x} [3]={:#x}, returning an event",
                    ctx.GetCommand(), ctx.CommandBuffer()[2], ctx.CommandBuffer()[3]);
        auto* event = m_context.CreateEvent(fmt::format("nx2:vi2:Event{}", m_events.size()));
        m_events.push_back(event);
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    // 31(u32 kind) and 32(u32 display): events signalled on every vblank. Title 1's main thread
    // waits on the 32 event until it has fired twice before it submits any GPU work, and hung
    // there while it never fired.
    void GetVsyncEvent(HLERequestContext& ctx) {
        LOG_INFO(Service, "nx2:vi2 {} vsync event, [2]={:#x}", ctx.GetCommand(), ctx.CommandBuffer()[2]);
        auto* event = m_context.CreateEvent(fmt::format("nx2:vi2:Vsync{}", m_events.size()));
        m_events.push_back(event);
        {
            std::scoped_lock lk{m_vsync->mutex};
            m_vsync->events.push_back(event);
        }
        if (!m_vsync_timer) {
            m_vsync_timer = Core::Timing::CreateEvent(
                "nx2:vi2 vsync",
                [vsync_state = m_vsync, &kernel = system.Kernel()](
                    s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                    std::scoped_lock lk{vsync_state->mutex};
                    for (auto* vsync : vsync_state->events) {
                        vsync->Signal(kernel);
                    }
                    return std::nullopt;
                });
            // EDEN_NX2_VSYNC_HZ=<rate> (experiment): the game starts frames on this event, so a
            // frame that misses a 60 Hz tick idles until the next one.
            static const std::chrono::nanoseconds VblankInterval{[] {
                const char* text = std::getenv("EDEN_NX2_VSYNC_HZ");
                const long long hz = text ? (std::max)(std::strtoll(text, nullptr, 10), 1LL) : 60LL;
                return 1'000'000'000LL / hz;
            }()};
            system.CoreTiming().ScheduleLoopingEvent(VblankInterval, VblankInterval, m_vsync_timer);
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    KernelHelpers::ServiceContext m_context;
    std::shared_ptr<Nx2NativeObjects> m_objects;
    std::vector<Kernel::KEvent*> m_events;
    // Shared with the vsync timer callback, which can outlive the session by one call.
    struct VsyncState {
        std::mutex mutex;
        std::vector<Kernel::KEvent*> events;
    };
    std::shared_ptr<VsyncState> m_vsync = std::make_shared<VsyncState>();
    std::shared_ptr<Core::Timing::EventType> m_vsync_timer;
    u64 m_presents{};
    u64 m_shown_layout{}; // image and crop size last logged
    std::shared_ptr<std::atomic<bool>> m_capture_pending = std::make_shared<std::atomic<bool>>(false);
};

// True the first time a Switch 2 video command is seen, so each one is logged once in full and new
// or unexpected commands stand out.
inline bool Nx2FirstVideoCommand(std::string_view service, u32 command) {
    static std::mutex mutex;
    static std::set<std::pair<std::string, u32>> seen;
    std::scoped_lock lk{mutex};
    return seen.emplace(std::string{service}, command).second;
}

// The Switch 2 video decoder (nn::codec::video::detail::ipc). gsv 902/906/1002 hand out a session
// whose TIPC 1 (CreateVideoDecoderService: handle at word 3, result at word 4) creates a decoder
// service, another session of this class. Layouts read from the SDK's nn::codec VP9 decoder:
// - 200(copy handle: work buffer transfer memory, u64 size, u32, u32, u32): initialize.
// - 101(A: codec name, "video/vp9\0"; u32 width, u32 height, u32 rate, u32): configure.
// - 3(): the submission-ready event (copy handle at word 3, result at word 4).
// - 6 / 207 / 8: start / stop / flush; 115: finalize.
// - 311(u32 image id): register an output image (gsv 911 creates it); reply its NativeSyncHandle,
//   signalled once a frame has been decoded into the image. The client acquires output images in
//   registration/release order from a ring of its own (no IPC), so frames fill them in that order.
// - 314(u32 image id, u32, NativeSyncHandle): give an output image back; the image may be
//   rewritten once that handle (the client's reads) has passed. Reply the image's next handle.
// - 212(u32 image id): unregister it.
// - 109(A: one compressed frame, u64 pts in us, u8 end of stream): submit input; reply a u64.
// - 213(): the next finished frame (16 words): u32 image id, u32 width, u32 height, u32 width,
//   u32 height, u32 colour space, TimeSpan pts (ns), u8 end of stream.
// A NativeSyncHandle is {u32 SyncCounterId (~syncpoint), u32 mode, u64 value}; clients wait on it
// with nx2:syncpt 103/201. Each decoder service owns a syncpoint that counts its finished frames.
// FFmpeg decodes on a thread of the session's own (a 1440p VP9 frame takes milliseconds, and the
// service thread also serves presents and syncpoint waits) into the layout gsv 961 describes.
class INx2VideoSession final : public ServiceFramework<INx2VideoSession> {
public:
    explicit INx2VideoSession(Core::System& system_, std::shared_ptr<Nx2NativeObjects> objects_,
                              const char* name)
        : ServiceFramework{system_, name}, m_context{system_, name},
          m_objects{std::move(objects_)} {
        RegisterHandlersTipc({
            {1, &INx2VideoSession::CreateDecoderService, "CreateVideoDecoderService"},
            {3, &INx2VideoSession::GetEvent, "GetEvent"},
            {6, &INx2VideoSession::Succeed, "Start"},
            {8, &INx2VideoSession::Flush, "Flush"},
            {101, &INx2VideoSession::Configure, "Configure"},
            {109, &INx2VideoSession::SubmitInput, "SubmitInputBuffer"},
            {115, &INx2VideoSession::Succeed, "Finalize"},
            {200, &INx2VideoSession::Succeed, "Initialize"},
            {207, &INx2VideoSession::Flush, "Stop"},
            {212, &INx2VideoSession::UnregisterBuffer, "UnregisterBuffer"},
            {213, &INx2VideoSession::GetOutputInfo, "GetOutputInfo"},
            {311, &INx2VideoSession::RegisterBuffer, "RegisterBuffer"},
            {314, &INx2VideoSession::ReleaseBuffer, "ReleaseBuffer"},
        });
    }

    ~INx2VideoSession() override {
        if (m_worker.joinable()) {
            {
                std::scoped_lock lk{m_mutex};
                m_quit = true;
            }
            m_cv.notify_all();
            m_worker.join();
        }
        // The ready timer runs on the HostTiming thread: clear its event under its lock first.
        {
            std::scoped_lock lk{m_ready->mutex};
            m_ready->event = nullptr;
        }
        if (m_ready_timer) {
            system.CoreTiming().UnscheduleEvent(m_ready_timer);
        }
        if (m_event != nullptr) {
            m_context.CloseEvent(m_event);
        }
        if (m_syncpoint != NoSyncpoint) {
            NvdrvSyncpoints().FreeSyncpoint(m_syncpoint);
        }
    }

private:
    struct Input {
        std::vector<u8> data;
        u64 pts; // us
        bool end;
    };
    struct Decoded {
        std::optional<Tegra::Host1x::Nx2Picture> picture; // none: an empty end-of-stream frame
        u64 pts;
        bool end;
    };
    struct Output {
        u32 image;
        u32 width;
        u32 height;
        u64 pts;
        bool end;
    };
    struct Fence {
        u32 syncpoint;
        u32 value;
    };

    static constexpr u32 NoSyncpoint = ~0U;
    static constexpr size_t MaxQueuedInputs = 8;
    static constexpr size_t MaxDecodedAhead = 2; // decoded frames held while no image is free
    static constexpr u32 HostSyncpoints = 192;   // Tegra::Host1x::SyncpointManager

    void Log(HLERequestContext& ctx, std::string_view what) {
        const auto* w = ctx.CommandBuffer();
        if (Nx2FirstVideoCommand(GetServiceName(), ctx.GetCommand())) {
            LOG_INFO(Service, "{} TIPC {} {} (first use): words {:08x}", GetServiceName(),
                     ctx.GetCommand(), what, fmt::join(std::span<const u32>{w, 12}, " "));
        } else {
            LOG_DEBUG(Service, "{} TIPC {} {}: [2]={:#x} [3]={:#x} [4]={:#x} [5]={:#x}",
                      GetServiceName(), ctx.GetCommand(), what, w[2], w[3], w[4], w[5]);
        }
    }

    Nvidia::NvCore::SyncpointManager& NvdrvSyncpoints() {
        return system.ServiceManager()
            .GetService<Nvidia::NVDRV>("nvdrv", true)
            ->GetModule()
            ->GetContainer()
            .GetSyncpointManager();
    }

    void CreateDecoderService(HLERequestContext& ctx) {
        Log(ctx, "create decoder service");
        ReturnRawSession(system, ctx,
                         std::make_shared<INx2VideoSession>(system, m_objects, "nx2:vdecsvc"));
    }

    void Succeed(HLERequestContext& ctx) {
        Log(ctx, "");
        TipcReplyWords(ctx, {});
    }

    void Configure(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        std::string codec;
        if (!ctx.BufferDescriptorA().empty()) {
            const auto name = ctx.ReadBufferA(0);
            codec.assign(reinterpret_cast<const char*>(name.data()),
                         strnlen(reinterpret_cast<const char*>(name.data()), name.size()));
        }
        Log(ctx, "configure");
        LOG_INFO(Service, "{} configure codec \"{}\" {}x{} rate {}", GetServiceName(), codec, w[5],
                 w[6], w[7]);
        {
            std::scoped_lock lk{m_mutex};
            m_codec = std::move(codec);
            m_width = w[5];
            m_height = w[6];
        }
        if (!m_worker.joinable()) {
            m_worker = std::thread{[this] { DecodeLoop(); }};
        }
        TipcReplyWords(ctx, {});
    }

    void GetEvent(HLERequestContext& ctx) {
        Log(ctx, "event");
        if (m_event == nullptr) {
            m_event = m_context.CreateEvent(fmt::format("{}:Ready", GetServiceName()));
            // The client attaches it auto-clearing, so one signal is consumed by the first wait:
            // while the input queue has room, re-signal every 4 ms; a spurious wake only makes it
            // check again.
            {
                std::scoped_lock lk{m_ready->mutex};
                m_ready->event = m_event;
            }
            m_ready_timer = Core::Timing::CreateEvent(
                "nx2:vdec ready",
                [ready = m_ready, &kernel = system.Kernel()](
                    s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                    std::scoped_lock lk{ready->mutex};
                    if (ready->event != nullptr && ready->room.load(std::memory_order_relaxed)) {
                        ready->event->Signal(kernel);
                    }
                    return std::nullopt;
                });
            constexpr std::chrono::nanoseconds ReadyInterval{std::chrono::milliseconds{4}};
            system.CoreTiming().ScheduleLoopingEvent(ReadyInterval, ReadyInterval, m_ready_timer);
        }
        m_event->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, m_event->GetReadableEvent());
    }

    // Queues an output image for the next decoded frame; returns the NativeSyncHandle words that
    // signal once that frame is in it.
    std::array<u32, 4> HandOut(u32 image, std::optional<Fence> release_fence) {
        if (m_syncpoint == NoSyncpoint) {
            m_syncpoint = NvdrvSyncpoints().AllocateSyncpoint(false);
            m_sync_base = system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(m_syncpoint);
            LOG_INFO(Service, "{} counts decoded frames on syncpoint {} from {}",
                     GetServiceName(), m_syncpoint, m_sync_base);
        }
        u32 value;
        {
            std::scoped_lock lk{m_mutex};
            m_free.push_back(image);
            if (release_fence) {
                m_release_fences[image] = *release_fence;
            }
            value = m_sync_base + ++m_handed;
        }
        m_cv.notify_one();
        return {~m_syncpoint, 0, value, 0};
    }

    void RegisterBuffer(HLERequestContext& ctx) {
        Log(ctx, "register output");
        const auto handle = HandOut(ctx.CommandBuffer()[2], std::nullopt);
        TipcReplyWords(ctx, {handle[0], handle[1], handle[2], handle[3]});
    }

    void ReleaseBuffer(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        Log(ctx, "release output");
        const auto handle = HandOut(w[2], Fence{~w[4], w[6]});
        TipcReplyWords(ctx, {handle[0], handle[1], handle[2], handle[3]});
    }

    void UnregisterBuffer(HLERequestContext& ctx) {
        Log(ctx, "unregister output");
        const u32 image = ctx.CommandBuffer()[2];
        {
            std::scoped_lock lk{m_mutex};
            std::erase(m_free, image);
            m_release_fences.erase(image);
        }
        TipcReplyWords(ctx, {});
    }

    void SubmitInput(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        // Words 2-4 are the A descriptor of the compressed frame.
        const u64 pts = u64{w[5]} | (u64{w[6]} << 32);
        const bool end = (w[7] & 0xFF) != 0;
        Log(ctx, "submit");
        Input input{{}, pts, end};
        if (!ctx.BufferDescriptorA().empty()) {
            const auto data = ctx.ReadBufferA(0);
            input.data.assign(data.begin(), data.end());
        }
        {
            std::scoped_lock lk{m_mutex};
            m_inputs.push_back(std::move(input));
            m_ready->room.store(m_inputs.size() < MaxQueuedInputs, std::memory_order_relaxed);
        }
        m_cv.notify_one();
        const u64 id = ++m_submitted;
        TipcReplyWords(ctx, {static_cast<u32>(id), static_cast<u32>(id >> 32)});
    }

    // Flush (8) and Stop (207): drop queued input and frames not yet in an image. Images already
    // handed out keep their place, so the sync values stay in step with the client's ring.
    void Flush(HLERequestContext& ctx) {
        Log(ctx, ctx.GetCommand() == 8 ? "flush" : "stop");
        {
            std::scoped_lock lk{m_mutex};
            m_inputs.clear();
            m_flush = true;
            m_ready->room.store(true, std::memory_order_relaxed);
        }
        m_cv.notify_one();
        TipcReplyWords(ctx, {});
    }

    void GetOutputInfo(HLERequestContext& ctx) {
        Log(ctx, "output info");
        std::optional<Output> output;
        {
            std::scoped_lock lk{m_mutex};
            if (!m_outputs.empty()) {
                output = m_outputs.front();
                m_outputs.pop_front();
            }
        }
        if (!output) {
            // Nothing finished: the code the client's own Acquire uses for an empty ring.
            IPC::ResponseBuilder rb{ctx, 2};
            rb.PushRaw<u32>(0x1c286f);
            return;
        }
        const u64 pts_ns = output->pts * 1000;
        TipcReplyWords(ctx, {output->image, output->width, output->height, output->width,
                             output->height, 0, static_cast<u32>(pts_ns),
                             static_cast<u32>(pts_ns >> 32), output->end ? 1U : 0U, 0, 0, 0, 0,
                             0, 0, 0});
    }

    // The decode thread: turns queued input into decoded frames and decoded frames into output
    // images, in order.
    void DecodeLoop() {
        Common::SetCurrentThreadName("nx2:vdec");
        std::unique_ptr<Tegra::Host1x::Nx2VideoDecoder> decoder;
        std::vector<Tegra::Host1x::Nx2Picture> pictures;
        std::vector<u8> bytes;
        std::unique_lock lk{m_mutex};
        while (true) {
            m_cv.wait(lk, [&] {
                return m_quit || m_flush || (!m_decoded.empty() && !m_free.empty()) ||
                       (!m_inputs.empty() && m_decoded.size() < MaxDecodedAhead);
            });
            if (m_quit) {
                return;
            }
            if (m_flush) {
                m_flush = false;
                m_decoded.clear();
                if (decoder) {
                    lk.unlock();
                    decoder->Reset();
                    lk.lock();
                }
                continue;
            }
            if (!m_decoded.empty() && !m_free.empty()) {
                Decoded frame = std::move(m_decoded.front());
                m_decoded.pop_front();
                const u32 image = m_free.front();
                m_free.pop_front();
                std::optional<Fence> fence;
                if (const auto it = m_release_fences.find(image); it != m_release_fences.end()) {
                    fence = it->second;
                    m_release_fences.erase(it);
                }
                lk.unlock();
                if (fence) {
                    WaitForRelease(*fence);
                }
                const auto [width, height] = WriteImage(decoder.get(), image, frame, bytes);
                lk.lock();
                // Even an image written during a flush counts: the client waits for its value.
                m_outputs.push_back({image, width, height, frame.pts, frame.end});
                system.Host1x().GetSyncpointManager().IncrementGuest(m_syncpoint);
                ++m_frames_out;
                if (m_frames_out == 1 || (m_frames_out % 600) == 0 || frame.end) {
                    LOG_INFO(Service, "{} frame {} ({}x{}, pts {} us{}) in image {}",
                             GetServiceName(), m_frames_out, width, height, frame.pts,
                             frame.end ? ", end of stream" : "", image);
                }
                continue;
            }
            Input input = std::move(m_inputs.front());
            m_inputs.pop_front();
            m_ready->room.store(m_inputs.size() < MaxQueuedInputs, std::memory_order_relaxed);
            const std::string codec = m_codec;
            lk.unlock();
            if (!decoder || decoder->Name() != codec) {
                decoder = std::make_unique<Tegra::Host1x::Nx2VideoDecoder>(codec);
            }
            pictures.clear();
            decoder->Decode(input.data, input.pts, pictures);
            if (input.end) {
                decoder->Drain(pictures);
            }
            lk.lock();
            if (m_flush) {
                continue;
            }
            for (auto& picture : pictures) {
                const u64 pts = picture.pts;
                m_decoded.push_back({std::move(picture), pts, false});
            }
            if (input.end) {
                // The client stops at the frame marked end of stream; give it one even when the
                // last input held no picture.
                if (m_decoded.empty()) {
                    m_decoded.push_back({std::nullopt, input.pts, true});
                } else {
                    m_decoded.back().end = true;
                }
            }
        }
    }

    // Waits (bounded) until the client's reads of an image it gave back have passed.
    void WaitForRelease(const Fence& fence) {
        if (fence.syncpoint >= HostSyncpoints) {
            return;
        }
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
        while (!syncpoints.IsReadyGuest(fence.syncpoint, fence.value) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    // Writes a decoded frame into an output image; returns the size the frame reports.
    std::pair<u32, u32> WriteImage(Tegra::Host1x::Nx2VideoDecoder* decoder, u32 image_id,
                                   const Decoded& frame, std::vector<u8>& bytes) {
        u64 address{};
        Nx2DecoderImageLayout layout{};
        {
            std::scoped_lock lk{m_objects->mutex};
            const auto image = m_objects->images.find(image_id);
            if (image == m_objects->images.end()) {
                return {m_width, m_height};
            }
            layout = Nx2CalculateDecoderImageLayout(image->second.preference.data());
            const auto memory = m_objects->memories.find(image->second.memory);
            if (memory != m_objects->memories.end() && memory->second.address != 0) {
                address = memory->second.address + image->second.offset;
            }
        }
        if (!frame.picture || decoder == nullptr || address == 0 ||
            system.ApplicationProcess() == nullptr) {
            return {std::min(m_width, layout.width), std::min(m_height, layout.height)};
        }
        // A frame larger than the image is scaled down to it.
        const u32 width = std::min(frame.picture->width, layout.width);
        const u32 height = std::min(frame.picture->height, layout.height);
        bytes.resize(layout.size);
        decoder->Render(*frame.picture, layout.format, width, height, layout.pitch,
                        layout.chroma_offset, bytes);
        DumpFrame(bytes, layout, width, height);
        auto& memory = system.ApplicationMemory();
        memory.WriteBlock(address, bytes.data(), layout.pitch * height);
        if (layout.format == Tegra::Host1x::Nx2PictureFormat::Nv12) {
            memory.WriteBlock(address + layout.chroma_offset, bytes.data() + layout.chroma_offset,
                              layout.pitch * ((height + 1) / 2));
        }
        return {width, height};
    }

    // EDEN_NX2_VDEC_DUMP=<dir>: save the luma of every 120th decoded frame, a quarter of its size,
    // as nx2-vdec-<frame>.pgm (to check what the decoder wrote, apart from how the game shows it).
    void DumpFrame(const std::vector<u8>& bytes, const Nx2DecoderImageLayout& layout, u32 width,
                   u32 height) {
        static const char* const directory = std::getenv("EDEN_NX2_VDEC_DUMP");
        if (directory == nullptr || (m_frames_out % 120) != 0) {
            return;
        }
        const u32 bpp = layout.format == Tegra::Host1x::Nx2PictureFormat::Nv12 ? 1 : 4;
        const u32 w = width / 4;
        const u32 h = height / 4;
        const std::string path = fmt::format("{}/nx2-vdec-{:05}.pgm", directory, m_frames_out + 1);
        if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fprintf(file, "P5 %u %u 255\n", w, h);
            for (u32 y = 0; y < h; ++y) {
                for (u32 x = 0; x < w; ++x) {
                    std::fputc(bytes[y * 4 * layout.pitch + x * 4 * bpp], file);
                }
            }
            std::fclose(file);
        }
    }

    struct ReadyState {
        std::mutex mutex;
        Kernel::KEvent* event{};
        std::atomic<bool> room{true}; ///< The input queue takes more frames
    };

    KernelHelpers::ServiceContext m_context;
    std::shared_ptr<Nx2NativeObjects> m_objects;
    Kernel::KEvent* m_event{};
    std::shared_ptr<ReadyState> m_ready = std::make_shared<ReadyState>();
    std::shared_ptr<Core::Timing::EventType> m_ready_timer;
    u64 m_submitted{};
    u32 m_syncpoint{NoSyncpoint}; ///< Counts finished frames (allocated at the first register)
    u32 m_sync_base{};            ///< Its value before the first frame

    // Shared with the decode thread.
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::string m_codec;
    u32 m_width{};
    u32 m_height{};
    u32 m_handed{}; ///< Output images handed out (registered or released) so far
    u64 m_frames_out{};
    std::deque<Input> m_inputs;
    std::deque<Decoded> m_decoded;     ///< Waiting for a free output image
    std::deque<u32> m_free;            ///< Output images, in the order the client acquires them
    std::map<u32, Fence> m_release_fences;
    std::deque<Output> m_outputs;      ///< Finished, for 213
    bool m_flush{};
    bool m_quit{};
    std::thread m_worker;
};

// Placeholder for the Switch 2 graphics-system server that nn::gsv::BootWithRemoteMemory talks
// to over TIPC. Nothing behind it is emulated: commands succeed and are logged, so boot can show
// what comes next. Commands 202 and 1402 hand out the syncpt and nvsrv sessions the SDK uses.
class INx2GraphicsSystemSession final : public ServiceFramework<INx2GraphicsSystemSession> {
public:
    explicit INx2GraphicsSystemSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:gsv"}, m_context{system_, "nx2:gsv"} {
        RegisterHandlersTipc({
            {4, &INx2GraphicsSystemSession::Stub, "Boot (Switch 2, guessed)"},
            {5, &INx2GraphicsSystemSession::Stub, "Unknown5"},
            {101, &INx2GraphicsSystemSession::Stub, "Unknown101"},
            {202, &INx2GraphicsSystemSession::OpenSyncptSession, "OpenSyncptSession (guessed)"},
            {204, &INx2GraphicsSystemSession::AttachSyncptMemory, "AttachSyncptMemory (guessed)"},
            {305, &INx2GraphicsSystemSession::OpenViSession, "OpenViSession (guessed)"},
            {497, &INx2GraphicsSystemSession::OpenGpuSession, "OpenGpuSession (guessed)"},
            {111, &INx2GraphicsSystemSession::CreateNativeMemory, "CreateNativeMemory"},
            {112, &INx2GraphicsSystemSession::DestroyNativeMemory, "DestroyNativeMemory"},
            {114, &INx2GraphicsSystemSession::BindNativeMemory, "BindNativeMemory"},
            {314, &INx2GraphicsSystemSession::ConfigureWindowImagePool, "DestroyNativeWindowAliasImagePool"},
            {315, &INx2GraphicsSystemSession::ConfigureWindowImagePool, "CreateNativeWindowAliasImagePool"},
            {423, &INx2GraphicsSystemSession::BindNativeGpuMemory, "BindNativeGpuMemory"},
            {429, &INx2GraphicsSystemSession::CreateNativeImage, "CreateNativeImage"},
            {430, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyNativeImage"},
            {426, &INx2GraphicsSystemSession::QueryNativeImage, "MapNativeImage"},
            {428, &INx2GraphicsSystemSession::QueryNativeImage, "QueryNativeImage"},
            {435, &INx2GraphicsSystemSession::CalculateNativeImageSize, "CalculateNativeImageSize"},
            {1402, &INx2GraphicsSystemSession::OpenNvsrvSession, "OpenNvsrvSession (guessed)"},
            {1510, &INx2GraphicsSystemSession::CalculateRequirements, "CalculateRequirements"},
            {991, &INx2GraphicsSystemSession::CalculateDecoderImagePoolBuffer,
             "CalculateDecoderImagePoolBuffer"},
            {901, &INx2GraphicsSystemSession::Stub, "VideoInitialize (guessed)"},
            {902, &INx2GraphicsSystemSession::OpenVideoSession, "OpenVideoDecoderSession (guessed)"},
            {906, &INx2GraphicsSystemSession::OpenVideoSession, "OpenVideoDecoderSessionWithVersion"},
            {1001, &INx2GraphicsSystemSession::Stub, "VideoInitialize2 (guessed)"},
            {1002, &INx2GraphicsSystemSession::OpenVideoSession, "OpenVideoDecoderSession2 (guessed)"},
            {601, &INx2GraphicsSystemSession::Stub, "CameraInitialize (guessed)"},
            {602, &INx2GraphicsSystemSession::OpenCameraSession, "OpenCameraContext (guessed)"},
            {1201, &INx2GraphicsSystemSession::Stub, "ChatInitialize (guessed)"},
            {1202, &INx2GraphicsSystemSession::OpenChatSession, "OpenChatSession (guessed)"},
            // Same request as 429: nn::chat::gsv::CreateImagePool (GameChat camera images).
            {1211, &INx2GraphicsSystemSession::CreateNativeImage, "CreateChatImagePool (guessed)"},
            {1291, &INx2GraphicsSystemSession::CalculateChatImagePoolSize,
             "CalculateChatImagePoolSize (guessed)"},
            {911, &INx2GraphicsSystemSession::CreateDecoderImage, "CreateDecoderImage"},
            {961, &INx2GraphicsSystemSession::GetCpuImageDescriptor, "GetCpuUserImageDescriptor"},
        });
    }

    ~INx2GraphicsSystemSession() override {
        if (m_syncpt_poll) {
            system.CoreTiming().UnscheduleEvent(m_syncpt_poll);
        }
        for (auto* event : m_syncpt_events) {
            if (event != nullptr) {
                m_context.CloseEvent(event);
            }
        }
    }

private:
    // Index of the first raw data word of a TIPC request: header (2 words), then an optional
    // special header with pid and copy/move handle words.
    static size_t TipcDataOffset(const u32* cmd_buf) {
        if ((cmd_buf[1] & 0x80000000) == 0) {
            return 2;
        }
        const u32 desc = cmd_buf[2];
        return 3 + ((desc & 1) ? 2 : 0) + ((desc >> 1) & 0xF) + ((desc >> 5) & 0xF);
    }

    void Stub(HLERequestContext& ctx) {
        const auto* cmd_buf = ctx.CommandBuffer();
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC {} [2]={:#x} [3]={:#x} [4]={:#x} [5]={:#x}",
                    ctx.GetCommand(), cmd_buf[2], cmd_buf[3], cmd_buf[4], cmd_buf[5]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // nn::codec::video::detail::ipc::Initialize sends 901, 902, 1001, 1002. 902 and 1002 return
    // a session handle at word 3 and the result at word 4; left to the auto stub, the handle was 0
    // and InitializeVp9Decoder failed with 2001-0114 (title 1 aborts when a movie starts).
    // Newer SDKs (nn::codec::video::detail::InitializeServerContext) send 906(u8, u64 decoder
    // library version) instead of 902; title 4 sends version 0x3'00030000 and aborted with
    // InvalidHandle at its first movie while 906 was auto-stubbed.
    void OpenVideoSession(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const bool second = ctx.GetCommand() == 1002;
        LOG_INFO(Service, "nx2:gsv TIPC {} [2]={:#x} [3]={:#x} [4]={:#x}, returning a video decoder session",
                 ctx.GetCommand(), w[2], w[3], w[4]);
        ReturnRawSession(system, ctx,
                         std::make_shared<INx2VideoSession>(system, m_objects,
                                                            second ? "nx2:vdec2" : "nx2:vdec"));
    }

    // nn::camera::sdk::CreateCameraContext sends 601() then 602(u64 1), which returns the camera
    // context session (handle at word 3, result at word 4). Auto-stubbed, 602 had no handle, the
    // SDK sent 603 (destroy) and title 1 (1.8.0) aborted with 2034-0000. Its commands fall
    // through to the nx2 auto stub.
    void OpenCameraSession(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 602, returning a nx2:camera session");
        ReturnRawSession(system, ctx,
                         std::make_shared<INx2StubInterface>(
                             system, INx2StubInterface::InternName("nx2:camera")));
    }

    // nn::chat::gsv::Initialize sends 1201 then 1202, the same pair as 901/902 and 1001/1002.
    // Left to the auto stub it aborted with 2162-0001 (GameChat), so 1202 gets a session too.
    void OpenChatSession(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 1202, returning a nx2:chat session");
        ReturnRawSession(system, ctx, std::make_shared<INx2ChatGsvSession>(system));
    }

    void OpenSyncptSession(HLERequestContext& ctx) {
        // Used by nn::syncpt::sdk::InitializeEx; handle at word 3, result at word 4.
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 202, returning a nx2:syncpt session");
        ReturnRawSession(system, ctx, std::make_shared<INx2SyncptSession>(system));
    }

    // nn::syncpt::sdk::InitializeEx sends two transfer memories (0x1000 bytes RW, then 0x2000
    // bytes the client maps read-only) and expects the server to have written a header into the
    // second one: u32 version (must be 1) at +0, u32 base id at +4, u32 count at +8 (the client
    // requires ((count + 0x40) >> 6) - 1 < 0x40), then offsets of: the u64 syncpoint values (RO,
    // +0xc), the RW "armed" summary word (+0x10), the RO summary word (+0x14), the RW armed bits
    // (+0x18) and the RO acknowledge bits (+0x1c).
    // To wait, the client makes its RW bit differ from the RO bit, signals the first returned
    // event (it attaches that handle as writable), and waits on the second until the server makes
    // the bits equal again; it then rereads the value and rearms if the threshold isn't reached.
    // Reply: the two handles (words 3 and 4), then the result.
    static constexpr u32 SyncptValuesOffset = 0x40;   // RO, u64 per syncpoint
    static constexpr u32 SyncptRoSummaryOffset = 0x1040;
    static constexpr u32 SyncptRoBitsOffset = 0x1100; // RO, up to 0x40 u64 words
    static constexpr u32 SyncptRwSummaryOffset = 0x0;
    static constexpr u32 SyncptRwBitsOffset = 0x100;  // RW, up to 0x40 u64 words
    static constexpr u32 HostSyncpoints = 192;        // Tegra::Host1x::SyncpointManager

    void AttachSyncptMemory(HLERequestContext& ctx) {
        // RequestParser doesn't skip TIPC handle words, so read the raw data words.
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        const u32 version_requested = cmd_buf[data];
        const u32 max_syncpoints = std::min<u32>(cmd_buf[data + 1], (0x1000 - 0x40) / 8);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 204, version={} max_syncpoints={:#x}",
                    version_requested, max_syncpoints);

        auto rw = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(0));
        auto ro = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(1));
        if (rw.IsNotNull() && ro.IsNotNull() && ro->GetSize() >= 0x2000) {
            m_syncpt_memory = &ro->GetOwner()->GetMemory();
            m_syncpt_rw = GetInteger(rw->GetSourceAddress());
            m_syncpt_ro = GetInteger(ro->GetSourceAddress());
            m_syncpt_count = max_syncpoints;
            auto& memory = *m_syncpt_memory;
            memory.Write32(m_syncpt_ro + 0x00, 1); // version
            memory.Write32(m_syncpt_ro + 0x04, 0); // base id (guess)
            memory.Write32(m_syncpt_ro + 0x08, max_syncpoints);
            memory.Write32(m_syncpt_ro + 0x0c, SyncptValuesOffset);
            memory.Write32(m_syncpt_ro + 0x10, SyncptRwSummaryOffset);
            memory.Write32(m_syncpt_ro + 0x14, SyncptRoSummaryOffset);
            memory.Write32(m_syncpt_ro + 0x18, SyncptRwBitsOffset);
            memory.Write32(m_syncpt_ro + 0x1c, SyncptRoBitsOffset);
            UpdateSyncptValues();
            if (!m_syncpt_poll) {
                m_syncpt_poll = Core::Timing::CreateEvent(
                    "nx2:gsv:SyncptPoll",
                    [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                        PollSyncpts();
                        return std::nullopt;
                    });
                system.CoreTiming().ScheduleLoopingEvent(SyncptPollInterval, SyncptPollInterval,
                                                         m_syncpt_poll);
            }
        } else {
            LOG_ERROR(Service, "nx2:gsv TIPC 204: missing syncpt transfer memory");
        }

        if (m_syncpt_events[0] == nullptr) {
            m_syncpt_events[0] = m_context.CreateEvent("nx2:gsv:SyncptRequest");
            m_syncpt_events[1] = m_context.CreateEvent("nx2:gsv:SyncptNotify");
        }
        IPC::ResponseBuilder rb{ctx, 2, 2, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, *m_syncpt_events[0]); // signalled by the client
        rb.PushCopyObjects(ctx, m_syncpt_events[1]->GetReadableEvent());
    }

    // Mirrors Eden's guest syncpoint values into the shared RO page. Returns true if any changed.
    bool UpdateSyncptValues() {
        auto& manager = system.Host1x().GetSyncpointManager();
        bool changed = false;
        for (u32 id = 0; id < std::min(m_syncpt_count, HostSyncpoints); ++id) {
            const u64 value = manager.GetGuestSyncpointValue(id);
            const u64 address = m_syncpt_ro + SyncptValuesOffset + id * 8;
            if (m_syncpt_memory->Read64(address) != value) {
                m_syncpt_memory->Write64(address, value);
                changed = true;
            }
        }
        return changed;
    }

    // Answers every armed wait: the value page is current, so the client rechecks its threshold
    // and rearms if it still has to wait. Acknowledging without a value change costs the client a
    // wakeup per poll, which keeps waits whose syncpoint we never see from hanging.
    void PollSyncpts() {
        if (m_syncpt_memory == nullptr) {
            return;
        }
        UpdateSyncptValues();
        bool acked = false;
        const u32 words = (m_syncpt_count + 63) / 64;
        for (u32 w = 0; w < words; ++w) {
            const u64 rw_bits = m_syncpt_memory->Read64(m_syncpt_rw + SyncptRwBitsOffset + w * 8);
            const u64 ro_address = m_syncpt_ro + SyncptRoBitsOffset + w * 8;
            if (m_syncpt_memory->Read64(ro_address) != rw_bits) {
                m_syncpt_memory->Write64(ro_address, rw_bits);
                acked = true;
            }
        }
        m_syncpt_memory->Write64(m_syncpt_ro + SyncptRoSummaryOffset,
                                 m_syncpt_memory->Read64(m_syncpt_rw + SyncptRwSummaryOffset));
        if (acked) {
            m_syncpt_events[1]->Signal(system.Kernel());
        }
    }

    // nn::gsv::shim::CalculateRequirements(u64* size, u64* alignment, preference, u32): the 0x40-byte
    // InterModuleUserImagePreference (words 2..17) and a u32 (word 18). A block-linear preference
    // (MakeInterModuleBlockUserImagePreference) holds kind 2 at word 3, (width - 1) | (height - 1)
    // << 16 at word 4, two u32s at words 6-7 and the pixel format at word 8. Reply: result, u64
    // size, u64 alignment; the client raises the alignment to at least 4 KiB and rounds the size.
    void CalculateRequirements(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const auto layout = Nx2CalculateImageLayout(w + 2);
        LOG_DEBUG(Service, "nx2:gsv 1510 kind={} {}x{} block={} pitch={:#x} format={} -> size={:#x} align={:#x}",
                  w[3], layout.width, layout.height, layout.block_height_log2, layout.pitch, w[8],
                  layout.size, layout.alignment);
        TipcReplyWords(ctx, {static_cast<u32>(layout.size), static_cast<u32>(layout.size >> 32),
                             static_cast<u32>(layout.alignment),
                             static_cast<u32>(layout.alignment >> 32)});
    }

    // nn::codec::CalculateDecoderImagePoolBuffer(size*, alignment*, VideoDecoderImagePreference,
    // u32 count) sends 991(preference[16], u32 count) and reads result, u64 size, u64 alignment;
    // the client raises the alignment to at least 4 KiB and rounds the size up to it. The
    // preference (MakeDecodeImagePreference) holds an output layout code at word 1 (1/4 for
    // format 3, which the movie player uses, read here as NV12) and (width - 1) | (height - 1) << 16
    // at word 2. Left to the auto stub, the reply words keep the request's, so the size became
    // 0x167027f'00000001 and title 1's movie player aborted (main+0x19e5b58) when its first movie
    // started. Sizes are block-linear NV12 (luma, then half-height chroma, 128-row blocks) or 4
    // bytes per pixel for the other codes; nothing is decoded into them yet.
    void CalculateDecoderImagePoolBuffer(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const auto layout = Nx2CalculateDecoderImageLayout(w + 2);
        const u32 count = std::max<u32>(w[18], 1);
        const u64 size = layout.size * count;
        const u64 alignment = 64 * 128;
        LOG_INFO(Service, "nx2:gsv 991 decoder image pool code={} {}x{} count={} -> size={:#x}",
                 layout.code, layout.width, layout.height, count, size);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32),
                             static_cast<u32>(alignment), static_cast<u32>(alignment >> 32)});
    }

    // 435(preference[16], u32 count): result, u64 size of a GPU native image.
    void CalculateNativeImageSize(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 size = Nx2CalculateImageLayout(w + 2).size * std::max<u32>(w[18], 1);
        LOG_DEBUG(Service, "nx2:gsv 435 count={} -> size={:#x}", w[18], size);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32)});
    }

    static u64 Pair(const u32* w, size_t i) {
        return u64{w[i]} | (u64{w[i + 1]} << 32);
    }

    // 111(u8 mode at word 2, u64 size): result, u32 native memory id.
    void CreateNativeMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 size = Pair(w, 3);
        std::scoped_lock lk{m_objects->mutex};
        const u32 id = m_objects->next_memory++;
        m_objects->memories[id] = {size, 0};
        LOG_INFO(Service, "nx2:gsv 111 CreateNativeMemory mode={} size={:#x} -> id {}", w[2] & 0xFF,
                 size, id);
        TipcReplyWords(ctx, {id});
    }

    void DestroyNativeMemory(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        std::scoped_lock lk{m_objects->mutex};
        m_objects->memories.erase(id);
        LOG_DEBUG(Service, "nx2:gsv 112 DestroyNativeMemory id {}", id);
        TipcReplyWords(ctx, {});
    }

    // 114(u32 id, u64 address, u64 size, u64 flags, u64 offset): back native memory with process
    // memory.
    void BindNativeMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        BindMemory(w[2], Pair(w, 3), Pair(w, 5), "114");
        TipcReplyWords(ctx, {});
    }

    // 423(u32 id, u64 address, u64 size, u32 parent GpuMemoryHandle, u64 offset, ...): back
    // native memory with part of GPU-accessible memory.
    void BindNativeGpuMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_DEBUG(Service, "nx2:gsv 423 parent={} offset={:#x}", w[7], Pair(w, 8));
        BindMemory(w[2], Pair(w, 3), Pair(w, 5), "423");
        TipcReplyWords(ctx, {});
    }

    void BindMemory(u32 id, u64 address, u64 size, const char* command) {
        std::scoped_lock lk{m_objects->mutex};
        auto& memory = m_objects->memories[id];
        memory.address = address;
        if (memory.size == 0) {
            memory.size = size;
        }
        LOG_INFO(Service, "nx2:gsv {} bind native memory {} at {:#x}+{:#x}", command, id, address,
                 size);
    }

    // 429(preference[16], u32 count, u32 memory id): result, u32 native image id.
    void CreateNativeImage(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        std::scoped_lock lk{m_objects->mutex};
        const u32 id = m_objects->next_image;
        auto& image = m_objects->images[id];
        image.count = std::max<u32>(w[18], 1);
        // nn::gs::sdk::CalculateNativeImageHandle(pool, i) is pool + i, so keep those free.
        m_objects->next_image += image.count;
        image.memory = w[19];
        std::copy_n(w + 2, image.preference.size(), image.preference.begin());
        image.size = Nx2CalculateImageLayout(w + 2).size;
        LOG_INFO(Service, "nx2:gsv 429 CreateNativeImage memory={} count={} size={:#x} -> id {}",
                 w[19], w[18], image.size, id);
        TipcReplyWords(ctx, {id});
    }

    // nn::chat::gsv::CalculateRequiredBufferMemory: 1291(preference[16], u32 count), the request
    // of 1510, replies result, u64 size, u64 alignment. Auto-stubbed, the size became 0x1000 and
    // the camera pool (1211) got memory for none of its images.
    void CalculateChatImagePoolSize(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const auto layout = Nx2CalculateImageLayout(w + 2);
        const u64 size = layout.size * std::max<u32>(w[18], 1);
        LOG_DEBUG(Service, "nx2:gsv 1291 {}x{} count={} -> size={:#x}", layout.width,
                  layout.height, w[18], size);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32),
                             static_cast<u32>(layout.alignment),
                             static_cast<u32>(layout.alignment >> 32)});
    }

    // 428(u32 native image handle) nn::gpu::sdk::QueryNativeImage and 426(u32 handle, u32, u32
    // address space, u8 permission) nn::gpu::sdk::MapNativeImage, used by NVN for images another
    // server created (GameChat's camera pool from 1211). Reply words 3-4 the GPU address of the
    // image (426 only; GPU == CPU address, see Nx2NativeObjects::Memory), 5-6 the u64 offset of
    // the image in its memory, 7-38 a GpuOunceUserImageDescriptor and 39 the flags. The SDK
    // requires descriptor byte 0x00 == 1, byte 0x20 == 1 and a non-zero size at 0x2c (offset at
    // 0x28); NVN reads (width - 1) | (height - 1) << 16 at 0x08 and the layout kind at 0x24 (1
    // takes RGBA formats 0x25/0x37/0x38, 2 the 0x99-0x9c formats). Block height and pitch use
    // the 429 preference words (0x10, 0x14), a guess.
    void QueryNativeImage(HLERequestContext& ctx) {
        const u32 handle = ctx.CommandBuffer()[2];
        std::array<u32, 16> preference{};
        u64 image_offset = 0;
        u64 memory_address = 0;
        {
            std::scoped_lock lk{m_objects->mutex};
            auto it = m_objects->images.upper_bound(handle);
            if (it != m_objects->images.begin()) {
                --it;
                if (handle - it->first < it->second.count) {
                    preference = it->second.preference;
                    image_offset = (handle - it->first) * it->second.size;
                    if (const auto memory = m_objects->memories.find(it->second.memory);
                        memory != m_objects->memories.end()) {
                        memory_address = memory->second.address;
                    }
                }
            }
        }
        const auto layout = Nx2CalculateImageLayout(preference.data());
        const u64 gpu_address = ctx.GetCommand() == 426 ? memory_address + image_offset : 0;
        std::array<u32, 37> words{};
        words[0] = static_cast<u32>(gpu_address);
        words[1] = static_cast<u32>(gpu_address >> 32);
        words[2] = static_cast<u32>(image_offset);
        words[3] = static_cast<u32>(image_offset >> 32);
        u32* const desc = words.data() + 4;
        desc[0] = 1;
        desc[1] = preference[1];
        desc[2] = preference[2];
        desc[4] = layout.block_height_log2;
        desc[5] = static_cast<u32>(layout.pitch / 64);
        desc[8] = 1;
        desc[9] = 1;
        desc[11] = static_cast<u32>(layout.size);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv {} handle={} {}x{} size={:#x} gpu={:#x}",
                    ctx.GetCommand(), handle, layout.width, layout.height, layout.size,
                    gpu_address);
        IPC::ResponseBuilder rb{ctx, static_cast<u32>(2 + words.size())};
        rb.PushRaw<u32>(ResultSuccess.raw);
        for (const u32 w : words) {
            rb.Push<u32>(w);
        }
    }

    // 911(preference[16], u32 count, u32 memory id) (nn::codec::InitializeDecoderImagePool):
    // result, the id of the first of `count` decoder output images; the client names image i of
    // the pool id + i. They share the pool's memory, one image size apart.
    void CreateDecoderImage(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u32 count = std::max<u32>(w[18], 1);
        const auto layout = Nx2CalculateDecoderImageLayout(w + 2);
        std::scoped_lock lk{m_objects->mutex};
        const u32 first = m_objects->next_image;
        m_objects->next_image += count;
        for (u32 i = 0; i < count; ++i) {
            auto& image = m_objects->images[first + i];
            image.memory = w[19];
            std::copy_n(w + 2, image.preference.size(), image.preference.begin());
            image.size = layout.size;
            image.offset = layout.size * i;
            image.decoder = true;
        }
        LOG_INFO(Service, "nx2:gsv 911 decoder images {}..{} code={} {}x{} in memory {}", first,
                 first + count - 1, layout.code, layout.width, layout.height, w[19]);
        TipcReplyWords(ctx, {first});
    }

    // 961(u32 image id): result, the 128-byte nn::gsv::CpuUserImageDescriptor that
    // nn::gs::sdk::ReadUserImage* (behind nn::codec::ReadDecoderOutputImage*) copy an image out
    // with: u8 kind at +0; for the pitch-linear kinds (8 RGBA, 9 BGRA, 10 NV12) the plane's
    // offset into the pool memory (u32 +0x8) and row pitch (u64 +0x10), NV12's chroma plane at
    // +0x18/+0x20. 11-13 are the block-linear kinds, not used here.
    void GetCpuImageDescriptor(HLERequestContext& ctx) {
        using Tegra::Host1x::Nx2PictureFormat;
        const u32 id = ctx.CommandBuffer()[2];
        std::array<u32, 32> words{};
        {
            std::scoped_lock lk{m_objects->mutex};
            const auto image = m_objects->images.find(id);
            if (image != m_objects->images.end() && image->second.decoder) {
                const auto layout = Nx2CalculateDecoderImageLayout(image->second.preference.data());
                const u64 offset = image->second.offset;
                words[0] = layout.format == Nx2PictureFormat::Nv12    ? 0xa
                           : layout.format == Nx2PictureFormat::Bgra8 ? 0x9
                                                                      : 0x8;
                words[2] = static_cast<u32>(offset);
                words[4] = static_cast<u32>(layout.pitch);
                if (layout.format == Nx2PictureFormat::Nv12) {
                    words[6] = static_cast<u32>(offset + layout.chroma_offset);
                    words[8] = static_cast<u32>(layout.pitch);
                }
            }
        }
        if (Nx2FirstVideoCommand("nx2:gsv", 961) || words[0] == 0) {
            LOG_INFO(Service, "nx2:gsv 961 image {} -> kind {:#x} offset {:#x} pitch {:#x}", id,
                     words[0], words[2], words[4]);
        }
        TipcReplyWords(ctx, words);
    }

    void DestroyNativeImage(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        std::scoped_lock lk{m_objects->mutex};
        m_objects->images.erase(id);
        TipcReplyWords(ctx, {});
    }

    // 315(u32 image, u32 count) / 314(u32 image): add/remove a window alias image pool.
    void ConfigureWindowImagePool(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        const bool create = ctx.GetCommand() == 315;
        std::scoped_lock lk{m_objects->mutex};
        if (auto it = m_objects->images.find(id); it != m_objects->images.end()) {
            it->second.window_pool = create;
        }
        LOG_INFO(Service, "nx2:gsv {} window image pool image {}", ctx.GetCommand(), id);
        TipcReplyWords(ctx, {});
    }

    void OpenViSession(HLERequestContext& ctx) {
        // Used by nn::vi2::sdk::Initialize; takes one u64, returns a session (handle at word 3,
        // result at word 4).
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 305 arg={:#x}, returning a nx2:vi2 session",
                    u64{cmd_buf[data]} | (u64{cmd_buf[data + 1]} << 32));
        ReturnRawSession(system, ctx, std::make_shared<INx2Vi2Session>(system, m_objects));
    }

    void OpenGpuSession(HLERequestContext& ctx) {
        // Used by nn::gpu::sdk::BootWithRemoteMemory; takes one u64, returns a session (handle at
        // word 3, result at word 4) that the GPU SDK keeps.
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 497 arg={:#x}, returning a nx2:gpu session",
                    u64{cmd_buf[data]} | (u64{cmd_buf[data + 1]} << 32));
        ReturnRawSession(system, ctx, std::make_shared<INx2GpuSession>(system));
    }

    void OpenNvsrvSession(HLERequestContext& ctx) {
        // The SDK reads the handle at word 3 and the result at word 4.
        // nvsrv speaks the nvdrv INvDrvServices protocol (Open/Ioctl/Close/...,
        // 13 = SetGraphicsFirmwareMemoryMarginEnabled), so serve it with Eden's nvdrv on the shared
        // module. The nx2: name keeps unknown commands auto-stubbed and logged.
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 1402, returning an nvdrv-backed nx2:nvsrv session");
        auto module = system.ServiceManager().GetService<Nvidia::NVDRV>("nvdrv", true)->GetModule();
        auto nvsrv = std::make_shared<Nvidia::NVDRV>(system, module, "nx2:nvsrv");
        nvsrv->InitializeForProcess(ctx.GetThread().GetOwnerProcess());
        ReturnRawSession(system, ctx, std::move(nvsrv));
    }

    static constexpr std::chrono::nanoseconds SyncptPollInterval{std::chrono::microseconds{500}};

    std::shared_ptr<Nx2NativeObjects> m_objects{std::make_shared<Nx2NativeObjects>()};
    KernelHelpers::ServiceContext m_context;
    std::array<Kernel::KEvent*, 2> m_syncpt_events{};
    Core::Memory::Memory* m_syncpt_memory{};
    u64 m_syncpt_rw{};
    u64 m_syncpt_ro{};
    u32 m_syncpt_count{};
    std::shared_ptr<Core::Timing::EventType> m_syncpt_poll;
};

} // namespace Service

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/assert.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/engines/puller.h"
#include "video_core/macro.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"

namespace Tegra::Control {
namespace {

// Match NVK/Nouveau's initial pushbuffer subchannel layout.
constexpr u32 Nvk3DSubchannel = 0;
constexpr u32 NvkComputeSubchannel = 1;
constexpr u32 NvkM2mfSubchannel = 2;
constexpr u32 Nvk2DSubchannel = 3;
constexpr u32 NvkCopySubchannel = 4;

// Switch 2 channels of a process share one TSG, whose graphics context holds the macro RAM.
std::shared_ptr<NativeMacroContext> Nx2MacroContext() {
    static const auto context = [] {
        auto created = std::make_shared<NativeMacroContext>();
        created->engine.native_turing = true;
        created->compute_engine.native_turing = true;
        return created;
    }();
    return context;
}

void BindNvkDefaultSubchannels(ChannelState::Payload& payload) {
    auto& dma_pusher = payload.dma_pusher;
    dma_pusher.BindSubchannel(&payload.maxwell_3d, Nvk3DSubchannel, Engines::EngineTypes::Maxwell3D);
    dma_pusher.BindSubchannel(&payload.kepler_compute, NvkComputeSubchannel,
                              Engines::EngineTypes::KeplerCompute);
    // Subchannel 2 is M2MF/inline-to-memory there (NVN uses it without binding a class);
    // KeplerMemory has the same LINE_LENGTH_IN/OFFSET_OUT/LAUNCH_DMA/LOAD_INLINE_DATA layout.
    dma_pusher.BindSubchannel(&payload.kepler_memory, NvkM2mfSubchannel,
                              Engines::EngineTypes::KeplerMemory);
    dma_pusher.BindSubchannel(&payload.fermi_2d, Nvk2DSubchannel, Engines::EngineTypes::Fermi2D);
    dma_pusher.BindSubchannel(&payload.maxwell_dma, NvkCopySubchannel,
                              Engines::EngineTypes::MaxwellDMA);
}

} // Anonymous namespace

ChannelState::Payload::Payload(Core::System& system, MemoryManager& memory_manager, ChannelState& channel_state)
    : maxwell_3d(memory_manager)
    , fermi_2d(memory_manager)
    , kepler_compute(memory_manager)
    , maxwell_dma(memory_manager)
    , kepler_memory(memory_manager)
    , nv01_timer(memory_manager)
    , dma_pusher(system, memory_manager, channel_state)
{}

ChannelState::ChannelState(s32 bind_id_)
    : bind_id{bind_id_}
{}

void ChannelState::Init(Core::System& system, u64 program_id_) {
    ASSERT(memory_manager);
    program_id = program_id_;
    payload.emplace(system, *memory_manager, *this);
    BindNvkDefaultSubchannels(*payload);
    if (g_nx2_ampere) {
        // Switch 2 command streams use Ampere classes with Turing+ (MME64) macros, and its
        // block-linear surfaces the console's GOB swizzle.
        memory_manager->EnableNativeTiling();
        const auto macros = Nx2MacroContext();
        payload->maxwell_3d.SetNativeMacroContext(macros);
        payload->maxwell_3d.SetNativeTuringMacroMode(true);
        payload->kepler_compute.SetNativeMacroMode(&macros->engine, macros->positions,
                                                   macros.get());
        payload->kepler_compute.SetNativeDataRam(&payload->maxwell_3d.GetNativeDataRam());
        payload->kepler_compute.SetGridMarker(&payload->maxwell_3d.grid_marker);
    }
    initialized = true;
}

void ChannelState::BindRasterizer(VideoCore::RasterizerInterface* rasterizer) {
    payload->dma_pusher.BindRasterizer(rasterizer);
    memory_manager->BindRasterizer(rasterizer);
    payload->maxwell_3d.BindRasterizer(rasterizer);
    payload->fermi_2d.BindRasterizer(rasterizer);
    payload->kepler_memory.BindRasterizer(rasterizer);
    payload->kepler_compute.BindRasterizer(rasterizer);
    payload->maxwell_dma.BindRasterizer(rasterizer);
    //payload->nv01_timer.BindRasterizer(rasterizer);
}

} // namespace Tegra::Control

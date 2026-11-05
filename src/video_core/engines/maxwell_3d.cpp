// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <fmt/ranges.h>
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/semaphore_ledger.h"
#include "video_core/textures/texture.h"
#ifdef ARCHITECTURE_x86_64
#include "video_core/macro_turing_jit.h"
#endif
#if defined(ARCHITECTURE_arm64) && defined(__ANDROID__)
#include "video_core/macro_turing_jit_arm64.h"
#endif

namespace Tegra::Engines {

/// First register id that is actually a Macro call.
constexpr u32 MacroRegistersStart = 0xE00;

Maxwell3D::Maxwell3D(MemoryManager& memory_manager_)
    : draw_manager()
    , memory_manager{memory_manager_}
#ifdef ARCHITECTURE_x86_64
    , macro_engine(bool(Settings::values.disable_macro_jit))
#else
    , macro_engine(true)
#endif
    , upload_state{memory_manager, regs.upload}
{
    dirty.flags.flip();
    InitializeRegisterDefaults();
    execution_mask.reset();
    for (size_t i = 0; i < execution_mask.size(); i++)
        execution_mask[i] = IsMethodExecutable(u32(i));
}

Maxwell3D::~Maxwell3D() = default;

void Maxwell3D::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
    upload_state.BindRasterizer(rasterizer_);
}

void Maxwell3D::InitializeRegisterDefaults() {
    // Initializes registers to their default values - what games expect them to be at boot. This is
    // for certain registers that may not be explicitly set by games.

    // Reset all registers to zero
    std::memset(&regs, 0, sizeof(regs));

    // Depth range near/far is not always set, but is expected to be the default 0.0f, 1.0f. This is
    // needed for ARMS.
    for (auto& viewport : regs.viewports) {
        viewport.depth_range_near = 0.0f;
        viewport.depth_range_far = 1.0f;
    }
    for (auto& viewport : regs.viewport_transform) {
        viewport.swizzle.x.Assign(Regs::ViewportSwizzle::PositiveX);
        viewport.swizzle.y.Assign(Regs::ViewportSwizzle::PositiveY);
        viewport.swizzle.z.Assign(Regs::ViewportSwizzle::PositiveZ);
        viewport.swizzle.w.Assign(Regs::ViewportSwizzle::PositiveW);
    }

    // Doom and Bomberman seems to use the uninitialized registers and just enable blend
    // so initialize blend registers with sane values
    regs.blend.color_op = Regs::Blend::Equation::Add_D3D;
    regs.blend.color_source = Regs::Blend::Factor::One_D3D;
    regs.blend.color_dest = Regs::Blend::Factor::Zero_D3D;
    regs.blend.alpha_op = Regs::Blend::Equation::Add_D3D;
    regs.blend.alpha_source = Regs::Blend::Factor::One_D3D;
    regs.blend.alpha_dest = Regs::Blend::Factor::Zero_D3D;
    for (auto& blend : regs.blend_per_target) {
        blend.color_op = Regs::Blend::Equation::Add_D3D;
        blend.color_source = Regs::Blend::Factor::One_D3D;
        blend.color_dest = Regs::Blend::Factor::Zero_D3D;
        blend.alpha_op = Regs::Blend::Equation::Add_D3D;
        blend.alpha_source = Regs::Blend::Factor::One_D3D;
        blend.alpha_dest = Regs::Blend::Factor::Zero_D3D;
    }
    regs.stencil_front_op.fail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.zfail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.zpass = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.func = Regs::ComparisonOp::Always_GL;
    regs.stencil_front_func_mask = 0xFFFFFFFF;
    regs.stencil_front_mask = 0xFFFFFFFF;
    regs.stencil_two_side_enable = 1;
    regs.stencil_back_op.fail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.zfail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.zpass = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.func = Regs::ComparisonOp::Always_GL;
    regs.stencil_back_func_mask = 0xFFFFFFFF;
    regs.stencil_back_mask = 0xFFFFFFFF;

    regs.depth_test_func = Regs::ComparisonOp::Always_GL;
    regs.gl_front_face = Regs::FrontFace::CounterClockWise;
    regs.gl_cull_face = Regs::CullFace::Back;

    // TODO(Rodrigo): Most games do not set a point size. I think this is a case of a
    // register carrying a default value. Assume it's OpenGL's default (1).
    regs.point_size = 1.0f;

    // TODO(bunnei): Some games do not initialize the color masks (e.g. Sonic Mania). Assuming a
    // default of enabled fixes rendering here.
    for (auto& color_mask : regs.color_mask) {
        color_mask.R.Assign(1);
        color_mask.G.Assign(1);
        color_mask.B.Assign(1);
        color_mask.A.Assign(1);
    }

    for (auto& format : regs.vertex_attrib_format) {
        format.constant.Assign(1);
    }

    // NVN games expect these values to be enabled at boot
    regs.rasterize_enable = 1;
    regs.color_target_mrt_enable = 1;
    regs.framebuffer_srgb = 1;
    regs.line_width_aliased = 1.0f;
    regs.line_width_smooth = 1.0f;
    regs.gl_front_face = Maxwell3D::Regs::FrontFace::ClockWise;
    regs.polygon_mode_back = Maxwell3D::Regs::PolygonMode::Fill;
    regs.polygon_mode_front = Maxwell3D::Regs::PolygonMode::Fill;

    shadow_state = regs;
}

bool Maxwell3D::IsMethodExecutable(u32 method) {
    if (method >= MacroRegistersStart) {
        return true;
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(draw.end):
    case MAXWELL3D_REG_INDEX(draw.begin):
    case MAXWELL3D_REG_INDEX(vertex_buffer.first):
    case MAXWELL3D_REG_INDEX(vertex_buffer.count):
    case MAXWELL3D_REG_INDEX(index_buffer.first):
    case MAXWELL3D_REG_INDEX(index_buffer.count):
    case MAXWELL3D_REG_INDEX(draw_inline_index):
    case MAXWELL3D_REG_INDEX(index_buffer32_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer16_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer8_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer32_first):
    case MAXWELL3D_REG_INDEX(index_buffer16_first):
    case MAXWELL3D_REG_INDEX(index_buffer8_first):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_first):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent):
    case MAXWELL3D_REG_INDEX(draw_texture.src_y0):
    case RootTables::SelectMethod:
    case RootTables::LoadMethod:
    case MAXWELL3D_REG_INDEX(wait_for_idle):
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
    case MAXWELL3D_REG_INDEX(falcon[4]):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
    case MAXWELL3D_REG_INDEX(topology_override):
    case MAXWELL3D_REG_INDEX(clear_surface):
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
    case MAXWELL3D_REG_INDEX(render_enable.mode):
    case MAXWELL3D_REG_INDEX(clear_report_value):
    case MAXWELL3D_REG_INDEX(sync_info):
    case MAXWELL3D_REG_INDEX(launch_dma):
    case MAXWELL3D_REG_INDEX(inline_data):
    case MAXWELL3D_REG_INDEX(fragment_barrier):
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return true;
    default:
        return false;
    }
}

void Maxwell3D::ProcessMacro(Core::System& system, u32 method, const u32* base_start, u32 amount, bool is_last_call) {
    if (executing_macro == 0) {
        // A macro call must begin by writing the macro method's register, not its argument.
        ASSERT((method % 2) == 0 && "Can't start macro execution by writing to the ARGS register");
        executing_macro = method;
    }

    macro_segments.emplace_back(current_dma_segment, amount);
    current_macro_dirty |= current_dirty;
    current_dirty = false;

    // Nearly every call arrives whole in one method run (title 1 makes ~1,000,000 a second):
    // copy it to the stack instead of growing the parameter vector. Still a copy: the command
    // list is often guest memory read in place, which the guest may reuse while the macro runs
    // (and the macro memo stores the parameters after it ran). Only accurate GPU mode rewrites
    // the parameters in place (RefreshParameters), so it keeps the vector.
    // EDEN_NX2_MACRO_COPY=1: always use the vector (for comparisons).
    static const bool always_copy = std::getenv("EDEN_NX2_MACRO_COPY") != nullptr;
    if (is_last_call && macro_params.empty() && amount <= 64 && !always_copy &&
        !Settings::IsGPULevelHigh()) {
        std::array<u32, 64> parameters;
        std::copy_n(base_start, amount, parameters.begin());
        ConsumeSink(system);
        CallMacroMethod(system, executing_macro, std::span<const u32>{parameters.data(), amount});
        macro_segments.clear();
        current_macro_dirty = false;
        return;
    }
    macro_params.insert(macro_params.end(), base_start, base_start + amount);

    // Call the macro when there are no more parameters in the command buffer
    if (is_last_call) {
        ConsumeSink(system);
        CallMacroMethod(system, executing_macro, macro_params);
        macro_params.clear();
        macro_segments.clear();
        current_macro_dirty = false;
    }
}

void Maxwell3D::RefreshParametersImpl() {
    if (!Settings::IsGPULevelHigh()) {
        return;
    }
    size_t current_index = 0;
    for (auto& segment : macro_segments) {
        if (segment.first == 0) {
            current_index += segment.second;
            continue;
        }
        memory_manager.ReadBlock(segment.first, &macro_params[current_index],
                                 sizeof(u32) * segment.second);
        current_index += segment.second;
    }
}

u32 Maxwell3D::GetMaxCurrentVertices() {
    u32 num_vertices = 0;
    for (size_t index = 0; index < Regs::NumVertexArrays; ++index) {
        const auto& array = regs.vertex_streams[index];
        if (array.enable == 0) {
            continue;
        }
        const auto& attribute = regs.vertex_attrib_format[index];
        if (attribute.constant) {
            num_vertices = (std::max)(num_vertices, 1U);
            continue;
        }
        const auto& limit = regs.vertex_stream_limits[index];
        const GPUVAddr gpu_addr_begin = array.Address();
        const GPUVAddr gpu_addr_end = limit.Address() + 1;
        const u64 stream_size = GetNativeMacroEngine().native_turing
                                    ? regs.NativeVertexStreamSize(index)
                                    : gpu_addr_end - gpu_addr_begin;
        const u32 address_size = static_cast<u32>((std::min<u64>)(stream_size, (std::numeric_limits<u32>::max)()));
        num_vertices = (std::max)(
            num_vertices, address_size / (std::max)(attribute.SizeInBytes(), array.stride.Value()));
        break;
    }
    return num_vertices;
}

size_t Maxwell3D::EstimateIndexBufferSize() {
    GPUVAddr start_address = regs.index_buffer.StartAddress();
    GPUVAddr end_address = regs.index_buffer.EndAddress();
    auto const byte_size = regs.index_buffer.FormatSizeInBytes();
    auto const max_size = 1ull << (byte_size * CHAR_BIT);
    auto const upper_cap = GetMaxCurrentVertices() * 4 * byte_size;
    auto const lower_cap = std::min<size_t>(size_t(end_address - start_address), upper_cap);
    return std::min<size_t>(memory_manager.GetMemoryLayoutSize(start_address, byte_size * max_size) / byte_size, lower_cap);
}

u32 Maxwell3D::ProcessShadowRam(u32 method, u32 argument) {
    // Keep track of the register value in shadow_state when requested.
    auto const c = shadow_state.shadow_ram_control;
    if (c == Regs::ShadowRamControl::Track || c == Regs::ShadowRamControl::TrackWithFilter)
        return shadow_state.reg_array[method] = argument;
    else if (c == Regs::ShadowRamControl::Replay)
        return shadow_state.reg_array[method];
    return argument;
}

void Maxwell3D::ConsumeSinkImpl(Core::System& system) {
    const auto control = shadow_state.shadow_ram_control;
    // Sunk methods are state writes only (not in execution_mask): skip unchanged values, as
    // CallMethod does. A deferred instanced draw is issued before the first write that is not
    // part of its loop (ProcessDirtyRegisters); without one when the sink is consumed, none can
    // appear while it is (only draw methods make one), so the check is made once.
    const bool skip_same = SkipSameValueWrites();
    const bool deferred_draw = draw_manager.HasDeferredDraw();
    const auto process = [&](u32 method, u32 value) {
        if (skip_same && regs.reg_array[method] == value) {
            return;
        }
        if (deferred_draw) [[unlikely]] {
            ProcessDirtyRegisters(method, value);
            return;
        }
        regs.reg_array[method] = value;
        for (auto const& table : dirty.tables)
            dirty.flags[table[method]] = true;
    };
    if (control == Regs::ShadowRamControl::Track || control == Regs::ShadowRamControl::TrackWithFilter) {
        for (auto [method, value] : method_sink) {
            shadow_state.reg_array[method] = value;
            process(method, value);
        }
    } else if (control == Regs::ShadowRamControl::Replay) {
        for (auto [method, value] : method_sink)
            process(method, shadow_state.reg_array[method]);
    } else {
        for (auto [method, value] : method_sink)
            process(method, value);
    }
    method_sink.clear();
}

void Maxwell3D::ProcessDirtyRegisters(u32 method, u32 argument) {
    // The instances after the first are drawn together once their loop ends, and they must see
    // the state they were begun with: NVN's draw macros reset the base instance (and its copy in
    // c[0][0x644]) right after the loop, before the macro returns.
    if (draw_manager.HasDeferredDraw() && !DrawManager::IsInstanceLoopMethod(method)) [[unlikely]] {
        draw_manager.DrawDeferred(*this, true);
    }
    regs.reg_array[method] = argument;
    for (auto const& table : dirty.tables)
        dirty.flags[table[method]] = true;
}

void Maxwell3D::ProcessMethodCall(u32 method, u32 argument, u32 nonshadow_argument, bool is_last_call) {
    if (GetNativeMacroEngine().native_turing && method == MAXWELL3D_REG_INDEX(clear_surface)) {
        LOG_DEBUG(HW_GPU, "Native GPU surface clear: value={:#x}", argument);
    }
    switch (method) {
    case RootTables::SelectMethod:
        root_tables.selected = argument & 7;
        root_tables.offset = (argument >> 8) & 0xff;
        return;
    case RootTables::LoadMethod:
        if (root_tables.offset < 256 && root_tables.offset % 4 == 0) {
            root_tables.tables[root_tables.selected][root_tables.offset / 4] = argument;
            root_tables.loaded = true;
        }
        root_tables.offset += 4;
        return;
    case MAXWELL3D_REG_INDEX(wait_for_idle):
        return rasterizer->WaitForIdle();
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
        shadow_state.shadow_ram_control = static_cast<Regs::ShadowRamControl>(nonshadow_argument);
        return;
    // With macros on the split GPU thread's front (FrontWrite), the front loads and binds them.
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
        if (front_macros) return;
        return GetNativeMacroEngine().ClearCode(regs.load_mme.instruction_ptr);
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
        if (front_macros) return;
        return GetNativeMacroEngine().AddCode(regs.load_mme.instruction_ptr, argument);
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
        if (front_macros) {
            ++regs.load_mme.start_address_ptr;
            return;
        }
        return ProcessMacroBind(argument);
    case MAXWELL3D_REG_INDEX(falcon[4]):
        return ProcessFirmwareCall4();
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
        return ProcessCBData(argument);
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
        return ProcessCBBind(0);
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
        return ProcessCBBind(1);
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
        return ProcessCBBind(2);
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
        return ProcessCBBind(3);
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
        return ProcessCBBind(4);
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
        return ProcessQueryGet();
    case MAXWELL3D_REG_INDEX(render_enable.mode):
        return ProcessQueryCondition();
    case MAXWELL3D_REG_INDEX(clear_report_value):
        return ProcessCounterReset();
    case MAXWELL3D_REG_INDEX(sync_info):
        return ProcessSyncPoint();
    case MAXWELL3D_REG_INDEX(launch_dma):
        return upload_state.ProcessExec(regs.launch_dma.memory_layout.Value() == Regs::LaunchDMA::Layout::Pitch);
    case MAXWELL3D_REG_INDEX(inline_data):
        upload_state.ProcessData(argument, is_last_call);
        return;
    case MAXWELL3D_REG_INDEX(fragment_barrier):
        return rasterizer->FragmentBarrier();
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
        rasterizer->InvalidateGPUCache();
        return rasterizer->WaitForIdle();
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return rasterizer->TiledCacheBarrier();
    default:
        draw_manager.ProcessMethodCall(*this, method, argument);
        break;
    }
}

void Maxwell3D::CallMacroMethod(Core::System& system, u32 method, std::span<const u32> parameters) {
    // Reset the current macro.
    executing_macro = 0;

    // Lookup the macro offset
    const u32 entry =
        ((method - MacroRegistersStart) >> 1) % static_cast<u32>(GetNativeMacroPositions().size());

    // Execute the current macro.
    GetNativeMacroEngine().Execute(system, *this, GetNativeMacroPositions()[entry], parameters);
    draw_manager.DrawDeferred(*this);
}

// EDEN_NX2_3D_METHODS: log the first write to each 3D register (finds methods Eden doesn't model).
static const bool g_trace_first_method_writes = std::getenv("EDEN_NX2_3D_METHODS") != nullptr;

void Maxwell3D::TraceFirstMethodWrite(u32 method, u32 argument) {
    static std::array<bool, Regs::NUM_REGS> seen{};
    if (method < seen.size() && !seen[method]) {
        seen[method] = true;
        LOG_INFO(HW_GPU, "3D method first write: {:#x} (byte {:#x}) = {:#x}", method, method * 4, argument);
    }
}

void Maxwell3D::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    // It is an error to write to a register other than the current macro's ARG register before
    // it has finished execution.
    if (executing_macro != 0 && method != executing_macro + 1) {
        DumpRecentWrites(method);
    }
    if (executing_macro != 0) {
        ASSERT(method == executing_macro + 1);
    }
    RecordWrite(method, method_argument, 1, is_last_call ? 1 : 2);

    // Methods after 0xE00 are special, they're actually triggers for some microcode that was
    // uploaded to the GPU during initialization.
    if (method >= MacroRegistersStart) {
        ProcessMacro(system, method, &method_argument, 1, is_last_call);
        return;
    }
    if (g_trace_first_method_writes) [[unlikely]] {
        TraceFirstMethodWrite(method, method_argument);
    }

    ASSERT(method < Regs::NUM_REGS && "Invalid Maxwell3D register, increase the size of the Regs structure");
    if (VideoCommon::Nx2Profile::Enabled()) [[unlikely]] {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::MethodWrites, 1);
        if (regs.reg_array[method] == method_argument) {
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::SameValueWrites, 1);
        }
    }
    if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && front_macros) [[unlikely]] {
        back_control_writes[back_control_count++ % back_control_writes.size()] = {
            method_argument, static_cast<u32>(shadow_state.shadow_ram_control), is_last_call ? 1U : 0U};
    }
    const u32 argument = ProcessShadowRam(method, method_argument);
    // A state register written with the value it holds changes nothing: no dirty flags (they
    // would make the next draw re-check that state), no method processing. Title 1 makes ~6
    // million 3D register writes a second, ~60% of them same-value, most from macro replays.
    // Methods with side effects (execution_mask: draws, clears, constant buffer data and
    // binds, semaphores, ...) always run. EDEN_NX2_SKIP_SAME=0 turns it off.
    if (!execution_mask[method]) {
        // ProcessMethodCall (and the draw manager behind it) acts only on executable methods.
        if (!SkipSameValueWrites() || regs.reg_array[method] != argument) {
            ProcessDirtyRegisters(method, argument);
        }
        return;
    }
    ProcessDirtyRegisters(method, argument);
    ProcessMethodCall(method, argument, method_argument, is_last_call);
}

void Maxwell3D::DumpRecentWrites(u32 method) {
    static u32 dumps{};
    if (dumps++ >= 4) {
        return;
    }
    LOG_ERROR(HW_GPU, "Write to {:#x} while macro {:#x} collects parameters ({} so far); last writes:",
              method, executing_macro, macro_params.size());
    if (!record_write_history) {
        LOG_ERROR(HW_GPU, "Register write history disabled; EDEN_NX2_3D_HISTORY=1 enables it");
        return;
    }
    for (size_t i = 0; i < recent_writes.size(); ++i) {
        const auto& w = recent_writes[(recent_write_index + i) % recent_writes.size()];
        if (w.amount != 0) {
            LOG_ERROR(HW_GPU, "  method {:#x} arg {:#x} amount {} pending {}", w.method, w.argument,
                      w.amount, w.pending);
        }
    }
}

void Maxwell3D::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    RecordWrite(method, amount ? base_start[0] : 0, amount, methods_pending);
    // Methods after 0xE00 are special, they're actually triggers for some microcode that was
    // uploaded to the GPU during initialization.
    if (method >= MacroRegistersStart) {
        ProcessMacro(system, method, base_start, amount, amount == methods_pending);
        return;
    }
    // As in ProcessDirtyRegisters: the paths below write without it.
    if (draw_manager.HasDeferredDraw() && !DrawManager::IsInstanceLoopMethod(method)) [[unlikely]] {
        draw_manager.DrawDeferred(*this, true);
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
        ProcessCBMultiData(base_start, amount);
        break;
    case MAXWELL3D_REG_INDEX(inline_data): {
        ASSERT(methods_pending == amount);
        upload_state.ProcessData(base_start, amount);
        return;
    }
    case MAXWELL3D_REG_INDEX(draw_inline_index):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
        if (shadow_state.shadow_ram_control != Regs::ShadowRamControl::Replay) {
            ProcessInlineIndexMultiData(method, base_start, amount);
            break;
        }
        [[fallthrough]];
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

void Maxwell3D::ProcessMacroUpload(u32 data) {
    GetNativeMacroEngine().AddCode(regs.load_mme.instruction_ptr++, data);
}

void Maxwell3D::ProcessMacroBind(u32 data) {
    const auto positions = GetNativeMacroPositions();
    const u32 index = regs.load_mme.start_address_ptr++;
    if (index >= positions.size()) {
        static std::atomic<u32> rejected{};
        if (rejected.fetch_add(1) < 8) {
            LOG_ERROR(HW_GPU,
                      "Rejecting out-of-range macro bind: index={:#x}, entries={}, data={:#x}",
                      index, positions.size(), data);
        }
        return;
    }
    positions[index] = data;
}

void Maxwell3D::ProcessFirmwareCall4() {
    LOG_DEBUG(HW_GPU, "(STUBBED) called");

    // Firmware call 4 is a blob that changes some registers depending on its parameters.
    // These registers don't affect emulation and so are stubbed by setting 0xd00 to 1.
    regs.shadow_scratch[0] = 1;
}

void Maxwell3D::StampQueryResult(Core::System& system, u64 payload, bool long_query) {
    const GPUVAddr sequence_address{regs.report_semaphore.Address()};
    if (long_query) {
        memory_manager.Write<u64>(sequence_address + sizeof(u64), system.GPU().GetTicks());
        memory_manager.Write<u64>(sequence_address, payload);
    } else {
        memory_manager.Write<u32>(sequence_address, static_cast<u32>(payload));
    }
}

void Maxwell3D::ProcessQueryGet() {
    VideoCommon::QueryPropertiesFlags flags{};
    if (regs.report_semaphore.query.short_query == 0) {
        flags |= VideoCommon::QueryPropertiesFlags::HasTimeout;
    }
    const GPUVAddr sequence_address{regs.report_semaphore.Address()};
    const VideoCommon::QueryType query_type =
        static_cast<VideoCommon::QueryType>(regs.report_semaphore.query.report.Value());
    const u32 payload = regs.report_semaphore.payload;
    const u32 subreport = regs.report_semaphore.query.sub_report;
    switch (regs.report_semaphore.query.operation) {
    case Regs::ReportSemaphore::Operation::Release:
        if (regs.report_semaphore.query.short_query != 0) {
            flags |= VideoCommon::QueryPropertiesFlags::IsAFence;
        }
        rasterizer->Query(sequence_address, query_type, flags, payload, subreport);
        break;
    case Regs::ReportSemaphore::Operation::Acquire:
        // TODO(Blinkhawk): Under this operation, the GPU waits for the CPU to write a value that
        // matches the current payload.
        UNIMPLEMENTED_MSG("Unimplemented query operation ACQUIRE");
        break;
    case Regs::ReportSemaphore::Operation::ReportOnly:
        rasterizer->Query(sequence_address, query_type, flags, payload, subreport);
        break;
    case Regs::ReportSemaphore::Operation::Trap:
        UNIMPLEMENTED_MSG("Unimplemented query operation TRAP");
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown query operation");
        break;
    }
}

void Maxwell3D::ProcessQueryCondition() {
    if (rasterizer->AccelerateConditionalRendering()) {
        execute_on = true;
        return;
    }
    const GPUVAddr condition_address{regs.render_enable.Address()};
    switch (regs.render_enable_override) {
    case Regs::RenderEnable::Override::AlwaysRender:
        execute_on = true;
        break;
    case Regs::RenderEnable::Override::NeverRender:
        execute_on = false;
        break;
    case Regs::RenderEnable::Override::UseRenderEnable: {
        switch (regs.render_enable.mode) {
        case Regs::RenderEnable::Mode::True: {
            execute_on = true;
            break;
        }
        case Regs::RenderEnable::Mode::False: {
            execute_on = false;
            break;
        }
        case Regs::RenderEnable::Mode::Conditional: {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence != 0U && cmp.initial_mode != 0U;
            break;
        }
        case Regs::RenderEnable::Mode::IfEqual: {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence == cmp.current_sequence &&
                         cmp.initial_mode == cmp.current_mode;
            break;
        }
        case Regs::RenderEnable::Mode::IfNotEqual: {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence != cmp.current_sequence ||
                         cmp.initial_mode != cmp.current_mode;
            break;
        }
        default: {
            UNIMPLEMENTED_MSG("Uninplemented Condition Mode!");
            execute_on = true;
            break;
        }
        }
        break;
    }
    }
}

void Maxwell3D::ProcessCounterReset() {
    const auto query_type = [clear_report = regs.clear_report_value]() {
        switch (clear_report) {
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::ZPassPixelCount:
            return VideoCommon::QueryType::ZPassPixelCount64;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::StreamingPrimitivesSucceeded:
            return VideoCommon::QueryType::StreamingPrimitivesSucceeded;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::PrimitivesGenerated:
            return VideoCommon::QueryType::PrimitivesGenerated;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::VtgPrimitivesOut:
            return VideoCommon::QueryType::VtgPrimitivesOut;
        default:
            LOG_DEBUG(HW_GPU, "Unimplemented counter reset={}", clear_report);
            return VideoCommon::QueryType::Payload;
        }
    }();
    rasterizer->ResetCounter(query_type);
}

void Maxwell3D::ProcessSyncPoint() {
    if (GetNativeMacroEngine().native_turing) {
        LOG_DEBUG(HW_GPU, "Native 3D syncpoint increment: id={}, raw={:#x}", regs.sync_info.sync_point.Value(), regs.reg_array[MAXWELL3D_REG_INDEX(sync_info)]);
    }
    const u32 sync_point = regs.sync_info.sync_point.Value();
    [[maybe_unused]] const u32 cache_flush = regs.sync_info.clean_l2.Value();
    rasterizer->SignalSyncPoint(sync_point);
}

void Maxwell3D::ProcessCBBind(size_t stage_index) {
    // Bind the buffer currently in CB_ADDRESS to the specified index in the desired shader
    // stage.
    const auto& bind_data = regs.bind_groups[stage_index];
    static std::array<std::array<bool, 32>, 5> seen{};
    if (!seen[stage_index][bind_data.shader_slot]) {
        seen[stage_index][bind_data.shader_slot] = true;
        LOG_INFO(HW_GPU, "Constant buffer bind: stage {} slot {} address {:#x} size {:#x}", stage_index,
                 bind_data.shader_slot.Value(), regs.const_buffer.Address(), regs.const_buffer.size);
    }
    if (bind_data.shader_slot >= Regs::MaxConstBuffers) {
        return; // slot field is 5 bits; the state array holds 18
    }
    auto& buffer = state.shader_stages[stage_index].const_buffers[bind_data.shader_slot];
    buffer.enabled = bind_data.valid.Value() != 0;
    buffer.address = regs.const_buffer.Address();
    buffer.size = regs.const_buffer.size;

    const bool is_enabled = bind_data.valid.Value() != 0;
    if (!is_enabled) {
        rasterizer->DisableGraphicsUniformBuffer(stage_index, bind_data.shader_slot);
        return;
    }
    const GPUVAddr gpu_addr = regs.const_buffer.Address();
    const u32 size = regs.const_buffer.size;
    rasterizer->BindGraphicsUniformBuffer(stage_index, bind_data.shader_slot, gpu_addr, size);
}

void Maxwell3D::ProcessCBMultiData(const u32* start_base, u32 amount) {
    // Write the input value to the current const buffer at the current position.
    const GPUVAddr buffer_address = regs.const_buffer.Address();
    ASSERT(buffer_address != 0);

    // Don't allow writing past the end of the buffer.
    const size_t copy_size = amount * sizeof(u32);
    const size_t room = regs.const_buffer.offset < regs.const_buffer.size
                            ? regs.const_buffer.size - regs.const_buffer.offset
                            : 0;
    const size_t write_size = std::min(copy_size, room / sizeof(u32) * sizeof(u32));
    if (write_size != 0) {
        const GPUVAddr address{buffer_address + regs.const_buffer.offset};
        memory_manager.WriteBlockCached(address, start_base, write_size);
    }
    if (write_size != copy_size) {
        static std::atomic<bool> warned{};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            LOG_ERROR(HW_GPU,
                      "Dropping constant buffer words past the end: offset={:#x}, size={:#x}, "
                      "bytes={:#x}, written={:#x}",
                      regs.const_buffer.offset, regs.const_buffer.size, copy_size, write_size);
        }
    }

    // Increment the current buffer position.
    regs.const_buffer.offset += static_cast<u32>(copy_size);
}

void Maxwell3D::ProcessCBData(u32 value) {
    ProcessCBMultiData(&value, 1);
}

void Maxwell3D::ProcessInlineIndexMultiData(u32 method, const u32* start_base, u32 amount) {
    if (amount == 0) {
        return;
    }
    const u32 argument = ProcessShadowRam(method, start_base[amount - 1]);
    ProcessDirtyRegisters(method, argument);
    draw_manager.SetInlineIndexBuffer(*this, method, start_base, amount);
}

Texture::TICEntry Maxwell3D::GetTICEntry(u32 tic_index) const {
    const GPUVAddr tic_address_gpu{regs.tex_header.Address() +
                                   tic_index * sizeof(Texture::TICEntry)};
    Texture::TICEntry tic_entry;
    memory_manager.ReadBlockUnsafe(tic_address_gpu, &tic_entry, sizeof(Texture::TICEntry));
    return tic_entry;
}

Texture::TSCEntry Maxwell3D::GetTSCEntry(u32 tsc_index) const {
    const GPUVAddr tsc_address_gpu{regs.tex_sampler.Address() +
                                   tsc_index * sizeof(Texture::TSCEntry)};
    Texture::TSCEntry tsc_entry;
    memory_manager.ReadBlockUnsafe(tsc_address_gpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

u32 Maxwell3D::GetRegisterValue(u32 method) const {
    ASSERT(method < Regs::NUM_REGS && "Invalid Maxwell3D register");
    return regs.reg_array[method];
}

void Maxwell3D::SetHLEReplacementAttributeType(u32 bank, u32 offset,
                                               HLEReplacementAttributeType name) {
    const u64 key = (static_cast<u64>(bank) << 32) | offset;
    replace_table.emplace(key, name);
}

// Split GPU thread with macros on its front half (gpu_split.h, EDEN_NX2_SPLIT=2). The front keeps
// what macros read: the registers as the back thread will hold them when it reaches the same
// point of the method stream. That is every write the front sends, with the shadow RAM applied,
// plus the few changes the back makes on its own (constant buffer offset, shadow_scratch[0] on
// falcon[4], the macro bind pointer); the front loads and binds the macro code itself.
struct Maxwell3D::FrontState {
    std::array<ControlWrite, 16> control_writes{};
    u32 control_count{};
    std::array<u32, Regs::NUM_REGS> regs{};
    std::array<u32, Regs::NUM_REGS> shadow{};
    Regs::ShadowRamControl control{};
    u32 executing_macro{};
    std::vector<u32> parameters;
    u64 calls{};
    /// A method the draw manager handles was sent since the back last ran DrawDeferred: only
    /// then can it have a deferred instanced draw to issue at the end of a macro.
    bool draw_pending{true};
    bool skip_same{}; ///< SkipSameValueWrites() && FrontSkipSame(), read once
    /// Per bind group: the raw config and constant buffer size/address of the last bind sent;
    /// a bind equal to it changes nothing on the back (ProcessCBBind) and is not sent.
    std::array<std::array<u32, 4>, 5> last_bind{};
#if defined(ARCHITECTURE_arm64) && defined(__ANDROID__)
    /// Methods whose emits the macro JIT must always call back for (TuringMacroJit::Elision):
    /// executable ones, the MME DMA reads (FrontDmaRead) and the macro range.
    std::array<u8, TuringMacroJit::MustCallMethods> jit_must_call{};
    TuringMacroJit::Elision elision{};
#endif
};

namespace {

// EDEN_NX2_SPLIT_VERIFY=1: every 4096 front macro calls, wait for the back and compare the
// front's registers with the back's (logs and resynchronizes differences).
bool FrontVerifyEnabled() {
    static const bool verify = std::getenv("EDEN_NX2_SPLIT_VERIFY") != nullptr;
    return verify;
}

// Executable methods ProcessMethodCall handles itself; the rest go to the draw manager.
bool FrontHandledOutsideDrawManager(u32 method) {
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    if (method >= cb_data && method < cb_data + 16) {
        return true;
    }
    switch (method) {
    case Maxwell3D::RootTables::SelectMethod:
    case Maxwell3D::RootTables::LoadMethod:
    case MAXWELL3D_REG_INDEX(wait_for_idle):
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
    case MAXWELL3D_REG_INDEX(falcon[4]):
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
    case MAXWELL3D_REG_INDEX(render_enable.mode):
    case MAXWELL3D_REG_INDEX(clear_report_value):
    case MAXWELL3D_REG_INDEX(sync_info):
    case MAXWELL3D_REG_INDEX(launch_dma):
    case MAXWELL3D_REG_INDEX(inline_data):
    case MAXWELL3D_REG_INDEX(fragment_barrier):
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return true;
    default:
        return false;
    }
}

// EDEN_NX2_JIT_SKIP_SAME=0: the ARM64 macro JIT calls back for every emit; the same-value
// writes are then skipped in FrontWrite as before (A/B switch for the in-JIT elision).
bool JitSkipSameEnabled() {
    static const bool skip = [] {
        const char* value = std::getenv("EDEN_NX2_JIT_SKIP_SAME");
        return value == nullptr || value[0] != '0';
    }();
    return skip;
}

// EDEN_NX2_CB_BIND_SKIP=0: send a constant buffer bind even when its stage's bind group holds
// the same configuration, buffer address and size as the last bind sent (ProcessCBBind would
// set the same binding again).
bool CbBindSkipEnabled() {
    static const bool skip = [] {
        const char* value = std::getenv("EDEN_NX2_CB_BIND_SKIP");
        return value == nullptr || value[0] != '0';
    }();
    return skip;
}

constexpr u32 BindGroupStride = MAXWELL3D_REG_INDEX(bind_groups[1].raw_config) -
                                MAXWELL3D_REG_INDEX(bind_groups[0].raw_config);

// EDEN_NX2_CB_BATCH=0: a front macro's constant buffer data words (CB_DATA) go to the back one
// by one (CallMethod each) instead of as one multi-method entry per run.
bool CbBatchEnabled() {
    static const bool batch = [] {
        const char* value = std::getenv("EDEN_NX2_CB_BATCH");
        return value == nullptr || value[0] != '0';
    }();
    return batch;
}

// EDEN_NX2_MACRO_SINK=0: the back runs every write a front macro makes through CallMethod at
// once (GpuSplit::MacroWrite), instead of putting state writes in its method sink like the
// pushbuffer's and applying them before the next executable method.
bool MacroSinkEnabled() {
    static const bool sink = [] {
        const char* value = std::getenv("EDEN_NX2_MACRO_SINK");
        return value == nullptr || value[0] != '0';
    }();
    return sink;
}

// EDEN_NX2_SPLIT_SKIP=0: send state writes that keep the register's value too.
bool FrontSkipSame() {
    static const bool skip = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT_SKIP");
        return value == nullptr || value[0] != '0';
    }();
    return skip;
}

// A report semaphore write the front just sent (ProcessQueryGet on the back, in ring order). The
// acquire ledger (semaphore_ledger.h) records a payload release, so acquires it meets need not
// drain the back, and forgets the words any other report writes. From
// sylvieistaken/selevet-eglantine bce3c83739.
void NoteFrontReport(MemoryManager& memory_manager, const u32* regs, u32 query) {
    using Ledger = Nx2SemaphoreLedger;
    using Report = Maxwell3D::Regs::ReportSemaphore;
    Report report{};
    report.address_high = regs[MAXWELL3D_REG_INDEX(report_semaphore.address_high)];
    report.address_low = regs[MAXWELL3D_REG_INDEX(report_semaphore.address_low)];
    report.query.raw = query;
    const auto operation = report.query.operation.Value();
    if (operation != Report::Operation::Release && operation != Report::Operation::ReportOnly) {
        return;
    }
    const auto device = memory_manager.GpuToCpuAddress(report.Address());
    if (!device) {
        return;
    }
    // MME DMA reads of the report (occlusion predicates) wait for it (FrontDmaRead).
    VideoCommon::GpuSplit::Get().NotePendingWrite(*device, 16);
    if (!Ledger::Enabled()) {
        return;
    }
    auto& ledger = Ledger::Get();
    if (operation == Report::Operation::Release &&
        report.query.report.Value() == Report::Report::Payload &&
        report.query.reduction_enable == 0) {
        ledger.NoteRelease(*device, regs[MAXWELL3D_REG_INDEX(report_semaphore.payload)],
                           VideoCommon::GpuSplit::Get().WritePosition());
    } else {
        ledger.Forget(*device);
    }
    if (report.query.short_query == 0) {
        // A long report writes 16 bytes: the value's high word and a timestamp.
        for (u64 offset = 4; offset < 16; offset += 4) {
            ledger.Forget(*device + offset);
        }
    }
}

// The device pages the inline upload in progress writes, noted again with each data word sent
// (the back writes them with the last one), so MME DMA reads of them wait for it.
struct FrontUpload {
    std::array<u64, 64> pages{};
    u32 count{};
    bool anywhere{}; ///< Too large to list
};
FrontUpload g_front_upload; // the front thread's

void NoteFrontUploadStart(MemoryManager& memory_manager, const u32* regs) {
    Upload::Registers upload{};
    std::memcpy(&upload, regs + MAXWELL3D_REG_INDEX(upload), sizeof(upload));
    const u64 lines = (std::max)(upload.line_count, 1U);
    const u64 size = (std::max)({u64{upload.line_length_in} * lines, u64{upload.dest.pitch} * lines,
                                 u64{upload.dest.width} * (std::max)(upload.dest.height, 1U) *
                                     (std::max)(upload.dest.depth, 1U)});
    FrontUpload& state = g_front_upload;
    state.count = 0;
    state.anywhere = false;
    const GPUVAddr begin = upload.dest.Address() & ~GPUVAddr{0xfff};
    const GPUVAddr end = upload.dest.Address() + (std::max<u64>)(size, 1);
    for (GPUVAddr page = begin; page < end; page += 0x1000) {
        if (state.count == state.pages.size()) {
            state.anywhere = true;
            return;
        }
        if (const auto device = memory_manager.GpuToCpuAddress(page)) {
            state.pages[state.count++] = *device;
        }
    }
}

void NoteFrontUploadWord() {
    auto& split = VideoCommon::GpuSplit::Get();
    const FrontUpload& state = g_front_upload;
    if (state.anywhere) {
        split.NotePendingWriteAnywhere();
        return;
    }
    for (u32 i = 0; i < state.count; ++i) {
        split.NotePendingWrite(state.pages[i], 1);
    }
}

} // Anonymous namespace

Maxwell3D::FrontState& Maxwell3D::Front(Core::System& system, VideoCommon::GpuSplit& split) {
    if (!front) [[unlikely]] {
        // Copy the back's state and take the macros over while it is idle (state writes it
        // still holds in its method sink first).
        split.Drain();
        ConsumeSink(system);
        front = std::make_unique<FrontState>();
        std::memcpy(front->regs.data(), regs.reg_array.data(), sizeof(front->regs));
        std::memcpy(front->shadow.data(), shadow_state.reg_array.data(), sizeof(front->shadow));
        front->control = shadow_state.shadow_ram_control;
        front->skip_same = SkipSameValueWrites() && FrontSkipSame();
        for (auto& bind : front->last_bind) {
            bind.fill(~u32{0});
        }
#if defined(ARCHITECTURE_arm64) && defined(__ANDROID__)
        for (u32 method = 0; method < front->jit_must_call.size(); ++method) {
            const bool dma_read = method == 0x157 || method == 0x158;
            front->jit_must_call[method] =
                method >= Regs::NUM_REGS || execution_mask[method] || dma_read ? 1 : 0;
        }
#endif
        front->executing_macro = executing_macro;
        front->parameters = std::move(macro_params);
        macro_params.clear();
        executing_macro = 0;
        front_macros = true;
    }
    return *front;
}

void Maxwell3D::FrontWrite(VideoCommon::GpuSplit& split, FrontState& fs, u32 method,
                           u32 argument, bool is_last_call, bool from_macro) {
    namespace Profile = VideoCommon::Nx2Profile;
    // A macro's constant buffer data: a run of words goes as one multi-method entry, which the
    // back uploads with ProcessCBMultiData as it does the pushbuffer's runs: the same bytes at
    // the same offsets, without touching the data register or its shadow copy (nor does the
    // front then). Not in replay mode, where a word written is the shadow RAM's.
    constexpr u32 cb_data_first = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    if (from_macro && method >= cb_data_first && method < cb_data_first + 16 &&
        fs.control != Regs::ShadowRamControl::Replay && CbBatchEnabled()) {
        if (Profile::Enabled()) [[unlikely]] {
            Profile::Add(Profile::Counter::MethodWrites, 1);
            Profile::Add(Profile::Counter::MacroWrites, 1);
        }
        fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)] += sizeof(u32);
        split.QueueWord(this, cb_data_first, argument);
        return;
    }
    // As ProcessShadowRam does on the back.
    u32 value = argument;
    bool shadow_kept = true;
    if (fs.control == Regs::ShadowRamControl::Track ||
        fs.control == Regs::ShadowRamControl::TrackWithFilter) {
        shadow_kept = fs.shadow[method] == argument;
        fs.shadow[method] = argument;
    } else if (fs.control == Regs::ShadowRamControl::Replay) {
        value = fs.shadow[method];
    }
    if (Profile::Enabled()) [[unlikely]] {
        Profile::Add(Profile::Counter::MethodWrites, 1);
        if (from_macro) {
            Profile::Add(Profile::Counter::MacroWrites, 1);
        }
    }
    if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && FrontVerifyEnabled()) [[unlikely]] {
        fs.control_writes[fs.control_count++ % fs.control_writes.size()] = {
            argument, static_cast<u32>(fs.control), from_macro ? 2U : 1U};
    }
    if (!execution_mask[method]) {
        // Nothing would change on the back: CallMethod and ConsumeSinkImpl skip same-value state
        // writes, and the shadow RAM (tracked by default) holds the value already.
        if (fs.regs[method] == value && shadow_kept && fs.skip_same) {
            if (Profile::Enabled()) [[unlikely]] {
                Profile::Add(Profile::Counter::SameValueWrites, 1);
            }
            return;
        }
        fs.regs[method] = value;
    } else {
        fs.regs[method] = value;
        constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
        switch (method) {
        case MAXWELL3D_REG_INDEX(shadow_ram_control):
            // The back's mode is shadow_state.shadow_ram_control: the shadow copy of this very
            // register, which a later Replay write reads back.
            fs.control = static_cast<Regs::ShadowRamControl>(argument);
            fs.shadow[method] = argument;
            break;
        case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
            GetNativeMacroEngine().ClearCode(fs.regs[MAXWELL3D_REG_INDEX(load_mme.instruction_ptr)]);
            break;
        case MAXWELL3D_REG_INDEX(load_mme.instruction):
            GetNativeMacroEngine().AddCode(fs.regs[MAXWELL3D_REG_INDEX(load_mme.instruction_ptr)], value);
            break;
        case MAXWELL3D_REG_INDEX(load_mme.start_address): {
            const auto positions = GetNativeMacroPositions();
            u32& pointer = fs.regs[MAXWELL3D_REG_INDEX(load_mme.start_address_ptr)];
            if (pointer < positions.size()) {
                positions[pointer] = value;
            }
            ++pointer;
            break;
        }
        case MAXWELL3D_REG_INDEX(falcon[4]):
            fs.regs[MAXWELL3D_REG_INDEX(shadow_scratch)] = 1;
            break;
        case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
        case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
        case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
        case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
        case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config): {
            // ProcessCBBind sets the stage's slot from this configuration and the constant
            // buffer registers: the same ones as last time set the binding it already has.
            const size_t stage = (method - MAXWELL3D_REG_INDEX(bind_groups[0].raw_config)) / BindGroupStride;
            const std::array<u32, 4> bind{value, fs.regs[MAXWELL3D_REG_INDEX(const_buffer.size)],
                                          fs.regs[MAXWELL3D_REG_INDEX(const_buffer.address_high)],
                                          fs.regs[MAXWELL3D_REG_INDEX(const_buffer.address_low)]};
            if (fs.last_bind[stage] == bind && CbBindSkipEnabled()) {
                if (Profile::Enabled()) [[unlikely]] {
                    Profile::Add(Profile::Counter::CbBindsSkipped, 1);
                }
                return;
            }
            fs.last_bind[stage] = bind;
            break;
        }
        default:
            if (method >= cb_data && method < cb_data + 16) {
                fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)] += sizeof(u32);
            } else if (!FrontHandledOutsideDrawManager(method)) {
                fs.draw_pending = true;
            }
            break;
        }
    }
    if (from_macro && (execution_mask[method] || !MacroSinkEnabled())) {
        split.MacroWrite(this, method, argument);
    } else {
        // A state write: the back sinks it and applies it (shadow RAM, same-value skip, dirty
        // flags) before the next executable method, as CallMethod would have at once. The
        // macro's end consumes the sink too (FinishFrontMacro).
        split.Method(this, method, argument, is_last_call, nullptr);
    }
    if (method == MAXWELL3D_REG_INDEX(report_semaphore.query)) [[unlikely]] {
        NoteFrontReport(memory_manager, fs.regs.data(), value);
    } else if (method == MAXWELL3D_REG_INDEX(launch_dma)) [[unlikely]] {
        NoteFrontUploadStart(memory_manager, fs.regs.data());
    } else if (method == MAXWELL3D_REG_INDEX(inline_data)) [[unlikely]] {
        NoteFrontUploadWord();
    }
}

void Maxwell3D::FrontCallMethod(Core::System& system, VideoCommon::GpuSplit& split, u32 method,
                                u32 argument, bool is_last_call) {
    auto& fs = Front(system, split);
    if (method >= MacroRegistersStart) {
        if (fs.executing_macro == 0) {
            fs.executing_macro = method;
        }
        fs.parameters.push_back(argument);
        if (is_last_call) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    FrontWrite(split, fs, method, argument, is_last_call, false);
}

void Maxwell3D::FrontCallMultiMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                     u32 method, const u32* base_start, u32 amount,
                                     u32 methods_pending) {
    auto& fs = Front(system, split);
    if (method >= MacroRegistersStart) {
        if (fs.executing_macro == 0) {
            fs.executing_macro = method;
        }
        fs.parameters.insert(fs.parameters.end(), base_start, base_start + amount);
        if (amount == methods_pending) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    // What CallMultiMethod does to the registers on the back, then one entry for all the words.
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    const auto tracked = [&fs] {
        return fs.control == Regs::ShadowRamControl::Track ||
               fs.control == Regs::ShadowRamControl::TrackWithFilter;
    };
    if (method >= cb_data && method < cb_data + 16) {
        fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)] += amount * sizeof(u32);
    } else if (method == MAXWELL3D_REG_INDEX(inline_data)) {
        // Upload data only.
    } else if ((method == MAXWELL3D_REG_INDEX(draw_inline_index) ||
                method == MAXWELL3D_REG_INDEX(inline_index_2x16.even) ||
                method == MAXWELL3D_REG_INDEX(inline_index_4x8.index0)) &&
               fs.control != Regs::ShadowRamControl::Replay) {
        if (amount != 0) {
            const u32 last = base_start[amount - 1];
            if (tracked()) {
                fs.shadow[method] = last;
            }
            fs.regs[method] = last;
        }
        fs.draw_pending = true;
    } else {
        // CallMethod for each word, macro uploads among them (load_mme.instruction). A word may
        // change the shadow RAM mode for the next (a run of SET_SHADOW_RAM_CONTROL writes).
        for (u32 i = 0; i < amount; ++i) {
            const u32 argument = base_start[i];
            u32 value = argument;
            if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && FrontVerifyEnabled()) [[unlikely]] {
                fs.control_writes[fs.control_count++ % fs.control_writes.size()] = {
                    argument, static_cast<u32>(fs.control), 3U};
            }
            if (tracked()) {
                fs.shadow[method] = argument;
            } else if (fs.control == Regs::ShadowRamControl::Replay) {
                value = fs.shadow[method];
            }
            fs.regs[method] = value;
            if (!execution_mask[method]) {
                continue;
            }
            switch (method) {
            case MAXWELL3D_REG_INDEX(shadow_ram_control):
                fs.control = static_cast<Regs::ShadowRamControl>(argument);
                fs.shadow[method] = argument; // The same word on the back (see FrontWrite)
                break;
            case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
                GetNativeMacroEngine().ClearCode(value);
                break;
            case MAXWELL3D_REG_INDEX(load_mme.instruction):
                GetNativeMacroEngine().AddCode(fs.regs[MAXWELL3D_REG_INDEX(load_mme.instruction_ptr)], value);
                break;
            case MAXWELL3D_REG_INDEX(load_mme.start_address): {
                const auto positions = GetNativeMacroPositions();
                u32& pointer = fs.regs[MAXWELL3D_REG_INDEX(load_mme.start_address_ptr)];
                if (pointer < positions.size()) {
                    positions[pointer] = value;
                }
                ++pointer;
                break;
            }
            case MAXWELL3D_REG_INDEX(falcon[4]):
                fs.regs[MAXWELL3D_REG_INDEX(shadow_scratch)] = 1;
                break;
            case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
            case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
            case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
            case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
            case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
                // Bound on the back word by word, not through FrontWrite: forget the last bind.
                fs.last_bind[(method - MAXWELL3D_REG_INDEX(bind_groups[0].raw_config)) / BindGroupStride]
                    .fill(~u32{0});
                break;
            default:
                if (!FrontHandledOutsideDrawManager(method)) {
                    fs.draw_pending = true;
                }
                break;
            }
        }
    }
    split.MultiMethod(this, method, base_start, amount, methods_pending, nullptr);
    if (method == MAXWELL3D_REG_INDEX(report_semaphore.query) && amount != 0) [[unlikely]] {
        NoteFrontReport(memory_manager, fs.regs.data(), fs.regs[method]);
    } else if (method == MAXWELL3D_REG_INDEX(launch_dma) && amount != 0) [[unlikely]] {
        NoteFrontUploadStart(memory_manager, fs.regs.data());
    } else if (method == MAXWELL3D_REG_INDEX(inline_data) && amount != 0) [[unlikely]] {
        NoteFrontUploadWord();
    }
}

void Maxwell3D::FrontRunMacro(Core::System& system, VideoCommon::GpuSplit& split,
                              FrontState& fs) {
#if defined(ARCHITECTURE_x86_64) || (defined(ARCHITECTURE_arm64) && defined(__ANDROID__))
    const u32 method = fs.executing_macro;
    fs.executing_macro = 0;
    auto& engine = GetNativeMacroEngine();
    const auto positions = GetNativeMacroPositions();
    const u32 start = positions[((method - MacroRegistersStart) >> 1) % positions.size()];
    {
        VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Macro};
        engine.native_interpreter.active_dram = &native_data_ram;
        if (!engine.native_jit) {
            engine.native_jit = std::make_shared<TuringMacroJit>();
        }
        engine.native_jit->Prepare(engine.native_code, engine.native_code_generation);
        const size_t parameter_count = fs.parameters.size();
        const auto read = [&fs](u32 address) {
            return address < Regs::NUM_REGS ? fs.regs[address] : 0U;
        };
        const auto emit = [&](u32 address, u32 value) {
            if (address >= MacroRegistersStart) {
                return false;
            }
            FrontWrite(split, fs, address, value, true, true);
            // MME_DMA_READ (into data RAM) and MME_DMA_READ_FIFOED (into the load FIFO).
            if (address == 0x157 || address == 0x158) {
                FrontDmaRead(split, fs, address, value);
            }
            return true;
        };
        // The ARM64 code stops being used when it differs from the interpreter on a checked
        // call (or could not be compiled): the front interprets the macros then. With macros
        // on the front the back never runs them, so the interpreter is the front's.
        const bool jit = engine.native_jit->Usable();
#if defined(ARCHITECTURE_arm64) && defined(__ANDROID__)
        // Same-value state writes are elided inside the compiled code, as FrontWrite would skip
        // them (never when the front sends them all, EDEN_NX2_SPLIT_SKIP=0).
        if (jit && fs.skip_same && JitSkipSameEnabled()) {
            fs.elision = TuringMacroJit::Elision{
                .must_call = fs.jit_must_call.data(),
                .regs = fs.regs.data(),
                .shadow = fs.shadow.data(),
                .control = reinterpret_cast<const u32*>(&fs.control),
                .skipped = 0,
            };
            engine.native_jit->SetElision(&fs.elision);
        } else {
            engine.native_jit->SetElision(nullptr);
        }
#endif
        const auto result =
            jit ? engine.native_jit->Execute(start, fs.parameters, std::span<u32>{native_data_ram}, read,
                                             emit)
                : engine.native_interpreter.Execute(engine.native_code, start, fs.parameters, read, emit);
#if defined(ARCHITECTURE_arm64) && defined(__ANDROID__)
        // Not left behind: the compute engine's front macros can run through the same JIT, and
        // must never elide against this engine's registers.
        engine.native_jit->SetElision(nullptr);
        if (const u32 skipped = fs.elision.skipped; skipped != 0) {
            fs.elision.skipped = 0;
            if (VideoCommon::Nx2Profile::Enabled()) [[unlikely]] {
                namespace Profile = VideoCommon::Nx2Profile;
                Profile::Add(Profile::Counter::MethodWrites, skipped);
                Profile::Add(Profile::Counter::MacroWrites, skipped);
                Profile::Add(Profile::Counter::SameValueWrites, skipped);
                Profile::Add(Profile::Counter::JitSkippedWrites, skipped);
            }
        }
#endif
        if (!result.success) {
            engine.native_execution_failed = true;
            LOG_ERROR(HW_GPU, "Native MME ({}, split front) failed: start={:#x}, consumed={}/{}, error={}",
                      jit ? "JIT" : "interpreter", start, result.consumed, parameter_count,
                      result.error);
        }
    }
    fs.parameters.clear();
    if (fs.draw_pending) {
        fs.draw_pending = false;
        split.Defer([this, system_ptr = &system] { FinishFrontMacro(*system_ptr); });
    }
    if (FrontVerifyEnabled() && (++fs.calls & 4095) == 0) [[unlikely]] {
        FrontVerify(split, fs);
    }
#else
    fs.executing_macro = 0;
    fs.parameters.clear();
#endif
}

namespace {
// How the front's MME DMA reads were served, logged every 10 s.
struct DmaReadStats {
    u64 window_start{};
    u64 reads{};
    u64 direct{};
    u64 copied{};
    u64 waited{};
    u64 drained{};
    u64 mismatches{};
};
DmaReadStats g_dma_read_stats; // the front thread's

// Reads `out` from `source` without draining the split's back: waits only for the reports,
// semaphores, uploads and copies queued for these pages (and for pages the GPU has written, the
// compute and copy work queued), then reads guest memory, or for pages the GPU has written the
// buffer cache's copy of them (made after each write, as for GPU-written command lists). False
// when that copy is unknown: drain.
bool DmaReadWithoutDrain(VideoCommon::GpuSplit& split, MemoryManager& memory_manager,
                         VideoCore::RasterizerInterface* rasterizer, GPUVAddr source,
                         std::span<u32> out, bool& waited, bool& copied) {
    struct Piece {
        GPUVAddr gpu;
        DAddr device;
        u32 offset;
        u32 size;
        bool gpu_written;
    };
    std::array<Piece, 6> pieces{};
    size_t count = 0;
    const GPUVAddr end = source + out.size_bytes();
    for (GPUVAddr chunk = source; chunk < end;) {
        const GPUVAddr chunk_end = (std::min)(end, (chunk & ~GPUVAddr{0xfff}) + 0x1000);
        const auto device = memory_manager.GpuToCpuAddress(chunk);
        if (!device || count == pieces.size()) {
            return false;
        }
        pieces[count++] = {chunk, *device, static_cast<u32>(chunk - source),
                           static_cast<u32>(chunk_end - chunk), Tegra::Nx2IsGpuWritten(*device)};
        chunk = chunk_end;
    }
    // Queued compute work matters only for pages the GPU writes: its shaders write through the
    // buffer cache (which then copies those pages). Waiting for it on every read, the front spent
    // 60% of its time in WaitForPosition. Direct writes (reports, semaphores, uploads, DMA
    // copies) are in PendingWrite.
    u64 target = 0;
    for (size_t i = 0; i < count; ++i) {
        target = (std::max)(target, split.PendingWrite(pieces[i].device, pieces[i].size));
        if (pieces[i].gpu_written) {
            target = (std::max)(target, split.WriterPosition());
        }
    }
    waited = split.WaitForPosition(target);
    u8* const bytes = reinterpret_cast<u8*>(out.data());
    for (size_t i = 0; i < count; ++i) {
        const Piece& piece = pieces[i];
        const std::span<u8> part{bytes + piece.offset, piece.size};
        if (piece.gpu_written && rasterizer != nullptr) {
            Tegra::Nx2MarkCommandListPage(piece.device);
            using GpuWrittenList = VideoCore::RasterizerInterface::GpuWrittenList;
            const GpuWrittenList where = rasterizer->ReadGpuWrittenCommandList(piece.device, part);
            if (where == GpuWrittenList::Copied) {
                copied = true;
                continue;
            }
            if (where != GpuWrittenList::Synced) {
                return false;
            }
        }
        memory_manager.ReadBlockUnsafe(piece.gpu, part.data(), part.size());
    }
    return true;
}
} // Anonymous namespace

void Maxwell3D::FrontDmaRead(VideoCommon::GpuSplit& split, FrontState& fs, u32 method,
                             u32 count) {
    namespace Profile = VideoCommon::Nx2Profile;
    Profile::Scope profile_scope{Profile::Zone::SplitDmaRead};
    // EDEN_NX2_DMA_READ_FAST=0: drain the back before every read. EDEN_NX2_DMA_READ_VERIFY=1:
    // read without draining, then drain and compare (logs the reads that differ).
    static const bool fast = [] {
        const char* const value = std::getenv("EDEN_NX2_DMA_READ_FAST");
        return value == nullptr || value[0] != '0';
    }();
    static const bool verify = [] {
        const char* const value = std::getenv("EDEN_NX2_DMA_READ_VERIFY");
        return value != nullptr && value[0] != '0';
    }();
    const GPUVAddr source = (GPUVAddr{fs.regs[0x154] & 0x1ffffff} << 32) | fs.regs[0x155];
    const u32 ram = fs.regs[0x156];
    const u32 words = std::min<u32>(count, 0x1000);
    const size_t read_bytes = size_t{words} * sizeof(u32);
    if (Profile::Enabled()) {
        Profile::Add(Profile::Counter::DmaReadBytes, read_bytes);
    }
    std::array<u32, 0x1000> data;
    const std::span<u32> values{data.data(), words};
    DmaReadStats& stats = g_dma_read_stats;
    ++stats.reads;
    bool undrained = false;
    bool copied = false;
    if (fast && words != 0) {
        bool waited = false;
        undrained = DmaReadWithoutDrain(split, memory_manager, rasterizer, source, values, waited,
                                        copied);
        stats.waited += waited ? 1 : 0;
        if (undrained) {
            ++(copied ? stats.copied : stats.direct);
        }
    }
    static std::array<u32, 0x1000> undrained_data; // the front thread's
    if (undrained && verify) {
        std::copy_n(data.begin(), words, undrained_data.begin());
    }
    if (!undrained || verify) {
        // The words may be written by methods still queued for the back (indirect draw arguments
        // from a culling dispatch), and the caches that may hold newer ones belong to it.
        stats.drained += undrained ? 0 : 1;
        split.Drain(Profile::Zone::SplitDmaRead);
        // Whole pages: GPU-written ones read from guest memory until the GPU writes them again.
        const GPUVAddr first_page = source & ~GPUVAddr{0xfff};
        const GPUVAddr end_page = (source + read_bytes + 0xfff) & ~GPUVAddr{0xfff};
        const GPUVAddr flush_begin = fast ? first_page : source;
        const u64 flush_size = fast ? end_page - first_page : read_bytes;
        if (memory_manager.IsMemoryDirty(flush_begin, flush_size,
                                         VideoCommon::CacheType::BufferCache)) {
            const u64 start = Profile::Now();
            memory_manager.FlushRegion(flush_begin, flush_size, VideoCommon::CacheType::BufferCache);
            Profile::NoteDmaReadFlush(Profile::Now() - start, read_bytes);
        }
        if (fast && rasterizer != nullptr) {
            for (GPUVAddr page = first_page; page < end_page; page += 0x1000) {
                if (const auto device = memory_manager.GpuToCpuAddress(page);
                    device && Tegra::Nx2IsCommandListPage(*device)) {
                    rasterizer->NoteGpuWrittenCommandListSynced(*device);
                }
            }
        }
        memory_manager.ReadBlockUnsafe(source, data.data(), read_bytes);
        if (undrained && !std::equal(data.begin(), data.begin() + words, undrained_data.begin())) {
            if (stats.mismatches++ < 32) {
                const auto device = memory_manager.GpuToCpuAddress(source);
                LOG_WARNING(HW_GPU,
                            "MME DMA read {:#x} at {:#x} ({} words, {}) differs undrained: {:08x} "
                            "drained: {:08x}",
                            method, source, words,
                            copied ? "from a copy"
                            : device && Tegra::Nx2IsGpuWritten(*device) ? "GPU-written, synced"
                                                                         : "guest memory",
                            fmt::join(undrained_data.begin(),
                                      undrained_data.begin() + (std::min<u32>)(words, 8), ","),
                            fmt::join(data.begin(), data.begin() + (std::min<u32>)(words, 8), ","));
            }
        }
    }
    if (const u64 now = Profile::Now(); stats.window_start == 0) {
        stats.window_start = now;
    } else if (now - stats.window_start >= 10'000'000'000ULL) {
        LOG_INFO(HW_GPU,
                 "MME DMA reads in {:.1f} s: {}, {} from guest memory and {} from GPU-written "
                 "page copies without a drain ({} waited for queued writers), {} drained{}",
                 static_cast<double>(now - stats.window_start) / 1e9, stats.reads, stats.direct,
                 stats.copied, stats.waited, stats.drained,
                 verify ? fmt::format(", {} differed", stats.mismatches) : std::string{});
        stats = {};
        stats.window_start = now;
    }
    for (u32 i = 0; i < words; ++i) {
        const u32 word = data[i];
        if (method == 0x158) {
            fs.parameters.push_back(word);
        } else if (ram + i < native_data_ram.size()) {
            native_data_ram[ram + i] = word;
        }
    }
}

void Maxwell3D::FrontVerify(VideoCommon::GpuSplit& split, FrontState& fs) {
    split.Drain();
    static u64 checks{};
    static u64 differences{};
    ++checks;
    // State writes the back still holds in its method sink count as done.
    std::array<u32, Regs::NUM_REGS> expected;
    std::memcpy(expected.data(), regs.reg_array.data(), sizeof(expected));
    for (const auto& [method, value] : method_sink) {
        expected[method] = shadow_state.shadow_ram_control == Regs::ShadowRamControl::Replay
                               ? shadow_state.reg_array[method]
                               : value;
    }
    static u64 control_differences{};
    if ((fs.control != shadow_state.shadow_ram_control ||
         fs.shadow[MAXWELL3D_REG_INDEX(shadow_ram_control)] != shadow_state.reg_array[MAXWELL3D_REG_INDEX(shadow_ram_control)] ||
         fs.regs[MAXWELL3D_REG_INDEX(shadow_ram_control)] != expected[MAXWELL3D_REG_INDEX(shadow_ram_control)]) &&
        control_differences < 4) {
        LOG_WARNING(HW_GPU, "Split shadow RAM control writes, front ({} total) / back ({} total):",
                    fs.control_count, back_control_count);
        for (u32 i = 0; i < 16; ++i) {
            const auto& f = fs.control_writes[(fs.control_count + i) % 16];
            const auto& b = back_control_writes[(back_control_count + i) % 16];
            LOG_WARNING(HW_GPU, "  front {:#x} (mode {}, path {}) | back {:#x} (mode {}, last {})",
                        f.argument, f.before, f.path, b.argument, b.before, b.path);
        }
    }
    for (u32 i = 0; i < Regs::NUM_REGS; ++i) {
        if (fs.regs[i] != expected[i]) {
            if (differences++ < 64) {
                LOG_WARNING(HW_GPU, "Split front register {:#x}: front {:#x}, back {:#x}", i,
                            fs.regs[i], expected[i]);
            }
            fs.regs[i] = expected[i];
        }
    }
    if (fs.control != shadow_state.shadow_ram_control) {
        if (control_differences++ < 16) {
            LOG_WARNING(HW_GPU, "Split front shadow RAM control {} vs back {}",
                        static_cast<u32>(fs.control),
                        static_cast<u32>(shadow_state.shadow_ram_control));
        }
        fs.control = shadow_state.shadow_ram_control;
    }
    for (u32 i = 0; i < Regs::NUM_REGS; ++i) {
        if (fs.shadow[i] != shadow_state.reg_array[i]) {
            if (control_differences++ < 16) {
                LOG_WARNING(HW_GPU, "Split front shadow register {:#x}: front {:#x}, back {:#x}", i,
                            fs.shadow[i], shadow_state.reg_array[i]);
            }
            fs.shadow[i] = shadow_state.reg_array[i];
        }
    }
    if ((checks & 63) == 1) {
        LOG_INFO(HW_GPU, "Split front register checks: {} done, {} differences", checks, differences);
    }
}

bool Maxwell3D::FrontLoadConstantRun(Core::System& system, VideoCommon::GpuSplit& split,
                                     u32 first_method, const u32* words, u32 count) {
    namespace Profile = VideoCommon::Nx2Profile;
    constexpr u32 cb_offset = MAXWELL3D_REG_INDEX(const_buffer.offset);
    auto& fs = Front(system, split);
    if (fs.control == Regs::ShadowRamControl::Replay) {
        return false; // A word written is then the shadow RAM's: word by word (FrontWrite)
    }
    const bool tracked = fs.control == Regs::ShadowRamControl::Track ||
                         fs.control == Regs::ShadowRamControl::TrackWithFilter;
    // What FrontWrite mirrors for each word: the shadow RAM, the register, the offset the data
    // words advance. The back makes the same changes in LoadConstantRun.
    for (u32 i = 0; i < count; ++i) {
        const u32 method = first_method + i;
        const u32 word = words[i];
        if (tracked) {
            fs.shadow[method] = word;
        }
        fs.regs[method] = word;
        if (method != cb_offset) {
            fs.regs[cb_offset] += sizeof(u32);
        }
    }
    if (Profile::Enabled()) [[unlikely]] {
        Profile::Add(Profile::Counter::MethodWrites, count);
    }
    split.ConstantRun(this, first_method, words, count);
    return true;
}

void Maxwell3D::LoadConstantRun(Core::System& system, u32 first_method, const u32* words,
                                u32 count) {
    constexpr u32 cb_offset = MAXWELL3D_REG_INDEX(const_buffer.offset);
    u32 index = 0;
    if (first_method == cb_offset) {
        // The offset: a state write, as CallMethod applies it (shadow RAM, same-value skip,
        // dirty flags).
        CallMethod(system, cb_offset, words[0], count == 1);
        index = 1;
    }
    const u32 data_count = count - index;
    if (data_count == 0) {
        return;
    }
    // The data words: the shadow RAM, register and dirty flags per word as CallMethod makes
    // them, then one upload from the offset (ProcessCBData per word advances it the same).
    std::array<u32, MaxConstantRun> values;
    for (u32 i = 0; i < data_count; ++i) {
        const u32 method = first_method + index + i;
        const u32 value = ProcessShadowRam(method, words[index + i]);
        ProcessDirtyRegisters(method, value);
        values[i] = value;
    }
    ProcessCBMultiData(values.data(), data_count);
}

void Maxwell3D::FinishFrontMacro(Core::System& system) {
    // What ProcessMacro and CallMacroMethod do around the macro.
    ConsumeSink(system);
    draw_manager.DrawDeferred(*this);
}

} // namespace Tegra::Engines

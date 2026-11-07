// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bitset>
#include <atomic>
#include <cstring>
#include <limits>
#include <chrono>
#include <optional>
#include <string>
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/nx2.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_cmdpage_diag.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_war_probe.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/textures/texture.h"
#ifdef ARCHITECTURE_x86_64
#include "video_core/front_macro_memo.h"
#include "video_core/macro_dma_forward.h"
#include "video_core/macro_turing_jit.h"
#endif

namespace Tegra::Engines {

/// First register id that is actually a Macro call.
constexpr u32 MacroRegistersStart = 0xE00;

// EDEN_NX2_3D_METHODS: log the first write to each 3D register (finds methods Eden doesn't model).
static const bool g_trace_first_method_writes = std::getenv("EDEN_NX2_3D_METHODS") != nullptr;

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
    fast_maxwell3d = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::Method) && !record_write_history &&
                     !g_trace_first_method_writes && !VideoCommon::Nx2Profile::Enabled();
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
    // CallMethod does.
    const bool skip_same = SkipSameValueWrites();
    const auto process = [&](u32 method, u32 value) {
        if (!skip_same || regs.reg_array[method] != value) {
            ProcessDirtyRegisters(method, value);
        }
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

void Maxwell3D::TraceFirstMethodWrite(u32 method, u32 argument) {
    static std::array<bool, Regs::NUM_REGS> seen{};
    if (method < seen.size() && !seen[method]) {
        seen[method] = true;
        LOG_INFO(HW_GPU, "3D method first write: {:#x} (byte {:#x}) = {:#x}", method, method * 4, argument);
    }
}

void Maxwell3D::CallStateMethod(Core::System&, u32 method, u32 method_argument) {
    DEBUG_ASSERT(executing_macro == 0);
    DEBUG_ASSERT(method < MacroRegistersStart && !execution_mask[method]);
    RecordWrite(method, method_argument, 1, 1);
    if (g_trace_first_method_writes) [[unlikely]] {
        TraceFirstMethodWrite(method, method_argument);
    }
    if (VideoCommon::Nx2Profile::Enabled()) [[unlikely]] {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::MethodWrites, 1);
        if (regs.reg_array[method] == method_argument) {
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::SameValueWrites, 1);
        }
    }
    const u32 argument = ProcessShadowRam(method, method_argument);
    if (!SkipSameValueWrites() || regs.reg_array[method] != argument) {
        ProcessDirtyRegisters(method, argument);
    }
}

void Maxwell3D::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    if (front_macros && !VideoCommon::GpuSplit::OnBackThread()) [[unlikely]] {
        ReleaseFront();
    }
    if (!fast_maxwell3d || executing_macro != 0 || method >= MacroRegistersStart ||
        method == MAXWELL3D_REG_INDEX(shadow_ram_control)) {
        return CallMethodSlow(system, method, method_argument, is_last_call);
    }
    if (VideoCommon::Nx2Perf::Verify()) {
        return VerifyCallMethod(system, method, method_argument, is_last_call);
    }
    CallMethodFast(method, method_argument, is_last_call);
}

namespace {

// Executable methods the draw manager does nothing for (ProcessDirtyRegisters is all they do).
bool IsStoreOnlyExecutable(u32 method) {
    switch (method) {
    case MAXWELL3D_REG_INDEX(vertex_buffer.first):
    case MAXWELL3D_REG_INDEX(vertex_buffer.count):
    case MAXWELL3D_REG_INDEX(index_buffer.first):
    case MAXWELL3D_REG_INDEX(topology_override):
        return true;
    default:
        return false;
    }
}

// Nx2Perf exec_same_drop: the shader stage of a constant buffer bind register, ~0 otherwise.
constexpr u32 BindGroupStage(u32 method) {
    constexpr u32 first = MAXWELL3D_REG_INDEX(bind_groups[0].raw_config);
    constexpr u32 stride = MAXWELL3D_REG_INDEX(bind_groups[1].raw_config) - first;
    const u32 offset = method - first;
    return offset % stride == 0 && offset / stride < Maxwell3D::Regs::MaxShaderStage
               ? offset / stride
               : ~0U;
}
static_assert(BindGroupStage(0x904) == 0 && BindGroupStage(0x90c) == 1 &&
              BindGroupStage(0x924) == 4 && BindGroupStage(0x92c) == ~0U &&
              BindGroupStage(0x905) == ~0U && BindGroupStage(0x5f7) == ~0U);

} // Anonymous namespace

// CallMethodSlow without the checks that cannot apply (CallMethod): no macro collects parameters,
// no write history, trace or profile, not a macro call or SET_SHADOW_RAM_CONTROL. The hot
// executable methods go straight to their handler.
void Maxwell3D::CallMethodFast(u32 method, u32 method_argument, bool is_last_call) {
    const u32 argument = ProcessShadowRam(method, method_argument);
    if (!execution_mask[method]) {
        if (!SkipSameValueWrites() || regs.reg_array[method] != argument) {
            ProcessDirtyRegisters(method, argument);
        }
        return;
    }
    ProcessDirtyRegisters(method, argument);
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    if (method - cb_data < 16) {
        return ProcessCBData(argument);
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(draw.begin):
        return draw_manager.DrawBegin(*this);
    case MAXWELL3D_REG_INDEX(draw.end):
        return draw_manager.DrawEnd(*this);
    case MAXWELL3D_REG_INDEX(vertex_buffer.first):
    case MAXWELL3D_REG_INDEX(vertex_buffer.count):
    case MAXWELL3D_REG_INDEX(index_buffer.first):
    case MAXWELL3D_REG_INDEX(topology_override):
        return;
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
    default:
        return ProcessMethodCall(method, argument, method_argument, is_last_call);
    }
}

void Maxwell3D::VerifyCallMethod(Core::System& system, u32 method, u32 method_argument,
                                 bool is_last_call) {
    // Only register writes can run twice: handlers draw, upload and bind, and a deferred draw
    // ProcessDirtyRegisters may issue is a draw too.
    if ((execution_mask[method] && !IsStoreOnlyExecutable(method)) ||
        draw_manager.HasDeferredDraw()) {
        return CallMethodSlow(system, method, method_argument, is_last_call);
    }
    const u32 reg = regs.reg_array[method];
    const u32 shadow = shadow_state.reg_array[method];
    const DirtyState::Flags flags = dirty.flags;
    CallMethodFast(method, method_argument, is_last_call);
    const u32 fast_reg = regs.reg_array[method];
    const u32 fast_shadow = shadow_state.reg_array[method];
    const DirtyState::Flags fast_flags = dirty.flags;
    regs.reg_array[method] = reg;
    shadow_state.reg_array[method] = shadow;
    dirty.flags = flags;
    CallMethodSlow(system, method, method_argument, is_last_call);
    if (fast_reg == regs.reg_array[method] && fast_shadow == shadow_state.reg_array[method] &&
        fast_flags == dirty.flags) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1, std::memory_order_relaxed) >= 64) {
        return;
    }
    std::string flag_differences;
    const DirtyState::Flags differing = fast_flags ^ dirty.flags;
    for (size_t i = 0; i < differing.size(); ++i) {
        if (differing[i]) {
            flag_differences += fmt::format(" {}={}", i, fast_flags[i] ? 1 : 0);
        }
    }
    LOG_ERROR(HW_GPU,
              "Nx2Perf method: {:#x} = {:#x}: register {:#x} (old {:#x}), shadow {:#x} (old {:#x}), "
              "fast dirty flags differ:{}",
              method, method_argument, fast_reg, regs.reg_array[method], fast_shadow,
              shadow_state.reg_array[method], flag_differences);
}

void Maxwell3D::CallConstBufferWords(Core::System& system, const u32* methods,
                                     const u32* arguments, u32 count) {
    const u32 offset = regs.const_buffer.offset;
    const u64 bytes = u64{count} * sizeof(u32);
    const GPUVAddr address = regs.const_buffer.Address();
    // Keep per-word translation across device pages and per-word clipping at buffer limits.
    if (!fast_maxwell3d || executing_macro != 0 || address == 0 ||
        offset > regs.const_buffer.size || bytes > regs.const_buffer.size - offset ||
        bytes > Core::DEVICE_PAGESIZE - ((address + offset) & Core::DEVICE_PAGEMASK)) {
        for (u32 i = 0; i < count; ++i) {
            CallMethod(system, methods[i], arguments[i], true);
        }
        return;
    }
    std::array<u32, 64> values;
    const bool replay = shadow_state.shadow_ram_control == Regs::ShadowRamControl::Replay;
    for (u32 i = 0; i < count; ++i) {
        values[i] = replay ? shadow_state.reg_array[methods[i]] : arguments[i];
    }
    if (VideoCommon::Nx2Perf::Verify()) {
        for (u32 i = 0; i < count; ++i) {
            CallMethod(system, methods[i], arguments[i], true);
        }
        std::array<u32, 64> written;
        const bool mapped = memory_manager.IsFullyMappedRange(address + offset, bytes);
        if (mapped) {
            memory_manager.ReadBlockUnsafe(address + offset, written.data(), bytes);
        }
        if (regs.const_buffer.offset != offset + bytes ||
            (mapped && std::memcmp(written.data(), values.data(), bytes) != 0)) {
            static std::atomic<u32> reports{};
            if (reports.fetch_add(1, std::memory_order_relaxed) < 64) {
                LOG_ERROR(HW_GPU,
                          "Nx2Perf method: {} constant buffer words at {:#x}+{:#x}: offset {:#x}, "
                          "expected {:#x}, memory {}",
                          count, address, offset, regs.const_buffer.offset, offset + bytes,
                          mapped && std::memcmp(written.data(), values.data(), bytes) != 0
                              ? "differs"
                              : "matches");
            }
        }
        return;
    }
    // As CallMethodFast for each word: the first may issue a deferred draw, before any upload.
    for (u32 i = 0; i < count; ++i) {
        ProcessDirtyRegisters(methods[i], ProcessShadowRam(methods[i], arguments[i]));
    }
    memory_manager.WriteWordsCached(address + offset, values.data(), count);
    regs.const_buffer.offset += static_cast<u32>(bytes);
}

size_t Maxwell3D::CallStateMethodRun(const u64* entry, size_t max_words) {
    const auto control = shadow_state.shadow_ram_control;
    const bool skip_same = SkipSameValueWrites();
    size_t count = 0;
    if (control == Regs::ShadowRamControl::Track ||
        control == Regs::ShadowRamControl::TrackWithFilter) {
        do {
            const u64 word = entry[count];
            const u32 method = static_cast<u32>((word >> 8) & 0xffff);
            const u32 value = static_cast<u32>(word >> 32);
            shadow_state.reg_array[method] = value;
            if (!skip_same || regs.reg_array[method] != value) {
                ProcessDirtyRegisters(method, value);
            }
            ++count;
            if (count >= max_words) {
                break;
            }
            const u64 next = entry[count];
            const u32 next_method = static_cast<u32>((next >> 8) & 0xffff);
            if ((next & 0x2f) != 2 || next_method >= Regs::NUM_REGS ||
                execution_mask[next_method]) {
                break;
            }
        } while (true);
    } else if (control == Regs::ShadowRamControl::Replay) {
        do {
            const u64 word = entry[count];
            const u32 method = static_cast<u32>((word >> 8) & 0xffff);
            const u32 value = shadow_state.reg_array[method];
            if (!skip_same || regs.reg_array[method] != value) {
                ProcessDirtyRegisters(method, value);
            }
            ++count;
            if (count >= max_words) {
                break;
            }
            const u64 next = entry[count];
            const u32 next_method = static_cast<u32>((next >> 8) & 0xffff);
            if ((next & 0x2f) != 2 || next_method >= Regs::NUM_REGS ||
                execution_mask[next_method]) {
                break;
            }
        } while (true);
    } else {
        do {
            const u64 word = entry[count];
            const u32 method = static_cast<u32>((word >> 8) & 0xffff);
            const u32 value = static_cast<u32>(word >> 32);
            if (!skip_same || regs.reg_array[method] != value) {
                ProcessDirtyRegisters(method, value);
            }
            ++count;
            if (count >= max_words) {
                break;
            }
            const u64 next = entry[count];
            const u32 next_method = static_cast<u32>((next >> 8) & 0xffff);
            if ((next & 0x2f) != 2 || next_method >= Regs::NUM_REGS ||
                execution_mask[next_method]) {
                break;
            }
        } while (true);
    }
    return count;
}

void Maxwell3D::CallMethodSlow(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
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
    if (front_macros && !VideoCommon::GpuSplit::OnBackThread()) [[unlikely]] {
        ReleaseFront();
    }
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
    if (index < positions.size()) {
        positions[index] = data;
    }
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
    std::vector<u32> dma_words; ///< FrontDmaReadEarly's words
    /// Nx2Perf dma_forward: registers the back writes from DMA words the front never read
    /// (regs holds a stale value), and macros (by index) seen doing an MME DMA read.
    std::bitset<Regs::NUM_REGS> unknown_regs;
    u32 unknown_count{};
    std::bitset<256> dma_macros;
    VideoCommon::DmaForward::Memo dma_memo; ///< Nx2Perf dma_forward_memo
    VideoCommon::FrontMacroMemo::Memo macro_memo; ///< Nx2Perf front_macro_memo
    void MarkUnknown(u32 method) {
        if (!unknown_regs[method]) {
            unknown_regs[method] = true;
            ++unknown_count;
        }
        if (exec_same_drop && BindGroupStage(method) != ~0U) {
            exec_bind_memo.Clear(); // The back binds a constant buffer the front can't see
        }
    }
    void ForgetUnknown(u32 method) {
        if (unknown_regs[method]) {
            unknown_regs[method] = false;
            --unknown_count;
            exec_relearned = true;
        }
    }
    /// Takes a forwarded register's value (and shadow copy) from the back, after draining it.
    void Learn(Maxwell3D& engine, VideoCommon::GpuSplit& split, u32 method) {
        split.Drain(VideoCommon::DrainSite::MacroDmaRead);
        regs[method] = engine.regs.reg_array[method];
        shadow[method] = engine.shadow_state.reg_array[method];
        ForgetUnknown(method);
    }
    u64 calls{};
    /// A method the draw manager handles was sent since the back last ran DrawDeferred: only
    /// then can it have a deferred instanced draw to issue at the end of a macro.
    bool draw_pending{true};
    bool skip_same{}; ///< SkipSameValueWrites() && FrontSkipSame(), read once
    /// The back's draw manager is in an instance loop (DrawMode::Instance): draw.end does nothing.
    bool instance_loop{};
    bool same_first{};  ///< Nx2Perf FrontSameFirst, read once
    bool class_table{}; ///< Nx2Perf FrontClassTable, read once
    bool writer_set_exec{}; ///< Nx2Perf WriterSetExec, read once
    bool draw_begin_coalesce{}; ///< Nx2Perf DrawBeginCoalesce, read once
    bool verify{};      ///< Nx2Perf::Verify(), read once
    /// MacroReadWriterSet (FrontDrawEffect): the last lookup of the programs the registers bind,
    /// valid while those registers, the program epoch, the proofs and the GPU map are unchanged.
    bool draw_key_valid{};
    bool draw_key_native{};
    bool draw_non_writer{};
    u64 draw_key_epoch{};
    u64 draw_key_version{};
    u64 draw_key_map{};
    /// Since draw_pending was last cleared: a draw the back issues may use any register state
    /// the front had meanwhile (state writes wait in its method sink), so one writer state or a
    /// change of program epoch makes every entry until then a writer.
    bool draw_window_writer{};
    u64 draw_window_epoch{};
    /// Nx2Perf exec_same_drop, read once; its bind memo; whether a register was learned again
    /// since the last executable write; the counts it logs (store-only and binds left out,
    /// store-only kept for a dirty flag) and the system its Verify checks run with.
    bool exec_same_drop{};
    bool exec_relearned{};
    VideoCommon::Nx2Perf::ExecBindMemo exec_bind_memo;
    std::array<u64, 3> exec_counts{};
    u64 exec_noted{};
    s64 exec_logged_ns{};
    Core::System* system{};
};

namespace {

// EDEN_NX2_SPLIT_VERIFY=1: every 4096 front macro calls, wait for the back and compare the
// front's registers with the back's (logs and resynchronizes differences).
bool FrontVerifyEnabled() {
    static const bool verify = std::getenv("EDEN_NX2_SPLIT_VERIFY") != nullptr;
    return verify;
}

// The front macro switches (Nx2Perf), read once.
struct FrontMacroSwitches {
    bool batch;
    bool borrow;
    bool keep_state;
    bool verify;
};
const FrontMacroSwitches& FrontMacroOptions() {
    using VideoCommon::Nx2Perf::On;
    using VideoCommon::Nx2Perf::Opt;
    static const FrontMacroSwitches switches{On(Opt::MacroBatch), On(Opt::MacroBorrow),
                                             On(Opt::MacroState), VideoCommon::Nx2Perf::Verify()};
    return switches;
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

// What an executable method may write on the back, besides a deferred draw it may issue
// (GpuSplit::Effect). Constant buffer data and reports write ranges the front notes.
VideoCommon::GpuSplit::Effect FrontMethodEffect(u32 method) {
    using Effect = VideoCommon::GpuSplit::Effect;
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    if (method >= cb_data && method < cb_data + 16) {
        return Effect::None;
    }
    switch (method) {
    case Maxwell3D::RootTables::SelectMethod:
    case Maxwell3D::RootTables::LoadMethod:
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
    case MAXWELL3D_REG_INDEX(wait_for_idle):
    case MAXWELL3D_REG_INDEX(render_enable.mode):
    case MAXWELL3D_REG_INDEX(clear_report_value):
    case MAXWELL3D_REG_INDEX(sync_info):
    case MAXWELL3D_REG_INDEX(fragment_barrier):
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return Effect::None;
    default:
        return FrontHandledOutsideDrawManager(method) ? Effect::Unknown : Effect::Gpu;
    }
}

// Where the next constant buffer data word goes (ProcessCBMultiData), from the front's registers.
GPUVAddr ConstBufferWriteAddress(std::span<const u32> regs) {
    return ((GPUVAddr{regs[MAXWELL3D_REG_INDEX(const_buffer.address_high)]} << 32) |
            regs[MAXWELL3D_REG_INDEX(const_buffer.address_low)]) +
           regs[MAXWELL3D_REG_INDEX(const_buffer.offset)];
}

// How many of the next constant buffer data words land before the bank's end (ProcessCBMultiData
// drops the rest).
u32 ConstBufferRoomWords(std::span<const u32> regs) {
    const u32 offset = regs[MAXWELL3D_REG_INDEX(const_buffer.offset)];
    const u32 size = regs[MAXWELL3D_REG_INDEX(const_buffer.size)];
    return offset < size ? (size - offset) / sizeof(u32) : 0;
}

// A report and what it may write: payload and timestamp.
GPUVAddr ReportAddress(std::span<const u32> regs) {
    return (GPUVAddr{regs[MAXWELL3D_REG_INDEX(report_semaphore.address_high)]} << 32) |
           regs[MAXWELL3D_REG_INDEX(report_semaphore.address_low)];
}
constexpr u64 ReportBytes = 16;

// Counters and timed releases are written when the host query completes, not when the back runs
// them: early MME DMA reads of their bytes wait until the back ran the report and the result
// landed (lichii #179, 166c21d96c). Short payload releases land when the back runs them.
void NoteLateReport(VideoCommon::GpuSplit& split, const MemoryManager& memory,
                    std::span<const u32> regs) {
    using Report = Maxwell3D::Regs::ReportSemaphore;
    decltype(Report::query) query{};
    query.raw = regs[MAXWELL3D_REG_INDEX(report_semaphore.query)];
    if (query.operation == Report::Operation::Release && query.short_query != 0 &&
        query.report == Report::Report::Payload) {
        return;
    }
    if (const auto device = memory.GpuToCpuAddress(ReportAddress(regs))) {
        split.NoteAsyncWrite(*device, ReportBytes);
    }
}

// EDEN_NX2_SPLIT_SKIP=0: send state writes that keep the register's value too.
bool FrontSkipSame() {
    static const bool skip = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT_SKIP");
        return value == nullptr || value[0] != '0';
    }();
    return skip;
}

[[nodiscard]] constexpr bool IsDrawMethod(u32 method) noexcept {
    switch (method) {
    case MAXWELL3D_REG_INDEX(draw.end):
    case MAXWELL3D_REG_INDEX(index_buffer32_first):
    case MAXWELL3D_REG_INDEX(index_buffer16_first):
    case MAXWELL3D_REG_INDEX(index_buffer8_first):
    case MAXWELL3D_REG_INDEX(index_buffer32_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer16_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer8_subsequent):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_first):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent):
    case MAXWELL3D_REG_INDEX(draw_texture.src_y0):
        return true;
    default:
        return false;
    }
}

// Nx2Perf FrontClassTable: the four functions above for each method, in one byte.
constexpr u8 ClassOutsideDrawManager = 1 << 0;
constexpr u32 ClassEffectShift = 1; ///< Two bits: GpuSplit::Effect
constexpr u8 ClassDraw = 1 << 3;
constexpr u8 ClassCbData = 1 << 4;

u8 ComputeFrontClass(u32 method) {
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    return static_cast<u8>(
        (FrontHandledOutsideDrawManager(method) ? ClassOutsideDrawManager : 0) |
        (static_cast<u8>(FrontMethodEffect(method)) << ClassEffectShift) |
        (IsDrawMethod(method) ? ClassDraw : 0) |
        (method >= cb_data && method < cb_data + 16 ? ClassCbData : 0));
}

const std::array<u8, Maxwell3D::Regs::NUM_REGS> front_class_table = [] {
    std::array<u8, Maxwell3D::Regs::NUM_REGS> table{};
    for (u32 method = 0; method < table.size(); ++method) {
        table[method] = ComputeFrontClass(method);
    }
    return table;
}();

// The method's class from the table; with PERF_VERIFY, from the functions (logs differences).
u8 FrontClass(u32 method, bool verify) {
    const u8 cls = front_class_table[method];
    if (verify) [[unlikely]] {
        const u8 old = ComputeFrontClass(method);
        if (old != cls) {
            static std::atomic<u32> reports{};
            if (reports.fetch_add(1, std::memory_order_relaxed) < 64) {
                LOG_ERROR(HW_GPU, "Nx2Perf front: class table method {:#x} has {:#x}, functions {:#x}",
                          method, cls, old);
            }
            return old;
        }
    }
    return cls;
}

VideoCommon::GpuSplit::Effect ClassEffect(u8 cls) {
    return static_cast<VideoCommon::GpuSplit::Effect>((cls >> ClassEffectShift) & 3);
}

// MacroReadWriterSet: the registers Nx2DrawProgramsOf reads.
[[nodiscard]] constexpr bool IsDrawProgramRegister(u32 method) noexcept {
    constexpr u32 pipelines = MAXWELL3D_REG_INDEX(pipelines);
    constexpr u32 pipelines_end =
        pipelines + static_cast<u32>(sizeof(Maxwell3D::Regs::pipelines) / sizeof(u32));
    constexpr u32 program_region = MAXWELL3D_REG_INDEX(program_region);
    return (method >= pipelines && method < pipelines_end) || method == program_region ||
           method == program_region + 1 || method == MAXWELL3D_REG_INDEX(rasterize_enable) ||
           method == MAXWELL3D_REG_INDEX(transform_feedback_enabled) ||
           method == Maxwell3D::Regs::MeshControl;
}

// Draw manager methods that only draw with the bound pipeline (or do nothing): a clear and a
// texture draw go through the texture cache instead and stay GPU work of unknown effect.
[[nodiscard]] bool FrontDrawsOnly(u32 method) noexcept {
    return !FrontHandledOutsideDrawManager(method) &&
           method != MAXWELL3D_REG_INDEX(clear_surface) &&
           method != MAXWELL3D_REG_INDEX(draw_texture.src_y0);
}

// WriterSetExec with PERF_VERIFY: the effect the old code gave an executable method's entry
// (old) against the new one (now). The only expected difference is a draws-only method whose
// proven draw (FrontDrawEffect) is no longer combined with the method's own Gpu class. Returns
// the old effect.
VideoCommon::GpuSplit::Effect VerifyWriterSetExec(u32 method, VideoCommon::GpuSplit::Effect old,
                                                  VideoCommon::GpuSplit::Effect now,
                                                  bool demoted) {
    using Effect = VideoCommon::GpuSplit::Effect;
    static std::atomic<u64> demotions{};
    static std::atomic<u64> differences{};
    if (old == now) {
        return now;
    }
    if (demoted && old == Effect::Gpu && now == Effect::Draw) {
        if (((demotions.fetch_add(1, std::memory_order_relaxed) + 1) & 0xfffff) == 0) {
            LOG_INFO(HW_GPU, "Nx2Perf writer_set_exec: {} draw-manager entries kept as proven "
                     "draws, {} other differences",
                     demotions.load(std::memory_order_relaxed),
                     differences.load(std::memory_order_relaxed));
        }
        // Counted only: a verify run keeps the old effect, so it waits as the old code did.
        return old;
    }
    if (differences.fetch_add(1, std::memory_order_relaxed) < 64) {
        LOG_ERROR(HW_GPU, "Nx2Perf writer_set_exec: method {:#x} effect {}, old {} (demoted {})",
                  method, static_cast<u32>(now), static_cast<u32>(old), demoted);
    }
    return old;
}

// The effect of an executable method's entry: what it had before (effect, a deferred draw) and
// what the method may write. A draws-only method draws with the bound programs, so it is Gpu
// only when FrontDrawEffect (draw_effect) cannot prove them non-writing. Without WriterSetExec
// the method's own class is combined as well, which keeps every such entry Gpu.
template <typename DrawEffect>
VideoCommon::GpuSplit::Effect FrontExecEffect(VideoCommon::GpuSplit::Effect effect, u32 method,
                                              u8 cls, bool class_table, bool writer_set_exec,
                                              bool verify, DrawEffect&& draw_effect) {
    using Effect = VideoCommon::GpuSplit::Effect;
    using VideoCommon::SplitWriteTracker;
    if (!writer_set_exec) {
        effect = SplitWriteTracker::Combine(effect,
                                            class_table ? ClassEffect(cls) : FrontMethodEffect(method));
        Effect method_effect = FrontMethodEffect(method);
        if (method_effect == Effect::Gpu && FrontDrawsOnly(method)) {
            method_effect = draw_effect();
        }
        return SplitWriteTracker::Combine(effect, method_effect);
    }
    const Effect class_effect = class_table ? ClassEffect(cls) : FrontMethodEffect(method);
    Effect m = class_effect;
    const bool demoted = m == Effect::Gpu && FrontDrawsOnly(method);
    if (demoted) {
        m = draw_effect();
    }
    const Effect now = SplitWriteTracker::Combine(effect, m);
    if (verify) [[unlikely]] {
        // FrontClass returns the computed class under verify, so class_effect is
        // FrontMethodEffect(method) and m is the old code's method_effect.
        const Effect old =
            SplitWriteTracker::Combine(SplitWriteTracker::Combine(effect, class_effect), m);
        return VerifyWriterSetExec(method, old, now, demoted);
    }
    return now;
}

} // Anonymous namespace

bool Maxwell3D::Nx2DrawProgramsOf(const Regs& regs, bool native, u64 memory_id, u64 map_serial,
                                  Tegra::Nx2DrawPrograms& out) {
    // As ShaderCache::RefreshStages resolves them. Without rasterization (no pixel program),
    // with transform feedback or with task and mesh programs a draw counts as a writer.
    if (regs.rasterize_enable == 0 || regs.transform_feedback_enabled != 0 ||
        (native && regs.IsMeshEnabled())) {
        return false;
    }
    out = {};
    out.memory_id = memory_id;
    out.map_serial = map_serial;
    out.native = native;
    const GPUVAddr base_addr = regs.program_region.Address();
    for (size_t index = 0; index < Regs::MaxShaderProgram; ++index) {
        if (!regs.IsShaderConfigEnabled(index)) {
            continue;
        }
        const auto& config = regs.pipelines[index];
        const GPUVAddr address =
            native ? (static_cast<u64>(config.reserved[0] & 0xff) << 32) | config.reserved[1]
                   : base_addr + config.offset;
        // Bit 63 tells an enabled program at address 0 from a disabled slot.
        out.addresses[index] = address | (u64{1} << 63);
    }
    return true;
}

VideoCommon::SplitWriteTracker::Effect Maxwell3D::FrontDrawEffect(VideoCommon::GpuSplit& split,
                                                                  FrontState& fs) {
    using Effect = VideoCommon::SplitWriteTracker::Effect;
    if (!VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::MacroReadWriterSet) ||
        fs.draw_window_writer) {
        return Effect::Gpu;
    }
    // The epoch before the proofs: a proof found below then holds for this epoch or is stale.
    const u64 epoch = Tegra::g_nx2_code_epoch.load(std::memory_order_acquire);
    const u64 version = Tegra::Nx2NonWriters().version.load(std::memory_order_acquire);
    const u64 map = memory_manager.GetMapSerial();
    const bool native = GetNativeMacroEngine().native_turing;
    if (!fs.draw_key_valid || fs.draw_key_epoch != epoch || fs.draw_key_version != version ||
        fs.draw_key_map != map || fs.draw_key_native != native) {
        fs.draw_key_valid = true;
        fs.draw_key_epoch = epoch;
        fs.draw_key_version = version;
        fs.draw_key_map = map;
        fs.draw_key_native = native;
        Tegra::Nx2DrawPrograms programs;
        fs.draw_non_writer =
            Nx2DrawProgramsOf(reinterpret_cast<const Regs&>(fs.regs), native,
                              memory_manager.GetID(), map, programs) &&
            Tegra::Nx2IsNonWriterDraw(programs, epoch);
    }
    if (!fs.draw_non_writer || (fs.draw_window_epoch != 0 && fs.draw_window_epoch != epoch)) {
        fs.draw_window_writer = true;
        return Effect::Gpu;
    }
    fs.draw_window_epoch = epoch;
    split.DrawEpoch(epoch);
    return Effect::Draw;
}

bool Maxwell3D::FrontIsWriterDraw(const Regs& regs) {
    if (regs.rasterize_enable == 0) {
        return true;
    }
    if (regs.transform_feedback_enabled != 0) {
        return true;
    }
    if (!Tegra::g_nx2_has_graphics_writer_shaders.load(std::memory_order_acquire)) {
        return false;
    }
    for (std::size_t stage = 0; stage < Regs::MaxShaderProgram; ++stage) {
        if (!regs.IsShaderConfigEnabled(stage)) {
            continue;
        }
        const u32 addr_low = regs.pipelines[stage].reserved[1];
        if (Tegra::Nx2IsGraphicsWriterShader(addr_low)) {
            return true;
        }
    }
    return false;
}

Maxwell3D::FrontState& Maxwell3D::Front(Core::System& system, VideoCommon::GpuSplit& split) {
    if (!front) [[unlikely]] {
        // Copy the back's state and take the macros over while it is idle (state writes it
        // still holds in its method sink first).
        split.Drain(VideoCommon::DrainSite::FrontStart);
        ConsumeSink(system);
        front = std::make_unique<FrontState>();
        std::memcpy(front->regs.data(), regs.reg_array.data(), sizeof(front->regs));
        std::memcpy(front->shadow.data(), shadow_state.reg_array.data(), sizeof(front->shadow));
        front->control = shadow_state.shadow_ram_control;
        front->skip_same = SkipSameValueWrites() && FrontSkipSame();
        front->same_first = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::FrontSameFirst);
        front->class_table = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::FrontClassTable);
        front->writer_set_exec = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::WriterSetExec);
        front->draw_begin_coalesce =
            VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::DrawBeginCoalesce);
        front->verify = VideoCommon::Nx2Perf::Verify();
        front->exec_same_drop = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ExecSameDrop);
        front->system = &system;
        front->executing_macro = executing_macro;
        front->parameters = std::move(macro_params);
        macro_params.clear();
        executing_macro = 0;
        front_macros = true;
    }
    return *front;
}

// With the split on, engine methods run on its back only: one here on another thread means the
// split stopped (accuracy settings changed mid-game). Hand the macros back: the front's copy
// goes stale from now on, and a restart copies the registers again (Front).
void Maxwell3D::ReleaseFront() {
    executing_macro = front->executing_macro;
    macro_params = std::move(front->parameters);
    std::memcpy(shadow_state.reg_array.data(), front->shadow.data(), sizeof(front->shadow));
    shadow_state.shadow_ram_control = front->control;
    front.reset();
    front_macros = false;
    LOG_INFO(HW_GPU, "GPU thread split stopped: 3D macros back on the engine");
}

void Maxwell3D::FrontWrite(VideoCommon::GpuSplit& split, FrontState& fs, u32 method,
                           u32 argument, bool is_last_call, bool from_macro) {
    namespace Profile = VideoCommon::Nx2Profile;
    if (fs.unknown_count != 0 && fs.unknown_regs[method]) [[unlikely]] {
        // dma_forward: a tracked write sets both the register and its shadow copy again; any
        // other mode keeps or reads one of them, so take the back's values first.
        if (fs.control == Regs::ShadowRamControl::Track ||
            fs.control == Regs::ShadowRamControl::TrackWithFilter) {
            fs.ForgetUnknown(method);
        } else {
            fs.Learn(*this, split, method);
        }
    }
    // Nx2Perf FrontSameFirst: the same-value state write skip below, decided first. Outside
    // Replay the value is the argument, and a tracked shadow RAM keeps it only if it holds it.
    bool same_first = false;
    if (fs.same_first) {
        const bool tracked = fs.control == Regs::ShadowRamControl::Track ||
                             fs.control == Regs::ShadowRamControl::TrackWithFilter;
        same_first = !execution_mask[method] && fs.skip_same &&
                     fs.control != Regs::ShadowRamControl::Replay &&
                     fs.regs[method] == argument && (!tracked || fs.shadow[method] == argument);
        if (same_first && !fs.verify) {
            if (Profile::Enabled()) [[unlikely]] {
                Profile::Add(Profile::Counter::MethodWrites, 1);
                Profile::Add(from_macro ? Profile::Counter::MacroWrites
                                        : Profile::Counter::DirectWrites, 1);
                Profile::Add(Profile::Counter::SameValueWrites, 1);
            }
            return;
        }
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
    if (fs.same_first && fs.verify) [[unlikely]] {
        const bool old_skip = !execution_mask[method] && fs.regs[method] == value &&
                              shadow_kept && fs.skip_same;
        if (old_skip != same_first) {
            static std::atomic<u32> reports{};
            if (reports.fetch_add(1, std::memory_order_relaxed) < 64) {
                LOG_ERROR(HW_GPU, "Nx2Perf front: same-first method {:#x} argument {:#x} skip {}, "
                          "old {} (control {}, regs {:#x})", method, argument, same_first, old_skip,
                          static_cast<u32>(fs.control), fs.regs[method]);
            }
        }
    }
    if (Profile::Enabled()) [[unlikely]] {
        Profile::Add(Profile::Counter::MethodWrites, 1);
        Profile::Add(from_macro ? Profile::Counter::MacroWrites : Profile::Counter::DirectWrites, 1);
    }
    using Effect = VideoCommon::GpuSplit::Effect;
    // What the entry may write on the back (FrontDmaRead): any method may issue a deferred
    // instanced draw first.
    Effect effect = fs.draw_pending ? FrontDrawEffect(split, fs) : Effect::None;
    if (execution_mask[method] && FrontSkipsLoopWrite(fs, method, value, shadow_kept)) {
        if (Profile::Enabled()) [[unlikely]] {
            Profile::Add(Profile::Counter::SameValueWrites, 1);
        }
        return;
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
        if (IsDrawProgramRegister(method)) {
            fs.draw_key_valid = false;
        }
        if (from_macro) {
            split.MacroWrite(this, method, argument, effect);
        } else {
            split.Method(this, method, argument, is_last_call, nullptr, effect);
        }
        return;
    }
    if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && FrontVerifyEnabled()) [[unlikely]] {
        fs.control_writes[fs.control_count++ % fs.control_writes.size()] = {
            argument, static_cast<u32>(fs.control), from_macro ? 2U : 1U};
    }
    // Nx2Perf exec_same_drop. With Verify the write still runs on the back, which checks that
    // it changed nothing; the front's state moves on as if it were left out.
    if (fs.exec_same_drop) [[unlikely]] {
        if (const u32 kind = FrontExecSameDrop(split, fs, method, value, shadow_kept);
            kind != ~0U) {
            if (fs.verify) {
                split.Defer(
                    [this, system = fs.system, method, argument,
                     last = from_macro || is_last_call,
                     kind] { VerifyExecSameDrop(*system, method, argument, last, kind); },
                    effect);
            } else if (Profile::Enabled()) [[unlikely]] {
                Profile::Add(Profile::Counter::SameValueWrites, 1);
            }
            return;
        }
    }
    std::optional<GPUVAddr> cb_write;
    u32 cb_room{};
    if (Profile::Enabled() && fs.regs[method] == value) [[unlikely]] {
        Profile::AddSameExecutable(method);
    }
    fs.regs[method] = value;
    if (VideoCommon::Nx2Perf::FenceTimeline::On() &&
        method == MAXWELL3D_REG_INDEX(sync_info)) [[unlikely]] {
        VideoCommon::Nx2Perf::FenceTimeline::StampFront(value & 0xffff); // sync_info.sync_point
    }
    if (method == MAXWELL3D_REG_INDEX(sync_info) && VideoCommon::Nx2Cow::On()) [[unlikely]] {
        VideoCommon::Nx2Cow::FrontIncrement(value & 0xffff); // run_ahead_cow_probe boundary
    }
    const u8 cls = fs.class_table ? FrontClass(method, fs.verify) : u8{0};
    if (IsDrawProgramRegister(method)) {
        fs.draw_key_valid = false;
    }
    effect = FrontExecEffect(effect, method, cls, fs.class_table, fs.writer_set_exec, fs.verify,
                             [&] { return FrontDrawEffect(split, fs); });
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
    default:
        if (fs.class_table ? (cls & ClassCbData) != 0
                           : method >= cb_data && method < cb_data + 16) {
            cb_write = ConstBufferWriteAddress(fs.regs);
            cb_room = ConstBufferRoomWords(fs.regs);
            fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)] += sizeof(u32);
        } else if (fs.class_table ? (cls & ClassOutsideDrawManager) == 0
                                  : !FrontHandledOutsideDrawManager(method)) {
            fs.draw_pending = true;
        }
        break;
    }
    if (fs.draw_begin_coalesce && method == MAXWELL3D_REG_INDEX(draw.begin) &&
        ((value >> 26) & 3) == 1) [[unlikely]] {
        // Nx2Perf draw_begin_coalesce: an instance loop's begin right after the same one only
        // counts one more instance on the back (DrawManager::AddInstances). With Verify it is
        // sent anyway, flagged for the back to check.
        if (split.RepeatableMethod(this, method, argument, is_last_call, from_macro, effect,
                                   fs.verify) &&
            Profile::Enabled()) [[unlikely]] {
            Profile::Add(Profile::Counter::DrawBeginCoalesced, 1);
        }
        if (!from_macro && (fs.class_table ? (cls & ClassDraw) != 0 : IsDrawMethod(method)) &&
            FrontIsWriterDraw(fs.regs)) {
            split.NoteWriter();
        }
    } else if (from_macro) {
        split.MacroWrite(this, method, argument, effect);
    } else {
        split.Method(this, method, argument, is_last_call, nullptr, effect);
        if ((fs.class_table ? (cls & ClassDraw) != 0 : IsDrawMethod(method)) &&
            FrontIsWriterDraw(fs.regs)) {
            split.NoteWriter();
        }
    }
    if (cb_write) {
        // The back writes the value after its shadow RAM step (ProcessCBData), as `value` here.
        split.NoteConstBufferWrite(memory_manager, *cb_write, &value, 1, (std::min)(cb_room, 1U));
    } else if (method == MAXWELL3D_REG_INDEX(report_semaphore.query)) {
        split.NoteWrite(memory_manager, ReportAddress(fs.regs), ReportBytes);
        NoteLateReport(split, memory_manager, fs.regs);
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
    if (front_run_record != nullptr) [[unlikely]] {
        front_run_record->push_back({method, argument, is_last_call ? 1U : 0U});
    }
    FrontWrite(split, fs, method, argument, is_last_call, false);
}

void Maxwell3D::FrontCallMethodRun(Core::System& system, VideoCommon::GpuSplit& split,
                                   u32 method, const u32* words, u32 count, bool incrementing,
                                   u32 methods_pending) {
    // FrontCallMethod for each word, as DmaPusher::ProcessCommands calls it word by word.
    DEBUG_ASSERT(count <= methods_pending &&
                 (incrementing ? method + count : method + 1) <= MacroRegistersStart);
    auto& fs = Front(system, split);
    for (u32 i = 0; i < count; ++i) {
        FrontWrite(split, fs, incrementing ? method + i : method, words[i],
                   methods_pending - i <= 1, false);
    }
}

void Maxwell3D::FrontCallMultiMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                     u32 method, const u32* base_start, u32 amount,
                                     u32 methods_pending) {
    auto& fs = Front(system, split);
    if (method >= MacroRegistersStart) {
        const auto& options = FrontMacroOptions();
        if (options.borrow && amount != 0 && amount == methods_pending &&
            fs.executing_macro == 0 && fs.parameters.empty()) {
            // The whole call in this run: the macro reads the words where they are.
            fs.executing_macro = method;
            if (!options.verify) [[likely]] {
                FrontRunMacro(system, split, fs, {base_start, amount});
                return;
            }
            static std::vector<u32> copy;
            copy.assign(base_start, base_start + amount);
            FrontRunMacro(system, split, fs, {base_start, amount});
            static u64 differences{};
            if (std::memcmp(copy.data(), base_start, amount * sizeof(u32)) != 0 &&
                ++differences <= 64) {
                LOG_ERROR(HW_GPU,
                          "Nx2Perf macro: macro {:#x}'s {} borrowed parameters changed during "
                          "the call ({} calls so far)",
                          method, amount, differences);
            }
            return;
        }
        if (fs.executing_macro == 0) {
            fs.executing_macro = method;
        }
        fs.parameters.insert(fs.parameters.end(), base_start, base_start + amount);
        if (amount == methods_pending) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    if (fs.exec_same_drop && BindGroupStage(method) != ~0U) [[unlikely]] {
        fs.exec_bind_memo.Clear(); // exec_same_drop: binds the front doesn't track
    }
    // What CallMultiMethod does to the registers on the back, then one entry for all the words.
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    const auto tracked = [&fs] {
        return fs.control == Regs::ShadowRamControl::Track ||
               fs.control == Regs::ShadowRamControl::TrackWithFilter;
    };
    using Effect = VideoCommon::GpuSplit::Effect;
    Effect effect = fs.draw_pending ? FrontDrawEffect(split, fs) : Effect::None;
    std::optional<GPUVAddr> cb_write;
    u32 cb_room{};
    const u8 cls = fs.class_table ? FrontClass(method, fs.verify) : u8{0};
    if (method >= cb_data && method < cb_data + 16) {
        cb_write = ConstBufferWriteAddress(fs.regs);
        cb_room = ConstBufferRoomWords(fs.regs);
        fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)] += amount * sizeof(u32);
    } else if (method == MAXWELL3D_REG_INDEX(inline_data)) {
        // Upload data only.
        effect = Effect::Unknown;
    } else if ((method == MAXWELL3D_REG_INDEX(draw_inline_index) ||
                method == MAXWELL3D_REG_INDEX(inline_index_2x16.even) ||
                method == MAXWELL3D_REG_INDEX(inline_index_4x8.index0)) &&
               fs.control != Regs::ShadowRamControl::Replay) {
        FrontTrackInstanceLoop(fs, method, 0);
        if (amount != 0) {
            const u32 last = base_start[amount - 1];
            if (tracked()) {
                fs.shadow[method] = last;
            }
            fs.regs[method] = last;
        }
        fs.draw_pending = true;
        effect = VideoCommon::SplitWriteTracker::Combine(effect, FrontDrawEffect(split, fs));
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
            FrontTrackInstanceLoop(fs, method, value); // DrawBegin runs for every word
            if (IsDrawProgramRegister(method)) {
                fs.draw_key_valid = false;
            }
            if (!execution_mask[method]) {
                continue;
            }
            effect = FrontExecEffect(effect, method, cls, fs.class_table, fs.writer_set_exec,
                                     fs.verify, [&] { return FrontDrawEffect(split, fs); });
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
            default:
                if (fs.class_table ? (cls & ClassOutsideDrawManager) == 0
                                   : !FrontHandledOutsideDrawManager(method)) {
                    fs.draw_pending = true;
                }
                break;
            }
        }
    }
    split.MultiMethod(this, method, base_start, amount, methods_pending, nullptr, effect);
    if ((fs.class_table ? (cls & ClassDraw) != 0 : IsDrawMethod(method)) &&
        FrontIsWriterDraw(fs.regs)) {
        split.NoteWriter();
    }
    if (cb_write) {
        split.NoteConstBufferWrite(memory_manager, *cb_write, base_start, amount,
                                   (std::min)(cb_room, amount));
    } else if (method == MAXWELL3D_REG_INDEX(report_semaphore.query) && amount != 0) {
        split.NoteWrite(memory_manager, ReportAddress(fs.regs), ReportBytes);
        NoteLateReport(split, memory_manager, fs.regs);
    }
}

void Maxwell3D::FrontRunMacro(Core::System& system, VideoCommon::GpuSplit& split,
                              FrontState& fs, std::span<const u32> borrowed) {
#ifdef ARCHITECTURE_x86_64
    namespace Profile = VideoCommon::Nx2Profile;
    const u32 method = fs.executing_macro;
    fs.executing_macro = 0;
    auto& engine = GetNativeMacroEngine();
    const auto positions = GetNativeMacroPositions();
    const u32 start = positions[((method - MacroRegistersStart) >> 1) % positions.size()];
    const auto& options = FrontMacroOptions();
    const bool profile = Profile::Enabled();
    // EDEN_NX2_MACRO_TRACE=<macro index>: log the first 32 calls of one macro (its parameters,
    // every write in order, the words its DMA reads return) and its code once.
    static const u32 trace_macro = [] {
        const char* value = std::getenv("EDEN_NX2_MACRO_TRACE");
        return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 0)) : ~0U;
    }();
    static u32 traced_calls{};
    const bool trace = trace_macro != ~0U && ((method - MacroRegistersStart) >> 1) == trace_macro &&
                       traced_calls < 32;
    std::string trace_line;
    if (trace) [[unlikely]] {
        if (traced_calls == 0) {
            std::string code;
            for (u32 pc = start; pc < start + 96 && pc * 3 + 2 < engine.native_code.size(); ++pc) {
                code += fmt::format(" {:08x}{:08x}{:08x}", engine.native_code[pc * 3 + 2],
                                    engine.native_code[pc * 3 + 1], engine.native_code[pc * 3]);
            }
            LOG_INFO(HW_GPU, "Nx2 macro trace {:#x}: start {:#x}, code (96-bit words):{}",
                     trace_macro, start, code);
        }
        const std::span<const u32> parameters =
            borrowed.empty() ? std::span<const u32>{fs.parameters} : borrowed;
        trace_line = "params";
        for (size_t i = 0; i < parameters.size() && i < 24; ++i) {
            trace_line += fmt::format(" {:#x}", parameters[i]);
        }
        trace_line += " | writes";
    }
    u64 writes_before{};
    u64 same_before{};
    u64 ring_before{};
    u32 dma_reads{};
    if (profile) [[unlikely]] {
        writes_before = Profile::CounterValue(Profile::Counter::MethodWrites);
        same_before = Profile::CounterValue(Profile::Counter::SameValueWrites);
        ring_before = split.WritePosition();
    }
    // Nx2Perf dma_forward: a macro that did an MME DMA read before is dry-run first. When its
    // DMA words only feed its last writes (render enable: macro 0x16 in Ocarina of Time), the
    // front sends the other writes and doesn't wait; the back reads the words in ring order,
    // after the same flushes FrontDmaRead waits for, and writes those values.
    const u32 macro_index = (method - MacroRegistersStart) >> 1;
    namespace DmaForward = VideoCommon::DmaForward;
    // Analyzed on demand: a dma_forward_memo hit uses its cached plan without building one.
    std::optional<DmaForward::Plan> analyzed_plan;
    const DmaForward::Plan* dma_plan = nullptr;
    bool forwarded_call = false;
    bool verify_forward = false;
    if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::DmaForward) && !trace &&
        macro_index < fs.dma_macros.size() && fs.dma_macros[macro_index]) [[unlikely]] {
        Profile::Scope profile_scope{Profile::Zone::Macro};
        static u64 forwarded_calls{};
        static u64 fallen_back{};
        static std::array<std::string_view, 8> reasons_logged{};
        const std::span<const u32> parameters =
            borrowed.empty() ? std::span<const u32>{fs.parameters} : borrowed;
        const auto read_register = [&fs](u32 address, bool& unknown) -> u32 {
            if (address >= Regs::NUM_REGS) {
                unknown = false;
                return 0;
            }
            unknown = fs.unknown_count != 0 && fs.unknown_regs[address];
            return fs.regs[address];
        };
        const auto forwardable = [](u32 address) {
            return address == MAXWELL3D_REG_INDEX(render_enable.mode);
        };
        // dma_forward_tape: the back evaluates a recorded tape instead of re-running the call.
        static const bool tape_on =
            VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::DmaForwardTape);
        const auto analyze = [&](auto&& read) {
            if (tape_on) {
                return DmaForward::Analyze<true>(
                    std::span<const u32>{engine.native_code}, start, parameters,
                    std::span<const u32>{native_data_ram}, MacroRegistersStart, read, forwardable);
            }
            return DmaForward::Analyze(std::span<const u32>{engine.native_code}, start, parameters,
                                       std::span<const u32>{native_data_ram}, MacroRegistersStart,
                                       read, forwardable);
        };
        const bool memo = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::DmaForwardMemo);
        const u64 code_generation = engine.native_code_generation;
        if (fs.control == Regs::ShadowRamControl::Replay) {
            // The back would write the shadow copy, not the words.
            static const DmaForward::Plan replay_plan = [] {
                DmaForward::Plan plan;
                plan.reason = "shadow RAM replay";
                return plan;
            }();
            dma_plan = &replay_plan;
        } else if (!memo) {
            dma_plan = &analyzed_plan.emplace(analyze(read_register));
        } else {
            DmaForward::Memo& cache = fs.dma_memo;
            const DmaForward::MemoEntry* hit =
                cache.Find(code_generation, start, parameters,
                           std::span<const u32>{native_data_ram}, read_register);
            if (hit != nullptr) {
                ++cache.hits;
                dma_plan = &hit->plan;
                if (VideoCommon::Nx2Perf::Verify()) [[unlikely]] {
                    static u64 checked{};
                    static u64 differed{};
                    dma_plan = &analyzed_plan.emplace(analyze(read_register));
                    ++checked;
                    if (!DmaForward::SamePlan(*dma_plan, hit->plan) && differed++ < 64) {
                        LOG_ERROR(HW_GPU,
                                  "Nx2Perf dma_forward_memo: plan differs for macro {:#x} ({} "
                                  "differences in {} hits)",
                                  macro_index, differed, checked);
                    }
                    if ((checked & 0xffff) == 0) {
                        LOG_INFO(HW_GPU, "Nx2Perf dma_forward_memo: {} hits checked, {} differed",
                                 checked, differed);
                    }
                }
            } else {
                ++cache.misses;
                std::array<DmaForward::RegisterRead, DmaForward::MaxReads> reads;
                u32 num_reads = 0;
                bool overflow = false;
                dma_plan = &analyzed_plan.emplace(
                    analyze([&](u32 address, bool& unknown) -> u32 {
                        const u32 value = read_register(address, unknown);
                        if (num_reads < reads.size()) {
                            reads[num_reads++] = {address, value, unknown};
                        } else {
                            overflow = true;
                        }
                        return value;
                    }));
                if (dma_plan->eligible && !overflow &&
                    parameters.size() <= DmaForward::MaxParameters) {
                    DmaForward::MemoEntry& entry = cache.Victim();
                    entry.valid = true;
                    entry.generation = code_generation;
                    entry.start = start;
                    entry.num_parameters = static_cast<u32>(parameters.size());
                    std::ranges::copy(parameters, entry.parameters.begin());
                    entry.num_reads = num_reads;
                    std::copy_n(reads.begin(), num_reads, entry.reads.begin());
                    entry.plan = *dma_plan;
                }
            }
            if (++cache.calls_since_check >= 64) {
                cache.calls_since_check = 0;
                const u64 now = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
                if (now - cache.last_log_ns >= 1'000'000'000ULL) {
                    if (cache.last_log_ns != 0) {
                        const u64 lookups = cache.hits + cache.misses;
                        LOG_INFO(HW_GPU,
                                 "Nx2Perf dma_forward_memo: {} hits / {} lookups ({}%), {} slim "
                                 "ring entries, {} slot-full fallbacks in the last second",
                                 cache.hits, lookups, lookups ? cache.hits * 100 / lookups : 0,
                                 cache.slim_entries, cache.slot_full);
                    }
                    cache.last_log_ns = now;
                    cache.hits = cache.misses = cache.slim_entries = cache.slot_full = 0;
                }
            }
        }
        if (tape_on && dma_plan->eligible) {
            static u64 tape_calls{};
            static u64 tape_overflow{};
            static u64 tape_unsupported{};
            if (dma_plan->tape_valid) {
                ++tape_calls;
            } else if (dma_plan->tape_fallback == DmaForward::TapeFallback::Overflow) {
                ++tape_overflow;
            } else {
                ++tape_unsupported;
            }
            if (((tape_calls + tape_overflow + tape_unsupported) & 0xffff) == 0) {
                LOG_INFO(HW_GPU,
                         "Nx2Perf dma_forward_tape: {} tape calls, fallbacks: {} overflow, {} "
                         "unsupported op",
                         tape_calls, tape_overflow, tape_unsupported);
            }
        }
        if (!dma_plan->eligible) {
            ++fallen_back;
            const auto logged = std::ranges::find(reasons_logged, dma_plan->reason);
            const auto free = std::ranges::find(reasons_logged, std::string_view{});
            if (logged == reasons_logged.end() && free != reasons_logged.end()) {
                *free = dma_plan->reason;
                LOG_INFO(HW_GPU, "Nx2Perf dma_forward: macro {:#x} not forwarded: {}", macro_index,
                         dma_plan->reason);
            }
        } else if (VideoCommon::Nx2Perf::Verify() && !(tape_on && dma_plan->tape_valid)) {
            verify_forward = true; // The old path runs; its writes are compared below.
        } else {
            // The back's half: the read's flushes, the words, the forwarded writes.
            struct ForwardRead {
                static void Run(Maxwell3D* engine, GPUVAddr source, u32 dma_words, u32* words) {
                    auto& memory = engine->memory_manager;
                    const size_t bytes = size_t{dma_words} * sizeof(u32);
                    if (memory.IsMemoryDirty(source, bytes, VideoCommon::CacheType::BufferCache)) {
                        memory.FlushRegion(source, bytes, VideoCommon::CacheType::BufferCache);
                    }
                    memory.FlushQueryResults(source, bytes);
                    memory.ReadWordsUnsafe(source, words, dma_words);
                }
            };
            struct ForwardCall {
                Maxwell3D* engine;
                Core::System* system;
                DmaForward::Snapshot snapshot;
                static void Run(Maxwell3D* engine, Core::System* system,
                                const DmaForward::SlimSnapshot& snapshot,
                                std::span<const u32> code) {
                    std::array<u32, DmaForward::MaxDmaWords> words{};
                    ForwardRead::Run(engine, snapshot.dma_source, snapshot.dma_words,
                                     words.data());
                    const bool ok = DmaForward::Recompute(
                        snapshot, code, std::span<const u32>{words.data(), snapshot.dma_words},
                        MacroRegistersStart, [engine, system](u32 forwarded_method, u32 value) {
                            engine->ConsumeSink(*system);
                            engine->CallMethod(*system, forwarded_method, value, true);
                        });
                    static u32 failures{};
                    if (!ok && failures++ < 16) {
                        LOG_ERROR(HW_GPU, "Nx2Perf dma_forward: the back's run of a forwarded "
                                          "macro call failed or made other writes");
                    }
                }
                void operator()() const {
                    Run(engine, system, snapshot, std::span<const u32>{snapshot.code});
                }
            };
            // dma_forward_memo: the code from an immutable slot instead of a copy in the entry.
            struct SlimForwardCall {
                Maxwell3D* engine;
                Core::System* system;
                const u32* code;
                DmaForward::SlimSnapshot snapshot;
                void operator()() const {
                    ForwardCall::Run(engine, system, snapshot,
                                     std::span<const u32>{code, snapshot.num_code_words});
                }
            };
            // dma_forward_tape: the same read, then the tape's writes.
            struct TapeCall {
                Maxwell3D* engine;
                Core::System* system;
                GPUVAddr dma_source;
                DmaForward::Tape tape;
                void operator()() const {
                    std::array<u32, DmaForward::TapeSlots> slots{};
                    ForwardRead::Run(engine, dma_source, tape.dma_words, slots.data());
                    DmaForward::EvalTape(tape, slots, [this](u32 forwarded_method, u32 value) {
                        engine->ConsumeSink(*system);
                        engine->CallMethod(*system, forwarded_method, value, true);
                    });
                }
            };
            static_assert(sizeof(TapeCall) == 0x70 && std::is_trivially_copyable_v<TapeCall>);
            // EDEN_NX2_PERF_VERIFY: runs both, compares them and applies Recompute's writes.
            struct VerifyTapeCall {
                Maxwell3D* engine;
                Core::System* system;
                u32 macro_index;
                DmaForward::Tape tape;
                DmaForward::Snapshot snapshot;
                void operator()() const {
                    using Clock = std::chrono::steady_clock;
                    static u64 calls{};
                    static u64 differed{};
                    static u64 logged_differed{};
                    static u64 lines{};
                    static u64 tape_ns{};
                    static u64 recompute_ns{};
                    static Clock::time_point last_log{};
                    std::array<u32, DmaForward::TapeSlots> slots{};
                    ForwardRead::Run(engine, snapshot.dma_source, snapshot.dma_words,
                                     slots.data());
                    std::array<std::pair<u32, u32>, DmaForward::MaxTapeWrites> from_tape{};
                    size_t num_tape{};
                    const auto t0 = Clock::now();
                    DmaForward::EvalTape(tape, slots, [&](u32 forwarded_method, u32 value) {
                        from_tape[num_tape++] = {forwarded_method, value};
                    });
                    const auto t1 = Clock::now();
                    std::array<std::pair<u32, u32>, DmaForward::MaxWrites> from_recompute{};
                    size_t num_recompute{};
                    const bool ok = DmaForward::Recompute(
                        snapshot,
                        std::span<const u32>{slots.data(), snapshot.dma_words},
                        MacroRegistersStart, [&](u32 forwarded_method, u32 value) {
                            if (num_recompute < from_recompute.size()) {
                                from_recompute[num_recompute++] = {forwarded_method, value};
                            }
                        });
                    const auto t2 = Clock::now();
                    tape_ns += static_cast<u64>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
                    recompute_ns += static_cast<u64>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
                    static u32 failures{};
                    if (!ok && failures++ < 16) {
                        LOG_ERROR(HW_GPU, "Nx2Perf dma_forward: the back's run of a forwarded "
                                          "macro call failed or made other writes");
                    }
                    bool same = num_tape == num_recompute;
                    for (size_t i = 0; i < (std::max)(num_tape, num_recompute); ++i) {
                        const auto tape_write = i < num_tape ? from_tape[i]
                                                             : std::pair<u32, u32>{~0U, ~0U};
                        const auto old_write = i < num_recompute
                                                   ? from_recompute[i]
                                                   : std::pair<u32, u32>{~0U, ~0U};
                        if (tape_write == old_write) {
                            continue;
                        }
                        same = false;
                        if (lines < 16) {
                            ++lines;
                            LOG_ERROR(HW_GPU,
                                      "Nx2Perf dma_forward_tape: macro {:#x} write {:#x} tape "
                                      "{:#x} recompute {:#x}",
                                      macro_index, old_write.first, tape_write.second,
                                      old_write.second);
                        }
                    }
                    if (!same) {
                        ++differed;
                    }
                    for (size_t i = 0; i < num_recompute; ++i) {
                        engine->ConsumeSink(*system);
                        engine->CallMethod(*system, from_recompute[i].first,
                                           from_recompute[i].second, true);
                    }
                    if ((++calls & 0xffff) == 0) {
                        LOG_INFO(HW_GPU,
                                 "Nx2Perf dma_forward_tape: {} calls verified, {} differed; "
                                 "EvalTape {} ns, Recompute {} ns (mean, last 65536)",
                                 calls, differed, tape_ns / 65536, recompute_ns / 65536);
                        tape_ns = recompute_ns = 0;
                    }
                    if (differed != logged_differed && t2 - last_log >= std::chrono::seconds{1}) {
                        last_log = t2;
                        logged_differed = differed;
                        LOG_ERROR(HW_GPU, "Nx2Perf dma_forward_tape: {} of {} calls differed",
                                  differed, calls);
                    }
                }
            };
            for (size_t i = 0; i < dma_plan->num_writes; ++i) {
                const DmaForward::Write& write = dma_plan->writes[i];
                if (write.forwarded) {
                    if (i == dma_plan->snapshot.first_forwarded && tape_on &&
                        dma_plan->tape_valid) {
                        if (VideoCommon::Nx2Perf::Verify()) {
                            split.Defer(VerifyTapeCall{this, &system, macro_index, dma_plan->tape,
                                                       dma_plan->snapshot});
                        } else {
                            split.Defer(TapeCall{this, &system, dma_plan->snapshot.dma_source,
                                                 dma_plan->tape});
                        }
                    } else if (i == dma_plan->snapshot.first_forwarded) {
                        DmaForward::CodeSlot* const slot =
                            memo ? fs.dma_memo.AcquireSlot(code_generation, dma_plan->snapshot,
                                                           split.ConsumedNow())
                                 : nullptr;
                        if (slot != nullptr) {
                            split.Defer(SlimForwardCall{this, &system, slot->code.data(),
                                                        dma_plan->snapshot});
                            slot->last_use = split.WritePosition();
                            ++fs.dma_memo.slim_entries;
                        } else {
                            if (memo) {
                                ++fs.dma_memo.slot_full;
                            }
                            split.Defer(ForwardCall{this, &system, dma_plan->snapshot});
                        }
                    }
                    fs.MarkUnknown(write.method);
                    continue;
                }
                // The DMA read's own write goes to the back as before; the read doesn't happen.
                FrontWrite(split, fs, write.method, write.value, true, true);
            }
            for (size_t i = 0; i < dma_plan->num_ram_writes; ++i) {
                native_data_ram[dma_plan->ram_writes[i].first] = dma_plan->ram_writes[i].second;
            }
            forwarded_call = true;
            if ((++forwarded_calls & 0xffff) == 0) {
                LOG_INFO(HW_GPU, "Nx2Perf dma_forward: {} macro calls forwarded, {} not",
                         forwarded_calls, fallen_back);
            }
        }
    }
    std::array<std::pair<u32, u32>, DmaForward::MaxWrites> verify_writes{};
    size_t verify_count{};
    std::array<u32, DmaForward::MaxDmaWords> verify_words{};
    u32 verify_word_count{};
    if (!forwarded_call) {
        Profile::Scope profile_scope{Profile::Zone::Macro};
        engine.native_interpreter.active_dram = &native_data_ram;
        if (!engine.native_jit) {
            engine.native_jit = std::make_shared<TuringMacroJit>();
        }
        engine.native_jit->Prepare(engine.native_code, engine.native_code_generation,
                                   options.batch);
        const size_t parameter_count = borrowed.empty() ? fs.parameters.size() : borrowed.size();
        // The early return FrontWrite takes for a write that changes nothing: it reads and
        // writes only fs.regs/fs.shadow at the method, so a batched write sees the earlier ones.
        const auto same_value = [&fs, this](u32 address, u32 value) {
            if (execution_mask[address] || !fs.skip_same) {
                return false;
            }
            if (fs.control == Regs::ShadowRamControl::Track ||
                fs.control == Regs::ShadowRamControl::TrackWithFilter) {
                return fs.shadow[address] == value && fs.regs[address] == value;
            }
            const u32 stored =
                fs.control == Regs::ShadowRamControl::Replay ? fs.shadow[address] : value;
            return fs.regs[address] == stored;
        };
        const auto front_read = [&](u32 address) {
            if (address >= Regs::NUM_REGS) {
                return 0U;
            }
            if (fs.unknown_count != 0 && fs.unknown_regs[address]) [[unlikely]] {
                fs.Learn(*this, split, address);
            }
            return fs.regs[address];
        };
        const auto front_emit = [&](u32 address, u32 value) {
            if (trace) [[unlikely]] {
                trace_line += fmt::format(" {:#x}={:#x}", address, value);
            }
            if (verify_forward && verify_count < verify_writes.size()) [[unlikely]] {
                verify_writes[verify_count++] = {address, value};
            }
            if (address >= MacroRegistersStart) {
                return false;
            }
            if (!options.batch || !same_value(address, value)) {
                FrontWrite(split, fs, address, value, true, true);
            } else if (!options.verify) [[likely]] {
                if (profile) [[unlikely]] {
                    Profile::Add(Profile::Counter::MethodWrites, 1);
                    Profile::Add(Profile::Counter::SameValueWrites, 1);
                }
            } else {
                const u64 ring = split.WritePosition();
                const u32 reg = fs.regs[address];
                const u32 shadow = fs.shadow[address];
                const auto control = fs.control;
                FrontWrite(split, fs, address, value, true, true);
                static u64 differences{};
                if ((split.WritePosition() != ring || fs.regs[address] != reg ||
                     fs.shadow[address] != shadow || fs.control != control) &&
                    ++differences <= 64) {
                    LOG_ERROR(HW_GPU,
                              "Nx2Perf macro: macro {:#x} write {:#x} = {:#x} taken as same "
                              "value: register {:#x} -> {:#x}, shadow {:#x} -> {:#x}, ring "
                              "words {} ({} so far)",
                              method, address, value, reg, fs.regs[address], shadow,
                              fs.shadow[address], split.WritePosition() - ring, differences);
                }
            }
            // MME_DMA_READ (into data RAM) and MME_DMA_READ_FIFOED (into the load FIFO).
            if (address == 0x157 || address == 0x158) {
                // The read waits for the back, which may write the borrowed words.
                if (!borrowed.empty() && fs.parameters.empty()) {
                    fs.parameters.assign(borrowed.begin(), borrowed.end());
                }
                ++dma_reads;
                if (macro_index < fs.dma_macros.size()) {
                    fs.dma_macros[macro_index] = true;
                }
                const size_t parameters_before = fs.parameters.size();
                FrontDmaRead(split, fs, method, address, value);
                if (verify_forward && address == 0x158) [[unlikely]] {
                    for (size_t i = parameters_before;
                         i < fs.parameters.size() && verify_word_count < verify_words.size();
                         ++i) {
                        verify_words[verify_word_count++] = fs.parameters[i];
                    }
                }
                if (trace) [[unlikely]] {
                    trace_line += " [dma";
                    if (address == 0x158) {
                        for (size_t i = parameters_before; i < fs.parameters.size(); ++i) {
                            trace_line += fmt::format(" {:#x}", fs.parameters[i]);
                        }
                    } else {
                        const u32 ram = fs.regs[0x156];
                        for (u32 i = 0; i < value && ram + i < native_data_ram.size(); ++i) {
                            trace_line += fmt::format(" ram[{:#x}]={:#x}", ram + i,
                                                      native_data_ram[ram + i]);
                        }
                    }
                    trace_line += "]";
                }
            }
            return true;
        };
        const TuringMacroJit::Options jit_options{.batch_emits = options.batch,
                                                  .keep_state = options.keep_state};
        // Nx2Perf front_macro_memo: replay a recorded call of a start whose code uses no data
        // RAM and no MME DMA (front_macro_memo.h). With PERF_VERIFY a hit runs the JIT anyway
        // and compares its emits with the entry's.
        namespace MacroMemo = VideoCommon::FrontMacroMemo;
        static const bool macro_memo_on =
            VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::FrontMacroMemo);
        bool replayed = false;
        bool record = false;
        const MacroMemo::Entry* check = nullptr;
        std::span<const u32> memo_parameters;
        const u64 generation = engine.native_code_generation;
        if (macro_memo_on && !trace && !verify_forward &&
            !(macro_index < fs.dma_macros.size() && fs.dma_macros[macro_index])) {
            auto& memo = fs.macro_memo;
            const u32 bucket = MacroMemo::Memo::Bucket(macro_index);
            memo_parameters = borrowed.empty() ? std::span<const u32>{fs.parameters} : borrowed;
            if (!memo.Memoizable(std::span<const u32>{engine.native_code}, generation, start,
                                 MacroRegistersStart)) {
                ++memo.skipped[bucket];
            } else if (const MacroMemo::Entry* hit = memo.Find(
                           generation, start, memo_parameters,
                           [&fs](u32 address, u32 value) {
                               return fs.regs[address] == value &&
                                      !(fs.unknown_count != 0 && fs.unknown_regs[address]);
                           })) {
                ++memo.hits[bucket];
                if (!options.verify) [[likely]] {
                    for (u32 i = 0; i < hit->num_emits; ++i) {
                        front_emit(hit->emits[i].first, hit->emits[i].second);
                    }
                    replayed = true;
                } else {
                    check = hit;
                }
            } else {
                ++memo.misses[bucket];
                record = true;
            }
            if (++memo.calls_since_check >= 64) {
                memo.calls_since_check = 0;
                const u64 now = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
                if (now - memo.last_log_ns >= 1'000'000'000ULL) {
                    if (memo.last_log_ns != 0) {
                        u64 hits = 0, calls = 0, skipped = 0;
                        std::string per_macro;
                        for (u32 i = 0; i <= MacroMemo::MaxMacros; ++i) {
                            const u64 macro_calls = memo.hits[i] + memo.misses[i] + memo.skipped[i];
                            hits += memo.hits[i];
                            skipped += memo.skipped[i];
                            calls += macro_calls;
                            if (macro_calls >= 1000) {
                                per_macro += fmt::format(" {:#x}:{}%/{}{}", i,
                                                         memo.hits[i] * 100 / macro_calls,
                                                         macro_calls,
                                                         memo.skipped[i] != 0 ? "(no)" : "");
                            }
                        }
                        LOG_INFO(HW_GPU,
                                 "Nx2Perf front_macro_memo: {} hits / {} calls ({}%), {} not "
                                 "memoizable; verify {} checked, {} differed; per macro "
                                 "(hit%/calls):{}",
                                 hits, calls, calls ? hits * 100 / calls : 0, skipped,
                                 memo.verify_checked, memo.verify_differed, per_macro);
                    }
                    memo.last_log_ns = now;
                    memo.hits.fill(0);
                    memo.misses.fill(0);
                    memo.skipped.fill(0);
                }
            }
        }
        if (!replayed) {
            // Writing `method` may change register `address` on the front (FrontWrite).
            const auto touches = [](u32 method, u32 address) {
                constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
                return method == address ||
                       (address == MAXWELL3D_REG_INDEX(const_buffer.offset) && method >= cb_data &&
                        method < cb_data + 16) ||
                       (address == MAXWELL3D_REG_INDEX(shadow_scratch) &&
                        method == MAXWELL3D_REG_INDEX(falcon[4])) ||
                       (address == MAXWELL3D_REG_INDEX(load_mme.start_address_ptr) &&
                        method == MAXWELL3D_REG_INDEX(load_mme.start_address));
            };
            MacroMemo::Recorder recorder;
            const auto result =
                record || check != nullptr
                    ? engine.native_jit->Execute(
                          start, fs.parameters, borrowed, std::span<u32>{native_data_ram},
                          [&](u32 address) {
                              const u32 value = front_read(address);
                              recorder.Read(address, value, Regs::NUM_REGS, touches);
                              return value;
                          },
                          [&](u32 address, u32 value) {
                              recorder.Emit(address, value, MacroRegistersStart);
                              return front_emit(address, value);
                          },
                          jit_options)
                    : engine.native_jit->Execute(start, fs.parameters, borrowed,
                                                 std::span<u32>{native_data_ram}, front_read,
                                                 front_emit, jit_options);
            if (!result.success) {
                engine.native_execution_failed = true;
                LOG_ERROR(HW_GPU, "Native MME (JIT, split front) failed: start={:#x}, consumed={}/{}, error={}",
                          start, result.consumed, parameter_count, result.error);
            }
            if (record && result.success && engine.native_code_generation == generation) {
                // Store looks at the parameters only when no MME DMA emit (which may move
                // borrowed ones into fs.parameters) was recorded.
                fs.macro_memo.Store(generation, start, memo_parameters, recorder);
            }
            if (check != nullptr) [[unlikely]] {
                auto& memo = fs.macro_memo;
                ++memo.verify_checked;
                if (!(result.success && recorder.Matches(*check)) &&
                    memo.verify_differed++ < 64) {
                    LOG_ERROR(HW_GPU,
                              "Nx2Perf front_macro_memo: macro {:#x} differs (start {:#x}, {} "
                              "emits, entry {}; {} differences in {} checks)",
                              macro_index, start, recorder.num_emits, check->num_emits,
                              memo.verify_differed, memo.verify_checked);
                }
            }
        }
        if (verify_forward) [[unlikely]] {
            // The forwarded path's answer against this drained run's.
            static u64 checked{};
            static u64 differed{};
            bool same = verify_count == dma_plan->num_writes;
            for (size_t i = 0; same && i < dma_plan->snapshot.first_forwarded; ++i) {
                same = verify_writes[i] ==
                       std::pair<u32, u32>{dma_plan->writes[i].method, dma_plan->writes[i].value};
            }
            size_t index = dma_plan->snapshot.first_forwarded;
            const bool ran = DmaForward::Recompute(
                dma_plan->snapshot, std::span<const u32>{verify_words.data(), verify_word_count},
                MacroRegistersStart, [&](u32 forwarded_method, u32 value) {
                    same = same && index < verify_count &&
                           verify_writes[index] == std::pair<u32, u32>{forwarded_method, value};
                    ++index;
                });
            same = same && ran;
            ++checked;
            if (!same && differed++ < 64) {
                LOG_ERROR(HW_GPU, "Nx2Perf dma_forward: macro {:#x} call differs from the drained "
                                  "run ({} writes, plan {}; {} differences in {} calls)",
                          macro_index, verify_count, dma_plan->num_writes, differed, checked);
            }
            if ((checked & 0xffff) == 0) {
                LOG_INFO(HW_GPU, "Nx2Perf dma_forward: {} calls checked, {} differed", checked,
                         differed);
            }
        }
    }
    if (trace) [[unlikely]] {
        LOG_INFO(HW_GPU, "Nx2 macro trace {:#x} call {}: {}", trace_macro, traced_calls,
                 trace_line);
        ++traced_calls;
    }
    fs.parameters.clear();
    if (fs.draw_pending) {
        const auto finish_effect = FrontDrawEffect(split, fs);
        fs.draw_pending = false;
        fs.draw_window_writer = false;
        fs.draw_window_epoch = 0;
        split.Defer([this, system_ptr = &system] { FinishFrontMacro(*system_ptr); },
                    finish_effect);
        if (FrontIsWriterDraw(fs.regs)) {
            split.NoteWriter();
        }
    }
    if (profile) [[unlikely]] {
        Profile::AddMacro((method - MacroRegistersStart) >> 1,
                          Profile::CounterValue(Profile::Counter::MethodWrites) - writes_before,
                          Profile::CounterValue(Profile::Counter::SameValueWrites) - same_before,
                          split.WritePosition() - ring_before, dma_reads);
    }
    if (FrontVerifyEnabled() && (++fs.calls & 4095) == 0) [[unlikely]] {
        FrontVerify(split, fs);
    }
#else
    fs.executing_macro = 0;
    fs.parameters.clear();
#endif
}

void Maxwell3D::FrontTrackInstanceLoop(FrontState& fs, u32 method, u32 value) {
    switch (method) {
    case MAXWELL3D_REG_INDEX(draw.begin):
        // Instance id, bits 27:26: First leaves an instance loop, Subsequent enters one.
        if (((value >> 26) & 3) == 0) {
            fs.instance_loop = false;
        } else if (((value >> 26) & 3) == 1) {
            fs.instance_loop = true;
        }
        break;
    case MAXWELL3D_REG_INDEX(draw_inline_index):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
        fs.instance_loop = false; // DrawMode::InlineIndex: draw.end draws
        break;
    default:
        break;
    }
}

bool Maxwell3D::FrontSkipsLoopWrite(FrontState& fs, u32 method, u32 value, bool shadow_kept) {
    FrontTrackInstanceLoop(fs, method, value);
    switch (method) {
    case MAXWELL3D_REG_INDEX(draw.end):
    case MAXWELL3D_REG_INDEX(vertex_buffer.first):
    case MAXWELL3D_REG_INDEX(vertex_buffer.count):
        break;
    default:
        return false;
    }
    if (!fs.skip_same || !shadow_kept || fs.regs[method] != value) {
        return false;
    }
    // Instance loop methods flush no deferred draw, and the draw manager ignores these: DrawEnd
    // draws nothing in an instance loop, and it only reads the vertex buffer first and count
    // (index_buffer writes also mark the index buffer dirty, so they are always sent).
    return method != MAXWELL3D_REG_INDEX(draw.end) || fs.instance_loop;
}

u32 Maxwell3D::FrontExecSameDrop(VideoCommon::GpuSplit& split, FrontState& fs, u32 method,
                                 u32 value, bool shadow_kept) {
    const bool relearned = std::exchange(fs.exec_relearned, false);
    const u32 stage = BindGroupStage(method);
    if (stage == ~0U && !IsStoreOnlyExecutable(method)) {
        return ~0U;
    }
    // Both sides hold the value and the shadow RAM keeps it; no deferred instanced draw can be
    // pending for ProcessDirtyRegisters to issue.
    bool drop = !relearned && fs.regs[method] == value && shadow_kept && fs.skip_same &&
                fs.control != Regs::ShadowRamControl::Replay && fs.executing_macro == 0 &&
                !fs.draw_pending && !fs.instance_loop && !FrontVerifyEnabled();
    if (drop && (dirty.tables[0][method] != 0 || dirty.tables[1][method] != 0)) {
        ++fs.exec_counts[2]; // It marks state dirty (index_buffer.first: Dirty::IndexBuffer)
        drop = false;
    }
    u32 kind = 0;
    if (stage != ~0U) {
        // The bind repeats the last one sent on the stage: same slot, address and size, and no
        // queued function or GPU map since that could translate the address differently.
        constexpr u32 high = MAXWELL3D_REG_INDEX(const_buffer.address_high);
        constexpr u32 low = MAXWELL3D_REG_INDEX(const_buffer.address_low);
        constexpr u32 size = MAXWELL3D_REG_INDEX(const_buffer.size);
        const bool known =
            fs.unknown_count == 0 ||
            (!fs.unknown_regs[high] && !fs.unknown_regs[low] && !fs.unknown_regs[size]);
        const u64 position = split.MapTarget();
        const u64 serial = memory_manager.GetMapSerial();
        drop = drop && known &&
               fs.exec_bind_memo.Same(stage, value, fs.regs[high], fs.regs[low], fs.regs[size],
                                      position, serial);
        if (!drop) {
            if (known) {
                fs.exec_bind_memo.Record(stage, value, fs.regs[high], fs.regs[low],
                                         fs.regs[size], position, serial);
            } else {
                fs.exec_bind_memo.Forget(stage);
            }
            return ~0U;
        }
        kind = 1;
    } else if (!drop) {
        return ~0U;
    }
    ++fs.exec_counts[kind];
    if ((++fs.exec_noted & 0x3ff) == 0) {
        const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        if (now - fs.exec_logged_ns >= 1'000'000'000) {
            LOG_INFO(HW_GPU,
                     "Nx2Perf exec_same_drop: {} store-only writes and {} constant buffer binds "
                     "{} in {} ms; {} same-value store-only writes kept (they mark state dirty)",
                     fs.exec_counts[0], fs.exec_counts[1],
                     fs.verify ? "would be left out (Verify: sent for the back to check)"
                               : "left out",
                     fs.exec_logged_ns != 0 ? (now - fs.exec_logged_ns) / 1'000'000 : 0,
                     fs.exec_counts[2]);
            fs.exec_counts = {};
            fs.exec_logged_ns = now;
        }
    }
    return kind;
}

void Maxwell3D::VerifyExecSameDrop(Core::System& system, u32 method, u32 argument, bool is_last,
                                   u32 kind) {
    // The executable Method entry consumes the sink first.
    ConsumeSink(system);
    const u32 reg = regs.reg_array[method];
    const u32 shadow = shadow_state.reg_array[method];
    const DirtyState::Flags flags = dirty.flags;
    const bool deferred = draw_manager.HasDeferredDraw();
    const u32 stage = BindGroupStage(method);
    const u32 slot = (argument >> 4) & 0x1f; // bind_groups shader_slot
    const bool bind = kind == 1 && stage != ~0U && slot < Regs::MaxConstBuffers;
    ConstBufferInfo buffer{};
    std::array<u64, 6> binding{};
    bool have_binding = false;
    if (bind) {
        buffer = state.shader_stages[stage].const_buffers[slot];
        have_binding = rasterizer->Nx2UniformBinding(stage, slot, binding);
    }
    CallMethod(system, method, argument, is_last);

    static std::atomic<u64> checks{};
    static std::atomic<u64> binds{};
    static std::atomic<u64> differences{};
    static std::atomic<u64> deleted_rebinds{};
    static std::atomic<u64> serial_refreshes{};
    static std::atomic<u32> lines{};
    static std::atomic<s64> logged_ns{};
    checks.fetch_add(1, std::memory_order_relaxed);
    std::string moved;
    if (deferred) {
        moved += " deferred-draw";
    }
    if (regs.reg_array[method] != reg) {
        moved += fmt::format(" register {:#x}->{:#x}", reg, regs.reg_array[method]);
    }
    if (shadow_state.reg_array[method] != shadow) {
        moved += fmt::format(" shadow {:#x}->{:#x}", shadow, shadow_state.reg_array[method]);
    }
    if (dirty.flags != flags) {
        const DirtyState::Flags changed = dirty.flags ^ flags;
        moved += " dirty";
        for (size_t i = 0; i < changed.size(); ++i) {
            if (changed[i]) {
                moved += fmt::format(" {}", i);
            }
        }
    }
    if (bind) {
        binds.fetch_add(1, std::memory_order_relaxed);
        const ConstBufferInfo& now = state.shader_stages[stage].const_buffers[slot];
        if (now.enabled != buffer.enabled || now.address != buffer.address ||
            now.size != buffer.size) {
            moved += fmt::format(" cb {}/{:#x}/{:#x}->{}/{:#x}/{:#x}", buffer.enabled,
                                 buffer.address, buffer.size, now.enabled, now.address, now.size);
        }
        std::array<u64, 6> after{};
        if (have_binding && rasterizer->Nx2UniformBinding(stage, slot, after) &&
            after != binding) {
            // A GPU map after the front's decision only refreshes the serial when the address
            // translates the same (only the bind's early-out reads it). After a buffer deletion
            // the rebind leaves the buffer to the draw's FindBuffer, which returns the one
            // still bound (DeleteBuffer cleared the ones it deleted).
            const bool same_target = after[0] == binding[0] && after[1] == binding[1] &&
                                     after[3] == binding[3];
            if (same_target && after[4] == binding[4] && after[5] == binding[5]) {
                serial_refreshes.fetch_add(1, std::memory_order_relaxed);
            } else if (same_target && (binding[5] & 2) != 0) {
                deleted_rebinds.fetch_add(1, std::memory_order_relaxed);
            } else {
                moved += fmt::format(" binding {:#x}/{:#x}/{}/{:#x}/{}/{}->{:#x}/{:#x}/{}/{:#x}/{}/{}",
                                     binding[0], binding[1], binding[2], binding[3], binding[4],
                                     binding[5], after[0], after[1], after[2], after[3],
                                     after[4], after[5]);
            }
        }
    }
    if (!moved.empty()) {
        differences.fetch_add(1, std::memory_order_relaxed);
        if (lines.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_ERROR(HW_GPU, "Nx2Perf exec_same_drop: method {:#x} = {:#x} would differ:{}",
                      method, argument, moved);
        }
    }
    {
        const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        if (now - logged_ns.load(std::memory_order_relaxed) >= 1'000'000'000) {
            logged_ns.store(now, std::memory_order_relaxed);
            const u64 total = checks.load(std::memory_order_relaxed);
            const u64 bind_total = binds.load(std::memory_order_relaxed);
            const u64 differed = differences.load(std::memory_order_relaxed);
            const std::string text = fmt::format(
                "Nx2Perf exec_same_drop: {} checks ({} store-only, {} binds), {} differed, {} "
                "binds after a buffer deletion (buffer re-resolved at the draw), {} map serial "
                "refreshes (same translation)",
                total, total - bind_total, bind_total, differed,
                deleted_rebinds.load(std::memory_order_relaxed),
                serial_refreshes.load(std::memory_order_relaxed));
            if (differed != 0) {
                LOG_ERROR(HW_GPU, "{}", text);
            } else {
                LOG_INFO(HW_GPU, "{}", text);
            }
        }
    }
}

void Maxwell3D::FrontDmaRead(VideoCommon::GpuSplit& split, FrontState& fs, u32 macro, u32 method,
                             u32 count) {
    const VideoCommon::Nx2Snap::FrontMacroScope snap_scope; // run_ahead_snapshot_probe
    // The words may be written by methods still queued for the back (indirect draw arguments
    // from a culling dispatch), and the caches that may hold newer ones belong to it.
    const GPUVAddr source = (GPUVAddr{fs.regs[0x154] & 0x1ffffff} << 32) | fs.regs[0x155];
    const u32 ram = fs.regs[0x156];
    const u32 words = std::min<u32>(count, 0x1000);
    const size_t read_bytes = size_t{words} * sizeof(u32);
    std::vector<u32> diag_before;
    if (Tegra::Nx2CmdPageDiag::g_enabled) {
        diag_before.resize(words);
        memory_manager.ReadBlockUnsafe(source, diag_before.data(), read_bytes);
    }
    if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::MacroReadEarly) && !Tegra::Nx2CmdPageDiag::g_enabled &&
        FrontDmaReadEarly(split, fs, macro, method, source, ram, words)) {
        return;
    }
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    if (VideoCommon::Nx2Profile::Enabled()) {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::DmaReadBytes, read_bytes);
    }
    // Occlusion reports and semaphore writes held in the query cache land before the read, or
    // macros read stale counts and the guest culls visible objects. The query cache belongs to
    // the back, so this runs there, in one call with the buffer flush.
    const bool flush_buffers =
        memory_manager.IsMemoryDirty(source, read_bytes, VideoCommon::CacheType::BufferCache);
    split.Run(
        [memory = &memory_manager, source, read_bytes, flush_buffers] {
            if (flush_buffers) {
                memory->FlushRegion(source, read_bytes, VideoCommon::CacheType::BufferCache);
            }
            memory->FlushQueryResults(source, read_bytes);
        },
        VideoCommon::DrainSite::MacroDmaFlush);
    if (diag_before.empty()) {
        if (method == 0x158) {
            const size_t old_size = fs.parameters.size();
            fs.parameters.resize(old_size + words);
            memory_manager.ReadWordsUnsafe(source, fs.parameters.data() + old_size, words);
        } else if (ram < native_data_ram.size()) {
            const size_t copy_words = (std::min<size_t>)(words, native_data_ram.size() - ram);
            memory_manager.ReadWordsUnsafe(source, native_data_ram.data() + ram, copy_words);
        }
        return;
    }
    u32 changed{};
    for (u32 i = 0; i < words; ++i) {
        const u32 word = memory_manager.Read<u32>(source + u64{i} * sizeof(u32));
        if (method == 0x158) {
            fs.parameters.push_back(word);
        } else if (ram + i < native_data_ram.size()) {
            native_data_ram[ram + i] = word;
        }
        if (diag_before[i] != word) {
            ++changed;
        }
    }
    if (Tegra::Nx2CmdPageDiag::g_enabled) {
        bool gpu_written = false;
        for (GPUVAddr page = source & ~GPUVAddr{0xfff}; page < source + read_bytes; page += 0x1000) {
            const auto device = memory_manager.GpuToCpuAddress(page);
            gpu_written |= device && Tegra::Nx2IsGpuWritten(*device);
        }
        Tegra::Nx2CmdPageDiag::RecordDmaRead(source, words, changed, gpu_written);
    }
}

bool Maxwell3D::FrontDmaReadEarly(VideoCommon::GpuSplit& split, FrontState& fs, u32 macro,
                                  u32 method, GPUVAddr source, u32 ram, u32 words) {
    namespace Profile = VideoCommon::Nx2Profile;
    const u64 bytes = u64{words} * sizeof(u32);
    const bool sparse = source < Tegra::g_nx2_sparse_high.load(std::memory_order_relaxed) &&
                        Tegra::g_nx2_sparse_low.load(std::memory_order_relaxed) < source + bytes;
    if (words == 0 || sparse) {
        return false;
    }
    const u64 consumed = split.SettleWrites();
    // MacroReadWriterSet: wait only for the entries that can write memory, not for the draws
    // the back proved write none (checked again below, once the wait is over).
    const bool writer_set =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::MacroReadWriterSet);
    std::array<u64, 5> pages;
    std::array<std::pair<u64, u64>, 5> read_ranges; // device bytes read in each page
    std::array<u64, 5> read_offsets;                 // their byte offsets in the read
    size_t page_count = 0;
    using WriteKind = VideoCommon::SplitWriteTracker::WriteKind;
    for (GPUVAddr page = source & ~GPUVAddr{0xfff}; page < source + bytes; page += 0x1000) {
        const std::optional<DAddr> device = memory_manager.GpuToCpuAddress(page);
        if (page_count == pages.size() || !device || Tegra::Nx2IsGpuWrittenRegion(*device) ||
            Tegra::Nx2IsGpuWritten(*device)) {
            return false;
        }
        const u64 device_page = *device & ~u64{0xfff};
        const GPUVAddr read_begin = (std::max)(page, source);
        const GPUVAddr read_end = (std::min)(page + 0x1000, source + bytes);
        read_ranges[page_count] = {device_page + (read_begin - page), read_end - read_begin};
        read_offsets[page_count] = read_begin - source;
        pages[page_count++] = device_page;
    }
    // ExactNotedWrites: the noted writes to the bytes read, not to their whole pages (the
    // per-page checks stay as they are: the cache state is kept per page).
    const bool exact = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ExactNotedWrites);
    struct Targets {
        u64 target;
        u64 write;
        u64 other; // noted writes other than constant buffer data
    };
    const auto targets_over = [&](bool exact_ranges) {
        Targets result{};
        for (size_t i = 0; i < page_count; ++i) {
            const u64 begin = exact_ranges ? read_ranges[i].first : pages[i];
            const u64 end =
                exact_ranges ? read_ranges[i].first + read_ranges[i].second : pages[i] + 0x1000;
            result.target = (std::max)(result.target, writer_set
                                                          ? split.Writes().WriterReadTarget(begin, end)
                                                          : split.ReadTarget(begin, end));
            result.write = (std::max)(result.write, split.Writes().LastWriteTo(begin, end));
            result.other =
                (std::max)(result.other, split.Writes().LastWriteTo(begin, end, WriteKind::Other));
        }
        return result;
    };
    const Targets targets = targets_over(exact);
    u64 target = targets.target;
    const u64 write_target = targets.write;
    const u64 other_write_target = targets.other;
    // Verify: the page targets are the old answer. Profiler: other writes the read waited for,
    // split into ones that overlap the bytes read and ones that only share their pages.
    const bool verify_exact = exact && VideoCommon::Nx2Perf::Verify();
    Targets page_targets = targets;
    Targets exact_targets = targets;
    if (verify_exact || Profile::Enabled()) {
        (exact ? page_targets : exact_targets) = targets_over(!exact);
    }
    // MacroReadCbOverlay: when the only queued writes noted to these pages are constant buffer
    // data, wait only for the unknown and GPU writers, then lay the queued words over what memory
    // holds. Invariant: after those, the entries still queued write the read bytes only through
    // noted writes (Effect::None), so no writer but the queued constant buffer words (kept in
    // order, by device address like the tracker) touches them before the back runs those words.
    // Async writes are checked below as before.
    // MacroReadOverlayWriters: wait for the unknown writes and the writers only. The queued
    // draws past them are ones the back proved write no memory, checked again after the wait as
    // for MacroReadWriterSet.
    const bool cb_overlay =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::MacroReadCbOverlay);
    const bool overlay_writers =
        cb_overlay && writer_set &&
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::MacroReadOverlayWriters);
    const u64 unlocalized_target = (std::max)(
        split.Writes().LastUnknown(),
        overlay_writers ? split.Writes().LastWriter() : split.Writes().LastGpu());
    const bool overlay = cb_overlay && write_target > unlocalized_target &&
                         other_write_target == 0 && split.ConstBufferWordsKept(consumed);
    if (overlay) {
        target = unlocalized_target;
    }
    u64 page_target = target;
    if (verify_exact) {
        page_target = cb_overlay && page_targets.write > unlocalized_target &&
                              page_targets.other == 0 && split.ConstBufferWordsKept(consumed)
                          ? unlocalized_target
                          : page_targets.target;
    }
    const bool exact_below = verify_exact && target < page_target;
    const auto wait_reason = target == split.Writes().LastUnknown()
                                 ? Profile::Counter::DmaReadsWaitUnknown
                             : target == write_target ? Profile::Counter::DmaReadsWaitWrite
                                                      : Profile::Counter::DmaReadsWaitGpu;
    split.WaitUntil(target, VideoCommon::DrainSite::MacroDmaWrites);
    const u64 consumed_now = split.ConsumedNow();
    // Draws still queued that the read did not wait for: their proofs must still hold (no
    // program dropped or deferred-written since they were classified), and no queued write
    // before them may reach proven code (the shader cache applies those at the next draw).
    const bool skipped_draws = writer_set && split.Writes().LastGpu() > consumed_now;
    if (skipped_draws) {
        const u64 epoch = Tegra::g_nx2_code_epoch.load(std::memory_order_acquire);
        // ExactNotedWrites: a noted write still counts for every page it touches.
        if (!split.Writes().DrawsValid(consumed_now, epoch) ||
            split.Writes().AnyPendingWrite(consumed_now, split.Writes().LastGpu(),
                                           [exact](u64 begin, u64 end) {
                                               if (exact) {
                                                   begin &= ~u64{0xfff};
                                                   end = (end + 0xfff) & ~u64{0xfff};
                                               }
                                               return Tegra::Nx2IsProvenCode(begin, end - begin);
                                           })) {
            if (Profile::Enabled()) {
                Profile::Add(overlay ? Profile::Counter::DmaReadsOverlayFallback
                                     : Profile::Counter::DmaReadsWriterSetFallback,
                             1);
            }
            return false;
        }
    }
    for (size_t i = 0; i < page_count; ++i) {
        if (Tegra::Nx2IsGpuWrittenRegion(pages[i]) || Tegra::Nx2IsGpuWritten(pages[i])) {
            return false;
        }
        // A counter report or delayed release to these bytes: read early only once the back ran
        // it and its result landed. The words in memory are then what a drained read sees.
        const auto [range_begin, range_size] = read_ranges[i];
        if (const u64 async_position = split.HasAsyncWrite(range_begin, range_size);
            async_position != 0 && (async_position > consumed_now ||
                                    !Tegra::Nx2AsyncReportsSettled(range_begin, range_size))) {
            return false;
        }
    }
    if (Profile::Enabled()) {
        Profile::Add(Profile::Counter::DmaReadBytes, bytes);
        Profile::Add(Profile::Counter::DmaReadsEarly, 1);
        if (target > consumed) {
            Profile::Add(wait_reason, 1);
            if (wait_reason == Profile::Counter::DmaReadsWaitWrite && cb_overlay) {
                Profile::Add(other_write_target != 0 ? Profile::Counter::DmaReadsWaitWriteOther
                             : !split.ConstBufferWordsKept(consumed)
                                 ? Profile::Counter::DmaReadsWaitWriteCbLost
                                 : Profile::Counter::DmaReadsWaitWriteCbOlder,
                             1);
                if (other_write_target != 0) {
                    Profile::Add(exact_targets.other != 0
                                     ? Profile::Counter::DmaReadsWaitWriteOtherExact
                                     : Profile::Counter::DmaReadsWaitWriteOtherPageOnly,
                                 1);
                }
            }
        }
        if (skipped_draws) {
            Profile::Add(Profile::Counter::DmaReadsWriterSet, 1);
        }
        if (overlay) {
            Profile::Add(target > consumed ? Profile::Counter::DmaReadsOverlayWaited
                                           : Profile::Counter::DmaReadsOverlayNoWait,
                         1);
        }
    }
    // Words the back ran up to consumed_now are in memory; the newer ones come from the overlay.
    const auto apply_overlay = [&](u32* out) {
        for (size_t i = 0; i < page_count; ++i) {
            split.OverlayConstBufferWords(consumed_now, read_ranges[i].first, read_ranges[i].second,
                                          reinterpret_cast<u8*>(out) + read_offsets[i]);
        }
    };
    if (!VideoCommon::Nx2Perf::Verify()) [[likely]] {
        if (overlay) {
            auto& words_read = fs.dma_words;
            words_read.resize(words);
            memory_manager.ReadWordsUnsafe(source, words_read.data(), words);
            apply_overlay(words_read.data());
            if (method == 0x158) {
                fs.parameters.insert(fs.parameters.end(), words_read.begin(), words_read.end());
            } else if (ram < native_data_ram.size()) {
                const size_t copy_words = (std::min<size_t>)(words, native_data_ram.size() - ram);
                std::memcpy(native_data_ram.data() + ram, words_read.data(),
                            copy_words * sizeof(u32));
            }
            return true;
        }
        if (method == 0x158) {
            const size_t old_size = fs.parameters.size();
            fs.parameters.resize(old_size + words);
            memory_manager.ReadWordsUnsafe(source, fs.parameters.data() + old_size, words);
        } else if (ram < native_data_ram.size()) {
            const size_t copy_words = (std::min<size_t>)(words, native_data_ram.size() - ram);
            memory_manager.ReadWordsUnsafe(source, native_data_ram.data() + ram, copy_words);
        }
        return true;
    }
    auto& read = fs.dma_words;
    read.resize(words);
    memory_manager.ReadWordsUnsafe(source, read.data(), words);
    if (overlay) {
        apply_overlay(read.data());
    }
    // The old path: drain, flush, read again. Its words are the ones used.
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    const bool verify_flush_buffers =
        memory_manager.IsMemoryDirty(source, bytes, VideoCommon::CacheType::BufferCache);
    split.Run(
        [memory = &memory_manager, source, bytes, verify_flush_buffers] {
            if (verify_flush_buffers) {
                memory->FlushRegion(source, bytes, VideoCommon::CacheType::BufferCache);
            }
            memory->FlushQueryResults(source, bytes);
        },
        VideoCommon::DrainSite::MacroDmaFlush);
    static u32 logged{};
    static u64 writer_set_reads{};
    static u64 writer_set_mismatches{};
    // MacroReadOverlayWriters: every overlay read it allows, compared like the others.
    static u32 overlay_writers_logged{};
    static u64 overlay_writers_reads{};
    static u64 overlay_writers_mismatches{};
    const bool overlay_writers_read = overlay && overlay_writers;
    bool mismatch = false;
    for (u32 i = 0; i < words; ++i) {
        const u32 drained = memory_manager.Read<u32>(source + u64{i} * sizeof(u32));
        if (drained != read[i]) {
            mismatch = true;
            if (overlay_writers_read) {
                if (overlay_writers_logged < 64) {
                    ++overlay_writers_logged;
                    LOG_ERROR(HW_GPU,
                              "Nx2Perf macro_read_overlay_writers: macro {:#x} DMA read of {} "
                              "words at {:#x}: word {} is {:#x} without the drain, {:#x} after it "
                              "({})",
                              macro, words, source, i, read[i], drained,
                              skipped_draws ? "queued proven draws skipped" : "no draws skipped");
                }
            } else if (logged < 64) {
                ++logged;
                LOG_ERROR(HW_GPU,
                          "Nx2Perf {}: macro {:#x} DMA read of {} words at {:#x}: word {} is "
                          "{:#x} without the drain, {:#x} after it",
                          skipped_draws ? "macro_read_writer_set" : "split", macro, words, source,
                          i, read[i], drained);
            }
        }
        read[i] = drained;
    }
    if (exact_below && VideoCommon::GpuSplit::VerifyExactRead(mismatch) <= 64 && mismatch) {
        LOG_ERROR(HW_GPU,
                  "Nx2Perf exact_noted_writes: macro {:#x} DMA read of {} words at {:#x} waited "
                  "for {} (page target {}{}) and differed from the drained read",
                  macro, words, source, target, page_target,
                  overlay ? ", constant buffer overlay" : "");
    }
    if (overlay_writers_read) {
        overlay_writers_mismatches += mismatch ? 1 : 0;
        if ((++overlay_writers_reads & 4095) == 0 ||
            (mismatch && overlay_writers_mismatches <= 64)) {
            LOG_INFO(HW_GPU,
                     "Nx2Perf overlay_writers totals: {} overlay reads, {} differed from the "
                     "drained read",
                     overlay_writers_reads, overlay_writers_mismatches);
        }
    }
    if (skipped_draws) {
        writer_set_mismatches += mismatch ? 1 : 0;
        if ((++writer_set_reads & 4095) == 0 || (mismatch && writer_set_mismatches <= 64)) {
            LOG_INFO(HW_GPU,
                     "Nx2Perf macro_read_writer_set: {} early reads past queued proven draws, {} "
                     "differed from the drained read",
                     writer_set_reads, writer_set_mismatches);
        }
    }
    for (u32 i = 0; i < words; ++i) {
        if (method == 0x158) {
            fs.parameters.push_back(read[i]);
        } else if (ram + i < native_data_ram.size()) {
            native_data_ram[ram + i] = read[i];
        }
    }
    return true;
}

void Maxwell3D::FrontVerify(VideoCommon::GpuSplit& split, FrontState& fs) {
    split.Drain(VideoCommon::DrainSite::FrontVerify);
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
        if (fs.unknown_count != 0 && fs.unknown_regs[i]) {
            // Forwarded (dma_forward): the back's values are the right ones.
            fs.regs[i] = expected[i];
            fs.shadow[i] = shadow_state.reg_array[i];
            fs.ForgetUnknown(i);
            continue;
        }
        if (fs.regs[i] != expected[i]) {
            if (differences++ < 64) {
                LOG_WARNING(HW_GPU, "Split front register {:#x}: front {:#x}, back {:#x}", i,
                            fs.regs[i], expected[i]);
            }
            fs.regs[i] = expected[i];
            // The front's draw states were wrong: trust none of them until the window ends.
            fs.draw_key_valid = false;
            fs.draw_window_writer = true;
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

void Maxwell3D::FinishFrontMacro(Core::System& system) {
    // What ProcessMacro and CallMacroMethod do around the macro.
    ConsumeSink(system);
    draw_manager.DrawDeferred(*this);
}

} // namespace Tegra::Engines

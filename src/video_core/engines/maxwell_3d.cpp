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
#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/textures/texture.h"
#include "video_core/macro_dma_forward.h"
#ifdef ARCHITECTURE_x86_64
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
    void MarkUnknown(u32 method) {
        if (!unknown_regs[method]) {
            unknown_regs[method] = true;
            ++unknown_count;
        }
    }
    void ForgetUnknown(u32 method) {
        if (unknown_regs[method]) {
            unknown_regs[method] = false;
            --unknown_count;
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

// The effect of an executable method's entry: what it had before (effect, a deferred draw), the
// method's class, and what the method may write (draw_effect for a draws-only method).
template <typename DrawEffect>
VideoCommon::GpuSplit::Effect FrontExecEffect(VideoCommon::GpuSplit::Effect effect, u32 method,
                                              u8 cls, bool class_table, DrawEffect&& draw_effect) {
    using Effect = VideoCommon::GpuSplit::Effect;
    using VideoCommon::SplitWriteTracker;
    effect = SplitWriteTracker::Combine(effect,
                                        class_table ? ClassEffect(cls) : FrontMethodEffect(method));
    Effect method_effect = FrontMethodEffect(method);
    if (method_effect == Effect::Gpu && FrontDrawsOnly(method)) {
        method_effect = draw_effect();
    }
    return SplitWriteTracker::Combine(effect, method_effect);
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
        front->draw_begin_coalesce =
            VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::DrawBeginCoalesce);
        front->verify = VideoCommon::Nx2Perf::Verify();
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
    std::optional<GPUVAddr> cb_write;
    u32 cb_room{};
    if (Profile::Enabled() && fs.regs[method] == value) [[unlikely]] {
        Profile::AddSameExecutable(method);
    }
    fs.regs[method] = value;
    const u8 cls = fs.class_table ? FrontClass(method, fs.verify) : u8{0};
    if (IsDrawProgramRegister(method)) {
        fs.draw_key_valid = false;
    }
    effect = FrontExecEffect(effect, method, cls, fs.class_table,
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
            effect = FrontExecEffect(effect, method, cls, fs.class_table,
                                     [&] { return FrontDrawEffect(split, fs); });
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
    // Analyzed on demand: a shadow RAM replay call uses a static plan.
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
        const auto analyze = [&](auto&& read) {
            return DmaForward::Analyze(std::span<const u32>{engine.native_code}, start, parameters,
                                       std::span<const u32>{native_data_ram}, MacroRegistersStart,
                                       read, forwardable);
        };
        if (fs.control == Regs::ShadowRamControl::Replay) {
            // The back would write the shadow copy, not the words.
            static const DmaForward::Plan replay_plan = [] {
                DmaForward::Plan plan;
                plan.reason = "shadow RAM replay";
                return plan;
            }();
            dma_plan = &replay_plan;
        } else {
            dma_plan = &analyzed_plan.emplace(analyze(read_register));
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
        } else if (VideoCommon::Nx2Perf::Verify()) {
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
                void operator()() const {
                    std::array<u32, DmaForward::MaxDmaWords> words{};
                    ForwardRead::Run(engine, snapshot.dma_source, snapshot.dma_words,
                                     words.data());
                    const bool ok = DmaForward::Recompute(
                        snapshot, std::span<const u32>{words.data(), snapshot.dma_words},
                        MacroRegistersStart, [this](u32 forwarded_method, u32 value) {
                            engine->ConsumeSink(*system);
                            engine->CallMethod(*system, forwarded_method, value, true);
                        });
                    static u32 failures{};
                    if (!ok && failures++ < 16) {
                        LOG_ERROR(HW_GPU, "Nx2Perf dma_forward: the back's run of a forwarded "
                                          "macro call failed or made other writes");
                    }
                }
            };
            for (size_t i = 0; i < dma_plan->num_writes; ++i) {
                const DmaForward::Write& write = dma_plan->writes[i];
                if (write.forwarded) {
                    if (i == dma_plan->snapshot.first_forwarded) {
                        split.Defer(ForwardCall{this, &system, dma_plan->snapshot});
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
    std::array<std::array<u32, 2>, DmaForward::MaxWrites> verify_writes{};
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
        const auto result = engine.native_jit->Execute(start, fs.parameters, borrowed,
                                                       std::span<u32>{native_data_ram},
                                                       front_read, front_emit, jit_options);
        if (!result.success) {
            engine.native_execution_failed = true;
            LOG_ERROR(HW_GPU, "Native MME (JIT, split front) failed: start={:#x}, consumed={}/{}, error={}",
                      start, result.consumed, parameter_count, result.error);
        }
        if (verify_forward) [[unlikely]] {
            // The forwarded path's answer against this drained run's.
            static u64 checked{};
            static u64 differed{};
            bool same = verify_count == dma_plan->num_writes;
            for (size_t i = 0; same && i < dma_plan->snapshot.first_forwarded; ++i) {
                same = verify_writes[i] ==
                       std::array<u32, 2>{dma_plan->writes[i].method, dma_plan->writes[i].value};
            }
            size_t index = dma_plan->snapshot.first_forwarded;
            const bool ran = DmaForward::Recompute(
                dma_plan->snapshot, std::span<const u32>{verify_words.data(), verify_word_count},
                MacroRegistersStart, [&](u32 forwarded_method, u32 value) {
                    same = same && index < verify_count &&
                           verify_writes[index] == std::array<u32, 2>{forwarded_method, value};
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
    (void)system;
    (void)split;
    (void)borrowed;
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

void Maxwell3D::FrontDmaRead(VideoCommon::GpuSplit& split, FrontState& fs, u32 macro, u32 method,
                             u32 count) {
    // The words may be written by methods still queued for the back (indirect draw arguments
    // from a culling dispatch), and the caches that may hold newer ones belong to it.
    const GPUVAddr source = (GPUVAddr{fs.regs[0x154] & 0x1ffffff} << 32) | fs.regs[0x155];
    const u32 ram = fs.regs[0x156];
    const u32 words = std::min<u32>(count, 0x1000);
    const size_t read_bytes = size_t{words} * sizeof(u32);
    if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::MacroReadEarly) &&
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
    if (method == 0x158) {
        const size_t old_size = fs.parameters.size();
        fs.parameters.resize(old_size + words);
        memory_manager.ReadWordsUnsafe(source, fs.parameters.data() + old_size, words);
    } else if (ram < native_data_ram.size()) {
        const size_t copy_words = (std::min<size_t>)(words, native_data_ram.size() - ram);
        memory_manager.ReadWordsUnsafe(source, native_data_ram.data() + ram, copy_words);
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
    u64 target = 0;
    u64 write_target = 0;
    u64 other_write_target = 0; // noted writes other than constant buffer data
    for (size_t i = 0; i < page_count; ++i) {
        const u64 begin = pages[i];
        const u64 end = pages[i] + 0x1000;
        target = (std::max)(target, writer_set ? split.Writes().WriterReadTarget(begin, end)
                                               : split.ReadTarget(begin, end));
        write_target = (std::max)(write_target, split.Writes().LastWriteTo(begin, end));
        other_write_target = (std::max)(other_write_target,
                                        split.Writes().LastWriteTo(begin, end, WriteKind::Other));
    }
    // MacroReadCbOverlay: when the only queued writes noted to these pages are constant buffer
    // data, wait only for the unknown and GPU writers, then lay the queued words over what memory
    // holds. Invariant: after those, the entries still queued write the read bytes only through
    // noted writes (Effect::None), so no writer but the queued constant buffer words (kept in
    // order, by device address like the tracker) touches them before the back runs those words.
    // Async writes are checked below as before.
    const bool cb_overlay =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::MacroReadCbOverlay);
    const u64 unlocalized_target =
        (std::max)(split.Writes().LastUnknown(), split.Writes().LastGpu());
    const bool overlay = cb_overlay && write_target > unlocalized_target &&
                         other_write_target == 0 && split.ConstBufferWordsKept(consumed);
    if (overlay) {
        target = unlocalized_target;
    }
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
        if (!split.Writes().DrawsValid(consumed_now, epoch) ||
            split.Writes().AnyPendingWrite(consumed_now, split.Writes().LastGpu(),
                                           [](u64 begin, u64 end) {
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
    bool mismatch = false;
    for (u32 i = 0; i < words; ++i) {
        const u32 drained = memory_manager.Read<u32>(source + u64{i} * sizeof(u32));
        if (drained != read[i]) {
            mismatch = true;
            if (logged < 64) {
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

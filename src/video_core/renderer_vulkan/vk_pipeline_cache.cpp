// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_set>
#include <vector>
#include <bit>
#include <numeric>
#include "common/cityhash.h"
#include "common/scope_exit.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/thread_worker.h"
#include "core/core.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/program_header.h"
#include "common/env_gate.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_perf.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/pipeline_statistics.h"
#include "video_core/renderer_vulkan/vertex_repack.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_cooperative_hmma_test.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/shader_cache.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_notify.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/gpu_logging/gpu_logging.h"

#ifdef __ANDROID__
#include "../../android/app/src/main/jni/android_settings.h"
#endif

namespace Vulkan {

namespace {
using Shader::Backend::SPIRV::EmitSPIRV;
using Shader::Maxwell::ConvertLegacyToGeneric;
using Shader::Maxwell::GenerateGeometryPassthrough;
using Shader::Maxwell::MergeDualVertexPrograms;
using Shader::Maxwell::TranslateProgram;
using VideoCommon::ComputeEnvironment;
using VideoCommon::FileEnvironment;
using VideoCommon::GenericEnvironment;
using VideoCommon::GraphicsEnvironment;

// Bump CACHE_VERSION for translation changes, as before: a cache from an older version is then
// translated again at boot and written back under the new one. Bump MIN_COMPATIBLE_CACHE_VERSION
// too only when the record layout (environment or key serialization) changes, which deletes older
// caches. 20 changed the environment layout; the keys last changed before it.
constexpr u32 CACHE_VERSION = 32;
constexpr u32 MIN_COMPATIBLE_CACHE_VERSION = 20;
constexpr size_t VULKAN_CACHE_FLUSH_PIPELINES = 128;
constexpr size_t VULKAN_CACHE_FLUSH_MIN_SECONDS = 30;
constexpr std::array<char, 8> VULKAN_CACHE_MAGIC_NUMBER{'y', 'u', 'z', 'u', 'v', 'k', 'c', 'h'};

template <typename Container>
auto MakeSpan(Container& container) {
    return std::span(container.data(), container.size());
}

Shader::OutputTopology MaxwellToOutputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::OutputTopology::PointList;
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::OutputTopology::LineStrip;
    default:
        return Shader::OutputTopology::TriangleStrip;
    }
}

Shader::CompareFunction MaxwellToCompareFunction(Maxwell::ComparisonOp comparison) {
    switch (comparison) {
    case Maxwell::ComparisonOp::Never_D3D:
    case Maxwell::ComparisonOp::Never_GL:
        return Shader::CompareFunction::Never;
    case Maxwell::ComparisonOp::Less_D3D:
    case Maxwell::ComparisonOp::Less_GL:
        return Shader::CompareFunction::Less;
    case Maxwell::ComparisonOp::Equal_D3D:
    case Maxwell::ComparisonOp::Equal_GL:
        return Shader::CompareFunction::Equal;
    case Maxwell::ComparisonOp::LessEqual_D3D:
    case Maxwell::ComparisonOp::LessEqual_GL:
        return Shader::CompareFunction::LessThanEqual;
    case Maxwell::ComparisonOp::Greater_D3D:
    case Maxwell::ComparisonOp::Greater_GL:
        return Shader::CompareFunction::Greater;
    case Maxwell::ComparisonOp::NotEqual_D3D:
    case Maxwell::ComparisonOp::NotEqual_GL:
        return Shader::CompareFunction::NotEqual;
    case Maxwell::ComparisonOp::GreaterEqual_D3D:
    case Maxwell::ComparisonOp::GreaterEqual_GL:
        return Shader::CompareFunction::GreaterThanEqual;
    case Maxwell::ComparisonOp::Always_D3D:
    case Maxwell::ComparisonOp::Always_GL:
        return Shader::CompareFunction::Always;
    }
    UNIMPLEMENTED_MSG("Unimplemented comparison op={}", comparison);
    return {};
}

Shader::AttributeType CastAttributeType(const FixedPipelineState::VertexAttribute& attr) {
    if (attr.enabled == 0) {
        return Shader::AttributeType::Disabled;
    }
    switch (attr.Type()) {
    case Maxwell::VertexAttribute::Type::UnusedEnumDoNotUseBecauseItWillGoAway:
        ASSERT_MSG(false, "Invalid vertex attribute type!");
        return Shader::AttributeType::Disabled;
    case Maxwell::VertexAttribute::Type::SNorm:
    case Maxwell::VertexAttribute::Type::UNorm:
    case Maxwell::VertexAttribute::Type::Float:
        return Shader::AttributeType::Float;
    case Maxwell::VertexAttribute::Type::SInt:
        return Shader::AttributeType::SignedInt;
    case Maxwell::VertexAttribute::Type::UInt:
        return Shader::AttributeType::UnsignedInt;
    case Maxwell::VertexAttribute::Type::UScaled:
        return Shader::AttributeType::UnsignedScaled;
    case Maxwell::VertexAttribute::Type::SScaled:
        return Shader::AttributeType::SignedScaled;
    }
    return Shader::AttributeType::Float;
}

Shader::AttributeType AttributeType(const FixedPipelineState& state, size_t index) {
    switch (state.DynamicAttributeType(index)) {
    case 0:
        return Shader::AttributeType::Disabled;
    case 1:
        return Shader::AttributeType::Float;
    case 2:
        return Shader::AttributeType::SignedInt;
    case 3:
        return Shader::AttributeType::UnsignedInt;
    }
    return Shader::AttributeType::Disabled;
}

Shader::RuntimeInfo MakeRuntimeInfo(std::span<const Shader::IR::Program> programs,
                                    const GraphicsPipelineCacheKey& key,
                                    const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous_program,
                                    const Vulkan::Device& device) {
    Shader::RuntimeInfo info;
    if (previous_program) {
        info.previous_stage_stores = previous_program->info.stores;
        info.previous_stage_legacy_stores_mapping = previous_program->info.legacy_stores_mapping;
        if (previous_program->is_geometry_passthrough) {
            info.previous_stage_stores.mask |= previous_program->info.passthrough.mask;
        }
    } else {
        info.previous_stage_stores.mask.set();
    }
    if (previous_program && previous_program->stage == Shader::Stage::Mesh) {
        for (const u32 line : previous_program->mesh.primitive_lines) {
            const auto attribute{static_cast<Shader::IR::Attribute>(line)};
            if (Shader::IR::IsGeneric(attribute)) {
                info.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
            }
        }
    }
    const Shader::Stage stage{program.stage};
    const bool has_geometry{key.unique_hashes[4] != 0 && !programs[4].is_geometry_passthrough};
    const bool gl_ndc{key.state.ndc_minus_one_to_one != 0};
    const float point_size{std::bit_cast<float>(key.state.point_size)};
    switch (stage) {
    case Shader::Stage::VertexB:
        if (!has_geometry) {
            if (key.state.topology == Maxwell::PrimitiveTopology::Points) {
                info.fixed_state_point_size = point_size;
            }
            if (key.state.xfb_enabled) {
                if (device.IsExtTransformFeedbackSupported()) {
                    auto [varyings, count] =
                        VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
                    info.xfb_varyings = varyings;
                    info.xfb_count = count;
                } else {
                    LOG_WARNING(Render_Vulkan, "XFB requested in pipeline key but device lacks VK_EXT_transform_feedback; ignoring XFB decorations");
                }
            }
            info.convert_depth_mode = gl_ndc;
        }
        if (key.state.dynamic_vertex_input) {
            for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
                info.generic_input_types[index] = AttributeType(key.state, index);
            }
        } else {
            std::ranges::transform(key.state.attributes, info.generic_input_types.begin(),
                                   &CastAttributeType);
        }
        break;
    case Shader::Stage::TessellationEval:
        info.tess_clockwise = key.state.tessellation_clockwise != 0;
        info.tess_primitive = [&key] {
            const u32 raw{key.state.tessellation_primitive.Value()};
            switch (static_cast<Maxwell::Tessellation::DomainType>(raw)) {
            case Maxwell::Tessellation::DomainType::Isolines:
                return Shader::TessPrimitive::Isolines;
            case Maxwell::Tessellation::DomainType::Triangles:
                return Shader::TessPrimitive::Triangles;
            case Maxwell::Tessellation::DomainType::Quads:
                return Shader::TessPrimitive::Quads;
            }
            ASSERT(false);
            return Shader::TessPrimitive::Triangles;
        }();
        info.tess_spacing = [&] {
            const u32 raw{key.state.tessellation_spacing};
            switch (static_cast<Maxwell::Tessellation::Spacing>(raw)) {
            case Maxwell::Tessellation::Spacing::Integer:
                return Shader::TessSpacing::Equal;
            case Maxwell::Tessellation::Spacing::FractionalOdd:
                return Shader::TessSpacing::FractionalOdd;
            case Maxwell::Tessellation::Spacing::FractionalEven:
                return Shader::TessSpacing::FractionalEven;
            }
            ASSERT(false);
            return Shader::TessSpacing::Equal;
        }();
        break;
    case Shader::Stage::Mesh:
        info.convert_depth_mode = gl_ndc;
        if (program.mesh.topology == 0) {
            info.fixed_state_point_size = point_size;
        }
        break;
    case Shader::Stage::Geometry:
        if (program.output_topology == Shader::OutputTopology::PointList) {
            info.fixed_state_point_size = point_size;
        }
        if (key.state.xfb_enabled != 0) {
            if (device.IsExtTransformFeedbackSupported()) {
                auto [varyings, count] =
                    VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
                info.xfb_varyings = varyings;
                info.xfb_count = count;
            } else {
                LOG_WARNING(Render_Vulkan, "XFB requested in pipeline key but device lacks VK_EXT_transform_feedback; ignoring XFB decorations");
            }
        }
        info.convert_depth_mode = gl_ndc;
        break;
    case Shader::Stage::Fragment: {
        info.alpha_test_func = MaxwellToCompareFunction(
            key.state.UnpackComparisonOp(key.state.alpha_test_func.Value()));
        info.alpha_test_reference = std::bit_cast<float>(key.state.alpha_test_ref);

        info.dual_source_blend = key.state.attachment0_dual_source_blend != 0;

        if (device.IsMoltenVK()) {
            for (size_t i = 0; i < 8; ++i) {
                const auto format = static_cast<Tegra::RenderTargetFormat>(key.state.color_formats[i]);
                if (format == Tegra::RenderTargetFormat::NONE) {
                    info.color_output_types[i] = Shader::AttributeType::Float;
                    continue;
                }
                const auto pixel_format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(format);
                if (VideoCore::Surface::IsPixelFormatInteger(pixel_format)) {
                    if (VideoCore::Surface::IsPixelFormatSignedInteger(pixel_format)) {
                        info.color_output_types[i] = Shader::AttributeType::SignedInt;
                    } else {
                        info.color_output_types[i] = Shader::AttributeType::UnsignedInt;
                    }
                } else {
                    info.color_output_types[i] = Shader::AttributeType::Float;
                }
            }
        }
        break;
    }
    default:
        break;
    }
    switch (key.state.topology) {
    case Maxwell::PrimitiveTopology::Points:
        info.input_topology = Shader::InputTopology::Points;
        break;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        info.input_topology = Shader::InputTopology::Lines;
        break;
    case Maxwell::PrimitiveTopology::Triangles:
    case Maxwell::PrimitiveTopology::TriangleStrip:
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Quads:
    case Maxwell::PrimitiveTopology::QuadStrip:
    case Maxwell::PrimitiveTopology::Polygon:
    case Maxwell::PrimitiveTopology::Patches:
        info.input_topology = Shader::InputTopology::Triangles;
        break;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        info.input_topology = Shader::InputTopology::LinesAdjacency;
        break;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        info.input_topology = Shader::InputTopology::TrianglesAdjacency;
        break;
    }
    info.force_early_z = key.state.early_z != 0;
    info.y_negate = key.state.y_negate != 0;
    return info;
}

size_t GetTotalPipelineWorkers() {
    const size_t max_core_threads =
        std::max<size_t>(static_cast<size_t>(std::thread::hardware_concurrency()), 2ULL) - 1ULL;
#ifdef __ANDROID__
    const int configured = AndroidSettings::values.pipeline_worker_count.GetValue();
    const int clamped = std::clamp(configured, 2, 8);
    const size_t desired = static_cast<size_t>(clamped);
    if (desired == 0) {
        return 1ULL;
    }
    return std::min(max_core_threads, desired);
#else
    return max_core_threads;
#endif
}

} // Anonymous namespace

size_t ComputePipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), sizeof *this);
    return static_cast<size_t>(hash);
}

bool ComputePipelineCacheKey::operator==(const ComputePipelineCacheKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, sizeof *this) == 0;
}

size_t GraphicsPipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), Size());
    return static_cast<size_t>(hash);
}

bool GraphicsPipelineCacheKey::operator==(const GraphicsPipelineCacheKey& rhs) const noexcept {
    if (unique_hashes != rhs.unique_hashes) {
        return false;
    }
    return std::memcmp(reinterpret_cast<const char*>(this) + sizeof(unique_hashes),
                       reinterpret_cast<const char*>(&rhs) + sizeof(unique_hashes),
                       state.Size()) == 0;
}

namespace {
// NVIDIA's and AMD's drivers keep a disk cache of built pipelines of their own. Eden's copy on top
// only kept every pipeline the title ever built in host memory (title 1: a 245 MB file, ~0.35 GB
// of RAM for the session) and copied it whole at each save, which stalled frames; pipelines built
// no slower without it.
bool DriverCachesPipelines(const Device& device) {
    switch (device.GetDriverID()) {
    case VK_DRIVER_ID_NVIDIA_PROPRIETARY:
    case VK_DRIVER_ID_AMD_PROPRIETARY:
        return true;
    default:
        return false;
    }
}
} // Anonymous namespace

PipelineCache::PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_,
                             const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             GuestDescriptorQueue& guest_descriptor_queue_,
                             DescriptorBufferRing& descriptor_buffer_ring_,
                             RenderPassCache& render_pass_cache_, BufferCache& buffer_cache_,
                             TextureCache& texture_cache_, VideoCore::ShaderNotify& shader_notify_)
    : VideoCommon::ShaderCache{device_memory_}, device{device_}, scheduler{scheduler_},
      descriptor_pool{descriptor_pool_}, guest_descriptor_queue{guest_descriptor_queue_},
      descriptor_buffer_ring{descriptor_buffer_ring_},
      render_pass_cache{render_pass_cache_}, buffer_cache{buffer_cache_},
      texture_cache{texture_cache_}, shader_notify{shader_notify_},
      use_asynchronous_shaders{Settings::values.use_asynchronous_shaders.GetValue()},
      use_vulkan_pipeline_cache{Settings::values.use_vulkan_driver_pipeline_cache.GetValue() &&
                                !DriverCachesPipelines(device_)},
      workers(device.HasBrokenParallelShaderCompiling() ? 1ULL : GetTotalPipelineWorkers(),
              "VkPipelineBuilder", {}, Common::ThreadPlacement::Background),
      disk_replay_workers(device.HasBrokenParallelShaderCompiling() ? 1ULL : 2ULL,
                          "VkDiskPipelineReplay", {}, Common::ThreadPlacement::Background),
      serialization_thread(1, "VkPipelineSerialization", {},
                           Common::ThreadPlacement::Background) {
    build_budget = std::chrono::milliseconds{[] {
        const char* const value = std::getenv("EDEN_NX2_SHADER_BUDGET_MS");
        return value != nullptr ? std::max(1, std::atoi(value)) : 24;
    }()};
    const auto& float_control{device.FloatControlProperties()};
    const VkDriverId driver_id{device.GetDriverID()};
    const VkShaderStageFlags subgroup_stages{device.GetSubgroupSupportedStages()};
    const auto subgroup_stage_bit{[subgroup_stages](VkShaderStageFlags flag, Shader::Stage stage) {
        return (subgroup_stages & flag) != 0 ? (1u << static_cast<u32>(stage)) : 0u;
    }};
    u32 supported_subgroup_stages{
        subgroup_stage_bit(VK_SHADER_STAGE_VERTEX_BIT, Shader::Stage::VertexA) |
        subgroup_stage_bit(VK_SHADER_STAGE_VERTEX_BIT, Shader::Stage::VertexB) |
        subgroup_stage_bit(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                           Shader::Stage::TessellationControl) |
        subgroup_stage_bit(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                           Shader::Stage::TessellationEval) |
        subgroup_stage_bit(VK_SHADER_STAGE_GEOMETRY_BIT, Shader::Stage::Geometry) |
        subgroup_stage_bit(VK_SHADER_STAGE_FRAGMENT_BIT, Shader::Stage::Fragment) |
        subgroup_stage_bit(VK_SHADER_STAGE_COMPUTE_BIT, Shader::Stage::Compute) |
        subgroup_stage_bit(VK_SHADER_STAGE_TASK_BIT_EXT, Shader::Stage::Task) |
        subgroup_stage_bit(VK_SHADER_STAGE_MESH_BIT_EXT, Shader::Stage::Mesh)};
    if (driver_id == VK_DRIVER_ID_MESA_TURNIP) {
        supported_subgroup_stages &= ~(1u << static_cast<u32>(Shader::Stage::Geometry));
    }
    profile = Shader::Profile{
        .supported_spirv = device.SupportedSpirvVersion(),
        .unified_descriptor_binding = true,
        .support_descriptor_aliasing = device.IsDescriptorAliasingSupported(),
        .support_int8 = device.IsInt8Supported(),
        .support_uniform_and_storage_buffer_8bit =
            device.IsUniformAndStorageBuffer8BitAccessSupported(),
        .support_storage_buffer_8bit = device.IsStorageBuffer8BitAccessSupported(),
        .support_int16 = device.IsShaderInt16Supported(),
        .support_uniform_and_storage_buffer_16bit =
            device.IsUniformAndStorageBuffer16BitAccessSupported(),
        .support_storage_buffer_16bit = device.IsStorageBuffer16BitAccessSupported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .support_vertex_instance_id = false,
        .support_float_controls = device.IsKhrShaderFloatControlsSupported(),
        .support_separate_denorm_behavior =
            float_control.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_separate_rounding_mode =
            float_control.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_fp16_denorm_preserve = float_control.shaderDenormPreserveFloat16 != VK_FALSE,
        .support_fp32_denorm_preserve = float_control.shaderDenormPreserveFloat32 != VK_FALSE,
        .support_fp16_denorm_flush = float_control.shaderDenormFlushToZeroFloat16 != VK_FALSE,
        .support_fp32_denorm_flush = float_control.shaderDenormFlushToZeroFloat32 != VK_FALSE,
        .support_fp16_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat16 != VK_FALSE,
        .support_fp32_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat32 != VK_FALSE,
        .support_fp64_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat64 != VK_FALSE,
        .support_explicit_workgroup_layout = device.IsKhrWorkgroupMemoryExplicitLayoutSupported(),
        .support_workgroup_layout_8bit_access =
            device.IsWorkgroupMemoryExplicitLayout8BitAccessSupported(),
        .support_workgroup_layout_16bit_access =
            device.IsWorkgroupMemoryExplicitLayout16BitAccessSupported(),
        .support_shader_quad_control = device.IsKhrShaderQuadControlSupported(),
        .support_quad_shuffles = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_QUAD_BIT),
        .support_vote = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_VOTE_BIT),
        .support_shuffle_relative =
            device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT),
        .supported_subgroup_stages = supported_subgroup_stages,
        .support_viewport_index_layer_non_geometry =
            device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_typeless_image_loads = device.IsFormatlessImageLoadSupported(),
        .support_demote_to_helper_invocation =
            device.IsExtShaderDemoteToHelperInvocationSupported(),
        .support_int64_atomics = device.IsExtShaderAtomicInt64Supported(),
        .support_shared_int64_atomics = device.IsSharedInt64AtomicsSupported(),
        .support_derivative_control = true,
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_native_ndc = device.IsExtDepthClipControlSupported(),
        .support_scaled_attributes = !device.MustEmulateScaledFormats(),
        .support_multi_viewport = device.SupportsMultiViewport(),
        .support_geometry_streams = device.AreTransformFeedbackGeometryStreamsSupported(),
        .support_sampled_image_array_nonuniform_indexing =
            device.IsSampledImageArrayNonUniformIndexingSupported(),
        .support_storage_image_array_nonuniform_indexing =
            device.IsStorageImageArrayNonUniformIndexingSupported(),
        .support_uniform_texel_buffer_array_nonuniform_indexing =
            device.IsUniformTexelBufferArrayNonUniformIndexingSupported(),
        .support_storage_texel_buffer_array_nonuniform_indexing =
            device.IsStorageTexelBufferArrayNonUniformIndexingSupported(),

        .warp_size_potentially_larger_than_guest = device.IsWarpSizePotentiallyBiggerThanGuest(),

        .lower_left_origin_mode = false,
        .need_declared_frag_colors = false,
        .need_gather_subpixel_offset = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                       driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                       driver_id == VK_DRIVER_ID_MESA_RADV ||
                                       driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS ||
                                       driver_id == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA ||
                                       driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
                                       driver_id == VK_DRIVER_ID_MESA_TURNIP,

        .has_broken_spirv_clamp = driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
        .has_broken_spirv_position_input = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .has_broken_unsigned_image_offsets = false,
        .has_broken_signed_operations = false,
        .has_broken_fp16_float_controls = driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY,
        .has_broken_fp32_denorm_flush = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .ignore_nan_fp_comparisons = false,
        .has_broken_spirv_subgroup_mask_vector_extract_dynamic =
            driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY &&
            device.GetDriverVersion() < VK_MAKE_VERSION(512, 672, 0),
        .has_broken_spirv_subgroup_shuffle = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .has_broken_spirv_fragment_shuffle =
            driver_id == VK_DRIVER_ID_AMD_PROPRIETARY &&
            Common::EnvGate("EGLANTINE_AMD_FRAGMENT_SHUFFLE"),
        .max_subgroup_size = device.GetMaxSubgroupSize(),
        .has_broken_robust =
            device.IsNvidia() && device.GetNvidiaArch() <= NvidiaArchitecture::Arch_Pascal,
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_user_clip_distances = device.GetMaxUserClipDistances()
    };
    profile.support_cooperative_hmma =
        device.IsCooperativeHmmaSupported() && TestCooperativeHmma(device);

    host_info = Shader::HostTranslateInfo{
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_per_stage_descriptor_sampled_images = device.GetMaxPerStageDescriptorSampledImages(),
        .max_per_stage_resources = device.GetMaxPerStageResources(),
        .max_per_stage_descriptor_storage_buffers = device.GetMaxPerStageDescriptorStorageBuffers(),
        .max_descriptor_set_samplers = device.GetMaxDescriptorSetSamplers(),
        .max_descriptor_set_uniform_buffers = device.GetMaxDescriptorSetUniformBuffers(),
        .max_descriptor_set_uniform_buffers_dynamic = device.GetMaxDescriptorSetUniformBuffersDynamic(),
        .max_descriptor_set_storage_buffers = device.GetMaxDescriptorSetStorageBuffers(),
        .max_descriptor_set_storage_buffers_dynamic = device.GetMaxDescriptorSetStorageBuffersDynamic(),
        .max_descriptor_set_sampled_images = device.GetMaxDescriptorSetSampledImages(),
        .max_descriptor_set_storage_images = device.GetMaxDescriptorSetStorageImages(),
        .max_descriptor_set_input_attachements = device.GetMaxDescriptorSetInputAttachments(),
        .support_float64 = device.IsFloat64Supported(),
        .support_float16 = device.IsFloat16Supported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .needs_demote_reorder = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                driver_id == VK_DRIVER_ID_SAMSUNG_PROPRIETARY,
        .support_snorm_render_buffer = true,
        .support_viewport_index_layer = device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_conditional_barrier = device.SupportsConditionalBarriers(),
        .single_lane_geometry_subgroups = !profile.SupportsSubgroupStage(Shader::Stage::Geometry),
    };
    host_info.ApplyDescriptorLimitPolicy();

    if (device.GetMaxVertexInputAttributes() < Maxwell::NumVertexAttributes) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputAttributes is too low: {} < {}",
                    device.GetMaxVertexInputAttributes(), Maxwell::NumVertexAttributes);
    }
    if (device.GetMaxVertexInputBindings() < Maxwell::NumVertexArrays) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputBindings is too low: {} < {}",
                    device.GetMaxVertexInputBindings(), Maxwell::NumVertexArrays);
    }

    LOG_INFO(Render_Vulkan, "DynamicState setting value: {}", u32(Settings::values.dyna_state.GetValue()));

    dynamic_features = {};
    dynamic_features.driver_id = device.GetDriverID();
    dynamic_features.driver_version = device.GetDriverVersion();

    // User granularity enforced in vulkan_device.cpp switch statement:
    //   Level 0: Core Dynamic States only
    //   Level 1: Core + EDS1
    //   Level 2: Core + EDS1 + EDS2 (accumulative)
    //   Level 3: Core + EDS1 + EDS2 + EDS3 (accumulative)
    // Here we only verify if extensions were successfully loaded by the device

    dynamic_features.has_extended_dynamic_state =
        device.IsExtExtendedDynamicStateSupported();

    dynamic_features.has_extended_dynamic_state_2 =
        device.IsExtExtendedDynamicState2Supported();
    dynamic_features.has_extended_dynamic_state_2_logic_op =
        device.IsExtExtendedDynamicState2ExtrasSupported();
    dynamic_features.has_extended_dynamic_state_2_patch_control_points = false;

    dynamic_features.has_extended_dynamic_state_3_blend =
        device.IsExtExtendedDynamicState3BlendingSupported();
    dynamic_features.has_extended_dynamic_state_3_enables =
        device.IsExtExtendedDynamicState3EnablesSupported();
    dynamic_features.has_color_write_enable =
        device.IsExtColorWriteEnableSupported();
    dynamic_features.has_dynamic_state3_depth_clamp_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3DepthClampEnable();
    dynamic_features.has_dynamic_state3_logic_op_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3LogicOpEnable();
    dynamic_features.has_dynamic_state3_line_stipple_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3LineStippleEnable();

    // VIDS: Independent toggle (not affected by dyna_state levels)
    dynamic_features.has_dynamic_vertex_input =
        device.IsExtVertexInputDynamicStateSupported() &&
        Settings::values.vertex_input_dynamic_state.GetValue();
    dynamic_features.has_static_vertex_repack =
        StaticVertexRepackEnabled(device.GetMinVertexInputBindingStrideAlignment());

    dynamic_features.has_provoking_vertex = device.IsExtProvokingVertexSupported();
    dynamic_features.has_provoking_vertex_first_mode =
        device.SupportsProvokingVertexFirstMode();
    dynamic_features.has_provoking_vertex_last_mode =
        device.SupportsProvokingVertexLastMode();
    dynamic_features.has_provoking_vertex_tf_preserve =
        device.SupportsTransformFeedbackProvokingVertexPreservation();
}

namespace {
// <title>/vulkan_used.bin: the graphics pipelines recent sessions used, as key hash + sessions
// since the last use.
constexpr u32 UsedPipelinesMagic = 0x31505545; // "EUP1"
// Pipelines the last two sessions used are built at boot. Each holds ~0.45 MB of driver memory:
// title 1 built 4,800 from eight sessions, 1.5 GB more than building them on first use (but that
// stalled frames 4x as often), while one session of its title screen uses ~1,500.
// MoltenVK keeps the previous eight sessions: a pipeline built while playing goes through
// SPIR-V -> MSL -> Metal, far more than the 2-3 ms the shorter window assumes, so each one
// built at boot is a stutter avoided.
// Ocarina still built pipelines for 1-2 s of every 10 s early in play with two sessions kept:
// keep sixteen (#161).
constexpr u8 UsedPipelinesMaxAge = 2;
constexpr u8 UsedPipelinesMaxAgePerf = 16;
constexpr u8 UsedPipelinesMaxAgeMoltenVK = 8;

u8 MaxUsedPipelineAge(const Device& device) {
    if (device.IsMoltenVK()) {
        return UsedPipelinesMaxAgeMoltenVK;
    }
    return VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::PipelineRetention) ? UsedPipelinesMaxAgePerf : UsedPipelinesMaxAge;
}

std::unordered_map<u64, u8> LoadUsedPipelines(const std::filesystem::path& path, u8 max_age) {
    std::unordered_map<u64, u8> used;
    std::ifstream file{path, std::ios::binary};
    u32 magic{}, count{};
    if (!file.read(reinterpret_cast<char*>(&magic), sizeof(magic)) || magic != UsedPipelinesMagic ||
        !file.read(reinterpret_cast<char*>(&count), sizeof(count))) {
        return used;
    }
    used.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u64 hash{};
        u8 age{};
        if (!file.read(reinterpret_cast<char*>(&hash), sizeof(hash)) ||
            !file.read(reinterpret_cast<char*>(&age), sizeof(age))) {
            break;
        }
        if (age < max_age) { // lists saved with a longer window
            used.emplace(hash, age);
        }
    }
    return used;
}

// Without a list of the pipelines recent sessions used (the first boot with this cache, or a session
// that ended before saving it), this many cached graphics pipelines at most are built at boot and the
// rest when first needed. A cache that grew over many builds held 48k pipelines, ~21 GB of driver
// memory built before the game started; the list is only saved at exit or after two minutes, so a
// boot that ran out of memory never saved one and the next boot did the same.
constexpr size_t MaxPipelinesWithoutUsedList = 3000;

// EDEN_NX2_BACKGROUND_PIPELINES=0: cached pipelines boot skips are built when first needed, on
// the GPU thread, as before. EDEN_NX2_BACKGROUND_PIPELINE_THREADS sets how many threads build
// them (default a quarter of the host's, 1 to 6).
bool BackgroundPipelinesEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_BACKGROUND_PIPELINES");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

size_t BackgroundPipelineThreads() {
    if (const char* value = std::getenv("EDEN_NX2_BACKGROUND_PIPELINE_THREADS")) {
        return std::clamp<size_t>(static_cast<size_t>(std::strtoull(value, nullptr, 10)), 1, 16);
    }
    return std::clamp<size_t>(std::thread::hardware_concurrency() / 4, 1, 6);
}

// EDEN_NX2_PRELOAD_ALL_PIPELINES=1 builds every cached pipeline at boot, as before.
bool PreloadAllPipelines() {
    static const bool all = [] {
        const char* value = std::getenv("EDEN_NX2_PRELOAD_ALL_PIPELINES");
        return value != nullptr && value[0] == '1';
    }();
    return all;
}
} // Anonymous namespace

PipelineCache::~PipelineCache() {
    StopBackgroundPipelines();
    SaveUsedPipelines();
    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
        if (!device.CanReadPipelineCacheDuringBuilds()) {
            // Drop queued builds, as destroying the workers would; running ones must end first.
            workers.ClearRequests();
            workers.WaitForRequests();
        }
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
    }
}

void PipelineCache::SaveUsedPipelines() {
    used_pipelines_saved = std::chrono::steady_clock::now();
    // A session too short to say what the game uses (a boot to the menu) keeps the old list.
    constexpr size_t MinimumUsed = 64;
    if (used_pipelines_filename.empty() ||
        used_this_session.size() + used_and_evicted.size() < MinimumUsed) {
        return;
    }
    std::unordered_map<u64, u8> next;
    next.reserve(used_pipelines.size() + used_this_session.size());
    for (const auto& [key, pipeline] : graphics_cache) {
        if (used_this_session.contains(pipeline.get())) {
            next.emplace(key.Hash(), u8{0});
        }
    }
    for (const u64 hash : used_and_evicted) {
        next.emplace(hash, u8{0});
    }
    for (const auto& [hash, age] : used_pipelines) {
        if (age + 1 < MaxUsedPipelineAge(device)) {
            next.try_emplace(hash, static_cast<u8>(age + 1));
        }
    }
    std::ofstream file{used_pipelines_filename, std::ios::binary | std::ios::trunc};
    const u32 count = static_cast<u32>(next.size());
    file.write(reinterpret_cast<const char*>(&UsedPipelinesMagic), sizeof(UsedPipelinesMagic));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& [hash, age] : next) {
        file.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
        file.write(reinterpret_cast<const char*>(&age), sizeof(age));
    }
}

bool PipelineCache::IsIndirectResetShader() const {
    constexpr size_t vertex_b = static_cast<size_t>(Maxwell::ShaderType::VertexB);
    constexpr u64 reset_key = 0x72fd62555e7196cdULL;
    const auto* info = shader_infos[vertex_b];
    return maxwell3d->regs.IsShaderConfigEnabled(vertex_b) && info && info->lowered_native &&
           info->lowered_native->source_size == 128 + 11 * 16 &&
           info->lowered_native->diagnostic_key == reset_key;
}

namespace {
size_t GraphicsFastCacheSlot(const GraphicsPipelineCacheKey& key, size_t size) {
    const auto* const bytes = reinterpret_cast<const u8*>(&key);
    const size_t length = key.Size();
    u64 hash = 0x9e3779b97f4a7c15ULL;
    size_t offset = 0;
    for (; offset + sizeof(u64) <= length; offset += sizeof(u64)) {
        u64 word;
        std::memcpy(&word, bytes + offset, sizeof(word));
        hash = (hash ^ word) * 0xff51afd7ed558ccdULL;
    }
    for (; offset < length; ++offset) {
        hash = (hash ^ bytes[offset]) * 0xc4ceb9fe1a85ec53ULL;
    }
    return static_cast<size_t>(hash ^ (hash >> 29)) & (size - 1);
}
} // Anonymous namespace

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline(bool is_indexed) {

    if (!RefreshStages(graphics_key.unique_hashes)) {
        current_pipeline = nullptr;
        graphics_miss = "stage-rejected";
        return nullptr;
    }
    graphics_key.state.Refresh(*maxwell3d, dynamic_features);

    if (current_pipeline) {
        GraphicsPipeline* const next{current_pipeline->Next(graphics_key)};
        if (next) {
            current_pipeline = next;
            current_pipeline->MarkUsed(frame_count);
            return BuiltPipeline(current_pipeline, is_indexed);
        }
    }
    static const bool fast_cache =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::PipelineFastCache);
    if (!fast_cache) {
        return CurrentGraphicsPipelineSlowPath(is_indexed);
    }
    // Every cached pipeline went through the slow path, so used_this_session already holds it.
    GraphicsPipeline*& fast_entry =
        graphics_fast_cache[GraphicsFastCacheSlot(graphics_key, GraphicsFastCacheSize)];
    if (fast_entry != nullptr && fast_entry->Key() == graphics_key) {
        bool use_fast = true;
        if (VideoCommon::Nx2Perf::Verify()) {
            const auto it = graphics_cache.find(graphics_key);
            GraphicsPipeline* const slow = it != graphics_cache.end() ? it->second.get() : nullptr;
            if (slow != fast_entry) {
                use_fast = false;
                if (graphics_fast_mismatches++ < 64) {
                    LOG_ERROR(HW_GPU,
                              "Nx2Perf pipeline_fast_cache: fast={} cache={} key={:016x} "
                              "mismatches={}",
                              fmt::ptr(fast_entry), fmt::ptr(slow), graphics_key.Hash(),
                              graphics_fast_mismatches);
                }
                fast_entry = nullptr;
            }
        }
        if (use_fast) {
            GraphicsPipeline* const pipeline = fast_entry;
            if (current_pipeline) {
                current_pipeline->AddTransition(pipeline);
            }
            current_pipeline = pipeline;
            current_pipeline->MarkUsed(frame_count);
            return BuiltPipeline(current_pipeline, is_indexed);
        }
    }
    GraphicsPipeline* const result = CurrentGraphicsPipelineSlowPath(is_indexed);
    if (current_pipeline != nullptr && current_pipeline->Key() == graphics_key) {
        fast_entry = current_pipeline;
    }
    return result;
}

constexpr u32 MaxComputeRebuilds = 4;

namespace {
/// Which pointer slots a native program's arguments fill depends on the area (Ocarina of Time's
/// foliage culling fills different list slots in Hyrule Field and Castle Town), so a translation
/// rebuilt in one area may lack pointers the first cached one binds. Try first the translations
/// that bind every pointer the first one binds, those binding the most first, then the others in
/// cache order.
void OrderComputeTranslations(std::vector<FileEnvironment>& candidates) {
    if (candidates.size() < 2) {
        return;
    }
    std::vector<std::vector<u64>> keys;
    keys.reserve(candidates.size());
    for (const FileEnvironment& env : candidates) {
        keys.push_back(env.MappedStoragePointerKeys());
    }
    std::vector<size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), size_t{0});
    const auto keeps_first = [&](size_t index) {
        return std::ranges::includes(keys[index], keys.front());
    };
    std::ranges::stable_sort(order, [&](size_t lhs, size_t rhs) {
        const bool lhs_keeps{keeps_first(lhs)};
        if (lhs_keeps != keeps_first(rhs)) {
            return lhs_keeps;
        }
        return lhs_keeps && keys[lhs].size() > keys[rhs].size();
    });
    std::vector<FileEnvironment> ordered;
    ordered.reserve(candidates.size());
    for (const size_t index : order) {
        ordered.push_back(std::move(candidates[index]));
    }
    candidates = std::move(ordered);
}
} // Anonymous namespace

bool PipelineCache::StaleNullPointerSlot(const ComputePipelineCacheEntry& entry) const {
    const auto& qmd{kepler_compute->launch_description};
    for (const auto& [index, offset] : entry.null_pointer_slots) {
        if (index >= qmd.const_buffer_config.size() ||
            ((qmd.const_buffer_enable_mask >> index) & 1) == 0) {
            continue;
        }
        const auto& cbuf{qmd.const_buffer_config[index]};
        if (offset + sizeof(u64) > cbuf.size) {
            continue; // The translator reads past the bound size as zero, too
        }
        const GPUVAddr slot{cbuf.Address() + offset};
        if (!gpu_memory->IsFullyMappedRange(slot, sizeof(u64))) {
            continue;
        }
        const GPUVAddr address{gpu_memory->Read<u64>(slot)};
        if (address != 0 && gpu_memory->IsWithinGPUAddressRange(address) &&
            gpu_memory->IsFullyMappedRange(address, 1)) {
            LOG_INFO(Render_Vulkan,
                     "Rebuilding compute program {:#x}: c{}[{:#x}] was null at translation and "
                     "now points to {:#x}",
                     kepler_compute->regs.code_loc.Address() + qmd.program_start, index, offset, address);
            return true;
        }
    }
    return false;
}

ComputePipeline* PipelineCache::CurrentComputePipeline(u64& native_key) {

    const ShaderInfo* const shader{ComputeShader()};
    if (!shader) {
        return nullptr;
    }
    native_key = shader->lowered_native ? shader->lowered_native->native_program_key : 0;
    const auto& qmd{kepler_compute->launch_description};
    const ComputePipelineCacheKey key{
        .unique_hash = shader->unique_hash,
        .shared_memory_size = qmd.shared_alloc,
        .workgroup_size{qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
    };
    const size_t fast_slot = static_cast<size_t>((key.unique_hash ^ (key.unique_hash >> 32) ^
                                                  key.shared_memory_size ^ key.workgroup_size[0]) &
                                                 (ComputeFastCacheSize - 1));
    auto& fast_entry = compute_fast_cache[fast_slot];
    if (fast_entry.pipeline != nullptr && fast_entry.key == key) {
        return fast_entry.pipeline;
    }
    const auto [pair, is_new]{compute_cache.try_emplace(key)};
    auto& entry{pair->second};
    const bool rebuild_async =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ComputeRebuildAsync);
    if (entry.pending) {
        if (entry.pending->IsBuilt()) {
            if (entry.pipeline) {
                retired_compute_pipelines.emplace_back(scheduler.CurrentTick(),
                                                       std::move(entry.pipeline));
            }
            entry.pipeline = std::move(entry.pending);
        } else if (!rebuild_async) {
            if (entry.pipeline) {
                retired_compute_pipelines.emplace_back(scheduler.CurrentTick(),
                                                       std::move(entry.pipeline));
            }
            entry.pipeline = std::move(entry.pending);
        }
    }
    if (is_new) {
        entry = CreateComputePipeline(key, shader);
    } else if (!entry.pending && !entry.null_pointer_slots.empty() &&
               entry.rebuilds < MaxComputeRebuilds && StaleNullPointerSlot(entry)) {
        bool swapped_alternate = false;
        if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ComputePreloadVariants)) {
            for (auto& alt : entry.alternates) {
                ComputePipelineCacheEntry alt_check{
                    .null_pointer_slots = alt.null_pointer_slots,
                };
                if (alt.pipeline && alt.pipeline->IsBuilt() && !StaleNullPointerSlot(alt_check)) {
                    std::swap(entry.pipeline, alt.pipeline);
                    std::swap(entry.null_pointer_slots, alt.null_pointer_slots);
                    std::swap(entry.mapped_pointers, alt.mapped_pointers);
                    ++entry.rebuilds;
                    swapped_alternate = true;
                    break;
                }
            }
        }
        if (!swapped_alternate) {
            // The translation dropped accesses through a pointer that was null back then. Which slots
            // the game fills depends on the area, so the new one keeps binding every pointer this one
            // binds, null now or not: a null pointer binds nothing.
            auto rebuilt{CreateComputePipeline(key, shader, entry.mapped_pointers)};
            if (rebuilt.pipeline) {
                if (rebuild_async && entry.pipeline && !rebuilt.pipeline->IsBuilt()) {
                    entry.pending = std::move(rebuilt.pipeline);
                    entry.null_pointer_slots = std::move(rebuilt.null_pointer_slots);
                    entry.mapped_pointers = std::move(rebuilt.mapped_pointers);
                    ++entry.rebuilds;
                } else {
                    if (entry.pipeline) {
                        retired_compute_pipelines.emplace_back(scheduler.CurrentTick(),
                                                               std::move(entry.pipeline));
                    }
                    rebuilt.alternates = std::move(entry.alternates);
                    rebuilt.rebuilds = entry.rebuilds + 1;
                    entry = std::move(rebuilt);
                }
            } else {
                // Keep the working translation when the new one cannot be built (for example a
                // narrow access that now stays global across two buffers), and stop retrying.
                LOG_WARNING(Render_Vulkan, "Keeping compute shader {:016x}: rebuild failed: {}",
                            key.unique_hash, rebuilt.failure);
                entry.rebuilds = MaxComputeRebuilds;
            }
        }
    }
    if (entry.pipeline && !entry.pending &&
        (entry.null_pointer_slots.empty() || entry.rebuilds >= MaxComputeRebuilds)) {
        fast_entry.key = key;
        fast_entry.pipeline = entry.pipeline.get();
    }
    return entry.pipeline.get();
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    if (title_id == 0) {
        return;
    }
    if (!pipeline_cache_filename.empty()) {
        serialization_thread.WaitForRequests();
        if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
            SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                         CACHE_VERSION);
        }
    }

    const auto shader_dir{Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir)};
    auto base_dir{shader_dir / fmt::format("{:016x}", title_id)};
    if (title_id == 0x0400C3F00006E000ULL) {
        base_dir /= "mkw_zero_depth_v1";
    }
    if (!Common::FS::CreateDir(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create pipeline cache directories");
        return;
    }
    pipeline_cache_filename = base_dir / "vulkan.bin";
    mesh_pipeline_cache_filename = base_dir / "vulkan_mesh.bin";
    used_pipelines_filename = base_dir / "vulkan_used.bin";
    used_pipelines = LoadUsedPipelines(used_pipelines_filename, MaxUsedPipelineAge(device));
    used_pipelines_saved = std::chrono::steady_clock::now();
    size_t skipped_pipelines = 0;
    size_t queued_graphics = 0;
    VideoCommon::CacheUpgrade main_upgrade;
    VideoCommon::CacheUpgrade mesh_upgrade;
    std::unordered_set<u64> boot_graphics; ///< Graphics keys built at boot (loading thread)
    std::vector<std::pair<GraphicsPipelineCacheKey, std::vector<FileEnvironment>>> deferred_graphics;

    if (use_vulkan_pipeline_cache) {
        vulkan_pipeline_cache_filename = base_dir / "vulkan_pipelines.bin";
        vulkan_pipeline_cache =
            LoadVulkanPipelineCache(vulkan_pipeline_cache_filename, CACHE_VERSION);
    }

    struct {
        std::mutex mutex;
        size_t total{};
        size_t built{};
        bool has_loaded{};
        std::unique_ptr<PipelineStatistics> statistics;
    } state;
    // A rebuilt translation is cached beside the stale one. Choose between them before building, so
    // loading only ever adds entries (see OrderComputeTranslations), and take the next when a
    // translation fails.
    ::Common::unordered_map<ComputePipelineCacheKey, std::vector<FileEnvironment>> compute_envs;

    if (device.IsKhrPipelineExecutablePropertiesEnabled()) {
        state.statistics = std::make_unique<PipelineStatistics>(device);
    }
    const auto load_compute{[&](std::ifstream& file, FileEnvironment env) {
        ComputePipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        compute_envs[key].push_back(std::move(env));
    }};
    const bool preload_variants =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ComputePreloadVariants);
    const auto queue_compute{[&](const ComputePipelineCacheKey& key,
                                 std::vector<FileEnvironment> candidates) {
        workers.QueueWork([this, key, envs_ = std::move(candidates), preload_variants, &state,
                           &callback]() mutable {
            ShaderPools pools;
            ComputePipelineCacheEntry entry;
            std::vector<std::vector<u64>> built_keys;
            for (auto& env : envs_) {
                std::vector<u64> mapped;
                if (preload_variants && entry.pipeline) {
                    mapped = env.MappedStoragePointerKeys();
                    if (std::ranges::find(built_keys, mapped) != built_keys.end()) {
                        continue;
                    }
                }
                pools.ReleaseContents();
                auto candidate =
                    CreateComputePipeline(pools, key, env, state.statistics.get(), false);
                if (!candidate.pipeline) {
                    continue;
                }
                if (!entry.pipeline) {
                    if (preload_variants) {
                        built_keys.push_back(candidate.mapped_pointers);
                    }
                    entry = std::move(candidate);
                    if (!preload_variants) {
                        break;
                    }
                } else {
                    built_keys.push_back(std::move(mapped));
                    entry.alternates.push_back(ComputePipelineVariant{
                        .pipeline = std::move(candidate.pipeline),
                        .null_pointer_slots = std::move(candidate.null_pointer_slots),
                        .mapped_pointers = std::move(candidate.mapped_pointers),
                    });
                }
            }
            std::scoped_lock lock{state.mutex};
            if (entry.pipeline) {
                compute_cache.emplace(key, std::move(entry));
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    bool driver_updated = false;
    bool loading_mesh_file = false;
    const auto load_graphics{[&](std::ifstream& file, std::vector<FileEnvironment> envs) {
        GraphicsPipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));

        // Task/mesh pipelines live in their own file, with their mesh registers; vulkan.bin
        // doesn't carry those, so skip any it holds.
        if (!loading_mesh_file && std::ranges::any_of(envs, [](const FileEnvironment& env) {
                return env.ShaderStage() == Shader::Stage::Task ||
                       env.ShaderStage() == Shader::Stage::Mesh;
            })) {
            return;
        }
        if ((key.state.extended_dynamic_state != 0) !=
                dynamic_features.has_extended_dynamic_state ||
            (key.state.extended_dynamic_state_2 != 0) !=
                dynamic_features.has_extended_dynamic_state_2 ||
            (key.state.extended_dynamic_state_2_logic_op != 0) !=
                dynamic_features.has_extended_dynamic_state_2_logic_op ||
            (key.state.extended_dynamic_state_3_blend != 0) !=
                dynamic_features.has_extended_dynamic_state_3_blend ||
            (key.state.extended_dynamic_state_3_enables != 0) !=
                dynamic_features.has_extended_dynamic_state_3_enables ||
            (key.state.color_write_enable_dynamic != 0) !=
                dynamic_features.has_color_write_enable ||
            (key.state.dynamic_vertex_input != 0) !=
                dynamic_features.has_dynamic_vertex_input) {
            return;
        }

        const bool key_requests_provoking_last = key.state.provoking_vertex_last != 0;
        if (key_requests_provoking_last && !dynamic_features.has_provoking_vertex_last_mode) {
            return;
        }

        const bool key_uses_transform_feedback = key.state.xfb_enabled != 0;
        if (key_uses_transform_feedback && key_requests_provoking_last &&
            !dynamic_features.has_provoking_vertex_tf_preserve) {
            return;
        }

        const u64 stored_hash = key.Hash();
        if (key.state.driver_id != dynamic_features.driver_id ||
            key.state.driver_version != dynamic_features.driver_version) {
            key.state.driver_id = dynamic_features.driver_id;
            key.state.driver_version = dynamic_features.driver_version;
        }
        const u64 hash = key.Hash();
        if (stored_hash != hash) {
            if (const auto it = used_pipelines.find(stored_hash); it != used_pipelines.end()) {
                const u8 age = it->second;
                used_pipelines.erase(it);
                used_pipelines.try_emplace(hash, age);
                driver_updated = true;
            }
        }
        {
            std::scoped_lock lock{state.mutex};
            disk_pipelines.insert(hash);
        }
        if (!PreloadAllPipelines() &&
            (used_pipelines.empty() ? queued_graphics >= MaxPipelinesWithoutUsedList
                                    : !used_pipelines.contains(hash))) {
            ++skipped_pipelines;
            if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ReplayDiskRecordings)) {
                std::scoped_lock lock{state.mutex};
                skipped_disk_envs.insert_or_assign(key, std::move(envs));
            } else if (BackgroundPipelinesEnabled()) {
                deferred_graphics.emplace_back(key, std::move(envs));
            }
            return;
        }
        {
            std::scoped_lock lock{state.mutex};
            skipped_disk_envs.erase(key);
        }
        ++queued_graphics;
        boot_graphics.insert(hash);

        workers.QueueWork([this, key, envs_ = std::move(envs), &state, &callback]() mutable {
            ShaderPools pools;
            boost::container::static_vector<Shader::Environment*, 5> env_ptrs;
            for (auto& env : envs_) {
                env_ptrs.push_back(&env);
            }
            auto pipeline{CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs),
                                                 state.statistics.get(), false)};

            std::scoped_lock lock{state.mutex};
            if (pipeline) {
                graphics_cache.emplace(key, std::move(pipeline));
            } else {
                disk_pipelines.erase(key.Hash());
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    VideoCommon::LoadPipelines(stop_loading, pipeline_cache_filename, CACHE_VERSION, load_compute,
                               load_graphics, false, MIN_COMPATIBLE_CACHE_VERSION, &main_upgrade);
    if (device.IsExtMeshShaderSupported()) {
        loading_mesh_file = true;
        VideoCommon::LoadPipelines(stop_loading, mesh_pipeline_cache_filename, CACHE_VERSION,
                                   load_compute, load_graphics, true, MIN_COMPATIBLE_CACHE_VERSION,
                                   &mesh_upgrade);
    }
    if (driver_updated && !used_pipelines_filename.empty()) {
        std::ofstream file{used_pipelines_filename, std::ios::binary | std::ios::trunc};
        const u32 count = static_cast<u32>(used_pipelines.size());
        file.write(reinterpret_cast<const char*>(&UsedPipelinesMagic), sizeof(UsedPipelinesMagic));
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const auto& [hash, age] : used_pipelines) {
            file.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
            file.write(reinterpret_cast<const char*>(&age), sizeof(age));
        }
    }
    for (auto& [key, candidates] : compute_envs) {
        OrderComputeTranslations(candidates);
        queue_compute(key, std::move(candidates));
    }
    compute_envs.clear();

    LOG_INFO(Render_Vulkan, "Total Pipeline Count: {}", state.total);
    if (skipped_pipelines != 0) {
        LOG_INFO(Render_Vulkan,
                 "{} cached graphics pipelines are built when first needed",
                 skipped_pipelines);
    }

    std::unique_lock state_lock{state.mutex};
    callback(VideoCore::LoadCallbackStage::Build, 0, state.total);
    state.has_loaded = true;
    state_lock.unlock();

    workers.WaitForRequests(stop_loading);

    // A cache from an older compatible version: its records were translated again above. Write
    // it back under the current version without the ones that failed (deferred ones are kept).
    const auto finish_upgrade = [&](VideoCommon::CacheUpgrade& upgrade,
                                    const std::filesystem::path& filename) {
        if (upgrade.from_version == 0 || stop_loading.stop_requested()) {
            return;
        }
        std::vector<const std::string*> kept;
        size_t retranslated = 0, dropped = 0;
        for (size_t index = 0; index < upgrade.records.size(); ++index) {
            const std::string& record = upgrade.records[index];
            if (upgrade.is_compute[index]) {
                ComputePipelineCacheKey key;
                if (record.size() < sizeof(key)) {
                    ++dropped;
                    continue;
                }
                std::memcpy(&key, record.data() + record.size() - sizeof(key), sizeof(key));
                const auto it = compute_cache.find(key);
                if (it == compute_cache.end() || !it->second.pipeline) {
                    ++dropped;
                    continue;
                }
                ++retranslated;
            } else {
                GraphicsPipelineCacheKey key;
                if (record.size() < sizeof(key)) {
                    ++dropped;
                    continue;
                }
                std::memcpy(&key, record.data() + record.size() - sizeof(key), sizeof(key));
                if (boot_graphics.contains(key.Hash())) {
                    const auto it = graphics_cache.find(key);
                    if (it == graphics_cache.end() || !it->second) {
                        ++dropped;
                        continue;
                    }
                    ++retranslated;
                }
            }
            kept.push_back(&record);
        }
        VideoCommon::RewritePipelines(filename, CACHE_VERSION, upgrade.from_version, kept);
        LOG_INFO(Render_Vulkan,
                 "Pipeline cache from version {} kept: {} records re-translated, {} dropped ({} "
                 "kept for later)",
                 upgrade.from_version, retranslated, dropped, kept.size() - retranslated);
    };
    finish_upgrade(main_upgrade, pipeline_cache_filename);
    finish_upgrade(mesh_upgrade, mesh_pipeline_cache_filename);

    if (!deferred_graphics.empty() && !stop_loading.stop_requested()) {
        const size_t threads = BackgroundPipelineThreads();
        LOG_INFO(Render_Vulkan, "Building {} cached graphics pipelines in the background ({} threads)",
                 deferred_graphics.size(), threads);
        background_left.store(deferred_graphics.size(), std::memory_order_relaxed);
        background_start = std::chrono::steady_clock::now();
        background_active.store(true, std::memory_order_release);
        background_workers.reset(new Common::ThreadWorker(threads, "VkPipelinePrebuild", {},
                                                          Common::ThreadPlacement::Background));
        for (auto& deferred : deferred_graphics) {
            background_workers->QueueWork([this, key = deferred.first,
                                           envs_ = std::move(deferred.second)]() mutable {
                bool build = !background_stop.load(std::memory_order_relaxed);
                if (build) {
                    std::scoped_lock lock{background_mutex};
                    build = !background_claimed.contains(key);
                }
                std::unique_ptr<GraphicsPipeline> pipeline;
                if (build) {
                    ShaderPools pools;
                    boost::container::static_vector<Shader::Environment*, 5> env_ptrs;
                    for (auto& env : envs_) {
                        env_ptrs.push_back(&env);
                    }
                    pipeline = CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs), nullptr, false);
                }
                std::unique_lock lock{background_mutex};
                if (pipeline && !background_claimed.contains(key)) {
                    background_ready.emplace(key, std::move(pipeline));
                }
                if (background_left.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    const auto seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - background_start).count();
                    LOG_INFO(Render_Vulkan,
                             "Background pipelines done in {:.1f} s: {} ready, {} taken by draws, "
                             "{} built by the GPU thread first",
                             seconds, background_ready.size() + background_taken, background_taken,
                             background_claimed.size());
                    background_claimed.clear();
                    background_active.store(false, std::memory_order_release);
                }
                lock.unlock();
                // A pipeline dropped here was never bound, so no GPU work can still use it.
                pipeline.reset();
            });
        }
    }

    if (use_vulkan_pipeline_cache) {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
        size_t size = 0;
        vulkan_pipeline_cache.Read(&size, nullptr);
        last_cache_size.store(size, std::memory_order_relaxed);
        last_flush = std::chrono::steady_clock::now();
    }

    if (state.statistics) {
        state.statistics->Report();
    }
}

void PipelineCache::QueueVulkanPipelineCacheFlush() {
    if (!use_vulkan_pipeline_cache || vulkan_pipeline_cache_filename.empty() ||
        !device.CanReadPipelineCacheDuringBuilds()) {
        return;
    }
    if (++pipelines_since_flush < VULKAN_CACHE_FLUSH_PIPELINES) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto megabytes = last_cache_size.load(std::memory_order_relaxed) / (1024 * 1024);
    const std::chrono::seconds interval{
        std::max<size_t>(VULKAN_CACHE_FLUSH_MIN_SECONDS, megabytes)};
    if (last_flush.time_since_epoch().count() != 0 && now - last_flush < interval) {
        return;
    }
    if (flush_in_flight.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    pipelines_since_flush = 0;
    last_flush = now;
    serialization_thread.QueueWork([this] {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
        size_t size = 0;
        vulkan_pipeline_cache.Read(&size, nullptr);
        last_cache_size.store(size, std::memory_order_relaxed);
        flush_in_flight.store(false, std::memory_order_release);
    });
}

namespace {
// EDEN_NX2_PIPELINE_DIFF=1: for a new graphics pipeline whose shaders an earlier one uses too,
// log which parts of the fixed state differ from that one (finds state that splits the same
// draws into many pipelines, each translated on the GPU thread).
void ReportNewPipelineKey(const GraphicsPipelineCacheKey& key) {
    static const bool enabled = std::getenv("EDEN_NX2_PIPELINE_DIFF") != nullptr;
    if (!enabled) {
        return;
    }
    static std::unordered_map<u64, GraphicsPipelineCacheKey> last_by_shaders;
    static u32 logged{};
    const u64 shaders = Common::CityHash64(reinterpret_cast<const char*>(key.unique_hashes.data()),
                                           sizeof(key.unique_hashes));
    const auto [it, first] = last_by_shaders.try_emplace(shaders, key);
    if (first) {
        return;
    }
    const FixedPipelineState& now = key.state;
    const FixedPipelineState& before = it->second.state;
    std::string diffs;
    if (now.raw1 != before.raw1) diffs += fmt::format(" raw1^{:#x}", now.raw1 ^ before.raw1);
    if (now.raw2 != before.raw2) diffs += fmt::format(" raw2^{:#x}", now.raw2 ^ before.raw2);
    if (now.color_formats != before.color_formats) {
        diffs += fmt::format(" color_formats {:02x} -> {:02x}", fmt::join(before.color_formats, ""),
                             fmt::join(now.color_formats, ""));
    }
    if (now.alpha_test_ref != before.alpha_test_ref) {
        diffs += fmt::format(" alpha_test_ref {:#x} -> {:#x} (func {})", before.alpha_test_ref,
                             now.alpha_test_ref, now.alpha_test_func.Value());
    }
    if (now.point_size != before.point_size) {
        diffs += fmt::format(" point_size {:#x} -> {:#x}", before.point_size, now.point_size);
    }
    if (now.viewport_swizzles != before.viewport_swizzles) diffs += " viewport_swizzles";
    if (now.attribute_types != before.attribute_types) {
        diffs += fmt::format(" attribute_types^{:#x}", now.attribute_types ^ before.attribute_types);
    }
    if (std::memcmp(&now.dynamic_state, &before.dynamic_state, sizeof(now.dynamic_state)) != 0) {
        diffs += " dynamic_state";
    }
    if (now.driver_id != before.driver_id || now.driver_version != before.driver_version) {
        diffs += " driver";
    }
    const size_t size = now.Size();
    if (diffs.empty() && size > offsetof(FixedPipelineState, attachments)) {
        diffs = fmt::format(" past attachments (key size {})", size);
    }
    it->second = key;
    if (logged++ < 400) {
        LOG_INFO(Render_Vulkan, "New pipeline for known shaders {:016x} differs in:{}", shaders,
                 diffs.empty() ? std::string{" nothing in the key prefix"} : diffs);
    }
}
u32 ActiveColorTargetCount(const Maxwell& regs) noexcept {
    const u32 count =
        std::min<u32>(regs.rt_control.count, static_cast<u32>(Maxwell::NumRenderTargets));
    u32 active = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 rt_index = regs.rt_control.Map(i);
        if (rt_index < Maxwell::NumRenderTargets &&
            regs.rt[rt_index].format != Tegra::RenderTargetFormat::NONE &&
            regs.color_mask[regs.color_mask_common ? 0 : rt_index].raw != 0) {
            ++active;
        }
    }
    return active;
}

bool IsDepthTestedColorDraw(const Maxwell& regs) noexcept {
    return regs.zeta_enable != 0 && ActiveColorTargetCount(regs) > 0;
}

bool IsDepthOnlyDraw(const Maxwell& regs) noexcept {
    return regs.zeta_enable != 0 && ActiveColorTargetCount(regs) == 0;
}

bool IsGBufferDraw(const Maxwell& regs) noexcept {
    return regs.zeta_enable != 0 && ActiveColorTargetCount(regs) > 1;
}

u64 DrawGeometryKey(const Tegra::Engines::Maxwell3D& maxwell3d, bool is_indexed) noexcept {
    const auto& draw_state = maxwell3d.draw_manager.draw_state;
    if (draw_state.draw_element_count == 0) {
        return 0;
    }
    u64 hash = 0x9e3779b97f4a7c15ULL;
    const auto mix = [&](u64 word) noexcept {
        hash = (hash ^ word) * 0xff51afd7ed558ccdULL;
        hash ^= hash >> 33;
    };
    if (is_indexed) {
        mix(draw_state.index_buffer.StartAddress());
        mix((u64{draw_state.index_buffer.first} << 32) | draw_state.index_buffer.count);
        mix(draw_state.base_index);
    } else {
        mix(maxwell3d.regs.vertex_streams[0].Address());
        mix((u64{draw_state.vertex_buffer.first} << 32) | draw_state.vertex_buffer.count);
    }
    return hash == 0 ? 1ULL : hash;
}

// Without VK_EXT_mesh_shader: logs, once per shader set, what a skipped task/mesh pipeline would
// need from an emulation (task stage, per-primitive outputs, topology, output sizes).
template <typename Programs>
void ReportUnsupportedMeshPipeline(const GraphicsPipelineCacheKey& key, const Programs& programs) {
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    static u32 with_task{}, per_primitive{}, non_triangle{}, total{};
    u64 shaders{};
    for (const u64 hash : key.unique_hashes) {
        shaders = shaders * 31 + hash;
    }
    std::scoped_lock lock{mutex};
    if (!seen.insert(shaders).second) {
        return;
    }
    const Shader::IR::Program* task{};
    const Shader::IR::Program* mesh{};
    bool has_pixel{};
    for (size_t index = 0; index < programs.size(); ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        const auto& program{programs[index]};
        if (program.stage == Shader::Stage::Task) {
            task = &program;
        } else if (program.stage == Shader::Stage::Mesh) {
            mesh = &program;
        } else if (program.stage == Shader::Stage::Fragment) {
            has_pixel = true;
        }
    }
    ++total;
    if (mesh == nullptr) {
        LOG_WARNING(Render_Vulkan, "Mesh emulation survey: pipeline {:016x} without a mesh stage",
                    shaders);
        return;
    }
    const auto& info{mesh->mesh};
    u32 position{}, generic{}, other{};
    std::string others;
    for (const u32 line : info.vertex_lines) {
        const auto attribute{static_cast<Shader::IR::Attribute>(line)};
        if (attribute >= Shader::IR::Attribute::PositionX &&
            attribute <= Shader::IR::Attribute::PositionW) {
            ++position;
        } else if (Shader::IR::IsGeneric(attribute)) {
            ++generic;
        } else {
            ++other;
            others += fmt::format(" {}", line);
        }
    }
    with_task += task != nullptr;
    per_primitive += !info.primitive_lines.empty();
    non_triangle += info.topology != 4;
    LOG_WARNING(Render_Vulkan,
                "Mesh emulation survey #{}: task={} (payload {} words, wg {}x{}x{}) mesh wg "
                "{}x{}x{} max_vertices={} max_primitives={} topology={} vertex_lines={} "
                "(position {} generic {} other {}:{}) primitive_lines={} pixel={} | totals: "
                "task {} per-primitive {} non-triangle {}",
                total, task != nullptr, info.payload_words, task ? task->workgroup_size[0] : 0,
                task ? task->workgroup_size[1] : 0, task ? task->workgroup_size[2] : 0,
                mesh->workgroup_size[0], mesh->workgroup_size[1], mesh->workgroup_size[2],
                info.max_vertices, info.max_primitives, info.topology, info.vertex_lines.size(),
                position, generic, other, others, info.primitive_lines.size(), has_pixel,
                with_task, per_primitive, non_triangle);
}
} // Anonymous namespace

void PipelineCache::RecordSkippedGeometry(bool is_indexed,
                                          const GraphicsPipeline* pipeline) noexcept {
    if (!maxwell3d ||
        !VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::GeometryConsistency) ||
        !IsDepthTestedColorDraw(maxwell3d->regs)) {
        return;
    }
    const u64 geom_key = DrawGeometryKey(*maxwell3d, is_indexed);
    if (geom_key == 0) {
        return;
    }
    const size_t base =
        static_cast<size_t>(geom_key ^ (geom_key >> 29)) & (SkippedGeometryTableSize - 1);
    size_t target_slot = base;
    for (size_t probe = 0; probe < 4; ++probe) {
        const size_t slot = (base + probe) & (SkippedGeometryTableSize - 1);
        const auto& entry = skipped_geometry[slot];
        if (entry.key == geom_key || entry.key == 0 || frame_count > entry.frame + 1 ||
            (entry.pipeline != nullptr && entry.pipeline->IsBuilt())) {
            target_slot = slot;
            break;
        }
    }
    skipped_geometry[target_slot] = {
        .key = geom_key,
        .frame = frame_count,
        .pipeline = pipeline,
    };
    last_skipped_geometry_frame = frame_count;
}

bool PipelineCache::ShouldSkipDepthOnlyGeometry(bool is_indexed) noexcept {
    if (!maxwell3d || last_skipped_geometry_frame == ~u64{} ||
        frame_count > last_skipped_geometry_frame + 1 ||
        !VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::GeometryConsistency) ||
        !IsDepthOnlyDraw(maxwell3d->regs)) {
        return false;
    }
    const u64 geom_key = DrawGeometryKey(*maxwell3d, is_indexed);
    if (geom_key == 0) {
        return false;
    }
    const size_t base =
        static_cast<size_t>(geom_key ^ (geom_key >> 29)) & (SkippedGeometryTableSize - 1);
    for (size_t probe = 0; probe < 4; ++probe) {
        auto& entry = skipped_geometry[(base + probe) & (SkippedGeometryTableSize - 1)];
        if (entry.key != geom_key) {
            continue;
        }
        if (frame_count <= entry.frame + 1 &&
            (entry.pipeline == nullptr || !entry.pipeline->IsBuilt())) {
            return true;
        }
        entry = {};
        return false;
    }
    return false;
}

void PipelineCache::ClearSkippedGeometryIfDrawn(bool is_indexed) noexcept {
    if (!maxwell3d || last_skipped_geometry_frame == ~u64{} ||
        frame_count > last_skipped_geometry_frame + 1 ||
        !VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::GeometryConsistency) ||
        !IsDepthTestedColorDraw(maxwell3d->regs)) {
        return;
    }
    const u64 geom_key = DrawGeometryKey(*maxwell3d, is_indexed);
    if (geom_key == 0) {
        return;
    }
    const size_t base =
        static_cast<size_t>(geom_key ^ (geom_key >> 29)) & (SkippedGeometryTableSize - 1);
    for (size_t probe = 0; probe < 4; ++probe) {
        auto& entry = skipped_geometry[(base + probe) & (SkippedGeometryTableSize - 1)];
        if (entry.key == geom_key) {
            entry = {};
            return;
        }
    }
}

bool PipelineCache::DeferRuntimeBuild() {
    if (!use_asynchronous_shaders ||
        !VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ShaderBuildBudget)) {
        return false;
    }
    const auto& regs = maxwell3d->regs;
    const bool use_priority =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::BuildOrderPriority);
    const u64 key_hash = use_priority ? graphics_key.Hash() : 0;
    const bool is_gbuffer = use_priority && IsGBufferDraw(regs);
    size_t priority_index = priority_build_key_count;
    if (use_priority) {
        for (size_t i = 0; i < priority_build_key_count; ++i) {
            if (priority_build_keys[i] == key_hash) {
                priority_index = i;
                break;
            }
        }
    }
    const bool has_priority =
        !use_priority || is_gbuffer || priority_index < priority_build_key_count;
    const auto effective_budget = has_priority ? build_budget : (build_budget / 2);
    if (build_budget_frame != frame_count || build_budget_spent < effective_budget) {
        if (priority_index < priority_build_key_count) {
            priority_build_keys[priority_index] =
                priority_build_keys[--priority_build_key_count];
        }
        return false;
    }
    // Small draws without depth wait for their pipeline (BuiltPipeline), so they are built now
    // unless stale_target_skip sees their target already holds an earlier frame's output.
    // Mesh draws are deferred too: their high-detail LODs then appear a few frames late.
    if (regs.IsMeshEnabled()) {
        ++over_budget_mesh;
    } else if (!regs.zeta_enable) {
        const auto& draw_state = maxwell3d->draw_manager.draw_state;
        const bool stale_skip =
            VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::StaleTargetSkip);
        const bool is_small_draw = stale_skip
                                       ? (draw_state.draw_element_count > 0 &&
                                          draw_state.draw_element_count <= 6)
                                       : (draw_state.draw_element_count <= 6);
        if (is_small_draw && (!stale_skip || !texture_cache.ColorTargetHasPriorFrameOutput())) {
            ++over_budget_small;
            return false;
        }
    }
    if (use_priority) {
        bool recorded = false;
        for (size_t i = 0; i < deferred_candidate_count; ++i) {
            if (deferred_candidates[i].key_hash == key_hash) {
                ++deferred_candidates[i].draws;
                deferred_candidates[i].is_gbuffer |= is_gbuffer;
                recorded = true;
                break;
            }
        }
        if (!recorded) {
            if (deferred_candidate_count < DeferredCandidateSlots) {
                deferred_candidates[deferred_candidate_count++] = {
                    .key_hash = key_hash,
                    .draws = 1,
                    .is_gbuffer = is_gbuffer,
                };
            } else if (is_gbuffer) {
                for (size_t i = 0; i < DeferredCandidateSlots; ++i) {
                    if (!deferred_candidates[i].is_gbuffer) {
                        deferred_candidates[i] = {
                            .key_hash = key_hash,
                            .draws = 1,
                            .is_gbuffer = true,
                        };
                        break;
                    }
                }
            }
        }
    }
    ++deferred_builds;
    return true;
}

void PipelineCache::ChargeRuntimeBuild(std::chrono::steady_clock::time_point begin) {
    if (build_budget_frame != frame_count) {
        build_budget_frame = frame_count;
        build_budget_spent = {};
    }
    build_budget_spent += std::chrono::steady_clock::now() - begin;
}

void PipelineCache::DrainReadyDiskPipelines() {
    if (!ready_disk_pipelines_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock lock{ready_disk_pipelines_mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        return;
    }
    for (auto& [key, built] : ready_disk_pipelines) {
        in_flight_disk_replays.erase(key.Hash());
        if (built) {
            auto [it, inserted] = graphics_cache.try_emplace(key, std::move(built));
            if (inserted) {
                ++disk_replayed_count;
                QueueVulkanPipelineCacheFlush();
            }
        }
    }
    ready_disk_pipelines.clear();
    ready_disk_pipelines_pending.store(false, std::memory_order_release);
}

bool PipelineCache::CanSkipForDiskReplay() const noexcept {
    if (!use_asynchronous_shaders || !maxwell3d || maxwell3d->regs.IsMeshEnabled()) {
        return false;
    }
    if (maxwell3d->regs.zeta_enable) {
        return true;
    }
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    const bool stale_skip = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::StaleTargetSkip);
    const bool is_small_draw = stale_skip
                                   ? (draw_state.draw_element_count > 0 &&
                                      draw_state.draw_element_count <= 6)
                                   : (draw_state.draw_element_count <= 6);
    if (is_small_draw) {
        return stale_skip && texture_cache.ColorTargetHasPriorFrameOutput();
    }
    return true;
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath(bool is_indexed) {
    DrainReadyDiskPipelines();
    const u64 key_hash = graphics_key.Hash();
    if (in_flight_disk_replays.contains(key_hash)) {
        RecordSkippedGeometry(is_indexed, nullptr);
        graphics_miss = "disk-replay-async";
        return nullptr;
    }
    const auto [pair, is_new]{graphics_cache.try_emplace(graphics_key)};
    if (is_new && VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ReplayDiskRecordings)) {
        if (const auto disk_it = skipped_disk_envs.find(graphics_key);
            disk_it != skipped_disk_envs.end() && CanSkipForDiskReplay()) {
            auto envs = std::move(disk_it->second);
            skipped_disk_envs.erase(disk_it);
            graphics_cache.erase(pair);
            in_flight_disk_replays.insert(key_hash);
            const bool native_turing =
                maxwell3d != nullptr && maxwell3d->GetNativeMacroEngine().native_turing;
            disk_replay_workers.QueueWork(
                [this, key = graphics_key, envs_ = std::move(envs), native_turing]() mutable {
                    ShaderPools pools;
                    boost::container::static_vector<Shader::Environment*, 5> env_ptrs;
                    for (auto& env : envs_) {
                        env_ptrs.push_back(&env);
                    }
                    auto built = CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs), nullptr,
                                                        false, native_turing);
                    std::scoped_lock lock{ready_disk_pipelines_mutex};
                    ready_disk_pipelines.emplace_back(key, std::move(built));
                    ready_disk_pipelines_pending.store(true, std::memory_order_release);
                });
            RecordSkippedGeometry(is_indexed, nullptr);
            graphics_miss = "disk-replay-async";
            return nullptr;
        }
    }
    auto& pipeline{pair->second};
    if (is_new) {
        pipeline = TakeBackgroundPipeline(graphics_key);
        if (!pipeline) {
            if (DeferRuntimeBuild()) {
                graphics_cache.erase(pair);
                RecordSkippedGeometry(is_indexed, nullptr);
                graphics_miss = "build-deferred";
                return nullptr;
            }
            ReportNewPipelineKey(graphics_key);
            pipeline = CreateGraphicsPipeline();
        }
    }
    if (!pipeline) {
        graphics_miss = "creation-failed";
        return nullptr;
    }
    const auto save_interval = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::PipelineRetention)
                                   ? std::chrono::steady_clock::duration{std::chrono::seconds{15}}
                                   : std::chrono::steady_clock::duration{std::chrono::minutes{2}};
    if (used_this_session.insert(pipeline.get()).second &&
        std::chrono::steady_clock::now() - used_pipelines_saved > save_interval) {
        SaveUsedPipelines();
    }
    if (current_pipeline) {
        current_pipeline->AddTransition(pipeline.get());
    }
    current_pipeline = pipeline.get();
    current_pipeline->MarkUsed(frame_count);
    return BuiltPipeline(current_pipeline, is_indexed);
}

void PipelineCache::TickFrame() {
    DrainReadyDiskPipelines();
    if (deferred_candidate_count != 0) {
        const auto span = std::span{deferred_candidates.data(), deferred_candidate_count};
        std::ranges::sort(span, [](const DeferredCandidate& lhs, const DeferredCandidate& rhs) {
            if (lhs.is_gbuffer != rhs.is_gbuffer) {
                return lhs.is_gbuffer;
            }
            return lhs.draws > rhs.draws;
        });
        priority_build_key_count = std::min(deferred_candidate_count, PriorityBuildKeySlots);
        for (size_t i = 0; i < priority_build_key_count; ++i) {
            priority_build_keys[i] = deferred_candidates[i].key_hash;
        }
        deferred_candidate_count = 0;
    } else {
        priority_build_key_count = 0;
    }
    ++frame_count;
    while (!evicted_pipelines.empty() && scheduler.IsFree(evicted_pipelines.front().first)) {
        evicted_pipelines.pop_front();
    }
    while (!retired_compute_pipelines.empty() &&
           scheduler.IsFree(retired_compute_pipelines.front().first) &&
           retired_compute_pipelines.front().second->IsBuildTaskDone()) {
        retired_compute_pipelines.pop_front();
    }
    constexpr u64 EvictCheckFrames = 600;
    if (frame_count % EvictCheckFrames == 0) {
        EvictUnusedPipelines();
        if (deferred_builds != 0 || over_budget_small != 0 || over_budget_mesh != 0) {
            LOG_INFO(Render_Vulkan,
                     "Shader build budget ({} ms/frame): {} draws skipped and their pipelines "
                     "built in a later frame in the last {} frames; built over budget: {} small "
                     "draws without depth; {} of the skipped draws were mesh draws",
                     std::chrono::duration_cast<std::chrono::milliseconds>(build_budget).count(),
                     deferred_builds, EvictCheckFrames, over_budget_small, over_budget_mesh);
            deferred_builds = 0;
            over_budget_small = 0;
            over_budget_mesh = 0;
        }
    }
}

void PipelineCache::EvictUnusedPipelines() {
    // Building a pipeline again on MoltenVK means a Metal compile mid-game, far dearer than the
    // rebuild cost below, so none are evicted there.
    if (device.IsMoltenVK()) {
        return;
    }
    // Each built pipeline holds ~0.45 MB of driver memory and none was ever destroyed, so it grew
    // with every place a session went (title 1: 1,500 on its title screen alone). One unused for
    // this long is destroyed; rebuilding it later requires a full GPU-thread shader translation
    // (~8.5 ms each) plus a driver pipeline cache hit.
    constexpr u64 EvictAfterFrames = 9000;
    if (frame_count <= EvictAfterFrames) {
        return;
    }
    const u64 cutoff = frame_count - EvictAfterFrames;
    const u64 tick = scheduler.CurrentTick();
    // Nx2Perf pipeline_keep_preloaded (on by default): the ones built at boot from the used list
    // that this session has not drawn with yet keep LastUse() == 0; they are kept, not evicted after
    // 150 s and rebuilt on the GPU thread when the player enters their area.
    const bool keep_preloaded =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::PipelineKeepPreloaded);
    size_t evicted = 0;
    size_t kept = 0;
    for (auto it = graphics_cache.begin(); it != graphics_cache.end();) {
        GraphicsPipeline* const pipeline = it->second.get();
        if (pipeline == nullptr || pipeline == current_pipeline || !pipeline->IsBuilt() ||
            pipeline->LastUse() >= cutoff) {
            ++it;
            continue;
        }
        if (keep_preloaded && !used_this_session.contains(pipeline)) {
            ++kept;
            ++it;
            continue;
        }
        if (used_this_session.erase(pipeline) != 0) {
            used_and_evicted.insert(it->first.Hash());
        }
        evicted_keys.insert(it->first.Hash());
        evicted_pipelines.emplace_back(tick, std::move(it->second));
        it = graphics_cache.erase(it);
        ++evicted;
    }
    if (keep_preloaded && VideoCommon::Nx2Perf::Verify()) {
        // Eviction only frees driver memory, so there is no wrong answer to fall back from.
        LOG_INFO(Render_Vulkan,
                 "Nx2Perf pipeline_keep_preloaded: old rule would evict {}, kept {} never-used",
                 evicted + kept, kept);
    }
    if (evicted == 0) {
        return;
    }
    for (auto& [key, pipeline] : graphics_cache) {
        if (pipeline) {
            pipeline->ClearTransitions();
        }
    }
    graphics_fast_cache.fill(nullptr);
    skipped_geometry = {};
    last_skipped_geometry_frame = ~u64{};
    LOG_INFO(Render_Vulkan, "Evicted {} graphics pipelines unused for {} frames, {} left", evicted,
             EvictAfterFrames, graphics_cache.size());
}

std::unique_ptr<GraphicsPipeline> PipelineCache::TakeBackgroundPipeline(
    const GraphicsPipelineCacheKey& key) {
    std::scoped_lock lock{background_mutex};
    if (const auto it = background_ready.find(key); it != background_ready.end()) {
        auto pipeline = std::move(it->second);
        background_ready.erase(it);
        ++background_taken;
        return pipeline;
    }
    if (background_active.load(std::memory_order_acquire)) {
        background_claimed.insert(key);
    }
    return nullptr;
}

void PipelineCache::StopBackgroundPipelines() {
    background_stop.store(true, std::memory_order_relaxed);
    // Joins the threads: a build in progress finishes, queued ones are dropped.
    background_workers.reset();
}

GraphicsPipeline* PipelineCache::BuiltPipeline(GraphicsPipeline* pipeline,
                                               bool is_indexed) noexcept {
    if (pipeline->IsBuilt()) {
        if (ShouldSkipDepthOnlyGeometry(is_indexed)) {
            graphics_miss = "geometry-consistency";
            return nullptr;
        }
        ClearSkippedGeometryIfDrawn(is_indexed);
        return pipeline;
    }
    if (!use_asynchronous_shaders || pipeline->IsMeshPipeline()) {
        ClearSkippedGeometryIfDrawn(is_indexed);
        return pipeline;
    }
    // Native draws can produce terrain data used only once. Submit them after compilation
    // instead of leaving permanent holes when their first use misses the shader cache.
    if (maxwell3d->GetNativeMacroEngine().native_turing) {
        return pipeline;
    }
    // If something is using depth, we can assume that games are not rendering anything which
    // will be used one time.
    if (maxwell3d->regs.zeta_enable) {
        RecordSkippedGeometry(is_indexed, pipeline);
        graphics_miss = "async-build";
        return nullptr;
    }
    // If games are using a small index count, we can assume these are full screen quads.
    // Usually these shaders are only used once for building textures so we can assume they
    // can't be built async unless the target already holds an earlier frame's output.
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    const bool stale_skip = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::StaleTargetSkip);
    const bool is_small_draw = stale_skip
                                   ? (draw_state.draw_element_count > 0 &&
                                      draw_state.draw_element_count <= 6)
                                   : (draw_state.draw_element_count <= 6);
    if (is_small_draw) {
        if (stale_skip && texture_cache.ColorTargetHasPriorFrameOutput()) {
            graphics_miss = "async-build-stale";
            return nullptr;
        }
        return pipeline;
    }
    graphics_miss = "async-build";
    return nullptr;
}

namespace {
// Where the GPU thread's time goes when it builds a pipeline at run time (ns, summed per 10 s).
enum class BuildPhase : size_t { Environments, Translate, Emit, Module, Object, Count };
std::array<std::atomic<u64>, static_cast<size_t>(BuildPhase::Count)> runtime_phase_ns{};

struct PhaseTimer {
    BuildPhase phase;
    bool active;
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    ~PhaseTimer() {
        if (active) {
            runtime_phase_ns[static_cast<size_t>(phase)].fetch_add(
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count()),
                std::memory_order_relaxed);
        }
    }
};
} // Anonymous namespace

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    ShaderPools& pools, const GraphicsPipelineCacheKey& key,
    std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
    bool build_in_parallel, bool native_turing) try {
    auto hash = key.Hash();
    LOG_DEBUG(Render_Vulkan, "{:#016x}", hash);
    size_t env_index{0};
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a{key.unique_hashes[0] != 0};
    const bool uses_vertex_b{key.unique_hashes[1] != 0};

    // Layer passthrough generation for devices without VK_EXT_shader_viewport_index_layer
    Shader::IR::Program* layer_source_program{};
    bool mesh_unsupported{};

    std::optional<PhaseTimer> translate_timer{std::in_place, BuildPhase::Translate, build_in_parallel};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && is_emulated_stage) {
            auto topology = MaxwellToOutputTopology(key.state.topology);
            programs[index] = GenerateGeometryPassthrough(pools.inst, pools.block, host_info,
                                                          *layer_source_program, topology);
            continue;
        }
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        if (index == static_cast<size_t>(Maxwell::ShaderType::Geometry) &&
            !device.IsGeometryShaderSupported() &&
            (native_turing || env.IsNativeProgram())) {
            // A native (Switch 2) geometry program on a device without geometry shaders: the
            // pipeline cannot run it, so its draws are skipped.
            static std::atomic<bool> logged{};
            if (!logged.exchange(true)) {
                LOG_WARNING(Render_Vulkan, "Skipping native geometry shader pipelines: the device has no geometry shaders");
            }
            return nullptr;
        }

        // Without VK_EXT_mesh_shader the driver cannot build a task/mesh pipeline; the draw is
        // skipped (see the pipeline check in the rasterizer). The stages are still translated
        // so the skipped pipeline's needs can be reported below.
        if ((env.ShaderStage() == Shader::Stage::Task ||
             env.ShaderStage() == Shader::Stage::Mesh) &&
            !device.IsExtMeshShaderSupported()) {
            static std::atomic<bool> logged{};
            if (!logged.exchange(true, std::memory_order_relaxed)) {
                LOG_WARNING(Render_Vulkan,
                            "{} has no VK_EXT_mesh_shader: task/mesh pipelines run through the "
                            "compute-based mesh emulation",
                            device.GetModelName());
            }
            mesh_unsupported = true;
        }

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        if (!uses_vertex_a || index != 1) {
            // Normal path
            programs[index] = TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
        } else {
            // VertexB path when VertexA is present.
            auto& program_va{programs[0]};
            auto program_vb{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
            programs[index] = MergeDualVertexPrograms(program_va, program_vb, env, host_info);
        }

        if (device.GetDriverID() != VK_DRIVER_ID_NVIDIA_PROPRIETARY && Shader::Optimization::HasBrokenPattern(programs[index])) {
            LOG_WARNING(Render_Vulkan, "0x{:016x}: Skipping shader with known issues", key.unique_hashes[index]);
            return nullptr;
        }

        if (Settings::values.dump_guest_shaders) {
            env.Dump(hash, key.unique_hashes[index]);
        }

        if (programs[index].info.requires_layer_emulation) {
            layer_source_program = &programs[index];
        }
    }
    translate_timer.reset();
    if (host_info.max_descriptor_set_storage_buffers != 0) {
        size_t total_storage_buffers{};
        for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0;
             index < Maxwell::MaxShaderProgram; ++index) {
            total_storage_buffers +=
                Shader::NumDescriptors(programs[index].info.storage_buffers_descriptors);
        }
        if (total_storage_buffers > host_info.max_descriptor_set_storage_buffers) {
            throw Shader::NotImplementedException(
                "Pipeline binds {} storage buffers; descriptor set limit is {}",
                total_storage_buffers, host_info.max_descriptor_set_storage_buffers);
        }
    }
    // Mesh emulation without VK_EXT_mesh_shader (prototype): a mesh program, after an optional
    // task program, drawing triangles without per-primitive outputs, before a pixel program only.
    std::optional<MeshEmulationSetup> mesh_emulation;
    size_t mesh_index{};
    if (mesh_unsupported) {
        ReportUnsupportedMeshPipeline(key, programs);
        const bool has_task{key.unique_hashes[1] != 0 &&
                            programs[1].stage == Shader::Stage::Task};
        for (size_t index = 1; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0 && programs[index].stage == Shader::Stage::Mesh) {
                mesh_index = index;
                break;
            }
        }
        bool eligible{mesh_index != 0 && (mesh_index == 1 || has_task) &&
                      layer_source_program == nullptr};
        const Shader::IR::Program& mesh{programs[mesh_index]};
        eligible = eligible && mesh.mesh.topology == 4 && mesh.mesh.primitive_lines.empty() &&
                   !mesh.mesh.vertex_lines.empty() && mesh.mesh.max_vertices != 0 &&
                   mesh.mesh.max_primitives != 0;
        for (size_t index = 1; index < Maxwell::MaxShaderProgram && eligible; ++index) {
            const bool emulated{index == mesh_index || (has_task && index == 1)};
            eligible = emulated || key.unique_hashes[index] == 0 ||
                       programs[index].stage == Shader::Stage::Fragment;
        }
        if (!eligible) {
            return nullptr;
        }
        // The output buffer's binding follows every program's (unified bindings, one per
        // descriptor element, as the SPIR-V backend numbers them). The task and mesh programs
        // share the compute stage, so their descriptors and the emulation's own buffers count
        // against one set of per-stage limits.
        u32 output_binding{};
        u32 compute_storage_buffers{has_task ? 4U : 1U};
        u32 compute_sampled_images{};
        u32 compute_resources{compute_storage_buffers};
        for (size_t index = 1; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] == 0) {
                continue;
            }
            const Shader::Info& info{programs[index].info};
            const bool emulated{index == mesh_index || (has_task && index == 1)};
            const auto add = [&](const auto& descriptors, u32* category) {
                const u32 count{Shader::NumDescriptors(descriptors)};
                output_binding += count;
                if (emulated) {
                    compute_resources += count;
                    if (category) {
                        *category += count;
                    }
                }
            };
            add(info.constant_buffer_descriptors, nullptr);
            add(info.storage_buffers_descriptors, &compute_storage_buffers);
            add(info.texture_buffer_descriptors, &compute_sampled_images);
            add(info.image_buffer_descriptors, nullptr);
            add(info.texture_descriptors, &compute_sampled_images);
            add(info.image_descriptors, nullptr);
        }
        if (compute_storage_buffers > device.GetMaxPerStageDescriptorStorageBuffers() ||
            compute_sampled_images > device.GetMaxPerStageDescriptorSampledImages() ||
            compute_resources > device.GetMaxPerStageResources()) {
            LOG_WARNING(Render_Vulkan,
                        "Mesh emulation skipped: {} storage buffers, {} sampled images and {} "
                        "resources exceed the compute stage limits",
                        compute_storage_buffers, compute_sampled_images, compute_resources);
            return nullptr;
        }
        mesh_emulation.emplace();
        mesh_emulation->output_binding = output_binding;
        if (has_task) {
            // After the output: the task payload, offset and argument buffers.
            mesh_emulation->has_task = true;
            mesh_emulation->mesh_stage = static_cast<u32>(mesh_index - 1);
            mesh_emulation->payload_stride = (std::max)(programs[1].mesh.payload_words, 2U);
            mesh_emulation->payload_binding = output_binding + 1;
            mesh_emulation->offsets_binding = output_binding + 2;
            mesh_emulation->args_binding = output_binding + 3;
            mesh_emulation->distribution_spirv =
                Shader::Backend::SPIRV::EmitMeshEmulationDistribution(profile);
        }
        mesh_emulation->block_words = Shader::Backend::SPIRV::MeshEmulationBlockWords(mesh.mesh);
        mesh_emulation->max_primitives = mesh.mesh.max_primitives;
        mesh_emulation->max_vertices = mesh.mesh.max_vertices;
        mesh_emulation->vertex_lines = static_cast<u32>(mesh.mesh.vertex_lines.size());
        for (u32 line = 0; line < mesh.mesh.vertex_lines.size(); ++line) {
            const u32 attribute{mesh.mesh.vertex_lines[line]};
            const u32 position_x{static_cast<u32>(Shader::IR::Attribute::PositionX)};
            if (attribute >= position_x && attribute < position_x + 4) {
                mesh_emulation->position_lines[attribute - position_x] = line;
            }
        }
        static std::atomic<u32> emulated{};
        if (const u32 count{++emulated}; count <= 64) {
            // The input region addresses the mesh program reads (0 is its workgroup index).
            std::string reads;
            for (const Shader::IR::Block* block : mesh.blocks) {
                for (const Shader::IR::Inst& inst : block->Instructions()) {
                    if (inst.GetOpcode() != Shader::IR::Opcode::IsbeRead) {
                        continue;
                    }
                    const Shader::IR::Value address{inst.Arg(0)};
                    reads += address.IsImmediate() ? fmt::format(" {:#x}", address.U32())
                                                   : std::string{" dynamic"};
                }
            }
            LOG_INFO(Render_Vulkan,
                     "Mesh emulation #{}: pipeline {:016x}, {} vertices x {} lines, {} triangles "
                     "per workgroup, output binding {}, task {} (payload {} words), input "
                     "reads:{}",
                     count, hash, mesh.mesh.max_vertices, mesh.mesh.vertex_lines.size(),
                     mesh.mesh.max_primitives, output_binding, has_task,
                     mesh_emulation->payload_stride, reads.empty() ? " none" : reads);
        }
    }
    // A tessellation pipeline the driver would refuse is skipped like a missing mesh pipeline:
    // the draw is dropped (see the pipeline check in the rasterizer) instead of failing the build.
    {
        const Shader::IR::Program* control{};
        const Shader::IR::Program* evaluation{};
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] == 0) {
                continue;
            }
            if (programs[index].stage == Shader::Stage::TessellationControl) {
                control = &programs[index];
            } else if (programs[index].stage == Shader::Stage::TessellationEval) {
                evaluation = &programs[index];
            }
        }
        if (control != nullptr || evaluation != nullptr) {
            const u32 max_patch_size = device.GetMaxTessellationPatchSize();
            const char* reason = nullptr;
            if (!device.IsTessellationSupported()) {
                reason = "the device has no tessellation shaders";
            } else if (control == nullptr || evaluation == nullptr) {
                reason = "it has only one of the control and evaluation stages";
            } else if (control->invocations == 0 || control->invocations > max_patch_size) {
                reason = "the control shader's output vertex count is out of range";
            }
            if (reason != nullptr) {
                LOG_WARNING(Render_Vulkan,
                            "Skipping tessellation pipeline: {} (tcs={} tes={}, tcs output "
                            "vertices {}, patch control points {}, device max patch size {})",
                            reason, control != nullptr, evaluation != nullptr,
                            control != nullptr ? control->invocations : 0,
                            key.state.patch_control_points_minus_one.Value() + 1, max_patch_size);
                return nullptr;
            }
        }
    }
    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};
    std::array<vk::ShaderModule, Maxwell::MaxShaderStage> modules;

    const Shader::IR::Program* previous_stage{};
    Shader::Backend::Bindings binding;
    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram;
         ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && !is_emulated_stage) {
            continue;
        }
        UNIMPLEMENTED_IF(index == 0);

        Shader::IR::Program& program{programs[index]};
        const size_t stage_index{index - 1};
        infos[stage_index] = &program.info;

        std::optional<PhaseTimer> emit_timer{std::in_place, BuildPhase::Emit, build_in_parallel};
        auto runtime_info{MakeRuntimeInfo(programs, key, program, previous_stage, device)};
        if (mesh_emulation && (program.stage == Shader::Stage::Mesh ||
                               program.stage == Shader::Stage::Task)) {
            runtime_info.mesh_as_compute = true;
            runtime_info.mesh_output_binding = mesh_emulation->output_binding;
            if (mesh_emulation->has_task) {
                runtime_info.mesh_task_payload_binding = mesh_emulation->payload_binding;
                runtime_info.mesh_task_offsets_binding = mesh_emulation->offsets_binding;
                runtime_info.mesh_task_args_binding = mesh_emulation->args_binding;
                runtime_info.mesh_task_payload_stride = mesh_emulation->payload_stride;
            }
            if (program.stage == Shader::Stage::Mesh) {
                mesh_emulation->vertex_spirv = Shader::Backend::SPIRV::EmitMeshEmulationVertex(
                    profile, program.mesh, mesh_emulation->output_binding,
                    mesh_emulation->has_task ? mesh_emulation->args_binding : ~0U,
                    runtime_info.convert_depth_mode && !profile.support_native_ndc);
            }
        }
        ConvertLegacyToGeneric(program, runtime_info);
        // EDEN_NX2_MESH_EMULATION_DUMP=<dir>: write the emulation's vertex shader for spirv-val.
        if (mesh_emulation && program.stage == Shader::Stage::Mesh) {
            if (const char* const dump{std::getenv("EDEN_NX2_MESH_EMULATION_DUMP")}) {
                const auto& code{mesh_emulation->vertex_spirv};
                std::ofstream file{fmt::format("{}/{:016x}.vert.spv", dump, hash),
                                   std::ios::binary};
                file.write(reinterpret_cast<const char*>(code.data()),
                           static_cast<std::streamsize>(code.size() * sizeof(u32)));
            }
        }
        const std::vector<u32> code{EmitSPIRV(profile, runtime_info, program, binding)};
        device.SaveShader(code);
        emit_timer.reset();
        {
            PhaseTimer module_timer{BuildPhase::Module, build_in_parallel};
            modules[stage_index] = BuildShader(device, code);
        }

        // Text log + .spv dump. Text log is gated by gpu_log_level != Off; .spv dump
        // is independent and gated only by gpu_log_shader_dumps.
        const bool should_log = GPU::Logging::IsActive();
        const bool should_dump = Settings::values.gpu_log_shader_dumps.GetValue();
        if (should_log || should_dump) {
            static constexpr std::array stage_names{"vertex", "tess_control", "tess_eval", "geometry", "fragment"};
            const std::string shader_name = fmt::format("shader_{:016x}_{}", key.unique_hashes[index], stage_names[stage_index]);
            if (should_log) {
                const std::string shader_info = fmt::format("SPIR-V size: {} bytes, hash: {:016x}",
                    code.size() * sizeof(u32), key.unique_hashes[index]);
                GPU::Logging::GPULogger::GetInstance().LogShaderCompilation(shader_name, shader_info);
            }
            if (should_dump) {
                GPU::Logging::DumpSpirvShader(key.unique_hashes[index],
                                              std::span<const u32>(code.data(), code.size()));
            }
        }

        if (device.HasDebuggingToolAttached()) {
            const std::string name{fmt::format("Shader {:016x}", key.unique_hashes[index])};
            modules[stage_index].SetObjectNameEXT(name.c_str());
        }
        previous_stage = &program;
    }
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    PhaseTimer object_timer{BuildPhase::Object, build_in_parallel};
    return std::make_unique<GraphicsPipeline>(
        scheduler, buffer_cache, texture_cache, vulkan_pipeline_cache, &shader_notify, device,
        descriptor_pool, guest_descriptor_queue, descriptor_buffer_ring, thread_worker, statistics,
        render_pass_cache, key, std::move(modules), infos,
        std::function<std::array<vk::ShaderModule,
                                 Tegra::Engines::Maxwell3D::Regs::MaxShaderStage>()>{},
        std::move(mesh_emulation));

} catch (const Shader::Exception& exception) {
    auto hash = key.Hash();
    size_t env_index{0};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        try {
            // Walks the program for the dump; the one that failed may fail here again, and an
            // exception leaving this handler would end the process.
            Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        } catch (const Shader::Exception&) {
        }
        env.Dump(hash, key.unique_hashes[index]);
    }
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    return nullptr;
}

namespace {
// Pipelines built on the GPU thread at run time (shader translation; the driver compile is on
// workers): every one over 2 ms is logged, and the total every 10 s, to tell how much of the
// stutter on new content is translation.
void ReportRuntimePipeline(const char* kind, std::chrono::steady_clock::time_point begin,
                           bool after_eviction, bool from_disk) {
    static std::chrono::steady_clock::duration total{};
    static u32 count{}, logged{}, rebuilt{}, on_disk{}, true_misses{};
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - begin;
    total += elapsed;
    ++count;
    rebuilt += after_eviction ? 1 : 0;
    on_disk += from_disk ? 1 : 0;
    true_misses += (!after_eviction && !from_disk) ? 1 : 0;
    const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    if (elapsed > std::chrono::milliseconds{2} && logged < 400) {
        ++logged;
        LOG_INFO(Render_Vulkan, "{} pipeline built on the GPU thread in {:.1f} ms{}", kind,
                 ms(elapsed),
                 after_eviction ? " (after eviction)"
                 : from_disk    ? " (in disk cache, skipped at boot)"
                                : " (new miss)");
    }
    if (now - window > std::chrono::seconds{10}) {
        const auto phase_ms = [](BuildPhase phase) {
            return static_cast<double>(runtime_phase_ns[static_cast<size_t>(phase)].exchange(0)) /
                   1e6;
        };
        const double env_ms = phase_ms(BuildPhase::Environments);
        const double translate_ms = phase_ms(BuildPhase::Translate);
        const double emit_ms = phase_ms(BuildPhase::Emit);
        const double module_ms = phase_ms(BuildPhase::Module);
        const double object_ms = phase_ms(BuildPhase::Object);
        LOG_INFO(Render_Vulkan,
                 "Runtime pipelines in the last 10 s: {} built ({} new misses, {} in disk cache, "
                 "{} rebuilt after eviction), {:.1f} ms on the GPU thread (graphics: environments "
                 "{:.1f}, translate {:.1f}, SPIR-V {:.1f}, modules {:.1f}, pipeline object {:.1f} "
                 "ms)",
                 count, true_misses, on_disk, rebuilt, ms(total), env_ms, translate_ms, emit_ms,
                 module_ms, object_ms);
        total = {};
        count = 0;
        rebuilt = 0;
        on_disk = 0;
        true_misses = 0;
        window = now;
    }
}
} // Anonymous namespace

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {
    const auto begin = std::chrono::steady_clock::now();
    const u64 key_hash = graphics_key.Hash();
    const bool after_eviction = evicted_keys.contains(key_hash);
    const bool from_disk = !after_eviction && disk_pipelines.contains(key_hash);
    SCOPE_EXIT {
        ReportRuntimePipeline("Graphics", begin, after_eviction, from_disk);
        ChargeRuntimeBuild(begin);
    };
    const bool native_turing =
        maxwell3d != nullptr && maxwell3d->GetNativeMacroEngine().native_turing;
    if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ReplayDiskRecordings)) {
        if (const auto disk_it = skipped_disk_envs.find(graphics_key);
            disk_it != skipped_disk_envs.end()) {
            auto envs = std::move(disk_it->second);
            skipped_disk_envs.erase(disk_it);
            boost::container::static_vector<Shader::Environment*, 5> env_ptrs;
            for (auto& env : envs) {
                env_ptrs.push_back(&env);
            }
            main_pools.ReleaseContents();
            auto pipeline{CreateGraphicsPipeline(main_pools, graphics_key, MakeSpan(env_ptrs),
                                                 nullptr, true, native_turing)};
            if (pipeline && !pipeline_cache_filename.empty()) {
                QueueVulkanPipelineCacheFlush();
            }
            return pipeline;
        }
    }
    skipped_disk_envs.erase(graphics_key);
    GraphicsEnvironments environments;
    {
        PhaseTimer env_timer{BuildPhase::Environments, true};
        GetGraphicsEnvironments(environments, graphics_key.unique_hashes);
    }

    main_pools.ReleaseContents();
    auto pipeline{CreateGraphicsPipeline(main_pools, graphics_key, environments.Span(), nullptr,
                                         true, native_turing)};
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    // Task and mesh programs are translated with the mesh registers, which vulkan.bin doesn't
    // carry: they go to their own file (builds without it never read those records).
    const bool is_mesh = pipeline->IsMeshPipeline();
    if (disk_pipelines.contains(graphics_key.Hash())) {
        // In the disk cache already, only not built at boot.
        QueueVulkanPipelineCacheFlush();
        return pipeline;
    }
    disk_pipelines.insert(graphics_key.Hash());
    serialization_thread.QueueWork([this, key = graphics_key, envs = std::move(environments.envs),
                                    is_mesh] {
        boost::container::static_vector<const GenericEnvironment*, Maxwell::MaxShaderProgram>
            env_ptrs;
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0) {
                env_ptrs.push_back(&envs[index]);
            }
        }
        SerializePipeline(key, env_ptrs,
                          is_mesh ? mesh_pipeline_cache_filename : pipeline_cache_filename,
                          CACHE_VERSION, is_mesh);
    });
    QueueVulkanPipelineCacheFlush();
    return pipeline;
}

ComputePipelineCacheEntry PipelineCache::CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                               const ShaderInfo* shader,
                                                               std::span<const u64> keep_pointers) {
    const auto begin = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        ReportRuntimePipeline("Compute", begin, false, false);
        ChargeRuntimeBuild(begin);
    };
    const GPUVAddr program_base{kepler_compute->regs.code_loc.Address()};
    const auto& qmd{kepler_compute->launch_description};
    ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base, qmd.program_start,
                           shader->lowered_native};
    env.SetCachedSize(shader->size_bytes);
    env.KeepStoragePointers(keep_pointers);

    main_pools.ReleaseContents();
    auto entry{CreateComputePipeline(main_pools, key, env, nullptr, true)};
    if (!entry.pipeline || pipeline_cache_filename.empty()) {
        return entry;
    }
    serialization_thread.QueueWork([this, key, env_ = std::move(env)] {
        SerializePipeline(key, std::array<const GenericEnvironment*, 1>{&env_},
                          pipeline_cache_filename, CACHE_VERSION);
    });
    QueueVulkanPipelineCacheFlush();
    return entry;
}

ComputePipelineCacheEntry PipelineCache::CreateComputePipeline(
    ShaderPools& pools, const ComputePipelineCacheKey& key, Shader::Environment& env,
    PipelineStatistics* statistics, bool build_in_parallel) try {
    auto hash = key.Hash();
    if (device.HasBrokenCompute()) {
        LOG_ERROR(Render_Vulkan, "Skipping {:#016x}", hash);
        return {.failure = "device has broken compute"};
    }

    LOG_DEBUG(Render_Vulkan, "{:#016x}", hash);

    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};

    // Dump it before error.
    if (Settings::values.dump_guest_shaders) {
        env.Dump(hash, key.unique_hash);
    }

    auto program{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
    const VkDriverIdKHR driver_id = device.GetDriverID();
    const bool needs_shared_mem_clamp =
        driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
        driver_id == VK_DRIVER_ID_ARM_PROPRIETARY || driver_id == VK_DRIVER_ID_MOLTENVK;
    const u32 max_shared_memory = device.GetMaxComputeSharedMemorySize();
    if (needs_shared_mem_clamp && program.shared_memory_size > max_shared_memory) {
        LOG_WARNING(Render_Vulkan,
                    "Compute shader {:#016x} requests {}KB shared memory but device max is {}KB - clamping",
                    key.unique_hash,
                    program.shared_memory_size / 1024,
                    max_shared_memory / 1024);
        program.shared_memory_size = max_shared_memory;
    }
    const std::vector<u32> code{EmitSPIRV(profile, program)};
    device.SaveShader(code);
    vk::ShaderModule spv_module{BuildShader(device, code)};

    // Text log + .spv dump. Same split as the graphics path.
    const bool should_log = GPU::Logging::IsActive();
    const bool should_dump = Settings::values.gpu_log_shader_dumps.GetValue();
    if (should_log || should_dump) {
        const std::string shader_name = fmt::format("shader_{:016x}_compute", key.unique_hash);
        if (should_log) {
            const std::string shader_info = fmt::format("SPIR-V size: {} bytes, hash: {:016x}",
                code.size() * sizeof(u32), key.unique_hash);
            GPU::Logging::GPULogger::GetInstance().LogShaderCompilation(shader_name, shader_info);
        }
        if (should_dump) {
            GPU::Logging::DumpSpirvShader(key.unique_hash,
                                          std::span<const u32>(code.data(), code.size()));
        }
    }

    if (device.HasDebuggingToolAttached()) {
        const auto name{fmt::format("Shader {:016x}", key.unique_hash)};
        spv_module.SetObjectNameEXT(name.c_str());
    }
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return {.pipeline = std::make_unique<ComputePipeline>(
                device, scheduler, vulkan_pipeline_cache, descriptor_pool, guest_descriptor_queue,
                descriptor_buffer_ring, thread_worker, statistics, &shader_notify, program.info,
                std::move(spv_module), key.unique_hash),
            .null_pointer_slots = env.IsNativeProgram() ? env.NullStoragePointerSlots()
                                                        : std::vector<std::pair<u32, u32>>{},
            .mapped_pointers =
                env.IsNativeProgram() ? env.MappedStoragePointerKeys() : std::vector<u64>{}};

} catch (const Shader::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    return {.failure = exception.what()};
}

void PipelineCache::SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                                 const vk::PipelineCache& pipeline_cache,
                                                 u32 cache_version) try {
    // Written beside the cache and renamed over it: the emulator stopped mid-write left the cache
    // empty, and every later boot failed to read it.
    auto temporary = filename;
    temporary += ".tmp";
    std::ofstream file(temporary, std::ios::binary);
    file.exceptions(std::ifstream::failbit);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    file.write(VULKAN_CACHE_MAGIC_NUMBER.data(), VULKAN_CACHE_MAGIC_NUMBER.size())
        .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));

    size_t cache_size = 0;
    std::vector<char> cache_data;
    if (pipeline_cache) {
        pipeline_cache.Read(&cache_size, nullptr);
        cache_data.resize(cache_size);
        pipeline_cache.Read(&cache_size, cache_data.data());
    }
    file.write(cache_data.data(), cache_size);
    file.close();
    std::error_code error;
    std::filesystem::rename(temporary, filename, error);
    if (error) {
        LOG_ERROR(Common_Filesystem, "Failed to replace Vulkan driver pipeline cache file {}: {}",
                  Common::FS::PathToUTF8String(filename), error.message());
        std::filesystem::remove(temporary, error);
        return;
    }

    LOG_INFO(Render_Vulkan, "Vulkan driver pipelines cached at: {}",
             Common::FS::PathToUTF8String(filename));

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

vk::PipelineCache PipelineCache::LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                                         u32 expected_cache_version) {
    const auto create_pipeline_cache = [this](size_t data_size, const void* data) {
        VkPipelineCacheCreateInfo pipeline_cache_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .initialDataSize = data_size,
            .pInitialData = data};
        return device.GetLogical().CreatePipelineCache(pipeline_cache_ci);
    };
    try {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return create_pipeline_cache(0, nullptr);
        }
        file.exceptions(std::ifstream::failbit);
        const auto end{file.tellg()};
        file.seekg(0, std::ios::beg);

        std::array<char, 8> magic_number;
        u32 cache_version;
        // A file cut short (the emulator stopped while writing it) would throw from the reads
        // below, and macOS's libc++ throws an ios_base::failure the handler doesn't match.
        if (static_cast<size_t>(end) < magic_number.size() + sizeof(cache_version)) {
            file.close();
            LOG_ERROR(Common_Filesystem, "Truncated Vulkan driver pipeline cache file, deleting it");
            Common::FS::RemoveFile(filename);
            return create_pipeline_cache(0, nullptr);
        }
        file.read(magic_number.data(), magic_number.size())
            .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
        if (magic_number != VULKAN_CACHE_MAGIC_NUMBER || cache_version != expected_cache_version) {
            file.close();
            if (Common::FS::RemoveFile(filename)) {
                if (magic_number != VULKAN_CACHE_MAGIC_NUMBER) {
                    LOG_ERROR(Common_Filesystem, "Invalid Vulkan driver pipeline cache file");
                }
                if (cache_version != expected_cache_version) {
                    LOG_INFO(Common_Filesystem, "Deleting old Vulkan driver pipeline cache");
                }
            } else {
                LOG_ERROR(Common_Filesystem,
                          "Invalid Vulkan pipeline cache file and failed to delete it in \"{}\"",
                          Common::FS::PathToUTF8String(filename));
            }
            return create_pipeline_cache(0, nullptr);
        }

        static constexpr size_t header_size = magic_number.size() + sizeof(cache_version);
        const size_t cache_size = static_cast<size_t>(end) - header_size;
        std::vector<char> cache_data(cache_size);
        file.read(cache_data.data(), cache_size);

        LOG_INFO(Render_Vulkan,
                 "Loaded Vulkan driver pipeline cache: ", Common::FS::PathToUTF8String(filename));

        return create_pipeline_cache(cache_size, cache_data.data());

    } catch (const std::ios_base::failure& e) {
        LOG_ERROR(Common_Filesystem, "{}", e.what());
        if (!Common::FS::RemoveFile(filename)) {
            LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                      Common::FS::PathToUTF8String(filename));
        }

        return create_pipeline_cache(0, nullptr);
    }
}

} // namespace Vulkan

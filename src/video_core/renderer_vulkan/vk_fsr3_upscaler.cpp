// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <numbers>
#include <span>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/fsr.h"
#include "video_core/host_shaders/fsr3_upscaler_accumulate_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_accumulate_sharpen_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_luma_instability_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_luma_pyramid_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_luma_pyramid_nowave_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_prepare_inputs_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_prepare_reactivity_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_rcas_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_shading_change_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_shading_change_pyramid_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_shading_change_pyramid_nowave_comp_spv.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_fsr3_upscaler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {

// cbFSR3Upscaler in ffx_fsr3upscaler_callbacks_glsl.h (std140; the SDK's Fsr3UpscalerConstants).
struct Constants {
    std::array<s32, 2> render_size;
    std::array<s32, 2> previous_render_size;
    std::array<s32, 2> upscale_size;
    std::array<s32, 2> previous_upscale_size;
    std::array<s32, 2> max_render_size;
    std::array<s32, 2> max_upscale_size;
    std::array<f32, 4> device_to_view_depth;
    std::array<f32, 2> jitter;
    std::array<f32, 2> previous_jitter;
    std::array<f32, 2> motion_vector_scale;
    std::array<f32, 2> downscale_factor;
    std::array<f32, 2> motion_vector_jitter_cancellation;
    f32 tan_half_fov;
    f32 jitter_phase_count;
    f32 delta_time;
    f32 delta_pre_exposure;
    f32 view_space_to_meters;
    f32 frame_index;
    f32 velocity_factor;
    f32 reactiveness_scale;
    f32 shading_change_scale;
    f32 accumulation_added_per_frame;
    f32 min_disocclusion_accumulation;
};
static_assert(sizeof(Constants) == 148);

// cbSPD and cbRCAS.
struct SpdConstants {
    u32 mips;
    u32 num_work_groups;
    std::array<u32, 2> work_group_offset;
    std::array<u32, 2> render_size;
};

struct RcasConstants {
    std::array<u32, 4> config;
};

// Descriptor kinds in binding order, as the pass files number them (FSR3UPSCALER_BIND_*). Every
// pass also has the two samplers at bindings 1000 (point clamp) and 1001 (linear clamp).
enum class Kind : u8 { Texture, Image, Uniform };
constexpr Kind T = Kind::Texture;
constexpr Kind I = Kind::Image;
constexpr Kind U = Kind::Uniform;

constexpr std::array PREPARE_INPUTS{T, T, T, I, I, I, I, I, U};
constexpr std::array LUMA_PYRAMID{T, T, I, I, I, I, I, I, I, I, I, U, U};
constexpr std::array SHADING_CHANGE_PYRAMID{T, T, T, T, I, I, I, I, I, I, I, U, U};
constexpr std::array SHADING_CHANGE{T, I, U};
constexpr std::array PREPARE_REACTIVITY{T, T, T, T, T, T, T, T, T, I, I, I, U};
constexpr std::array LUMA_INSTABILITY{T, T, T, T, T, T, T, I, I, U};
constexpr std::array ACCUMULATE{T, T, T, T, T, T, T, T, T, I, I, I, U};
constexpr std::array RCAS{T, T, I, U, U};

enum PassId : size_t {
    PassPrepareInputs,
    PassLumaPyramid,
    PassShadingChangePyramid,
    PassShadingChange,
    PassPrepareReactivity,
    PassLumaInstability,
    PassAccumulate,
    PassAccumulateSharpen,
    PassRcas,
    PassCount,
};

constexpr u32 SPD_STORED_MIPS = 6;

/// One descriptor of a dispatch: an image view, or a uniform buffer range.
struct Resource {
    VkImageView view{};
    VkBuffer buffer{};
    VkDeviceSize offset{};
    VkDeviceSize size{};
};

constexpr VkMemoryBarrier MemoryBarrier(VkAccessFlags src, VkAccessFlags dst) {
    return VkMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src,
        .dstAccessMask = dst,
    };
}

constexpr VkImageSubresourceRange ALL_LEVELS{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = VK_REMAINING_MIP_LEVELS,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

// ffxFsr3UpscalerGetJitterPhaseCount.
s32 JitterPhaseCount(u32 render_width, u32 display_width) {
    const f32 ratio = static_cast<f32>(display_width) / static_cast<f32>(render_width);
    return static_cast<s32>(8.0f * ratio * ratio);
}

// DLSS takes no camera, so FSR's depth-to-view-space conversion (which only scales its
// disocclusion thresholds) uses a typical near plane and vertical field of view.
constexpr f32 CAMERA_NEAR = 0.1f;
constexpr f32 CAMERA_FOV = 60.0f * std::numbers::pi_v<f32> / 180.0f;

} // Anonymous namespace

class Fsr3Upscaler::Pass final : public ComputePass {
public:
    explicit Pass(const Device& device_, Scheduler& scheduler, DescriptorPool& descriptor_pool,
                  std::span<const Kind> kinds, std::span<const u32> code,
                  const std::array<VkSampler, 2>& samplers)
        : Pass(device_, scheduler, descriptor_pool, MakeLayout(kinds, samplers), code) {}

    void Dispatch(Scheduler& scheduler, ComputePassDescriptorQueue& queue,
                  std::span<const Resource> resources, u32 groups_x, u32 groups_y) {
        queue.Acquire(scheduler, resources.size());
        for (const Resource& resource : resources) {
            if (resource.buffer) {
                queue.AddBuffer(resource.buffer, resource.offset, resource.size);
            } else {
                queue.AddImage(resource.view);
            }
        }
        const void* const descriptor_data{queue.UpdateData()};
        scheduler.Record([this, descriptor_data, groups_x, groups_y](vk::CommandBuffer cmdbuf) {
            const VkDescriptorSet set = descriptor_allocator.Commit();
            device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptor_data);
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
            cmdbuf.Dispatch(groups_x, groups_y, 1);
            // The next pass reads what this one wrote (everything stays in GENERAL).
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                   MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT,
                                                 VK_ACCESS_SHADER_READ_BIT |
                                                     VK_ACCESS_SHADER_WRITE_BIT));
        });
    }

private:
    struct Layout {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<VkDescriptorUpdateTemplateEntry> templates;
        DescriptorBankInfo bank;
    };

    explicit Pass(const Device& device_, Scheduler& scheduler, DescriptorPool& descriptor_pool,
                  const Layout& pass_layout, std::span<const u32> code)
        : ComputePass(device_, scheduler, descriptor_pool, pass_layout.bindings,
                      pass_layout.templates, pass_layout.bank, {}, code) {}

    static Layout MakeLayout(std::span<const Kind> kinds,
                             const std::array<VkSampler, 2>& samplers) {
        Layout result;
        for (u32 binding = 0; binding < kinds.size(); ++binding) {
            VkDescriptorType type{};
            switch (kinds[binding]) {
            case Kind::Texture:
                type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                ++result.bank.sampled_images;
                break;
            case Kind::Image:
                type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                ++result.bank.images;
                break;
            case Kind::Uniform:
                type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                ++result.bank.uniform_buffers;
                break;
            }
            result.bindings.push_back({
                .binding = binding,
                .descriptorType = type,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .pImmutableSamplers = nullptr,
            });
            result.templates.push_back({
                .dstBinding = binding,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = type,
                .offset = binding * sizeof(DescriptorUpdateEntry),
                .stride = sizeof(DescriptorUpdateEntry),
            });
        }
        for (u32 index = 0; index < samplers.size(); ++index) {
            result.bindings.push_back({
                .binding = 1000 + index,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .pImmutableSamplers = &samplers[index],
            });
            ++result.bank.samplers;
        }
        result.bank.score = static_cast<s32>(result.bindings.size());
        return result;
    }
};

Fsr3Upscaler::Fsr3Upscaler(const Device& device_, Scheduler& scheduler_,
                           MemoryAllocator& memory_allocator_,
                           StagingBufferPool& staging_buffer_pool_,
                           DescriptorPool& descriptor_pool,
                           ComputePassDescriptorQueue& descriptor_queue_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      staging_buffer_pool{staging_buffer_pool_}, descriptor_queue{descriptor_queue_} {
    const auto sampler = [&](VkFilter filter, VkSamplerMipmapMode mipmap) {
        return device.GetLogical().CreateSampler(VkSamplerCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .magFilter = filter,
            .minFilter = filter,
            .mipmapMode = mipmap,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .mipLodBias = 0.0f,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0f,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_NEVER,
            .minLod = 0.0f,
            .maxLod = VK_LOD_CLAMP_NONE,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        });
    };
    point_sampler = sampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST);
    linear_sampler = sampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR);
    const std::array<VkSampler, 2> samplers{*point_sampler, *linear_sampler};

    // SPD reduces with quad shuffles where the device has them in compute shaders.
    const bool quads = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_QUAD_BIT) &&
                       (device.GetSubgroupSupportedStages() & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
    const auto pass = [&](std::span<const Kind> kinds, std::span<const u32> code) {
        return std::make_unique<Pass>(device, scheduler, descriptor_pool, kinds, code, samplers);
    };
    passes.resize(PassCount);
    passes[PassPrepareInputs] = pass(PREPARE_INPUTS, FSR3_UPSCALER_PREPARE_INPUTS_COMP_SPV);
    passes[PassLumaPyramid] =
        pass(LUMA_PYRAMID, quads ? std::span<const u32>(FSR3_UPSCALER_LUMA_PYRAMID_COMP_SPV)
                                 : std::span<const u32>(FSR3_UPSCALER_LUMA_PYRAMID_NOWAVE_COMP_SPV));
    passes[PassShadingChangePyramid] = pass(
        SHADING_CHANGE_PYRAMID,
        quads ? std::span<const u32>(FSR3_UPSCALER_SHADING_CHANGE_PYRAMID_COMP_SPV)
              : std::span<const u32>(FSR3_UPSCALER_SHADING_CHANGE_PYRAMID_NOWAVE_COMP_SPV));
    passes[PassShadingChange] = pass(SHADING_CHANGE, FSR3_UPSCALER_SHADING_CHANGE_COMP_SPV);
    passes[PassPrepareReactivity] =
        pass(PREPARE_REACTIVITY, FSR3_UPSCALER_PREPARE_REACTIVITY_COMP_SPV);
    passes[PassLumaInstability] = pass(LUMA_INSTABILITY, FSR3_UPSCALER_LUMA_INSTABILITY_COMP_SPV);
    passes[PassAccumulate] = pass(ACCUMULATE, FSR3_UPSCALER_ACCUMULATE_COMP_SPV);
    passes[PassAccumulateSharpen] = pass(ACCUMULATE, FSR3_UPSCALER_ACCUMULATE_SHARPEN_COMP_SPV);
    passes[PassRcas] = pass(RCAS, FSR3_UPSCALER_RCAS_COMP_SPV);
}

Fsr3Upscaler::~Fsr3Upscaler() = default;

bool Fsr3Upscaler::IsSupported(const Device& device) {
    if (!device.IsStorageImageExtendedFormatsSupported()) {
        return false;
    }
    constexpr VkFormatFeatureFlags storage =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    for (const VkFormat format :
         {VK_FORMAT_R8_UNORM, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_SFLOAT,
          VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT}) {
        if (!device.IsFormatSupported(format, storage, FormatType::Optimal)) {
            return false;
        }
    }
    return device.IsFormatSupported(VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
                                    FormatType::Optimal) &&
           device.IsFormatSupported(VK_FORMAT_R32G32B32A32_SFLOAT,
                                    VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, FormatType::Optimal) &&
           device.IsFormatSupported(VK_FORMAT_R16_SNORM,
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
                                    FormatType::Optimal);
}

VkImage Fsr3Upscaler::OutputImage() const noexcept {
    return *output.image;
}

VkImageView Fsr3Upscaler::OutputView() const noexcept {
    return *output.view;
}

Fsr3Upscaler::Texture Fsr3Upscaler::CreateTexture(VkFormat format, u32 width, u32 height,
                                                  u32 levels) {
    Texture texture;
    texture.image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {(std::max)(width, 1U), (std::max)(height, 1U), 1},
        .mipLevels = levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = static_cast<VkImageUsageFlags>(
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (format == VK_FORMAT_R16_SNORM ? 0 : VK_IMAGE_USAGE_STORAGE_BIT)),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    const auto view = [&](u32 base_level, u32 level_count) {
        return device.GetLogical().CreateImageView(VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *texture.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .components{},
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = base_level,
                .levelCount = level_count,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        });
    };
    texture.view = view(0, levels);
    if (levels > 1) {
        for (u32 level = 0; level < levels; ++level) {
            texture.levels.push_back(view(level, 1));
        }
    }
    return texture;
}

void Fsr3Upscaler::CreateResources(VkExtent2D render_size, VkExtent2D upscale_size) {
    const u32 rw = render_size.width;
    const u32 rh = render_size.height;
    const u32 uw = upscale_size.width;
    const u32 uh = upscale_size.height;
    for (size_t i = 0; i < 2; ++i) {
        accumulation[i] = CreateTexture(VK_FORMAT_R8_UNORM, rw, rh);
        luma[i] = CreateTexture(VK_FORMAT_R16_SFLOAT, rw, rh);
        // The pass files declare the luma history as rgba8 (the SDK allocates RGBA16F, which a
        // storage format qualifier does not match); allocate what the shaders store.
        luma_history[i] = CreateTexture(VK_FORMAT_R8G8B8A8_UNORM, rw, rh);
        internal_upscaled[i] = CreateTexture(VK_FORMAT_R16G16B16A16_SFLOAT, uw, uh);
    }
    farthest_depth = CreateTexture(VK_FORMAT_R16_SFLOAT, rw, rh);
    shading_change = CreateTexture(VK_FORMAT_R8_UNORM, rw / 2, rh / 2);
    new_locks = CreateTexture(VK_FORMAT_R8_UNORM, uw, uh);
    const u32 spd_levels =
        static_cast<u32>(std::floor(std::log2(static_cast<f32>((std::max)(rw / 2, rh / 2))))) + 1;
    spd_mips = CreateTexture(VK_FORMAT_R16G16_SFLOAT, rw / 2, rh / 2,
                             (std::max)(spd_levels, SPD_STORED_MIPS));
    farthest_depth_mip1 = CreateTexture(VK_FORMAT_R16_SFLOAT, rw / 2, rh / 2);
    dilated_motion_vectors = CreateTexture(VK_FORMAT_R16G16_SFLOAT, rw, rh);
    dilated_depth = CreateTexture(VK_FORMAT_R32_SFLOAT, rw, rh);
    previous_nearest_depth = CreateTexture(VK_FORMAT_R32_UINT, rw, rh);
    dilated_reactive_masks = CreateTexture(VK_FORMAT_R8G8B8A8_UNORM, rw, rh);
    frame_info = CreateTexture(VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1);
    spd_atomic = CreateTexture(VK_FORMAT_R32_UINT, 1, 1);
    lanczos_lut = CreateTexture(VK_FORMAT_R16_SNORM, 128, 1);
    default_reactive = CreateTexture(VK_FORMAT_R8_UNORM, 1, 1);
    output = CreateTexture(VK_FORMAT_R16G16B16A16_SFLOAT, uw, uh);
    render_extent = render_size;
    upscale_extent = upscale_size;
    first_execution = true;
    frame_parity = 0;
    LOG_INFO(Render_Vulkan, "FSR 3.1 upscaler: {}x{} -> {}x{}", rw, rh, uw, uh);
}

void Fsr3Upscaler::Upscale(const Inputs& in) {
    if (!output.image || in.render_size.width != render_extent.width ||
        in.render_size.height != render_extent.height ||
        in.upscale_size.width != upscale_extent.width ||
        in.upscale_size.height != upscale_extent.height) {
        if (output.image) {
            scheduler.Finish(); // the old images may still be in use
        }
        CreateResources(in.render_size, in.upscale_size);
    }
    const bool first = first_execution;
    const bool reset = in.reset || first;
    first_execution = false;

    const s32 rw = static_cast<s32>(render_extent.width);
    const s32 rh = static_cast<s32>(render_extent.height);
    const s32 uw = static_cast<s32>(upscale_extent.width);
    const s32 uh = static_cast<s32>(upscale_extent.height);

    // Constants, as fsr3upscalerDispatch computes them.
    Constants constants{};
    constants.render_size = {rw, rh};
    constants.previous_render_size = first ? constants.render_size : previous_render_size;
    constants.upscale_size = {uw, uh};
    constants.previous_upscale_size = first ? constants.upscale_size : previous_upscale_size;
    constants.max_render_size = constants.render_size;
    constants.max_upscale_size = constants.upscale_size;
    {
        // Reversed infinite depth (setupDeviceDepthToViewSpaceDepthParams with near = FLT_MAX).
        const f32 near_plane = CAMERA_NEAR;
        const f32 fov = CAMERA_FOV;
        const f32 aspect = static_cast<f32>(rw) / static_cast<f32>(rh);
        const f32 cot_half_fov = std::cos(0.5f * fov) / std::sin(0.5f * fov);
        constants.device_to_view_depth = {-1.0f * (0.0f + FLT_EPSILON), near_plane,
                                          1.0f / (cot_half_fov / aspect), 1.0f / cot_half_fov};
        constants.tan_half_fov = std::tan(std::atan(std::tan(fov * 0.5f) * aspect));
    }
    constants.jitter = in.jitter;
    constants.previous_jitter = first ? in.jitter : previous_jitter;
    constants.motion_vector_scale = {in.motion_vector_scale[0] / static_cast<f32>(rw),
                                     in.motion_vector_scale[1] / static_cast<f32>(rh)};
    constants.downscale_factor = {static_cast<f32>(rw) / static_cast<f32>(uw),
                                  static_cast<f32>(rh) / static_cast<f32>(uh)};
    constants.motion_vector_jitter_cancellation = {0.0f, 0.0f};
    const s32 phase_count = JitterPhaseCount(render_extent.width, upscale_extent.width);
    if (reset || jitter_phase_count == 0.0f) {
        jitter_phase_count = static_cast<f32>(phase_count);
    } else if (static_cast<f32>(phase_count) > jitter_phase_count) {
        jitter_phase_count += 1.0f;
    } else if (static_cast<f32>(phase_count) < jitter_phase_count) {
        jitter_phase_count -= 1.0f;
    }
    constants.jitter_phase_count = jitter_phase_count;
    constants.delta_time = std::clamp(in.frame_time_ms / 1000.0f, 0.0f, 1.0f);
    constants.delta_pre_exposure = 1.0f;
    constants.view_space_to_meters = 1.0f;
    frame_index = reset ? 0.0f : frame_index + 1.0f;
    constants.frame_index = frame_index;
    // The SDK's defaults (FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_*).
    constants.velocity_factor = 1.0f;
    constants.reactiveness_scale = 1.0f;
    constants.shading_change_scale = 1.0f;
    constants.accumulation_added_per_frame = 1.0f / 3.0f;
    constants.min_disocclusion_accumulation = -1.0f / 3.0f;
    previous_render_size = constants.render_size;
    previous_upscale_size = constants.upscale_size;
    previous_jitter = in.jitter;

    // ffxSpdSetup over the whole render area.
    const u32 spd_groups_x = (render_extent.width - 1) / 64 + 1;
    const u32 spd_groups_y = (render_extent.height - 1) / 64 + 1;
    const SpdConstants spd{
        .mips = static_cast<u32>((std::min)(
            std::floor(std::log2(static_cast<f32>(
                (std::max)(render_extent.width, render_extent.height)))),
            12.0f)),
        .num_work_groups = spd_groups_x * spd_groups_y,
        .work_group_offset = {0, 0},
        .render_size = {render_extent.width, render_extent.height},
    };
    RcasConstants rcas{};
    FSR::FsrRcasCon(rcas.config.data(), 2.0f - 2.0f * std::clamp(in.sharpness, 0.0f, 1.0f));

    const VkDeviceSize alignment = (std::max<VkDeviceSize>)(device.GetUniformBufferAlignment(), 16);
    const VkDeviceSize slot = Common::AlignUp(VkDeviceSize{sizeof(Constants)}, alignment);
    const VkDeviceSize spd_slot = Common::AlignUp(VkDeviceSize{sizeof(SpdConstants)}, alignment);
    const VkDeviceSize rcas_slot = Common::AlignUp(VkDeviceSize{sizeof(RcasConstants)}, alignment);
    const StagingBufferRef uniforms =
        staging_buffer_pool.Request(slot + spd_slot + rcas_slot, MemoryUsage::Upload);
    std::memcpy(uniforms.mapped_span.data(), &constants, sizeof(constants));
    std::memcpy(uniforms.mapped_span.data() + slot, &spd, sizeof(spd));
    std::memcpy(uniforms.mapped_span.data() + slot + spd_slot, &rcas, sizeof(rcas));
    const Resource cb_fsr{.buffer = uniforms.buffer, .offset = uniforms.offset, .size = slot};
    const Resource cb_spd{
        .buffer = uniforms.buffer, .offset = uniforms.offset + slot, .size = spd_slot};
    const Resource cb_rcas{
        .buffer = uniforms.buffer, .offset = uniforms.offset + slot + spd_slot, .size = rcas_slot};

    // Clears: everything when the images are new, the history on a reset, and the per-frame
    // scratch (previous nearest depth, the SPD counter and mips) always.
    const size_t p = frame_parity;
    const size_t q = p ^ 1;
    std::vector<VkImage> clear_zero{*previous_nearest_depth.image, *spd_atomic.image,
                                    *spd_mips.image};
    std::vector<VkImage> clear_frame_info;
    if (reset) {
        clear_zero.push_back(*accumulation[p].image);
        clear_frame_info.push_back(*frame_info.image);
    }
    std::vector<VkImage> initialize;
    if (first) {
        for (const auto* textures : {&accumulation, &luma, &luma_history, &internal_upscaled}) {
            for (const Texture& texture : *textures) {
                initialize.push_back(*texture.image);
            }
        }
        for (const Texture* texture :
             {&farthest_depth, &shading_change, &new_locks, &spd_mips, &farthest_depth_mip1,
              &dilated_motion_vectors, &dilated_depth, &previous_nearest_depth,
              &dilated_reactive_masks, &frame_info, &spd_atomic, &lanczos_lut, &default_reactive,
              &output}) {
            initialize.push_back(*texture->image);
        }
        clear_zero = initialize;
        std::erase(clear_zero, *lanczos_lut.image);
        std::erase(clear_zero, *frame_info.image);
        clear_frame_info = {*frame_info.image};
    }
    std::optional<StagingBufferRef> lut_upload;
    if (first) {
        // FSR's Lanczos-2 table (fsr3upscalerCreate).
        std::array<s16, 128> weights{};
        for (size_t i = 0; i < weights.size(); ++i) {
            const f32 x = 2.0f * static_cast<f32>(i) / static_cast<f32>(weights.size() - 1);
            const f32 pi_x = std::numbers::pi_v<f32> * x;
            const f32 y = std::abs(x) < FLT_EPSILON
                              ? 1.0f
                              : (std::sin(pi_x) / pi_x) * (std::sin(0.5f * pi_x) / (0.5f * pi_x));
            weights[i] = static_cast<s16>(std::round(y * 32767.0f));
        }
        lut_upload = staging_buffer_pool.Request(sizeof(weights), MemoryUsage::Upload);
        std::memcpy(lut_upload->mapped_span.data(), weights.data(), sizeof(weights));
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([initialize, clear_zero, clear_frame_info, lut = *lanczos_lut.image,
                      lut_buffer = lut_upload ? lut_upload->buffer : VkBuffer{},
                      lut_offset = lut_upload ? lut_upload->offset : 0](vk::CommandBuffer cmdbuf) {
        // The inputs come from the game's draws and dispatches; our images from the last frame.
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0,
                               MemoryBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_SHADER_WRITE_BIT |
                                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                                 VK_ACCESS_TRANSFER_WRITE_BIT));
        if (!initialize.empty()) {
            std::vector<VkImageMemoryBarrier> barriers;
            for (const VkImage image : initialize) {
                barriers.push_back(VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = image,
                    .subresourceRange = ALL_LEVELS,
                });
            }
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, {}, {}, barriers);
        }
        if (lut_buffer) {
            const VkBufferImageCopy copy{
                .bufferOffset = lut_offset,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .imageOffset{0, 0, 0},
                .imageExtent{128, 1, 1},
            };
            cmdbuf.CopyBufferToImage(lut_buffer, lut, VK_IMAGE_LAYOUT_GENERAL, copy);
        }
        for (const VkImage image : clear_zero) {
            cmdbuf.ClearColorImage(image, VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{}, ALL_LEVELS);
        }
        for (const VkImage image : clear_frame_info) {
            VkClearColorValue value{};
            value.float32[0] = -1.0f;
            value.float32[1] = 1.0f;
            cmdbuf.ClearColorImage(image, VK_IMAGE_LAYOUT_GENERAL, value, ALL_LEVELS);
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0,
                               MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT));
    });

    const auto tex = [](const Texture& texture) { return Resource{.view = *texture.view}; };
    const auto mip = [&](u32 level) {
        return Resource{.view = *spd_mips.levels[(std::min)(level, static_cast<u32>(
                                                                       spd_mips.levels.size() - 1))]};
    };
    const Resource color{.view = in.color};
    const Resource depth{.view = in.depth};
    const Resource motion{.view = in.motion_vectors};
    const Resource exposure = tex(frame_info); // FSR's own exposure (auto exposure)
    const Resource current_luma = tex(luma[p]);
    const auto groups = [](s32 size, u32 tile) {
        return Common::DivCeil(static_cast<u32>(size), tile);
    };

    passes[PassPrepareInputs]->Dispatch(
        scheduler, descriptor_queue,
        std::array{motion, depth, color, tex(dilated_motion_vectors), tex(dilated_depth),
                   tex(previous_nearest_depth), tex(farthest_depth), current_luma, cb_fsr},
        groups(rw, 8), groups(rh, 8));
    passes[PassLumaPyramid]->Dispatch(
        scheduler, descriptor_queue,
        std::array{current_luma, tex(farthest_depth), tex(spd_atomic), tex(frame_info), mip(0),
                   mip(1), mip(2), mip(3), mip(4), mip(5), tex(farthest_depth_mip1), cb_fsr,
                   cb_spd},
        spd_groups_x, spd_groups_y);
    passes[PassShadingChangePyramid]->Dispatch(
        scheduler, descriptor_queue,
        std::array{current_luma, tex(luma[q]), tex(dilated_motion_vectors), exposure,
                   tex(spd_atomic), mip(0), mip(1), mip(2), mip(3), mip(4), mip(5), cb_fsr, cb_spd},
        spd_groups_x, spd_groups_y);
    passes[PassShadingChange]->Dispatch(
        scheduler, descriptor_queue, std::array{tex(spd_mips), tex(shading_change), cb_fsr},
        groups(rw / 2, 8), groups(rh / 2, 8));
    passes[PassPrepareReactivity]->Dispatch(
        scheduler, descriptor_queue,
        std::array{tex(previous_nearest_depth), tex(dilated_motion_vectors), tex(dilated_depth),
                   tex(default_reactive), tex(default_reactive), tex(accumulation[p]),
                   tex(shading_change), current_luma, exposure, tex(dilated_reactive_masks),
                   tex(new_locks), tex(accumulation[q]), cb_fsr},
        groups(rw, 8), groups(rh, 8));
    passes[PassLumaInstability]->Dispatch(
        scheduler, descriptor_queue,
        std::array{exposure, tex(dilated_reactive_masks), tex(dilated_motion_vectors),
                   tex(frame_info), tex(luma_history[p]), tex(farthest_depth_mip1), current_luma,
                   tex(luma_history[q]), tex(farthest_depth), cb_fsr},
        groups(rw, 8), groups(rh, 8));
    passes[in.sharpen ? PassAccumulateSharpen : PassAccumulate]->Dispatch(
        scheduler, descriptor_queue,
        std::array{exposure, tex(dilated_reactive_masks), tex(dilated_motion_vectors),
                   tex(internal_upscaled[p]), tex(lanczos_lut), tex(farthest_depth_mip1),
                   current_luma, tex(farthest_depth), color, tex(internal_upscaled[q]),
                   tex(output), tex(new_locks), cb_fsr},
        groups(uw, 8), groups(uh, 8));
    if (in.sharpen) {
        passes[PassRcas]->Dispatch(
            scheduler, descriptor_queue,
            std::array{exposure, tex(internal_upscaled[q]), tex(output), cb_fsr, cb_rcas},
            groups(uw, 16), groups(uh, 16));
    }
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                               MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
    });
    frame_parity = q;
}

} // namespace Vulkan

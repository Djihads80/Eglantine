// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/renderer_vulkan/vk_cooperative_hmma_test.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {

using Shader::Backend::SPIRV::COOPERATIVE_HMMA_TEST_LANE_WORDS;

constexpr u32 LANES = GuestWarpSize;
constexpr u32 BUFFER_WORDS = LANES * COOPERATIVE_HMMA_TEST_LANE_WORDS;

/// The half-precision encoding of a small integer. Products and sums of the test's integers stay
/// exact in half precision, so the results compare bit for bit, without any rounding involved.
u16 IntegerToHalf(s32 value) {
    if (value == 0) {
        return 0;
    }
    const u32 magnitude = static_cast<u32>(value < 0 ? -value : value);
    const u32 exponent = static_cast<u32>(std::bit_width(magnitude)) - 1;
    const u32 mantissa = (magnitude << (10 - exponent)) & 0x3ff;
    return static_cast<u16>((value < 0 ? 0x8000U : 0U) | ((exponent + 15) << 10) | mantissa);
}

u32 PackHalves(s32 low, s32 high) {
    return IntegerToHalf(low) | (static_cast<u32>(IntegerToHalf(high)) << 16);
}

struct Matrices {
    std::array<std::array<s32, 16>, 16> a{};
    std::array<std::array<s32, 8>, 16> b{};
    std::array<std::array<s32, 8>, 16> c{};
    std::array<std::array<s32, 8>, 16> d{};
};

Matrices MakeMatrices() {
    // Every element differs from most others, so that any misplaced element changes the results.
    u32 state = 0x2545f491;
    const auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<s32>((state >> 16) % 7) - 3;
    };
    Matrices m;
    for (auto& row : m.a) {
        for (s32& value : row) {
            value = next();
        }
    }
    for (auto& row : m.b) {
        for (s32& value : row) {
            value = next();
        }
    }
    for (auto& row : m.c) {
        for (s32& value : row) {
            value = next();
        }
    }
    for (size_t row = 0; row < 16; ++row) {
        for (size_t column = 0; column < 8; ++column) {
            s32 sum = m.c[row][column];
            for (size_t k = 0; k < 16; ++k) {
                sum += m.a[row][k] * m.b[k][column];
            }
            m.d[row][column] = sum;
        }
    }
    return m;
}

/// Writes one lane's guest registers, in the layout of mma.m16n8k16: with g = lane / 4 and
/// t = lane % 4, a lane holds rows g and g + 8 of A, C and D, and column g of B.
void WriteLane(const Matrices& m, u32 lane, std::span<u32> words) {
    const size_t g = lane >> 2;
    const size_t t = lane & 3;
    const size_t k = t * 2;
    words[0] = PackHalves(m.a[g][k], m.a[g][k + 1]);
    words[1] = PackHalves(m.a[g + 8][k], m.a[g + 8][k + 1]);
    words[2] = PackHalves(m.a[g][k + 8], m.a[g][k + 9]);
    words[3] = PackHalves(m.a[g + 8][k + 8], m.a[g + 8][k + 9]);
    words[4] = PackHalves(m.b[k][g], m.b[k + 1][g]);
    words[5] = PackHalves(m.b[k + 8][g], m.b[k + 9][g]);
    words[6] = PackHalves(m.c[g][k], m.c[g][k + 1]);
    words[7] = PackHalves(m.c[g + 8][k], m.c[g + 8][k + 1]);
    words[8] = 0;
    words[9] = 0;
}

bool SameHalf(u32 got, s32 expected) {
    const u32 want = IntegerToHalf(expected);
    // A sum that cancels to zero may have either sign.
    return got == want || (expected == 0 && (got & 0x7fff) == 0);
}

u32 CountWrongLanes(const Matrices& m, std::span<const u32> words) {
    u32 wrong = 0;
    for (u32 lane = 0; lane < LANES; ++lane) {
        const size_t g = lane >> 2;
        const size_t k = (lane & 3) * 2;
        const auto lane_words = words.subspan(lane * COOPERATIVE_HMMA_TEST_LANE_WORDS);
        const u32 d0 = lane_words[8];
        const u32 d1 = lane_words[9];
        if (!SameHalf(d0 & 0xffff, m.d[g][k]) || !SameHalf(d0 >> 16, m.d[g][k + 1]) ||
            !SameHalf(d1 & 0xffff, m.d[g + 8][k]) || !SameHalf(d1 >> 16, m.d[g + 8][k + 1])) {
            ++wrong;
        }
    }
    return wrong;
}

bool RunTest(const Device& device) {
    const auto& logical = device.GetLogical();
    const std::vector<u32> code =
        Shader::Backend::SPIRV::EmitCooperativeHmmaTest(device.SupportedSpirvVersion());
    const vk::ShaderModule module = logical.CreateShaderModule(VkShaderModuleCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    });
    const VkDescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    };
    const vk::DescriptorSetLayout set_layout =
        logical.CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .bindingCount = 1,
            .pBindings = &binding,
        });
    const VkDescriptorSetLayout set_layout_handle = *set_layout;
    const vk::PipelineLayout layout = logical.CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout_handle,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = nullptr,
    });
    const vk::Pipeline pipeline = logical.CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *module,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        .layout = *layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    });
    const VkDescriptorPoolSize pool_size{
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
    };
    const vk::DescriptorPool pool = logical.CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    });
    const vk::DescriptorSets sets = pool.Allocate(VkDescriptorSetAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = *pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout_handle,
    });
    const VkDescriptorSet set = sets[0];

    const MemoryAllocator allocator{device};
    vk::Buffer buffer = allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = BUFFER_WORDS * sizeof(u32),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        },
        MemoryUsage::Download);

    const Matrices matrices = MakeMatrices();
    std::array<u32, BUFFER_WORDS> words{};
    for (u32 lane = 0; lane < LANES; ++lane) {
        WriteLane(matrices, lane,
                  std::span{words}.subspan(lane * COOPERATIVE_HMMA_TEST_LANE_WORDS,
                                           COOPERATIVE_HMMA_TEST_LANE_WORDS));
    }
    std::memcpy(buffer.Mapped().data(), words.data(), sizeof(words));
    buffer.Flush();

    const VkDescriptorBufferInfo buffer_info{
        .buffer = *buffer,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pImageInfo = nullptr,
        .pBufferInfo = &buffer_info,
        .pTexelBufferView = nullptr,
    };
    logical.UpdateDescriptorSets(write, {});

    // Its own submission: this runs while the renderer is being built, before the scheduler can
    // flush (it needs the query cache) and before anything else submits to the queue.
    const vk::CommandPool command_pool = logical.CreateCommandPool(VkCommandPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = device.GetGraphicsFamily(),
    });
    const vk::CommandBuffers command_buffers = command_pool.Allocate(1);
    const vk::CommandBuffer cmdbuf{command_buffers[0], device.GetDispatchLoader()};
    cmdbuf.Begin(VkCommandBufferBeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
    cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
    cmdbuf.Dispatch(1, 1, 1);
    const VkMemoryBarrier read_results{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                           read_results);
    cmdbuf.End();
    const vk::Fence fence = logical.CreateFence(VkFenceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    });
    const VkCommandBuffer submitted = *cmdbuf;
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &submitted,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    vk::Check(device.GetGraphicsQueue().Submit(submit, *fence));
    vk::Check(fence.Wait());

    buffer.Invalidate();
    std::memcpy(words.data(), buffer.Mapped().data(), sizeof(words));
    const u32 wrong = CountWrongLanes(matrices, words);
    if (wrong != 0) {
        LOG_WARNING(Render_Vulkan,
                    "Cooperative matrices lay out elements differently on this driver ({} of {} "
                    "lanes wrong): guest tensor operations stay in shader emulation",
                    wrong, LANES);
        return false;
    }
    LOG_INFO(Render_Vulkan, "Guest tensor operations run on cooperative matrices");
    return true;
}

} // Anonymous namespace

bool TestCooperativeHmma(const Device& device) {
    try {
        return RunTest(device);
    } catch (const std::exception& exception) {
        LOG_WARNING(Render_Vulkan,
                    "Cooperative matrix self-test failed ({}): guest tensor operations stay in "
                    "shader emulation",
                    exception.what());
        return false;
    }
}

} // namespace Vulkan

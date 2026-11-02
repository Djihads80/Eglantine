// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "video_core/renderer_vulkan/vertex_repack.h"

namespace {
using Regs = Tegra::Engines::Maxwell3D::Regs;
using Size = Regs::VertexAttribute::Size;

Regs MakeLayout() {
    Regs regs{};
    for (auto& attr : regs.vertex_attrib_format) {
        attr.constant.Assign(1);
    }
    regs.vertex_streams[0].stride.Assign(69);
    auto& position = regs.vertex_attrib_format[0];
    position.constant.Assign(0);
    position.size.Assign(Size::Size_R32_G32_B32);
    auto& packed = regs.vertex_attrib_format[1];
    packed.constant.Assign(0);
    packed.offset.Assign(65);
    packed.size.Assign(Size::Size_A2_B10_G10_R10);
    return regs;
}
}

TEST_CASE("Vertex repacking preserves aligned fields and relocates packed tail", "[vertex_repack]") {
    const auto regs = MakeLayout();
    const auto mask = Vulkan::VertexRepackMask(regs);
    REQUIRE(mask == 1);
    REQUIRE(Vulkan::RepackedVertexOffset(mask, 0, 0) == 0);
    REQUIRE(Vulkan::RepackedVertexOffset(mask, 0, 64) == 64);
    REQUIRE(Vulkan::RepackedVertexOffset(mask, 0, 65) == 68);
    REQUIRE(Vulkan::RepackedVertexStride(mask, 0, 69) == 72);
    REQUIRE(Vulkan::RepackedVertexStride(mask, 1, 16) == 16);
}

TEST_CASE("Vertex repacking excludes overlapping and oversized attributes", "[vertex_repack]") {
    auto regs = MakeLayout();
    auto& attr = regs.vertex_attrib_format[2];
    attr.constant.Assign(0);
    attr.offset.Assign(64);
    attr.size.Assign(Size::Size_R32);
    REQUIRE(Vulkan::VertexRepackMask(regs) == 0);
    attr.offset.Assign(68);
    REQUIRE(Vulkan::VertexRepackMask(regs) == 0);
    attr.constant.Assign(1);
    REQUIRE(Vulkan::VertexRepackMask(regs) == 1);
}

TEST_CASE("Vertex repacking ignores constant invalid formats", "[vertex_repack]") {
    auto regs = MakeLayout();
    REQUIRE(Vulkan::VertexRepackMask(regs) == 1);
    regs.vertex_attrib_format[2].constant.Assign(0);
    REQUIRE(Vulkan::VertexRepackMask(regs) == 0);
}

TEST_CASE("Vertex repacking selects bindings independently", "[vertex_repack]") {
    auto regs = MakeLayout();
    regs.vertex_streams[31].stride.Assign(69);
    auto& attr = regs.vertex_attrib_format[2];
    attr = regs.vertex_attrib_format[1];
    attr.buffer.Assign(31);
    REQUIRE(Vulkan::VertexRepackMask(regs) == 0x80000001U);
    regs.vertex_streams[0].stride.Assign(72);
    REQUIRE(Vulkan::VertexRepackMask(regs) == 0x80000000U);
    REQUIRE(Vulkan::RepackedVertexOffset(0x80000000U, 0, 65) == 65);
}

TEST_CASE("Vertex repacking requires the captured tail and stride", "[vertex_repack]") {
    auto regs = MakeLayout();
    regs.vertex_streams[0].stride.Assign(68);
    REQUIRE(Vulkan::VertexRepackMask(regs) == 0);
    regs.vertex_streams[0].stride.Assign(69);
    regs.vertex_attrib_format[1].offset.Assign(64);
    REQUIRE(Vulkan::VertexRepackMask(regs) == 0);
}

TEST_CASE("Static vertex repacking follows the device stride alignment", "[vertex_repack]") {
    REQUIRE(Vulkan::VertexRepackInputSupported(true, false));
    REQUIRE_FALSE(Vulkan::VertexRepackInputSupported(false, false));
    REQUIRE(Vulkan::VertexRepackInputSupported(false, true));
    REQUIRE_FALSE(Vulkan::StaticVertexRepackSelected(nullptr, 1));
    REQUIRE(Vulkan::StaticVertexRepackSelected(nullptr, 4));
    REQUIRE(Vulkan::StaticVertexRepackSelected("1", 1));
    REQUIRE_FALSE(Vulkan::StaticVertexRepackSelected("0", 4));
    REQUIRE(Vulkan::StaticVertexRepackSelected("other", 4));
}

TEST_CASE("Vertex repack planning keeps descriptor offsets aligned", "[vertex_repack]") {
    for (const u32 alignment : {4U, 16U, 64U, 256U, 4096U}) {
        for (const u32 offset : {0U, 1U, 3U, 255U, 4099U}) {
            const auto plan = Vulkan::MakeVertexRepackParameters(offset, 70, alignment, 1U << 20,
                                                                 65535, 1U << 20);
            REQUIRE(plan.has_value());
            REQUIRE(plan->source_base % alignment == 0);
            REQUIRE(plan->source_base + plan->source_bias == offset);
            REQUIRE(plan->vertices == 2);
            REQUIRE(plan->output_size == 144);
            REQUIRE(plan->source_range >= plan->source_bias + 70);
            REQUIRE(plan->source_range % 4 == 0);
            REQUIRE(plan->dispatch_groups == 1);
        }
    }
}

TEST_CASE("Vertex repack planning rejects overflow and device-limit violations", "[vertex_repack]") {
    REQUIRE_FALSE(Vulkan::MakeVertexRepackParameters(0, 0, 256, 1U << 20, 65535, 1U << 20));
    REQUIRE_FALSE(Vulkan::MakeVertexRepackParameters(0, 69, 0, 1U << 20, 65535, 1U << 20));
    REQUIRE_FALSE(Vulkan::MakeVertexRepackParameters(0, 69, 256, 71, 65535, 1U << 20));
    REQUIRE_FALSE(Vulkan::MakeVertexRepackParameters(255, 69, 256, 323, 65535, 1U << 20));
    REQUIRE_FALSE(Vulkan::MakeVertexRepackParameters(0, 69, 256, 1U << 20, 0, 1U << 20));
    REQUIRE_FALSE(
        Vulkan::MakeVertexRepackParameters(0, 0xffffffffU, 256, ~u64{0}, 0xffffffffU, ~u64{0}));
    const auto one = Vulkan::MakeVertexRepackParameters(0, 69, 256, 72, 1, 72);
    REQUIRE(one.has_value());
    REQUIRE(one->source_range == 72);
}

TEST_CASE("Vertex repack planning keeps the source range inside the buffer", "[vertex_repack]") {
    const auto clamped = Vulkan::MakeVertexRepackParameters(0, 69, 256, 1U << 20, 65535, 69);
    REQUIRE(clamped.has_value());
    REQUIRE(clamped->source_range == 69);
    REQUIRE_FALSE(Vulkan::MakeVertexRepackParameters(0, 69, 256, 1U << 20, 65535, 68));
    REQUIRE_FALSE(Vulkan::MakeVertexRepackParameters(300, 69, 256, 1U << 20, 65535, 256));
}

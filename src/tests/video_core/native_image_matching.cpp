// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/image_view_info.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/decoders.h"

namespace {
using namespace VideoCommon;
constexpr GPUVAddr Address = 0x100000;

ImageInfo Surface() {
    ImageInfo info;
    info.type = ImageType::e2D;
    info.format = PixelFormat::D32_FLOAT;
    info.size = {960, 540, 1};
    info.resources = {.levels = 1, .layers = 1};
    info.block = {0, 2, 0};
    info.layer_stride = CalculateLayerStride(info);
    info.maybe_unaligned_layer_stride = CalculateLayerSize(info);
    return info;
}
} // namespace

TEST_CASE("Native single-layer padding does not split an otherwise exact image identity",
          "[video_core][native_image_matching]") {
    auto sampled = Surface();
    auto attachment = sampled;
    attachment.layer_stride += 0x10000;
    attachment.maybe_unaligned_layer_stride = attachment.layer_stride;
    CHECK_FALSE(IsLayerStrideCompatible(attachment, sampled));
    CHECK(IsLayerStrideCompatible(attachment, sampled, true));
    CHECK(CalculateGuestSizeInBytes(attachment) == CalculateGuestSizeInBytes(sampled));
    ImageBase image{attachment, Address, 0x200000};
    CHECK_FALSE(FindSubresource(sampled, image, Address, {}, false, false));
    const auto base =
        FindSubresource(sampled, image, Address, RelaxedOptions::SingleLayerStride, false, false);
    REQUIRE(base);
    CHECK(base->level == 0);
    CHECK(base->layer == 0);
}

TEST_CASE("Single-layer padding relaxation preserves pixel extents and layout checks",
          "[video_core][native_image_matching]") {
    auto existing = Surface();
    existing.layer_stride += 0x10000;
    existing.maybe_unaligned_layer_stride = existing.layer_stride;
    ImageBase image{existing, Address, 0x200000};
    auto candidate = Surface();
    SECTION("A smaller sampled view still requires its exact normalized dimensions") {
        --candidate.size.width;
    }
    SECTION("A larger view cannot overrun the existing image") {
        ++candidate.size.height;
    }
    SECTION("Different sample counts remain incompatible") {
        candidate.num_samples = 4;
    }
    SECTION("Different block heights cannot use padding relaxation") {
        ++candidate.block.height;
    }
    SECTION("Different tile spacing cannot use padding relaxation") {
        ++candidate.tile_width_spacing;
    }
    SECTION("Incompatible depth and colour formats stay separate") {
        candidate.format = PixelFormat::R16G16B16A16_FLOAT;
    }
    CHECK_FALSE(FindSubresource(candidate, image, Address, RelaxedOptions::SingleLayerStride, false,
                                false));
}

TEST_CASE("Arrays volumes and mip chains still require their layer layout",
          "[video_core][native_image_matching]") {
    auto lhs = Surface();
    auto rhs = lhs;
    lhs.layer_stride += 0x10000;
    lhs.maybe_unaligned_layer_stride = lhs.layer_stride;
    SECTION("Array layers") {
        lhs.resources.layers = rhs.resources.layers = 2;
    }
    SECTION("Volume slices") {
        lhs.type = rhs.type = ImageType::e3D;
        lhs.size.depth = rhs.size.depth = 4;
    }
    SECTION("Mip chains") {
        lhs.resources.levels = rhs.resources.levels = 2;
    }
    CHECK_FALSE(IsLayerStrideCompatible(lhs, rhs, true));
}

TEST_CASE("Attachment sub-extents preserve tiled storage and sampled texture dimensions",
          "[video_core][native_image_matching]") {
    auto backing = Surface();
    backing.size = {112, 112, 1};
    backing.layer_stride = CalculateLayerStride(backing);
    backing.maybe_unaligned_layer_stride = CalculateLayerSize(backing);
    ImageBase image{backing, Address, 0x200000};
    auto smaller = backing;
    smaller.size = {97, 97, 1};
    REQUIRE(CanShareAttachmentStorage(backing, smaller));
    CHECK_FALSE(FindSubresource(smaller, image, Address, {}, false, false));
    CHECK(FindSubresource(smaller, image, Address, RelaxedOptions::RenderTarget, false, false));
    CHECK_FALSE(
        FindSubresource(smaller, image, Address + 4, RelaxedOptions::RenderTarget, false, false));

    auto incompatible = smaller;
    incompatible.size.width = 96; // A different tiled row pitch, despite fitting in the host image.
    CHECK_FALSE(CanShareAttachmentStorage(backing, incompatible));
    CHECK_FALSE(
        FindSubresource(incompatible, image, Address, RelaxedOptions::RenderTarget, false, false));
    incompatible = smaller;
    incompatible.size.height = 96; // A different aligned storage height.
    CHECK_FALSE(CanShareAttachmentStorage(backing, incompatible));
    incompatible = smaller;
    incompatible.size.width = 113;
    CHECK_FALSE(
        FindSubresource(incompatible, image, Address, RelaxedOptions::RenderTarget, false, false));
}

TEST_CASE("Growing attachment copies stay inside the old logical source",
          "[video_core][native_image_matching]") {
    for (const auto old_size : {Extent3D{97, 112, 1}, Extent3D{112, 97, 1},
                                Extent3D{97, 97, 1}}) {
        auto old_info = Surface();
        old_info.size = old_size;
        auto new_info = old_info;
        new_info.size = {112, 112, 1};
        REQUIRE(CanShareAttachmentStorage(new_info, old_info));
        for (const u32 scale : {1U, 2U}) {
            const auto copies = MakeShrinkImageCopies(new_info, old_info, {}, scale, 0);
            REQUIRE(copies.size() == 1);
            CHECK(copies[0].extent.width == old_size.width * scale);
            CHECK(copies[0].extent.height == old_size.height * scale);
            CHECK(copies[0].extent.depth == 1);
            CHECK(copies[0].src_offset == Offset3D{0, 0, 0});
            CHECK(copies[0].dst_offset == Offset3D{0, 0, 0});
        }
    }
}

TEST_CASE("Growing attachments merge only within an unchanged guest memory footprint",
          "[video_core][native_image_matching]") {
    auto old_info = Surface();
    old_info.size = {112, 97, 1};
    old_info.layer_stride = CalculateLayerStride(old_info);
    old_info.maybe_unaligned_layer_stride = CalculateLayerSize(old_info);
    auto incoming = old_info;
    incoming.size = {97, 112, 1};
    const auto grown_size = GrowAttachmentExtent(incoming, old_info);
    REQUIRE(grown_size);
    CHECK(grown_size->width == 112);
    CHECK(grown_size->height == 112);
    incoming.size = *grown_size;
    CHECK(CalculateGuestSizeInBytes(incoming) == CalculateGuestSizeInBytes(old_info));
    ImageBase image{old_info, Address, 0x200000};
    const auto merged =
        ResolveOverlap(incoming, Address, 0x200000, image, false, false, false, true);
    REQUIRE(merged);
    CHECK(merged->gpu_addr == Address);
    CHECK(merged->resources.layers == 1);
    CHECK(merged->resources.levels == 1);
    incoming.size.width = 113;
    CHECK_FALSE(GrowAttachmentExtent(incoming, old_info));
}

TEST_CASE("Attachment reuse rejects unsupported layouts and incomplete backing",
          "[video_core][native_image_matching]") {
    auto lhs = Surface();
    auto rhs = lhs;
    SECTION("Array") {
        rhs.resources.layers = 2;
    }
    SECTION("Mip chain") {
        rhs.resources.levels = 2;
    }
    SECTION("MSAA") {
        rhs.num_samples = 4;
    }
    SECTION("Volume") {
        rhs.type = ImageType::e3D;
    }
    SECTION("Sparse") {
        rhs.is_sparse = true;
    }
    SECTION("Block layout") {
        ++rhs.block.height;
    }
    SECTION("Tile spacing") {
        ++rhs.tile_width_spacing;
    }
    SECTION("Format") {
        rhs.format = PixelFormat::R16G16B16A16_FLOAT;
    }
    CHECK_FALSE(CanShareAttachmentStorage(lhs, rhs));
    for (const auto flag : {ImageFlagBits::Unbacked, ImageFlagBits::TrimmedLevels,
                            ImageFlagBits::Rescaled, ImageFlagBits::Sparse, ImageFlagBits::Remapped,
                            ImageFlagBits::SparseLayers, ImageFlagBits::SparseTiles}) {
        ImageBase image{lhs, Address, 0x200000};
        image.flags |= flag;
        CHECK_FALSE(IsAttachmentBackingUsable(image));
    }
}

TEST_CASE("Attachment views retain their logical render area without changing sampled extents",
          "[video_core][native_image_matching]") {
    auto backing = Surface();
    backing.size = {112, 112, 1};
    ImageViewInfo attachment{ImageViewType::e2D, backing.format};
    attachment.render_size = {97, 97};
    ImageViewBase render_view{attachment, backing, ImageId{1}, Address};
    CHECK(render_view.size.width == 97);
    CHECK(render_view.size.height == 97);
    CHECK(True(render_view.flags & ImageViewFlagBits::RenderSubExtent));
    auto other_area = attachment;
    other_area.render_size = {100, 100};
    CHECK(attachment != other_area);
    ImageViewInfo sampled;
    sampled.type = ImageViewType::e2D;
    sampled.format = backing.format;
    REQUIRE_FALSE(sampled.IsRenderTarget());
    ImageViewBase sampled_view{sampled, backing, ImageId{1}, Address};
    CHECK(sampled_view.size.width == 112);
    CHECK(sampled_view.size.height == 112);
    CHECK(False(sampled_view.flags & ImageViewFlagBits::RenderSubExtent));
}

TEST_CASE("Attachment sub-extents address the same native tiled texels as their backing",
          "[video_core][native_image_matching]") {
    std::mt19937 random{0x762};
    for (const auto format :
         {PixelFormat::D32_FLOAT, PixelFormat::R16G16B16A16_FLOAT, PixelFormat::B10G11R11_FLOAT}) {
        for (u32 block_height = 0; block_height <= 5; ++block_height) {
            for (u32 tile_spacing = 0; tile_spacing <= 2; ++tile_spacing) {
                INFO("format=" << static_cast<u32>(format) << " block_height=" << block_height
                               << " tile_spacing=" << tile_spacing);
                auto backing = Surface();
                backing.format = format;
                backing.size = {256, 128, 1};
                backing.block.height = block_height;
                backing.tile_width_spacing = tile_spacing;
                auto cropped = backing;
                cropped.size = {255, 127, 1};
                REQUIRE(CanShareAttachmentStorage(backing, cropped));
                std::vector<u8> guest(CalculateGuestSizeInBytes(backing));
                std::generate(guest.begin(), guest.end(),
                              [&] { return static_cast<u8>(random()); });
                const auto unswizzle = [&](const ImageInfo& info) {
                    std::vector<u8> output(CalculateUnswizzledSizeBytes(info));
                    const auto block = MipBlockSize(info, 0);
                    Tegra::Texture::UnswizzleTexture(
                        output, guest, VideoCore::Surface::BytesPerBlock(format), info.size.width,
                        info.size.height, 1, block.height, block.depth,
                        CalculateLevelStrideAlignment(info, 0), true);
                    return output;
                };
                const auto full = unswizzle(backing);
                const auto subset = unswizzle(cropped);
                const size_t bpp = VideoCore::Surface::BytesPerBlock(format);
                for (u32 y = 0; y < cropped.size.height; ++y) {
                    const auto full_row = full.begin() + y * backing.size.width * bpp;
                    const auto subset_row = subset.begin() + y * cropped.size.width * bpp;
                    REQUIRE(
                        std::equal(subset_row, subset_row + cropped.size.width * bpp, full_row));
                }
            }
        }
    }
}

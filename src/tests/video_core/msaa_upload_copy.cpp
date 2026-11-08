// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/samples_helper.h"

namespace {
VideoCommon::BufferImageCopy Upload(s32 x, s32 y, u32 width, u32 height) {
    return {
        .buffer_offset = 0,
        .buffer_size = 0,
        .buffer_row_length = 0,
        .buffer_image_height = 0,
        .image_subresource = {.base_level = 1, .base_layer = 2, .num_layers = 1},
        .image_offset = {x, y, 0},
        .image_extent = {width, height, 1},
    };
}
} // namespace

TEST_CASE("An MSAA upload is copied from where it landed in the scratch image",
          "[video_core][msaa_upload]") {
    using VideoCommon::MsaaUploadCopy;

    SECTION("a partial upload into a 4x image") {
        const auto copy = MsaaUploadCopy(Upload(64, 32, 128, 16), 4);
        // The scratch image holds the upload where the copy put it, in samples...
        CHECK(copy.src_offset.x == 64);
        CHECK(copy.src_offset.y == 32);
        // ...and the MSAA image takes it at the same place in pixels (2x2 samples per pixel).
        CHECK(copy.dst_offset.x == 32);
        CHECK(copy.dst_offset.y == 16);
        CHECK(copy.extent.width == 64);
        CHECK(copy.extent.height == 8);
        CHECK(copy.src_subresource.base_level == 1);
        CHECK(copy.dst_subresource.base_layer == 2);
    }
    SECTION("2x samples are side by side") {
        const auto copy = MsaaUploadCopy(Upload(8, 6, 16, 4), 2);
        CHECK(copy.src_offset.x == 8);
        CHECK(copy.src_offset.y == 6);
        CHECK(copy.dst_offset.x == 4);
        CHECK(copy.dst_offset.y == 6);
        CHECK(copy.extent.width == 8);
        CHECK(copy.extent.height == 4);
    }
    SECTION("a whole-image upload starts at the origin of both") {
        const auto copy = MsaaUploadCopy(Upload(0, 0, 256, 256), 8);
        CHECK(copy.src_offset.x == 0);
        CHECK(copy.dst_offset.x == 0);
        CHECK(copy.extent.width == 64);
        CHECK(copy.extent.height == 128);
    }
}

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <type_traits>
#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/upload_batch.h"

namespace {
template <typename Handle>
Handle FakeHandle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(value);
    } else {
        return static_cast<Handle>(value);
    }
}
}

TEST_CASE("Upload batches preserve copy dependencies", "[vulkan][upload_batch]") {
    const VkBuffer source{FakeHandle<VkBuffer>(1)};
    const VkBuffer target{FakeHandle<VkBuffer>(2)};
    const VkBuffer other{FakeHandle<VkBuffer>(3)};
    Vulkan::UploadBatch batch;
    const VkBufferCopy first{0, 100, 20};
    const VkBufferCopy overlap{32, 119, 10};
    const VkBufferCopy adjacent{32, 120, 10};

    REQUIRE(batch.CanAdd(source, target, {&first, 1}));
    batch.Add(source, target, {&first, 1});
    REQUIRE_FALSE(batch.CanAdd(source, target, {&overlap, 1}));
    REQUIRE(batch.CanAdd(source, target, {&adjacent, 1}));
    REQUIRE_FALSE(batch.CanAdd(target, other, {&adjacent, 1}));
    REQUIRE_FALSE(batch.CanAdd(other, source, {&adjacent, 1}));
    batch.Add(source, target, {&adjacent, 1});
    REQUIRE(batch.groups.size() == 1);
    REQUIRE(batch.groups[0].count == 2);
}

TEST_CASE("Upload batches stop at their capacity", "[vulkan][upload_batch]") {
    const VkBuffer source{FakeHandle<VkBuffer>(1)};
    const VkBuffer target{FakeHandle<VkBuffer>(2)};
    Vulkan::UploadBatch batch;
    for (std::size_t i = 0; i < Vulkan::UploadBatch::MaxCopies; ++i) {
        const VkBufferCopy copy{0, i * 16, 8};
        REQUIRE(batch.CanAdd(source, target, {&copy, 1}));
        batch.Add(source, target, {&copy, 1});
    }
    const VkBufferCopy extra{0, 4096, 8};
    REQUIRE_FALSE(batch.CanAdd(source, target, {&extra, 1}));
}

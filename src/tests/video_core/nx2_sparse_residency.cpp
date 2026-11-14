// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include "common/alignment.h"
#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/gpu.h"
#include "video_core/renderer_null/null_rasterizer.h"
#include "core/device_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"

namespace {
constexpr u64 KiB = 1024;
constexpr u64 MiB = 1024 * KiB;

struct SparseRegionScope {
    explicit SparseRegionScope(u64 address_) : address{address_} {}
    ~SparseRegionScope() {
        Tegra::Nx2SparseRegionDestroyed(address);
        Tegra::g_nx2_sparse_low.store(low);
        Tegra::g_nx2_sparse_high.store(high);
    }
    u64 address;
    u64 low = Tegra::g_nx2_sparse_low.load();
    u64 high = Tegra::g_nx2_sparse_high.load();
};
} // namespace

TEST_CASE("GPU-only terrain pools become sparse when explicit tiles arrive", "[nx2_memory]") {
    using namespace VideoCommon;
    constexpr u64 address = 0x9000000000ULL;
    constexpr u64 size = 64 * MiB;
    SparseRegionScope scope{address};
    Tegra::Nx2AddSparseSpan(address, size);
    CHECK_FALSE(Tegra::Nx2InSparseRegion(address, size));

    Tegra::Nx2SparseRegionTiled(address, size, 4 * MiB);
    REQUIRE(Tegra::Nx2InSparseRegion(address, size));
    Tegra::Nx2SparseTilesMapped(address, 64 * KiB, 128 * KiB, true);
    const auto ranges = Tegra::Nx2SparseDataRanges(address, size);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges.front() == std::pair<u64, u64>{64 * KiB, 128 * KiB});

    ImageInfo info;
    info.type = ImageType::e2D;
    info.format = PixelFormat::R8_UNORM;
    info.size = {8192, 8192, 1};
    info.resources = {1, 1};
    info.block = {0, 4, 0};
    info.native_tiling = true;
    const auto rects = SparseTileRects(info, ranges, 256, 256, 1);
    REQUIRE_FALSE(rects.empty());
    CHECK(UnswizzledRectsBytes(info, rects) < MiB);

    Tegra::Nx2SparseTilesMapped(address, 64 * KiB, 128 * KiB, false);
    CHECK(Tegra::Nx2SparseDataRanges(address, size).empty());
}

TEST_CASE("Tiled sparse registration preserves existing residency", "[nx2_memory]") {
    constexpr u64 address = 0x9100000000ULL;
    constexpr u64 size = 16 * MiB;
    SparseRegionScope scope{address};
    Tegra::Nx2SparseRegionCreated(address, size, 4 * MiB);
    Tegra::Nx2SparseBlockWritten(address, 2);
    Tegra::Nx2SparseRegionTiled(address, size, 4 * MiB);
    CHECK(Tegra::Nx2SparseBlockIsWritten(address, 2));
    CHECK_FALSE(Tegra::Nx2SparseBlockIsWritten(address, 1));

    Tegra::Nx2SparseTilesMapped(address, 8 * MiB, 64 * KiB, true);
    const auto ranges = Tegra::Nx2SparseDataRanges(address, size);
    Tegra::Nx2SparseRegionTiled(address, size, 4 * MiB);
    CHECK(Tegra::Nx2SparseDataRanges(address, size) == ranges);
}


TEST_CASE("Sparse HDR atlas tracks mapped tiles beyond 4 GiB", "[nx2_memory]") {
    using namespace VideoCommon;
    constexpr GPUVAddr address = 0x9200000000ULL;
    constexpr s32 layer = 67;
    constexpr u32 stride = 0x5560000;
    constexpr u64 layer_offset = u64{layer} * stride;
    ImageInfo info;
    info.type = ImageType::e2D;
    info.format = PixelFormat::BC6H_UFLOAT;
    info.size = {8192, 8192, 1};
    info.resources = {12, 68};
    info.block = {0, 4, 0};
    info.layer_stride = stride;
    info.native_tiling = true;

    constexpr u64 size = 0x16ad80000ULL;
    REQUIRE(CalculateGuestSizeInBytes(info) == size);
    const ImageBase image(info, address, 0);
    const auto base = image.TryFindBase(address + layer_offset);
    REQUIRE(base);
    CHECK(base->layer == layer);
    CHECK_FALSE(image.TryFindBase(address + size));
    CHECK_FALSE(image.TryFindBase(address + size + 4));

    SparseRegionScope scope{address};
    Tegra::Nx2SparseRegionTiled(address, size, 4 * MiB);
    Tegra::Nx2SparseTilesMapped(address, layer_offset, 64 * KiB, true);
    const auto ranges = Tegra::Nx2SparseDataRanges(address, size);
    const auto rects = SparseTileRects(info, ranges, 256, 256, 5);
    REQUIRE_FALSE(rects.empty());
    for (const LevelRect& rect : rects) {
        CHECK(rect.layer == layer);
        CHECK(rect.level == 0);
    }
    CHECK(UnswizzledRectsBytes(info, rects) > 0);
    CHECK(UnswizzledRectsBytes(info, rects) < MiB);
}

TEST_CASE("Sparse BC6H upload batches multiple GOB rows into one copy", "[nx2_memory]") {
    using namespace VideoCommon;
    Core::System system;
    Core::DeviceMemory device_memory;
    Tegra::MaxwellDeviceMemoryManager smmu(device_memory);
    Tegra::MemoryManager memory(system, smmu, 32);
    memory.EnableNativeTiling();

    ImageInfo info;
    info.type = ImageType::e2D;
    info.format = PixelFormat::BC6H_UFLOAT;
    info.size = {128, 128, 1};
    info.resources = {1, 2};
    info.layer_stride = CalculateLayerStride(info);
    info.native_tiling = true;

    const LevelRect rect{.level = 0, .x = 16, .y = 16, .width = 32, .height = 80, .layer = 1};
    const std::span rects{&rect, 1};
    std::vector<u8> staging(UnswizzledRectsBytes(info, rects), 0xff);
    const auto copies = UnswizzleImageRects(memory, 0x100000, info, rects, staging);
    REQUIRE(copies.size() == 1);
    const auto& copy = copies.front();
    CHECK(copy.buffer_offset == 512);
    CHECK(copy.buffer_size == 2560);
    CHECK(copy.buffer_row_length == 32);
    CHECK(copy.buffer_image_height == 80);
    CHECK(copy.image_subresource.base_layer == 1);
    CHECK(copy.image_offset == Offset3D{16, 16, 0});
    CHECK(copy.image_extent == Extent3D{32, 80, 1});
    CHECK(std::ranges::all_of(staging, [](u8 value) { return value == 0; }));
}

TEST_CASE("Sparse compressed uploads match full uploads across layers and mips", "[nx2_memory]") {
    using namespace VideoCommon;
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    Kernel::KProcess process{system.Kernel()};
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(24, 12);
    Tegra::MaxwellDeviceMemoryManager device_memory{system.DeviceMemory()};
    const auto asid = device_memory.RegisterProcess(&process.GetMemory());
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    device_memory.BindInterface(&rasterizer);
    Tegra::MemoryManager memory{system, device_memory, 32};
    memory.BindRasterizer(&rasterizer);
    memory.EnableNativeTiling();
    constexpr GPUVAddr address = 0x100000;
    constexpr DAddr device_address = 0x10000;
    constexpr VAddr virtual_address = 0x10000;
    for (const u32 block_height : {0U, 1U, 2U}) {
        CAPTURE(block_height);
        ImageInfo info;
        info.type = ImageType::e2D;
        info.format = PixelFormat::BC6H_UFLOAT;
        info.size = {128, 128, 1};
        info.resources = {3, 2};
        info.block = {0, block_height, 0};
        info.native_tiling = true;
        info.layer_stride = CalculateLayerStride(info);
        const size_t mapped_size = Common::AlignUp(CalculateGuestSizeInBytes(info), u64{4096});
        std::vector<u8> guest(mapped_size);
        for (size_t i = 0; i < guest.size(); ++i) {
            guest[i] = static_cast<u8>(i * 37 + (i >> 7) + (i >> 13));
        }
        auto* const backing = system.DeviceMemory().GetPointerFromRaw<u8>(0x10000);
        std::ranges::copy(guest, backing);
        for (size_t offset = 0; offset < mapped_size; offset += 4096) {
            table.entries.GetAndFault((virtual_address + offset) >> 12)
                .Store(false, Common::PageType::Memory, 1,
                       reinterpret_cast<uintptr_t>(backing) - virtual_address);
        }
        process.GetMemory().SetCurrentPageTable(process);
        device_memory.Map(device_address, virtual_address, mapped_size, asid);
        SCOPE_EXIT { device_memory.Unmap(device_address, mapped_size); };
        memory.Map(address, device_address, mapped_size, Tegra::PTEKind::PITCH, false);
        std::vector<u8> full(CalculateUnswizzledSizeBytes(info));
        const auto full_copies = UnswizzleImage(memory, address, info, guest, full);
        const std::array rects{
            LevelRect{0, 16, 16, 32, 80, 1},
            LevelRect{1, 16, 0, 32, 32, 0},
            LevelRect{2, 0, 0, 32, 32, 1},
        };
        std::vector<u8> staging(UnswizzledRectsBytes(info, rects));
        const auto copies = UnswizzleImageRects(memory, address, info, rects, staging);
        REQUIRE(copies.size() == rects.size());
        for (size_t i = 0; i < rects.size(); ++i) {
            const auto& rect = rects[i];
            const auto& copy = copies[i];
            const auto& reference = full_copies[rect.level];
            REQUIRE(copy.image_subresource.base_layer == rect.layer);
            const size_t reference_pitch = reference.buffer_row_length / 4 * 16;
            const size_t reference_offset = reference.buffer_offset +
                reference.buffer_size / 2 * rect.layer + rect.y / 4 * reference_pitch +
                rect.x / 4 * 16;
            const size_t pitch = copy.buffer_row_length / 4 * 16;
            for (u32 row = 0; row < rect.height / 4; ++row) {
                const auto actual = std::span{staging}.subspan(copy.buffer_offset + row * pitch,
                                                             rect.width / 4 * 16);
                const auto expected = std::span{full}.subspan(reference_offset + row * reference_pitch,
                                                            actual.size());
                CHECK(std::ranges::equal(actual, expected));
            }
        }
    }
    device_memory.UnregisterProcess(asid);
}

TEST_CASE("Sparse atlas ranges split at layer boundaries", "[nx2_memory]") {
    using namespace VideoCommon;
    ImageInfo info;
    info.type = ImageType::e2D;
    info.format = PixelFormat::BC6H_UFLOAT;
    info.size = {128, 128, 1};
    info.resources = {3, 2};
    info.layer_stride = CalculateLayerStride(info);
    const std::array ranges{std::pair<u64, u64>{info.layer_stride - 512, 1024}};
    const auto rects = SparseTileRects(info, ranges, 64, 64, 1);
    REQUIRE_FALSE(rects.empty());
    CHECK(std::ranges::any_of(rects, [](const auto& rect) { return rect.layer == 0 && rect.level == 2; }));
    CHECK(std::ranges::any_of(rects, [](const auto& rect) { return rect.layer == 1 && rect.level == 0; }));
    CHECK(std::ranges::all_of(rects, [](const auto& rect) { return rect.layer < 2; }));
}

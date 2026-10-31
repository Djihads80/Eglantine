// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "common/lz4_compression.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/nso.h"

namespace {

class ShortReadFile final : public FileSys::VectorVfsFile {
public:
    using VectorVfsFile::VectorVfsFile;

    size_t Read(u8* output, size_t length, size_t offset) const override {
        return VectorVfsFile::Read(output, offset == 0 ? length : length - 1, offset);
    }
};

TEST_CASE("NSO loading rejects invalid segments", "[core][loader][nso]") {
    Core::System system;
    Kernel::KProcess process{system.Kernel()};
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    std::vector<u8> text(16, 0x42);
    bool valid = true;
    bool short_read = false;

    SECTION("Uncompressed segments do not use compressed sizes") {}

    SECTION("Valid LZ4") {
        header.flags = 1;
        text = Common::Compression::CompressDataLZ4(text.data(), text.size());
    }

    SECTION("Invalid LZ4") {
        header.flags = 1;
        text = {0, 0, 0};
        valid = false;
    }

    SECTION("Short segment read") {
        short_read = true;
        valid = false;
    }

    SECTION("LZ4 output is shorter than declared") {
        header.flags = 1;
        text.resize(8);
        text = Common::Compression::CompressDataLZ4(text.data(), text.size());
        valid = false;
    }

    SECTION("Valid ZBIC") {
        header.flags = 0x81;
        text = {0x5a, 0x42, 0x49, 0x43, 0x20, 0x10, 0x81, 0, 0};
        text.resize(25, 0x42);
    }

    SECTION("ZBIC output is shorter than declared") {
        header.flags = 0x81;
        text = {0x5a, 0x42, 0x49, 0x43, 0x20, 0x08, 0x41, 0, 0};
        text.resize(17, 0x42);
        valid = false;
    }

    header.segments[0] = {sizeof(header), 0, 16, {}};
    header.segments[1] = {static_cast<u32>(sizeof(header) + text.size()), 0x1000, 16, {}};
    header.segments[2] = {static_cast<u32>(sizeof(header) + text.size() + 16), 0x2000, 16, {}};
    if (header.IsSegmentCompressed(0)) {
        header.segments_compressed_size[0] = static_cast<u32>(text.size());
    }
    std::vector<u8> bytes(sizeof(header) + text.size() + 32);
    std::memcpy(bytes.data() + sizeof(header), text.data(), text.size());

    SECTION("Truncated segment") {
        bytes.pop_back();
        valid = false;
    }

    SECTION("Segment offset is past EOF") {
        header.segments[0].offset = std::numeric_limits<u32>::max();
        valid = false;
    }

    SECTION("Segment is outside the image") {
        header.segments[0].location = 0x3000;
        valid = false;
    }

    SECTION("Image size overflows") {
        header.segments[2].location = std::numeric_limits<u32>::max();
        valid = false;
    }

    std::memcpy(bytes.data(), &header, sizeof(header));
    const std::unique_ptr<FileSys::VfsFile> file = short_read
        ? std::make_unique<ShortReadFile>(std::move(bytes), "test.nso")
        : std::make_unique<FileSys::VectorVfsFile>(std::move(bytes), "test.nso");
    const auto result = Loader::AppLoader_NSO::LoadModule(process, system, *file, 0x8000000,
                                                        false, false);
    if (valid) {
        REQUIRE(result == 0x8003000ULL);
    } else {
        REQUIRE_FALSE(result.has_value());
    }
}

} // namespace

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/zstd_compression.h"

namespace {

std::vector<u8> Compress(const std::vector<u8>& data) {
    return Common::Compression::CompressDataZSTDDefault(data.data(), data.size());
}

} // namespace

TEST_CASE("ZSTD frames within the size limit decompress", "[common][zstd]") {
    const std::vector<u8> data(100, 0x5a);
    const auto compressed = Compress(data);

    const auto result = Common::Compression::TryDecompressDataZSTD(compressed, data.size());
    REQUIRE(result.has_value());
    REQUIRE(*result == data);
    REQUIRE(Common::Compression::DecompressDataZSTD(compressed) == data);

    const auto empty = Common::Compression::TryDecompressDataZSTD(Compress({}), 0);
    REQUIRE(empty.has_value());
    REQUIRE(empty->empty());
}

TEST_CASE("ZSTD frames over the size limit are rejected", "[common][zstd]") {
    const std::vector<u8> data(100, 0x5a);
    const auto compressed = Compress(data);
    REQUIRE_FALSE(Common::Compression::TryDecompressDataZSTD(compressed, data.size() - 1));
}

TEST_CASE("Invalid ZSTD frames are rejected without throwing", "[common][zstd]") {
    const std::array<u8, 1> one_byte{0x01};
    const std::array<u8, 5> bad_magic{0x01, 0x02, 0x03, 0x04, 0x05};
    REQUIRE_FALSE(Common::Compression::TryDecompressDataZSTD(one_byte, 0x10000));
    REQUIRE_FALSE(Common::Compression::TryDecompressDataZSTD(bad_magic, 0x10000));
    REQUIRE(Common::Compression::DecompressDataZSTD(one_byte).empty());

    // The header declares the size, but the data after it is cut off
    std::vector<u8> truncated = Compress(std::vector<u8>(100, 0x5a));
    truncated.pop_back();
    REQUIRE_FALSE(Common::Compression::TryDecompressDataZSTD(truncated, 0x10000));
}

TEST_CASE("ZSTD frames without a declared size are rejected", "[common][zstd]") {
    // Magic number, a frame header without the content size, then one raw 3-byte last block
    const std::array<u8, 12> frame{0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x00,
                                   0x19, 0x00, 0x00, 'a',  'b',  'c'};
    REQUIRE_FALSE(Common::Compression::TryDecompressDataZSTD(frame, 0x10000));
    REQUIRE(Common::Compression::DecompressDataZSTD(frame).empty());
}

// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "core/hle/service/ro/ro_types.h"

namespace {
template <typename Header>
Header MakeHeader(u32 hash_offset, u32 hash_count) {
    std::array<u8, sizeof(Header)> bytes{};
    const auto put = [&](size_t offset, u32 value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    constexpr size_t metadata = Header::GetSignedAreaOffset();
    put(0, Header::Magic);
    put(metadata + 8, 0x1000);
    put(metadata + 0x10, hash_offset);
    put(metadata + 0x14, hash_count);
    Header header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    return header;
}
}

TEST_CASE("NRR hash metadata follows each signature layout", "[nrr]") {
    using namespace Service::RO;
    const auto classic = MakeHeader<NrrHeader>(0x350, 36);
    const auto native = MakeHeader<NrrHeaderNx2>(0x4d0, 36);
    REQUIRE(classic.IsHashRangeValid(0x1000));
    REQUIRE(native.IsHashRangeValid(0x1000));
    REQUIRE(classic.GetHashesOffset() == 0x350);
    REQUIRE(native.GetHashesOffset() == 0x4d0);
    REQUIRE(native.GetNumHashes() == 36);
}

TEST_CASE("NRR rejects hash ranges before allocating", "[nrr]") {
    using Header = Service::RO::NrrHeaderNx2;
    REQUIRE_FALSE(MakeHeader<Header>(0x4d0, 0xffffffff).IsHashRangeValid(0x1000));
    REQUIRE_FALSE(MakeHeader<Header>(0x4c0, 1).IsHashRangeValid(0x1000));
    REQUIRE_FALSE(MakeHeader<Header>(0x1001, 0).IsHashRangeValid(0x1000));
    REQUIRE_FALSE(MakeHeader<Header>(0xff0, 1).IsHashRangeValid(0x1000));
    REQUIRE_FALSE(MakeHeader<Header>(0x4d0, 1).IsHashRangeValid(0x2000));
    REQUIRE(MakeHeader<Header>(0xfe0, 1).IsHashRangeValid(0x1000));
    REQUIRE(MakeHeader<Header>(0x1000, 0).IsHashRangeValid(0x1000));
}

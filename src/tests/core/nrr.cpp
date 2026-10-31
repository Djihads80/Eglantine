// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <limits>
#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/ro/ro_types.h"

namespace {

template <typename Header>
void CheckNrrLayout(size_t signed_offset, size_t header_size) {
    REQUIRE(sizeof(Header) == header_size);
    REQUIRE(Header::GetSignedAreaOffset() == signed_offset);

    std::array<u8, sizeof(Header)> bytes{};
    const auto put = [&]<typename T>(size_t offset, T value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    put(0, Header::Magic);
    put(signed_offset, u64{0x04005b902517c000});
    put(signed_offset + 8, u32{0x1000});
    put(signed_offset + 0x10, static_cast<u32>(header_size));
    put(signed_offset + 0x14, u32{1});

    const auto read = [&] {
        Header header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        return header;
    };
    REQUIRE(read().GetProgramId() == 0x04005b902517c000ULL);
    REQUIRE(read().GetNrrKind() == Service::RO::NrrKind::User);
    REQUIRE(read().GetNumHashes() == 1U);
    REQUIRE(read().IsValid(0x1000));
    REQUIRE_FALSE(read().IsValid(0x2000));
    REQUIRE_FALSE(read().IsValid(header_size - 1));

    SECTION("Invalid magic") {
        put(0, u32{});
        REQUIRE_FALSE(read().IsValid(0x1000));
    }
    SECTION("Hash table overlaps header") {
        put(signed_offset + 0x10, static_cast<u32>(header_size - 1));
        REQUIRE_FALSE(read().IsValid(0x1000));
    }
    SECTION("Hash table starts beyond request") {
        put(signed_offset + 0x10, u32{0x1001});
        REQUIRE_FALSE(read().IsValid(0x1000));
    }
    SECTION("Hash table ends beyond request") {
        put(signed_offset + 0x10, u32{0xff0});
        REQUIRE_FALSE(read().IsValid(0x1000));
    }
    SECTION("Hash count cannot overflow") {
        put(signed_offset + 0x14, std::numeric_limits<u32>::max());
        REQUIRE_FALSE(read().IsValid(0x1000));
    }
    SECTION("Switch 2 signature bytes are not a valid hash count") {
        put(signed_offset + 0x14, u32{0x331b0c51});
        REQUIRE_FALSE(read().IsValid(0x1000));
    }
    SECTION("Empty table at end of request") {
        put(signed_offset + 0x10, u32{0x1000});
        put(signed_offset + 0x14, u32{});
        REQUIRE(read().IsValid(0x1000));
    }
    SECTION("Exactly fitting hash table") {
        put(signed_offset + 0x10, u32{0xfe0});
        REQUIRE(read().IsValid(0x1000));
    }
}

TEST_CASE("Legacy NRR layout and hash bounds", "[core][ro][nrr]") {
    CheckNrrLayout<Service::RO::NrrHeader>(0x330, 0x350);
}

TEST_CASE("Switch 2 NRR layout and hash bounds", "[core][ro][nrr]") {
    CheckNrrLayout<Service::RO::NrrHeaderNx2>(0x4b0, 0x4d0);
}

} // namespace

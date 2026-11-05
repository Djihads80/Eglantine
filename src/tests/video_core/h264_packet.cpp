// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/host1x/h264_packet.h"

namespace {

using Bytes = std::vector<u8>;

void AppendNal(Bytes& packet, Bytes& expected, size_t length) {
    for (const int shift : {24, 16, 8, 0}) {
        packet.push_back(static_cast<u8>(length >> shift));
    }
    expected.insert(expected.end(), {0, 0, 0, 1});
    Bytes payload(length, 0x55);
    payload[0] = 0x41;
    packet.insert(packet.end(), payload.begin(), payload.end());
    expected.insert(expected.end(), payload.begin(), payload.end());
}

} // namespace

TEST_CASE("H264 length-prefixed streams convert short NALs", "[video_core][h264]") {
    Tegra::Host1x::H264PacketConverter converter;
    Bytes first{0, 0, 0, 2, 0x67, 0x55};
    converter.Convert(first);
    REQUIRE(first == Bytes{0, 0, 0, 1, 0x67, 0x55});

    for (size_t length = 1; length <= 1024; ++length) {
        CAPTURE(length);
        Bytes packet, expected;
        AppendNal(packet, expected, length);
        AppendNal(packet, expected, 17);
        converter.Convert(packet);
        CHECK(packet == expected);
    }
}

TEST_CASE("H264 Annex-B streams keep ambiguous three-byte start codes", "[video_core][h264]") {
    Tegra::Host1x::H264PacketConverter converter;
    const Bytes first{0, 0, 0, 1, 0x67, 0x42, 0, 0x1f};
    auto packet = first;
    converter.Convert(packet);
    REQUIRE(packet == first);

    // Also describes one 321-byte length-prefixed NAL, including a plausible NAL header.
    Bytes ambiguous(325, 0x41);
    ambiguous[0] = 0;
    ambiguous[1] = 0;
    ambiguous[2] = 1;
    const auto expected = ambiguous;
    converter.Convert(ambiguous);
    CHECK(ambiguous == expected);

    const Bytes three_byte{0, 0, 1, 0x41, 0x55};
    packet = three_byte;
    converter.Convert(packet);
    CHECK(packet == three_byte);
}

TEST_CASE("H264 ambiguous first packets do not lock the stream format", "[video_core][h264]") {
    Tegra::Host1x::H264PacketConverter converter;
    Bytes packet(325, 0x41);
    packet[0] = 0;
    packet[1] = 0;
    packet[2] = 1;
    const auto ambiguous = packet;
    converter.Convert(packet);
    REQUIRE(packet == ambiguous);

    packet = {0, 0, 0, 2, 0x67, 0x55};
    converter.Convert(packet);
    REQUIRE(packet == Bytes{0, 0, 0, 1, 0x67, 0x55});
    packet = ambiguous;
    converter.Convert(packet);
    auto expected = ambiguous;
    expected[2] = 0;
    expected[3] = 1;
    CHECK(packet == expected);
}

TEST_CASE("H264 malformed packets are never partially converted", "[video_core][h264]") {
    const std::vector<Bytes> malformed{
        {}, {0}, {0, 0}, {0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0, 0x41}, {0, 0, 0, 8, 0x41},
        {0xff, 0xff, 0xff, 0xff, 0x41},
        {0, 0, 0, 2, 0x41, 0x55, 0},
        {0, 0, 0, 2, 0x41, 0x55, 0, 0},
        {0, 0, 0, 2, 0x41, 0x55, 0, 0, 0},
        {0, 0, 0, 2, 0x41, 0x55, 0, 0, 0, 0},
        {0, 0, 0, 2, 0x41, 0x55, 0, 0, 0, 8, 0x41},
    };
    for (const auto& input : malformed) {
        Tegra::Host1x::H264PacketConverter converter;
        auto packet = input;
        converter.Convert(packet);
        CHECK(packet == input);
        Bytes first{0, 0, 0, 2, 0x67, 0x55};
        converter.Convert(first);
        REQUIRE(first == Bytes{0, 0, 0, 1, 0x67, 0x55});
        packet = input;
        converter.Convert(packet);
        CHECK(packet == input);
    }
}

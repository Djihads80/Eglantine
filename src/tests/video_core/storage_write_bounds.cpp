// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/storage_write_bounds.h"

using namespace Shader::StorageWriteBounds;

TEST_CASE("Physical element rounding includes an explicit nonaligned base bias", "[write_bounds]") {
    const std::array nodes{Node{Op::Constant, 0}};
    const std::array stores{Store{0, 8, 8}};
    const auto result = Evaluate(nodes, stores, {{1, 1, 1}, {1, 1, 1}, true}, 3, 32, true);
    REQUIRE(result);
    CHECK(*result == std::vector<ByteRange>{{0, 11}});
}

TEST_CASE("A constant16-byte store preserves only its aligned-binding location", "[write_bounds]") {
    const std::array nodes{Node{Op::Constant, 0}};
    const std::array stores{Store{0, 16}};
    const Dispatch shape{{19, 2, 1}, {32, 1, 1}, true};
    const auto result = Evaluate(nodes, stores, shape, 250, 8 * 1024 * 1024, true);
    REQUIRE(result);
    REQUIRE(*result == std::vector<ByteRange>{{250, 266}});
}

TEST_CASE("Two affine output arrays cover all actual thread stores without the rest of the binding",
          "[write_bounds]") {
    // Synthetic expression shape observed in OOT c1d394dc24239c40, not guest bytes/data.
    std::vector<Node> nodes{{Op::Workgroup, 0}, {Op::LocalInvocation, 0},
                            {Op::Constant, 32}, {Op::MulU32, 0, 2}, {Op::AddU32, 3, 1},
                            {Op::Constant, 16}, {Op::MulU32, 4, 5}};
    std::vector<Store> stores;
    for (const u32 bias : {0U, 4U, 8U, 12U, 0x2600U, 0x2604U, 0x2608U, 0x260cU}) {
        const u32 value = static_cast<u32>(nodes.size());
        nodes.push_back({Op::Constant, bias});
        nodes.push_back({Op::AddU32, 6, value});
        stores.push_back({value + 1, 4});
    }
    for (const u32 groups : {1U, 3U, 19U}) {
        for (const u32 local : {1U, 7U, 32U}) {
            const auto result = Evaluate(nodes, stores, {{groups, 1, 1}, {local, 1, 1}, true},
                                         64, 8 * 1024 * 1024, true);
            REQUIRE(result);
            for (u32 group = 0; group < groups; ++group) {
                for (u32 lane = 0; lane < local; ++lane) {
                    for (const u32 bias : {0U, 4U, 8U, 12U, 0x2600U, 0x2604U, 0x2608U, 0x260cU}) {
                        const u64 actual = 64 + 16 * (32 * group + lane) + bias;
                        REQUIRE(std::ranges::any_of(*result, [=](ByteRange range) {
                            return range.begin <= actual && range.end >= actual + 4;
                        }));
                    }
                }
            }
            CHECK(result->back().end < 8 * 1024 * 1024);
        }
    }
}

TEST_CASE("Ambiguous stores, indirect dispatch and modular overflow keep full-bound fallback",
          "[write_bounds]") {
    const Dispatch shape{{1, 1, 1}, {32, 1, 1}, true};
    const std::array nodes{Node{Op::Constant, 0}, Node{Op::Unknown},
                          Node{Op::Constant, 0xffffffffU}, Node{Op::Constant, 1},
                          Node{Op::AddU32, 2, 3}, Node{Op::MulU32, 4, 0}};
    CHECK_FALSE(Evaluate(nodes, std::array{Store{0, 4}, Store{1, 4}}, shape, 0, 4096, true));
    CHECK_FALSE(Evaluate(nodes, std::array{Store{0, 4}}, shape, 0, 4096, false));
    CHECK_FALSE(Evaluate(nodes, std::array{Store{0, 4}}, {{1, 1, 1}, {32, 1, 1}, false}, 0, 4096, true));
    CHECK_FALSE(Evaluate(nodes, std::array{Store{5, 4}}, shape, 0, 4096, true));
    CHECK_FALSE(Evaluate(nodes, std::array{Store{0, 16}}, shape, 4090, 4096, true));
    CHECK_FALSE(Evaluate(nodes, std::array{Store{0, 1}}, shape, 0, 4096, true));
    CHECK_FALSE(Evaluate(nodes, std::array{Store{0, 4}}, shape, ~u64{0}, ~u64{0}, true));
    const auto empty = Evaluate(nodes, std::array{Store{0, 4}}, {{0, 1, 1}, {32, 1, 1}, true}, 0, 4096, true);
    REQUIRE(empty);
    CHECK(empty->empty());
}

TEST_CASE("Offset alternatives union without executing predicates or reading guest memory",
          "[write_bounds]") {
    const std::array nodes{Node{Op::Constant, 20}, Node{Op::Constant, 100},
                          Node{Op::SelectUnion, 0, 1}};
    const auto result = Evaluate(nodes, std::array{Store{2, 8}}, {{1, 1, 1}, {1, 1, 1}, true}, 3, 4096, true);
    REQUIRE(result);
    REQUIRE(*result == std::vector<ByteRange>{{23, 111}});
}

TEST_CASE("Shift and multiply wrap or a one-byte binding overrun reject the whole proof",
          "[write_bounds]") {
    const Dispatch shape{{1, 1, 1}, {1, 1, 1}, true};
    const std::array nodes{Node{Op::Constant, 0x80000000U}, Node{Op::Constant, 2},
                          Node{Op::MulU32, 0, 1}, Node{Op::Constant, 1},
                          Node{Op::ShiftLeftU32, 0, 3}, Node{Op::Constant, 4},
                          Node{Op::ShiftLeftU32, 5, 3}};
    CHECK_FALSE(Evaluate(nodes, std::array{Store{2, 4}}, shape, 0, ~u64{0}, true));
    CHECK_FALSE(Evaluate(nodes, std::array{Store{4, 4}}, shape, 0, ~u64{0}, true));
    const auto exact = Evaluate(nodes, std::array{Store{6, 8}}, shape, 3, 19, true);
    REQUIRE(exact);
    REQUIRE(*exact == std::vector<ByteRange>{{11, 19}});
    CHECK_FALSE(Evaluate(nodes, std::array{Store{6, 8}}, shape, 3, 18, true));
}

TEST_CASE("Unsigned bit extraction intervals cover enumerated values across bit boundaries",
          "[write_bounds][write_bounds_bitfield]") {
    for (const u32 first : {0U, 5U, 240U, 0xfff0U, 0xfffffff0U}) {
        for (const u32 count : {1U, 4U, 16U}) {
            for (const u32 offset : {0U, 1U, 4U, 8U, 16U, 31U, 32U}) {
                for (const u32 width : {0U, 1U, 4U, 8U, 16U, 31U, 32U}) {
                    if (width > 32 - offset) continue;
                    const std::array nodes{Node{Op::LocalInvocation, 0},
                        Node{Op::Constant, first}, Node{Op::AddU32, 0, 1},
                        Node{Op::BitFieldExtractU32, 2, (offset << 8) | width}};
                    const std::array stores{Store{3, 4}};
                    const auto ranges = Evaluate(nodes, stores, {{1, 1, 1}, {count, 1, 1}, true},
                                                 0, u64{1} << 33, true);
                    INFO("start=" << first << " count=" << count << " offset=" << offset << " width=" << width);
                    REQUIRE(ranges);
                    REQUIRE(ranges->size() == 1);
                    const u64 mask = (u64{1} << width) - 1;
                    for (u32 lane = 0; lane < count; ++lane) {
                        // Concrete forward execution is the oracle, independent of the
                        // interval implementation's bucket comparison.
                        const u64 extracted = (u64{first + lane} >> offset) & mask;
                        REQUIRE(ranges->front().begin <= extracted);
                        REQUIRE(ranges->front().end >= extracted + 4);
                    }
                }
            }
        }
    }
}

TEST_CASE("Bit extraction crossing a modulo boundary uses the full field interval",
          "[write_bounds][write_bounds_bitfield]") {
    // [15,16] &15 is {15,0}; applying a mask only to endpoints would reverse/miss it.
    const std::array nodes{Node{Op::LocalInvocation, 0}, Node{Op::Constant, 15},
        Node{Op::AddU32, 0, 1}, Node{Op::BitFieldExtractU32, 2, 4}};
    const std::array stores{Store{3, 4}};
    const auto ranges = Evaluate(nodes, stores, {{1, 1, 1}, {2, 1, 1}, true}, 0, 64, true);
    REQUIRE(ranges);
    CHECK(*ranges == std::vector<ByteRange>{{0, 19}});
}

TEST_CASE("Bit extraction cannot hide unknown values or previous integer overflow",
          "[write_bounds][write_bounds_bitfield]") {
    const std::array stores{Store{3, 4}};
    std::array nodes{Node{Op::LocalInvocation, 0}, Node{Op::Constant, 0x80000000U},
        Node{Op::MulU32, 0, 1}, Node{Op::BitFieldExtractU32, 2, 16}};
    CHECK_FALSE(Evaluate(nodes, stores, {{1, 1, 1}, {3, 1, 1}, true}, 0, 65540, true));
    nodes[3].b = 0; // Even width zero must not restore an already unknown expression.
    CHECK_FALSE(Evaluate(nodes, stores, {{1, 1, 1}, {3, 1, 1}, true}, 0, 65540, true));
    nodes[2] = {Op::Unknown};
    CHECK_FALSE(Evaluate(nodes, stores, {{1, 1, 1}, {1, 1, 1}, true}, 0, 65540, true));
}

TEST_CASE("Invalid bit extraction metadata fails closed without undefined shifts",
          "[write_bounds][write_bounds_bitfield]") {
    const std::array stores{Store{1, 4}};
    for (const u32 packed : {33U, (31U << 8) | 2U, (33U << 8), 0x10000U}) {
        const std::array nodes{Node{Op::Constant, 1}, Node{Op::BitFieldExtractU32, 0, packed}};
        CHECK_FALSE(Evaluate(nodes, stores, {{1, 1, 1}, {1, 1, 1}, true}, 0, 4096, true));
    }
}

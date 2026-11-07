// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <random>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "video_core/front_macro_memo.h"
#include "video_core/nx2_page_hash.h"
#include "video_core/nx2_perf.h"

namespace {

using namespace VideoCommon::Nx2Perf;
using VideoCommon::Nx2PageHash::PageHash;
using VideoCommon::Nx2PageHash::PageHashScalar;

constexpr size_t Index(Opt opt) {
    return static_cast<size_t>(opt);
}

std::vector<u8> RandomBytes(size_t size, u64 seed) {
    std::mt19937_64 rng(seed);
    std::vector<u8> bytes(size);
    for (u8& byte : bytes) {
        byte = static_cast<u8>(rng());
    }
    return bytes;
}

} // namespace

TEST_CASE("Nx2Perf spin_handoff is off unless ON names it", "[nx2_perf]") {
    static_assert(Index(Opt::SpinHandoff) == 70);
    REQUIRE(opt_names[Index(Opt::SpinHandoff)] == "spin_handoff");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::SpinHandoff)));
    REQUIRE(IsOff(always_off, Index(Opt::SpinHandoff)));
    REQUIRE(IsOff(ParseOffMask(false, "", "", false), Index(Opt::SpinHandoff)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::SpinHandoff)));
    REQUIRE(ParseOffMask(false, "spin_handoff", "", false) ==
            (always_off & ~Bit(Opt::SpinHandoff)));
    REQUIRE(ParseOffMask(false, "spin_handoff", "spin_handoff", false) == always_off);
}

TEST_CASE("Nx2Perf run_ahead_cow_probe is off unless ON names it", "[nx2_perf]") {
    static_assert(Index(Opt::RunAheadCowProbe) == 72);
    REQUIRE(opt_names[Index(Opt::RunAheadCowProbe)] == "run_ahead_cow_probe");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::RunAheadCowProbe)));
    REQUIRE(IsOff(always_off, Index(Opt::RunAheadCowProbe)));
    REQUIRE(IsOff(ParseOffMask(false, "", "", false), Index(Opt::RunAheadCowProbe)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::RunAheadCowProbe)));
    REQUIRE(ParseOffMask(false, "run_ahead_cow_probe", "", false) ==
            (always_off & ~Bit(Opt::RunAheadCowProbe)));
    REQUIRE(ParseOffMask(false,
                         "frames_ahead,present_one_ahead,run_ahead_ledger,run_ahead_cow_probe",
                         "", false) ==
            (always_off & ~(Bit(Opt::FramesAhead) | Bit(Opt::PresentOneAhead) |
                            Bit(Opt::RunAheadLedger) | Bit(Opt::RunAheadCowProbe))));
    REQUIRE(ParseOffMask(false, "run_ahead_cow_probe", "run_ahead_cow_probe", false) ==
            always_off);
}

TEST_CASE("Nx2Perf sync_free_fetch is off unless ON names it", "[nx2_perf]") {
    static_assert(Index(Opt::SyncFreeFetch) == 73);
    REQUIRE(opt_names[Index(Opt::SyncFreeFetch)] == "sync_free_fetch");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::SyncFreeFetch)));
    REQUIRE(IsOff(always_off, Index(Opt::SyncFreeFetch)));
    REQUIRE(IsOff(ParseOffMask(false, "", "", false), Index(Opt::SyncFreeFetch)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::SyncFreeFetch)));
    REQUIRE(ParseOffMask(false, "sync_free_fetch", "", false) ==
            (always_off & ~Bit(Opt::SyncFreeFetch)));
    // Not live-safe: read once into static bools.
    REQUIRE(!IsOff(live_safe, Index(Opt::SyncFreeFetch)));
}

TEST_CASE("Nx2Perf pending_wake is on by default and off under SAFE", "[nx2_perf]") {
    static_assert(Index(Opt::PendingWake) == 74);
    REQUIRE(opt_names[Index(Opt::PendingWake)] == "pending_wake");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::PendingWake)));
    REQUIRE(!IsOff(always_off, Index(Opt::PendingWake)));
    REQUIRE(!IsOff(ParseOffMask(false, "", "", false), Index(Opt::PendingWake)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::PendingWake)));
    REQUIRE(!IsOff(ParseOffMask(true, "pending_wake", "", false), Index(Opt::PendingWake)));
    REQUIRE(ParseOffMask(false, "", "pending_wake", false) ==
            (always_off | Bit(Opt::PendingWake)));
}

TEST_CASE("Nx2Perf bank_hash_shadow is off unless ON names it", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::BankHashShadow)] == "bank_hash_shadow");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::BankHashShadow)));
    REQUIRE(IsOff(always_off, Index(Opt::BankHashShadow)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::BankHashShadow)));
    REQUIRE(ParseOffMask(false, "bank_hash_shadow", "", false) ==
            (always_off & ~Bit(Opt::BankHashShadow)));
}

TEST_CASE("Nx2Perf front_macro_memo is off unless ON names it", "[nx2_perf]") {
    static_assert(Index(Opt::FrontMacroMemo) == 76);
    REQUIRE(opt_names[Index(Opt::FrontMacroMemo)] == "front_macro_memo");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::FrontMacroMemo)));
    REQUIRE(IsOff(always_off, Index(Opt::FrontMacroMemo)));
    REQUIRE(IsOff(ParseOffMask(false, "", "", false), Index(Opt::FrontMacroMemo)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::FrontMacroMemo)));
    REQUIRE(ParseOffMask(false, "front_macro_memo", "", false) ==
            (always_off & ~Bit(Opt::FrontMacroMemo)));
    // Read once into a static bool: not live-safe.
    REQUIRE(!IsOff(live_safe, Index(Opt::FrontMacroMemo)));
}

namespace {
// One native macro instruction: lane 0 runs `op` on r0 (zero) operands, writes nothing, and
// sets the method to `method` (immediate) and emits the first FIFO load.
std::array<u32, 3> MemoInstruction(u32 op, u32 method, bool end) {
    std::array<u32, 3> bits{};
    const auto field = [&](u32 offset, u32 count, u32 value) {
        for (u32 bit = 0; bit < count; ++bit) {
            bits[(offset + bit) / 32] |= ((value >> bit) & 1) << ((offset + bit) % 32);
        }
    };
    field(0, 1, end);
    field(10, 5, op);
    for (u32 lane = 0; lane < 2; ++lane) {
        field(15 + lane * 36, 5, 24);
        field(20 + lane * 36, 5, 24);
        field(25 + lane * 36, 5, 24);
    }
    field(30, 16, method);
    field(82, 3, 5);
    field(85, 4, 3);
    return {bits[2], bits[1], bits[0]};
}

std::vector<u32> MemoCode(std::initializer_list<std::array<u32, 3>> instructions) {
    std::vector<u32> code;
    for (const auto& instruction : instructions) {
        code.insert(code.end(), instruction.begin(), instruction.end());
    }
    return code;
}
} // namespace

TEST_CASE("front_macro_memo: classification rejects data RAM and MME DMA starts", "[nx2_perf]") {
    using VideoCommon::FrontMacroMemo::Classify;
    constexpr u32 macro_start = 0xe00;
    // Two emits to ordinary methods, then END and its last instruction.
    const auto plain = MemoCode({MemoInstruction(0, 0x200, false), MemoInstruction(0, 0x201, true),
                                 MemoInstruction(0, 0x202, false)});
    CHECK(Classify(plain, 0, macro_start).empty());
    CHECK(Classify(plain, 1, macro_start).empty());
    // A data RAM read (op 30) or write (op 31) in the reachable code.
    for (const u32 op : {30U, 31U}) {
        const auto dram = MemoCode({MemoInstruction(0, 0x200, false),
                                    MemoInstruction(op, 0x201, true),
                                    MemoInstruction(0, 0x202, false)});
        CHECK(!Classify(dram, 0, macro_start).empty());
    }
    // An emit to MME_DMA_ADDRESS_A (0x154) and to MME_DMA_READ_FIFOED (0x158).
    for (const u32 method : {0x154U, 0x158U}) {
        const auto dma = MemoCode({MemoInstruction(0, 0x200, false),
                                   MemoInstruction(0, method, true),
                                   MemoInstruction(0, 0x202, false)});
        CHECK(!Classify(dma, 0, macro_start).empty());
    }
    // A method at or above the macro registers.
    const auto macro = MemoCode({MemoInstruction(0, 0xe10, true), MemoInstruction(0, 0x200, false)});
    CHECK(!Classify(macro, 0, macro_start).empty());
    // Code after the END's last instruction belongs to another macro and is not scanned.
    const auto after_end = MemoCode({MemoInstruction(0, 0x200, true),
                                     MemoInstruction(0, 0x201, false),
                                     MemoInstruction(30, 0x202, false)});
    CHECK(Classify(after_end, 0, macro_start).empty());
    CHECK(!Classify(after_end, 2, macro_start).empty());
}

TEST_CASE("front_macro_memo: a call is stored and found only with the same reads", "[nx2_perf]") {
    using namespace VideoCommon::FrontMacroMemo;
    const auto memo_storage = std::make_unique<Memo>();
    Memo& memo = *memo_storage;
    const auto code = MemoCode({MemoInstruction(0, 0x200, true), MemoInstruction(0, 0x201, false)});
    REQUIRE(memo.Memoizable(code, 7, 0, 0xe00));
    const std::array<u32, 2> parameters{5, 6};
    Recorder recorder;
    const auto touches = [](u32 method, u32 address) { return method == address; };
    recorder.Read(0x300, 42, 0xe00, touches);
    recorder.Emit(0x200, 5, 0xe00);
    recorder.Emit(0x201, 6, 0xe00);
    memo.Store(7, 0, parameters, recorder);
    const auto holds = [](u32 value) {
        return [value](u32 address, u32 read) { return address == 0x300 && read == value; };
    };
    const Entry* hit = memo.Find(7, 0, parameters, holds(42));
    REQUIRE(hit != nullptr);
    CHECK(recorder.Matches(*hit));
    CHECK(memo.Find(7, 0, parameters, holds(43)) == nullptr);
    CHECK(memo.Find(8, 0, parameters, holds(42)) == nullptr);
    const std::array<u32, 2> other{5, 7};
    CHECK(memo.Find(7, 0, other, holds(42)) == nullptr);
    // A read of a register an earlier emit wrote, and an MME DMA emit, are not recorded.
    Recorder stale;
    stale.Emit(0x300, 1, 0xe00);
    stale.Read(0x300, 1, 0xe00, touches);
    CHECK(stale.overflow);
    Recorder dma;
    dma.Emit(0x157, 1, 0xe00);
    CHECK(dma.excluded);
    memo.Store(7, 0, other, dma);
    CHECK(!memo.Memoizable(code, 7, 0, 0xe00));
}

TEST_CASE("Nx2PageHash: every single-bit flip of a 4 KiB page changes the hash", "[nx2_perf]") {
    std::vector<u8> page = RandomBytes(0x1000, 1);
    const u64 base = PageHash(page.data(), page.size());
    REQUIRE(base == PageHashScalar(page.data(), page.size()));
    size_t same = 0;
    for (size_t bit = 0; bit < page.size() * 8; ++bit) {
        page[bit / 8] ^= static_cast<u8>(1U << (bit % 8));
        same += PageHash(page.data(), page.size()) == base;
        page[bit / 8] ^= static_cast<u8>(1U << (bit % 8));
    }
    REQUIRE(same == 0);
    REQUIRE(PageHash(page.data(), page.size()) == base);
}

TEST_CASE("Nx2PageHash: every single-u32 change of a 4 KiB page changes the hash", "[nx2_perf]") {
    std::vector<u8> page = RandomBytes(0x1000, 2);
    const u64 base = PageHash(page.data(), page.size());
    std::mt19937_64 rng(3);
    size_t same = 0;
    for (size_t offset = 0; offset < page.size(); offset += 4) {
        u32 original;
        std::memcpy(&original, page.data() + offset, 4);
        u32 random_xor = static_cast<u32>(rng());
        random_xor += random_xor == 0 ? 1U : 0U;
        for (const u32 changed : {original + 1, original - 1, original ^ 0x80000000U, ~original,
                                  original ^ random_xor, 0U, 0x3F800000U}) {
            if (changed == original) {
                continue;
            }
            std::memcpy(page.data() + offset, &changed, 4);
            same += PageHash(page.data(), page.size()) == base;
            std::memcpy(page.data() + offset, &original, 4);
        }
    }
    REQUIRE(same == 0);
}

TEST_CASE("Nx2PageHash: partial page lengths", "[nx2_perf]") {
    // The bank loop hashes 1..4096-byte chunks (a bank that starts or ends inside a page).
    const std::vector<u8> source = RandomBytes(0x1000 + 64, 4);
    std::vector<size_t> lengths;
    for (size_t length = 1; length <= 160; ++length) {
        lengths.push_back(length);
    }
    for (size_t length = 0x1000 - 100; length <= 0x1000; ++length) {
        lengths.push_back(length);
    }
    for (const size_t length : {size_t{0x200}, size_t{0x7F8}, size_t{0x800}, size_t{0xC04}}) {
        lengths.push_back(length);
    }
    for (const size_t length : lengths) {
        INFO(length);
        for (const size_t misalign : {size_t{0}, size_t{3}, size_t{8}}) {
            std::vector<u8> page(source.begin() + misalign, source.begin() + misalign + length);
            const u64 base = PageHash(page.data(), length);
            REQUIRE(base == PageHashScalar(page.data(), length));
#ifdef ARCHITECTURE_x86_64
            REQUIRE(base == VideoCommon::Nx2PageHash::PageHashSse2(page.data(), length));
            if (Common::g_cpu_caps.avx2) {
                REQUIRE(base == VideoCommon::Nx2PageHash::PageHashAvx2(page.data(), length));
            }
#endif
            if (length <= 160 || length == 0x1000) {
                size_t same = 0;
                for (size_t bit = 0; bit < length * 8; ++bit) {
                    page[bit / 8] ^= static_cast<u8>(1U << (bit % 8));
                    same += PageHash(page.data(), length) == base;
                    page[bit / 8] ^= static_cast<u8>(1U << (bit % 8));
                }
                REQUIRE(same == 0);
            }
        }
    }
    // A zero-padded tail word does not alias a shorter length.
    const std::array<u8, 16> zeros{};
    for (size_t length = 1; length < zeros.size(); ++length) {
        REQUIRE(PageHash(zeros.data(), length) != PageHash(zeros.data(), length + 1));
    }
}

TEST_CASE("Nx2PageHash: 64 KiB hash vs two-stream memcmp (timing only)", "[nx2_perf]") {
    using Clock = std::chrono::steady_clock;
    // ~272 banks of 64 KiB per frame in Kokiri; the shadow is cold when the compare runs.
    constexpr size_t banks = 272;
    constexpr size_t bank_size = 0x10000;
    constexpr size_t page = 0x1000;
    const std::vector<u8> guest = RandomBytes(banks * bank_size, 5);
    const std::vector<u8> shadow = guest;
    std::vector<u8> evict(size_t{128} << 20);
    u64 sink = 0;
    const auto cool_shadow = [&] {
        for (size_t i = 0; i < evict.size(); i += 64) {
            ++evict[i];
        }
        for (size_t i = 0; i < guest.size(); i += 64) {
            sink += guest[i];
        }
    };
    double best_memcmp = 1e30;
    double best_hash = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        cool_shadow();
        auto start = Clock::now();
        for (size_t i = 0; i < banks * bank_size; i += page) {
            sink += std::memcmp(shadow.data() + i, guest.data() + i, page) != 0 ? 1 : 0;
        }
        best_memcmp = (std::min)(
            best_memcmp, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        cool_shadow();
        start = Clock::now();
        for (size_t i = 0; i < banks * bank_size; i += page) {
            sink += PageHash(guest.data() + i, page);
        }
        best_hash = (std::min)(
            best_hash, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    WARN(fmt::format("bank_hash_shadow: {} x 64 KiB: memcmp (cold shadow) {:.3f} ms, PageHash "
                     "{:.3f} ms, {:.2f}x (sink {})",
                     banks, best_memcmp, best_hash, best_memcmp / best_hash, sink & 1));
}

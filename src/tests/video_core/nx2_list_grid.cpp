// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/nx2_list_grid.h"

namespace {

namespace ListGrid = Tegra::Nx2ListGrid;
using Tegra::TuringMacroInterpreter;

// Ocarina of Time's compute macros, as its command stream uploads them (from #1336's tests): the
// macro that stores a dispatch's grid (two instructions), then the one that uploads the QMD with
// that grid.
constexpr std::array<u32, 51> GameCode{
    0x00000087, 0xb9c7c008, 0x399c7f01, 0x00000003, 0x18c00008, 0xb99c7f00,
    0x00000087, 0x190f8008, 0x31917b00, 0x00000003, 0x1c280008, 0xb1907b00,
    0x00000420, 0xb81c0040, 0x0b824300, 0x00540840, 0xb8c40418, 0x318c0300,
    0x30a00003, 0x18c00000, 0x718c0300, 0x6a600107, 0x18c0001b, 0x318c0300,
    0x001801b7, 0x18c00000, 0xf04c5f00, 0x40600003, 0x18c00000, 0x318c0300,
    0x40600003, 0x18c00000, 0x318c0300, 0x20200003, 0x01c00000, 0x302c0300,
    0x60400003, 0x00c00000, 0xf03c5f00, 0x40600003, 0x18c00000, 0x318c0300,
    0x40600003, 0x18c00000, 0x318c0300, 0x00000003, 0x18c00000, 0x318c0301,
    0x00000003, 0x18c00000, 0x318c0300,
};
constexpr u32 StoreGrid = 0;
constexpr u32 UploadQmd = 2;
constexpr ListGrid::GridCells Cells{0x20, 0x21, 0x22};
constexpr u64 KeepId = 5;

u64 Hash(std::span<const u32> code, u32 start) {
    return TuringMacroInterpreter::CodeHash(code, start,
                                            TuringMacroInterpreter::AnalyzeEffects(code, start));
}

ListGrid::DataRam MarkerRam() {
    ListGrid::DataRam ram{};
    for (u32 word = 0; word < ram.size(); ++word) {
        ram[word] = 0x1000 + word;
    }
    for (u32 i = 0; i < 3; ++i) {
        ram[Cells[i]] = ListGrid::Marker(KeepId, i);
    }
    return ram;
}

ListGrid::GridMarker MarkerState() {
    ListGrid::GridMarker state;
    state.cells = Cells;
    state.keep_id = KeepId;
    state.set = true;
    state.Remember({.keep_id = KeepId});
    return state;
}

// One macro instruction, as TuringMacroInterpreter::Decode reads its 96 bits (from #1336's tests).
struct Lane {
    u32 op{};    ///< 0 = add
    u32 dst{24}; ///< 24 = none
    u32 a{24};   ///< 24 = zero, 25 = this lane's immediate
    u32 b{24};
    u32 imm{};
    u32 method{}; ///< Output selectors: 0 none, 1/2 ALU, 5/6 immediates
    u32 emit{};
};

struct Instruction {
    Lane lane0;
    Lane lane1;
    bool end{};
};

void Append(std::vector<u32>& code, const Instruction& instruction) {
    std::array<u32, 3> bits{};
    const auto set = [&bits](u32 low, u32 count, u32 value) {
        for (u32 i = 0; i < count; ++i) {
            if ((value >> i) & 1) {
                bits[(low + i) / 32] |= 1U << ((low + i) % 32);
            }
        }
    };
    set(0, 1, instruction.end ? 1 : 0);
    const std::array<const Lane*, 2> lanes{&instruction.lane0, &instruction.lane1};
    for (u32 lane = 0; lane < 2; ++lane) {
        const Lane& l = *lanes[lane];
        set(10 + lane * 36, 5, l.op);
        set(15 + lane * 36, 5, l.dst);
        set(20 + lane * 36, 5, l.a);
        set(25 + lane * 36, 5, l.b);
        set(30 + lane * 36, 16, l.imm);
        set(82 + lane * 7, 3, l.method);
        set(85 + lane * 7, 4, l.emit);
    }
    code.push_back(bits[2]);
    code.push_back(bits[1]);
    code.push_back(bits[0]);
}

constexpr u32 Imm = 25;
constexpr u32 ReadRegister = 22;
constexpr u32 DataRead = 30;

// r0 = `load` (a data RAM read or a register read of the address), then the second
// instruction with END.
std::vector<u32> ReadThen(u32 load, u32 address, const Lane& second) {
    std::vector<u32> code;
    Append(code, {.lane0 = {.op = load, .dst = 0, .a = Imm, .imm = address}});
    Append(code, {.lane0 = second, .end = true});
    Append(code, {});
    return code;
}

// Emits r0, unchanged, to the method.
Lane MoveTo(u32 method) {
    return {.a = 0, .imm = method, .method = 5, .emit = 1};
}

// A linear upload of one 0x100-byte line to `address`, nothing written yet.
ListGrid::UploadState Upload(u64 address = 0x10000, u32 exec = 1) {
    ListGrid::UploadState upload;
    upload.line_length = 0x100;
    upload.line_count = 1;
    upload.address = address;
    upload.exec = exec;
    return upload;
}

ListGrid::DryRunResult Run(std::span<const u32> code, u32 start, std::span<const u32> parameters,
                           std::array<u32, 3> grid_registers = {}) {
    const auto ram = MarkerRam();
    auto state = MarkerState();
    return ListGrid::DryRun(
        code, start, parameters, ram, Cells, state.Marked(ram),
        [&](u32 address) {
            return address - ListGrid::QmdGridRegister < 3
                       ? grid_registers[address - ListGrid::QmdGridRegister]
                       : 0U;
        },
        [&](u32 address, u32 value) {
            return address - ListGrid::QmdGridRegister < 3 && ListGrid::IsMarker(value) &&
                   state.Find(value) != nullptr;
        });
}

// Mario Kart World's two readers of the words 055743923e135e09 stores in the data RAM, with the
// parameters of a call each (EDEN_NX2_LIST_MACRO_TRACE dumps). 70d6cdec2430674c (method 0xe76)
// moves them into the inline QMD's grid registers.
constexpr std::array<u32, 105> MkwQmdReader{
    0x00000003, 0x18c0000b, 0x3191fb00, 0x00000087, 0x190f8008, 0x31917b00, 0x0078431f, 0x18c00008,
    0xb1907b00, 0x40600003, 0x18c00000, 0x31810066, 0x40600003, 0x18c00000, 0x318c0300, 0x40600003,
    0x18c00000, 0x318c0300, 0x40600003, 0x18c00000, 0x318c0300, 0x40600003, 0x18c00000, 0x318c0300,
    0x40600003, 0x18c00000, 0x318c0300, 0x20200003, 0x01c00000, 0x302c0300, 0x30200003, 0x18c00000,
    0x300c0300, 0x40600003, 0x18c00000, 0x318c0300, 0x40600003, 0x18c00000, 0x318c0300, 0x40600003,
    0x18c00000, 0x318c0300, 0x40600003, 0x18c00000, 0x318c0300, 0x40600003, 0x18c00000, 0x318c0300,
    0x40600003, 0x18c00000, 0x318c0300, 0x20200093, 0x19c78008, 0xf19c7b00, 0x2020009b, 0x19c78009,
    0x719c7b00, 0x40600003, 0x18c00000, 0x318c0300, 0x40600003, 0x18c00000, 0x318c0300, 0x40600003,
    0x18c00000, 0x318c0300, 0x40600003, 0x18c00000, 0x318c0300, 0x40600003, 0x18c00000, 0x318c0300,
    0x40600003, 0x18c00000, 0x318c0300, 0x40600003, 0x18c00000, 0x318c0300, 0x40600003, 0x18c00000,
    0x318c0300, 0x40600003, 0x18c00000, 0x318c0300, 0x40600003, 0x18c00000, 0x318c0300, 0x40600003,
    0x18c00000, 0x318c0300, 0x40600003, 0x18c00000, 0x318c0300, 0x50600003, 0x18c00000, 0x318c0300,
    0x50a00003, 0x18c00000, 0x318c0300, 0x50a00003, 0x18c00000, 0x318c0301, 0x40600003, 0x18c00000,
    0x318c0300
};
constexpr std::array<u32, 53> MkwQmdParameters{
    0x21457cf2, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000060, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x04030000, 0x21457ccc, 0x80000009, 0x34040000,
    0x0040ff30, 0x00010001, 0x00020e19, 0x00000000, 0x00000000, 0x00000000, 0x05650010, 0x44800021,
    0x00000001, 0x00000000, 0x457cf300, 0x00800021, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x7e8f1300, 0x00800022, 0x7e8f2300, 0x00800022, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x4ca5d300, 0x00000011, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0xff40ff10, 0x00000000, 0x00000b07
};
// e2203512be240fe5 (method 0xe7a) uploads them, with buffer addresses, to compute constant
// buffer 0 at +0x4/+0x8/+0xc.
constexpr std::array<u32, 33> MkwUploadReader{
    0x0000068f, 0x980c0000, 0x31c10300, 0x00184183, 0x18c00000, 0xb2100300, 0x60200007, 0x18c003e8,
    0x018c4300, 0x40600003, 0x18c00000, 0x318c0300, 0x00d40107, 0x18c0001b, 0x318c0300, 0x00540840,
    0x58c4001b, 0x718c0300, 0x20200087, 0x19c78008, 0x319c7b00, 0x0040008b, 0x19c78000, 0xb01c5f00,
    0x40600003, 0x18c00000, 0x318c0300, 0x00000003, 0x18c00000, 0x318c0301, 0x00000003, 0x18c00000,
    0x318c0300
};
constexpr std::array<u32, 31> MkwUploadParameters{
    0x00800070, 0x00000021, 0x457cf300, 0x7e8f1300, 0x00000022, 0x00000080, 0x00000000, 0x7e8f2300,
    0x00000022, 0x00000100, 0x00000000, 0x7eb0e000, 0x00000022, 0x00571800, 0x00000000, 0x7fafe000,
    0x00000022, 0x00000bc0, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x7eafa840,
    0x00000022, 0x00001600, 0x00000000, 0x7eafbe40, 0x00000022, 0x00000040, 0x00000000
};

// A call of one of them on the markers, the data RAM otherwise as captured.
ListGrid::DryRunResult RunMkw(std::span<const u32> code, std::span<const u32> parameters) {
    ListGrid::DataRam ram{};
    ram[0x2c] = 1;
    for (u32 i = 0; i < 3; ++i) {
        ram[Cells[i]] = ListGrid::Marker(KeepId, i);
    }
    auto state = MarkerState();
    return ListGrid::DryRun(
        code, 0, parameters, ram, Cells, state.Marked(ram), [](u32) { return 0U; },
        [](u32, u32) { return false; });
}

// The words the back patches in an inline write of `words` to `address`, the front having said
// `markers` (address, marker) land there: offset and list word of each.
std::vector<std::pair<u32, u32>> Patched(u64 address, std::span<const u32> words,
                                         const std::vector<std::pair<u64, u32>>& markers) {
    std::vector<u8> data(words.size_bytes());
    std::memcpy(data.data(), words.data(), data.size());
    const auto patches = ListGrid::PlanPatches(
        address, data, [&markers](u64 word) -> std::optional<std::pair<u64, u32>> {
            for (const auto& [at, marker] : markers) {
                if (at == word) {
                    return std::pair{KeepId, ListGrid::MarkerIndex(marker)};
                }
            }
            return std::nullopt;
        });
    std::vector<std::pair<u32, u32>> patched;
    for (const auto& patch : patches) {
        CHECK(patch.keep_id == KeepId);
        patched.emplace_back(patch.offset, patch.index);
    }
    return patched;
}

} // namespace

TEST_CASE("List grid recognizes the store macro by its hash", "[list_grid]") {
    const u64 hash = Hash(GameCode, StoreGrid);
    CHECK(hash == ListGrid::HleMacros[0].hash);
    CHECK(ListGrid::RecognizeHleMacro(GameCode, StoreGrid, hash) == Cells);
    // Another macro, or the same code under a hash the table doesn't list.
    CHECK_FALSE(ListGrid::RecognizeHleMacro(GameCode, UploadQmd, Hash(GameCode, UploadQmd)));
    CHECK_FALSE(ListGrid::RecognizeHleMacro(GameCode, StoreGrid, hash ^ 1));
}

TEST_CASE("List grid markers name their list and word", "[list_grid]") {
    for (u32 index = 0; index < 3; ++index) {
        const u32 marker = ListGrid::Marker(0x12345, index);
        CHECK(ListGrid::IsMarker(marker));
        CHECK(ListGrid::MarkerIndex(marker) == index);
        CHECK(ListGrid::MarkerKeepBits(marker) == 0x2345);
    }
    // No word 3, no keep id ending in 0, nothing outside the tag.
    CHECK_FALSE(ListGrid::IsMarker(ListGrid::MarkerBase | (3U << 16) | 1));
    CHECK_FALSE(ListGrid::IsMarker(ListGrid::MarkerBase));
    CHECK_FALSE(ListGrid::IsMarker(8));
}

TEST_CASE("List grid guard admits the game's QMD upload", "[list_grid]") {
    // Parameter 0 packs the counts: 3 groups of four words before the grid, 1 after, and the
    // upload's line length. Then the upload's two address words and the QMD words themselves.
    std::vector<u32> parameters{(0x50U << 16) | (1U << 8) | 3U, 0x21, 0x145a9800};
    for (u32 word = 0; word < 16; ++word) {
        parameters.push_back(0xA000 + word);
    }
    const auto result = Run(GameCode, UploadQmd, parameters);
    REQUIRE(result.completed);
    CHECK(result.read_grid);
    // It sets up a linear upload itself before the data.
    CHECK(ListGrid::Admit(result, {}) == ListGrid::Admission::Moves);
}

TEST_CASE("List grid guard admits only unchanged moves to their places", "[list_grid]") {
    using ListGrid::Admission;
    const auto admit = [](const std::vector<u32>& code, const ListGrid::UploadState& upload,
                          std::array<u32, 3> registers = {}) {
        return ListGrid::Admit(Run(code, 0, {}, registers), upload);
    };
    const u32 x = Cells[0];
    const auto to_upload = ReadThen(DataRead, x, MoveTo(ListGrid::DataUploadMethod));
    // Into a linear upload's data, not a block-linear one.
    CHECK(admit(to_upload, Upload()) == Admission::Moves);
    CHECK(admit(to_upload, Upload(0x10000, 0)) == Admission::Resolve);
    // Only where the back can patch it: 4-byte aligned in memory, in an upload of one line (one
    // of more lines is written whole on every data word), before its end.
    CHECK(admit(to_upload, Upload(0x10002)) == Admission::Resolve);
    auto line_end = Upload();
    line_end.line_length = 6;
    CHECK(admit(to_upload, line_end) == Admission::Moves);
    line_end.offset = 4;
    CHECK(admit(to_upload, line_end) == Admission::Resolve);
    auto two_lines = Upload();
    two_lines.line_count = 2;
    CHECK(admit(to_upload, two_lines) == Admission::Resolve);
    auto full = Upload();
    full.offset = full.Size();
    CHECK(admit(to_upload, full) == Admission::Resolve);
    // Into the QMD grid register of its own word only.
    CHECK(admit(ReadThen(DataRead, x, MoveTo(ListGrid::QmdGridRegister)), {}) ==
          Admission::Moves);
    CHECK(admit(ReadThen(DataRead, x, MoveTo(ListGrid::QmdGridRegister + 1)), {}) ==
          Admission::Resolve);
    // Any other method, and any operation on it.
    CHECK(admit(ReadThen(DataRead, x, MoveTo(0x200)), Upload()) == Admission::Resolve);
    Lane add_one = MoveTo(ListGrid::DataUploadMethod);
    add_one.b = Imm;
    add_one.imm = 1;
    CHECK(admit(ReadThen(DataRead, x, add_one), Upload()) == Admission::Resolve);
    // A data RAM word that holds no marker.
    CHECK(admit(ReadThen(DataRead, 0x30, MoveTo(0x200)), {}) == Admission::Unaffected);
    // A marker a QMD grid register holds is followed like one in the data RAM.
    const std::array<u32, 3> registers{ListGrid::Marker(KeepId, 0), 7, 7};
    CHECK(admit(ReadThen(ReadRegister, ListGrid::QmdGridRegister,
                         MoveTo(ListGrid::DataUploadMethod)),
                Upload(), registers) == Admission::Moves);
    CHECK(admit(ReadThen(ReadRegister, ListGrid::QmdGridRegister, MoveTo(0x200)), Upload(),
                registers) == Admission::Resolve);
    // A register the call moved a marker into holds it when the call reads it back.
    std::vector<u32> read_back;
    Append(read_back, {.lane0 = {.op = DataRead, .dst = 0, .a = Imm, .imm = x}});
    Append(read_back, {.lane0 = MoveTo(ListGrid::QmdGridRegister)});
    Append(read_back,
           {.lane0 = {.op = ReadRegister, .dst = 1, .a = Imm, .imm = ListGrid::QmdGridRegister}});
    Lane elsewhere = MoveTo(0x200);
    elsewhere.a = 1;
    Append(read_back, {.lane0 = elsewhere, .end = true});
    Append(read_back, {});
    CHECK(admit(read_back, {}) == Admission::Resolve);
}

TEST_CASE("List grid never leaves a marker when a list can't be read", "[list_grid]") {
    auto ram = MarkerRam();
    auto state = MarkerState();
    u32 reads = 0;
    bool readable = false;
    struct Context {
        u32* reads;
        bool* readable;
    } context{&reads, &readable};
    state.recent[KeepId % state.recent.size()].reader = {
        [](void* self, GPUVAddr, u64, std::span<u32> words) {
            auto& c = *static_cast<Context*>(self);
            ++*c.reads;
            words[0] = 60;
            words[1] = 270;
            words[2] = 1;
            return *c.readable;
        },
        &context};
    ListGrid::ResolveCells(state, ram);
    CHECK(ram[Cells[0]] == 0);
    CHECK(ram[Cells[1]] == 0);
    CHECK(ram[Cells[2]] == 0);
    // Read once per list, then from its words.
    readable = true;
    CHECK(ListGrid::ResolveWord(state, ListGrid::Marker(KeepId, 1)) == 270u);
    CHECK(ListGrid::ResolveWord(state, ListGrid::Marker(KeepId, 2)) == 1u);
    CHECK(reads == 4);
    // A list no longer among the recent ones.
    CHECK_FALSE(ListGrid::ResolveWord(state, ListGrid::Marker(KeepId + 1, 0)));
}

TEST_CASE("List grid resolves a list's markers before its kept slot is reused", "[list_grid]") {
    ListGrid::g_slot_owners = {};
    auto ram = MarkerRam();
    auto state = MarkerState();
    u32 reads = 0;
    struct Context {
        ListGrid::GridMarker* state;
        ListGrid::DataRam* ram;
        u32* reads;
        bool evictable;
        u32 forced;
    } context{&state, &ram, &reads, false, 0};
    state.recent[KeepId % ListGrid::KeptSlots].reader = {
        [](void* self, GPUVAddr, u64, std::span<u32> words) {
            ++*static_cast<Context*>(self)->reads;
            words[0] = 60;
            words[1] = 270;
            words[2] = 1;
            return true;
        },
        &context};
    const auto evict = [](void* self, u64 keep_id) {
        auto& c = *static_cast<Context*>(self);
        if (!c.evictable) {
            return false;
        }
        c.forced += ListGrid::EvictFromRam(*c.state, *c.ram, keep_id);
        return true;
    };
    REQUIRE(ListGrid::ClaimSlot({KeepId, evict, &context}));
    // Another slot: nothing to evict.
    REQUIRE(ListGrid::ClaimSlot({KeepId + 1, evict, &context}));
    CHECK(reads == 0);
    // The slot's list can't be evicted yet: the slot stays its own.
    const u64 next = KeepId + ListGrid::KeptSlots;
    CHECK_FALSE(ListGrid::ClaimSlot({next, evict, &context}));
    CHECK(ListGrid::g_slot_owners[KeepId % ListGrid::KeptSlots].keep_id == KeepId);
    CHECK(ram[Cells[0]] == ListGrid::Marker(KeepId, 0));
    // Evicted: the data RAM gets the real words, read once, never 0 or a marker.
    context.evictable = true;
    REQUIRE(ListGrid::ClaimSlot({next, evict, &context}));
    CHECK(context.forced == 3);
    CHECK(reads == 1);
    CHECK(ram[Cells[0]] == 60);
    CHECK(ram[Cells[1]] == 270);
    CHECK(ram[Cells[2]] == 1);
    CHECK(ListGrid::g_slot_owners[next % ListGrid::KeptSlots].keep_id == next);
    // A data RAM a macro stored over has nothing left to resolve.
    ram = MarkerRam();
    ram[Cells[1]] = 9;
    state.recent[KeepId % ListGrid::KeptSlots].resolved = false;
    CHECK(ListGrid::EvictFromRam(state, ram, KeepId) == 2);
    CHECK(ram[Cells[1]] == 9);
    ListGrid::ReleaseSlots(&context);
    CHECK(ListGrid::g_slot_owners[next % ListGrid::KeptSlots].keep_id == 0);
}

TEST_CASE("List grid patches only the words the front expects", "[list_grid]") {
    const u64 list = 0x10000 + KeepId;
    const std::array<u32, 6> words{1, ListGrid::Marker(list, 0), ListGrid::Marker(list, 2), 2,
                                   ListGrid::Marker(list, 1), 3};
    std::array<u8, sizeof(words)> data{};
    std::memcpy(data.data(), words.data(), sizeof(words));
    // Words 1 and 2 carry the list's markers; word 4 only looks like one.
    const auto expected = [list](u64 address) -> std::optional<std::pair<u64, u32>> {
        switch (address) {
        case 0x1004:
            return std::pair{list, 0U};
        case 0x1008:
            return std::pair{list, 2U};
        case 0x100c:
            return std::pair{list, 1U}; // A word that holds no marker after all
        default:
            return std::nullopt;
        }
    };
    const auto patches = ListGrid::PlanPatches(0x1000, data, expected);
    REQUIRE(patches.size() == 2);
    CHECK(patches[0].offset == 4);
    CHECK(patches[0].keep_id == list);
    CHECK(patches[0].index == 0);
    CHECK(patches[1].offset == 8);
    CHECK(patches[1].index == 2);
    // Only words aligned in memory.
    CHECK(ListGrid::PlanPatches(0x1002, data, expected).empty());
}

TEST_CASE("List grid dispatches a list's markers from its kept words", "[list_grid]") {
    using ListGrid::GridKind;
    const auto marker = [](u64 keep_id, u32 index) { return ListGrid::Marker(keep_id, index); };
    CHECK(ListGrid::QmdGridSource(8, 1, 1).kind == GridKind::Direct);
    const auto indirect = ListGrid::QmdGridSource(marker(7, 0), marker(7, 1), marker(7, 2));
    CHECK(indirect.kind == GridKind::Indirect);
    CHECK(indirect.keep_bits == 7);
    CHECK(ListGrid::QmdGridSource(marker(7, 0), 1, 1).kind == GridKind::ReadBack);
    CHECK(ListGrid::QmdGridSource(marker(7, 0), marker(8, 1), marker(7, 2)).kind ==
          GridKind::ReadBack);
    CHECK(ListGrid::QmdGridSource(marker(7, 1), marker(7, 0), marker(7, 2)).kind ==
          GridKind::ReadBack);
}

TEST_CASE("List grid trusts a QMD marker only where its list patched it", "[list_grid]") {
    using Registered = std::optional<std::pair<u64, u32>>;
    const u32 y = ListGrid::Marker(KeepId, 1);
    // The word the back patched from the list, which its slot still holds.
    CHECK(ListGrid::QmdWordList(y, Registered{{KeepId, 1}}, KeepId) == KeepId);
    // The id wrapped: a list 2^16 later, with the same 16 bits, holds the slot now. The marker
    // matches it, but the word was patched from the old list.
    const u64 wrapped = KeepId + 0x10000;
    CHECK(ListGrid::Marker(wrapped, 1) == y);
    CHECK_FALSE(ListGrid::QmdWordList(y, Registered{{KeepId, 1}}, wrapped));
    // A word written since (unregistered), another word of the list, or another list's bits.
    CHECK_FALSE(ListGrid::QmdWordList(y, std::nullopt, KeepId));
    CHECK_FALSE(ListGrid::QmdWordList(y, Registered{{KeepId, 0}}, KeepId));
    CHECK_FALSE(ListGrid::QmdWordList(y, Registered{{KeepId + 1, 1}}, KeepId + 1));
    CHECK_FALSE(ListGrid::QmdWordList(8, Registered{{KeepId, 1}}, KeepId));
}

TEST_CASE("List grid verify compares a run on the markers with the real run", "[list_grid]") {
    using Emits = std::vector<ListGrid::Emit>;
    const u32 x = ListGrid::Marker(KeepId, 0);
    const auto real_word = [](u32 marker) -> std::optional<u32> {
        return marker == ListGrid::Marker(KeepId, 0) ? std::optional<u32>{60} : std::nullopt;
    };
    const Emits marked{{0x6c, 0x41, false}, {0x6d, x, true}};
    CHECK(ListGrid::MatchesRealRun(marked, Emits{{0x6c, 0x41, false}, {0x6d, 60, false}}, real_word));
    CHECK_FALSE(
        ListGrid::MatchesRealRun(marked, Emits{{0x6c, 0x41, false}, {0x6d, 61, false}}, real_word));
    CHECK_FALSE(ListGrid::MatchesRealRun(marked, Emits{{0x6c, 0x41, false}}, real_word));
    // A marker without a real word never matches.
    const Emits unknown{{0x6d, ListGrid::Marker(KeepId, 1), true}};
    CHECK_FALSE(ListGrid::MatchesRealRun(
        unknown, Emits{{0x6d, ListGrid::Marker(KeepId, 1), false}}, real_word));
    // A clean value that only looks like a marker compares as it is.
    const Emits constant{{0x6d, x, false}};
    CHECK(ListGrid::MatchesRealRun(constant, Emits{{0x6d, x, false}}, real_word));
}

TEST_CASE("List grid admits Mario Kart World's inline QMD reader", "[list_grid]") {
    CHECK(Hash(MkwQmdReader, 0) == 0x70d6cdec2430674c);
    constexpr u64 qmd_address = u64{MkwQmdParameters[0]} << 8;
    const auto check = [](std::span<const u32> parameters) {
        const auto result = RunMkw(MkwQmdReader, parameters);
        REQUIRE(result.completed);
        CHECK(ListGrid::Admit(result, {}) == ListGrid::Admission::Moves);
        std::vector<std::pair<u32, u32>> grid;
        std::array<u32, 64> qmd{};
        for (const auto& emit : result.emits) {
            if (emit.grid) {
                grid.emplace_back(emit.method, emit.value);
            }
            if (emit.method - 0xc8 < qmd.size()) {
                qmd[emit.method - 0xc8] = emit.value;
            }
        }
        // Each marker into the grid register of its word, then the QMD is written (0x107).
        CHECK(grid == std::vector<std::pair<u32, u32>>{{0xd4, ListGrid::Marker(KeepId, 0)},
                                                       {0xd5, ListGrid::Marker(KeepId, 1)},
                                                       {0xd6, ListGrid::Marker(KeepId, 2)}});
        CHECK(result.emits.back().method == 0x107);
        // As KeplerCompute::FrontApply expects them at 0x107: the grid words of the QMD.
        std::vector<std::pair<u64, u32>> markers;
        for (u32 i = 0; i < 3; ++i) {
            if (ListGrid::IsMarker(qmd[12 + i])) {
                markers.emplace_back(qmd_address + (12 + i) * 4, qmd[12 + i]);
            }
        }
        CHECK(Patched(qmd_address, qmd, markers) ==
              std::vector<std::pair<u32, u32>>{{48, 0}, {52, 1}, {56, 2}});
    };
    check(MkwQmdParameters);
    // A buffer address that looks like the list's marker (QMD word 38) stays as it is.
    auto parameters = MkwQmdParameters;
    REQUIRE(parameters[32] == 0x7e8f1300);
    parameters[32] = ListGrid::Marker(KeepId, 1);
    check(parameters);
}

TEST_CASE("List grid admits Mario Kart World's constant buffer reader", "[list_grid]") {
    CHECK(Hash(MkwUploadReader, 0) == 0xe2203512be240fe5);
    constexpr u64 cbuf = 0x21457cf300;
    const auto check = [](std::span<const u32> parameters) {
        const auto result = RunMkw(MkwUploadReader, parameters);
        REQUIRE(result.completed);
        CHECK(ListGrid::Admit(result, {}) == ListGrid::Admission::Moves);
        // Writes 6 to 8 put the markers in constant buffer 0 at +0x4/+0x8/+0xc.
        const auto upload_markers = ListGrid::UploadMarkerWords(result, {});
        CHECK(upload_markers ==
              std::vector<ListGrid::UploadMarker>{{6, cbuf + 0x4, ListGrid::Marker(KeepId, 0)},
                                                  {7, cbuf + 0x8, ListGrid::Marker(KeepId, 1)},
                                                  {8, cbuf + 0xc, ListGrid::Marker(KeepId, 2)}});
        std::vector<std::pair<u64, u32>> markers;
        for (const auto& marker : upload_markers) {
            markers.emplace_back(marker.address, marker.marker);
        }
        std::vector<u32> data;
        for (const auto& emit : result.emits) {
            if (emit.method == ListGrid::DataUploadMethod) {
                data.push_back(emit.value);
            }
        }
        REQUIRE(data.size() == 32);
        CHECK(Patched(cbuf, data, markers) ==
              std::vector<std::pair<u32, u32>>{{4, 0}, {8, 1}, {12, 2}});
    };
    check(MkwUploadParameters);
    // A buffer address that looks like the list's marker (upload word 4) stays as it is.
    auto parameters = MkwUploadParameters;
    REQUIRE(parameters[3] == 0x7e8f1300);
    parameters[3] = ListGrid::Marker(KeepId, 0);
    check(parameters);
}

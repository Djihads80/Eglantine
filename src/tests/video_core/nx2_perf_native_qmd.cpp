// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/native_qmd.h"

namespace {

using Tegra::NativeQmdScheduler;
using Descriptor = NativeQmdScheduler::Descriptor;

constexpr u32 NumQmds = 24;

struct Event {
    u32 kind;
    u64 a;
    u32 b;
    u32 c;
    bool operator==(const Event&) const = default;
};

// One scheduler over its own copy of the QMDs: the schedulers share their state table, so each
// copy lives at its own base and the events are recorded relative to it.
struct Run {
    explicit Run(u64 base_, bool nx2_perf) : base{base_} {
        scheduler.nx2_perf = nx2_perf;
    }

    u64 Qmd(u32 index) const {
        return base + u64{index} * 256;
    }

    void Store(u32 index, const Descriptor& words) {
        Descriptor placed = words;
        // Release destinations and the dependent QMD are addresses: move them to this base.
        for (const u32 slot : {24U, 28U, 52U}) {
            const u64 destination = (u64{placed[slot + 1] & 0xff} << 32) | placed[slot];
            const u64 moved = destination + base;
            placed[slot] = static_cast<u32>(moved);
            placed[slot + 1] = (placed[slot + 1] & ~0xffU) | static_cast<u32>(moved >> 32);
        }
        placed[15] = static_cast<u32>((Qmd(placed[15]) >> 8));
        memory[Qmd(index)] = placed;
    }

    std::string Signal(u32 index, u32 argument) {
        const auto read = [&](u64 address, Descriptor& descriptor) {
            ++reads;
            const auto it = memory.find(address);
            if (it == memory.end()) {
                return false;
            }
            descriptor = it->second;
            return true;
        };
        const auto release = [&](u64 address, u32 payload, u32 flags) {
            events.push_back({1, address - base, payload, flags & ~0xffU});
            return true;
        };
        const auto dispatch = [&](const Descriptor& descriptor) {
            events.push_back({2, descriptor[0], descriptor[12], descriptor[26]});
            return (descriptor[1] & 1) == 0;
        };
        const std::string error{scheduler.Signal(Qmd(index), argument, read, release, dispatch)};
        events.push_back({3, index, argument, static_cast<u32>(error.size())});
        return error;
    }

    u64 base;
    NativeQmdScheduler scheduler;
    std::unordered_map<u64, Descriptor> memory;
    std::vector<Event> events;
    u64 reads{};
};

Descriptor RandomQmd(std::mt19937& rng, u32 index) {
    Descriptor qmd{};
    qmd[0] = index;
    qmd[1] = rng() % 8 == 0 ? 1 : 0; // Dispatch reports failure
    qmd[4] = (rng() % 4 == 0 ? rng() % 3 : 0) << 14;
    qmd[12] = rng() % 4 == 0 ? 0 : 1 + rng() % 64;
    qmd[13] = 1;
    qmd[14] = 1;
    qmd[18] = 0x30 | (64U << 16);
    for (const u32 slot : {24U, 28U, 52U}) {
        if (rng() % 2 == 0) {
            continue;
        }
        const bool add = rng() % 3 == 0;
        qmd[slot] = 0x1000 + (rng() % 64) * 16;
        qmd[slot + 1] = (1U << 23) | (1U << 30) | (add ? 1U << 26 : 0U);
        qmd[slot + 2] = static_cast<u32>(rng());
    }
    if (rng() % 3 == 0) {
        static constexpr std::array<u32, 3> actions{4, 3, 1};
        qmd[16] = 1 | (actions[rng() % actions.size()] << 1);
        qmd[15] = rng() % NumQmds;
    }
    return qmd;
}

} // Anonymous namespace

TEST_CASE("Nx2Perf compute: individual opt-outs select only the named optimizations",
          "[video_core][nx2_perf][compute_path]") {
    using VideoCommon::Nx2Perf::Opt;
    const std::array options{
        std::pair{Opt::QmdScratch, "qmd_scratch"},
        std::pair{Opt::QueryDiagnostics, "query_diagnostics"},
        std::pair{Opt::QueryUploadScratch, "query_upload_scratch"},
    };
    const char* master = std::getenv("EDEN_NX2_PERF_OPT");
    const char* disabled = std::getenv("EDEN_NX2_PERF_OFF");
    const std::string off = std::string{","} + (disabled ? disabled : "") + ",";
    for (const auto& [option, name] : options) {
        CAPTURE(name);
        const bool expected = (!master || master[0] != '0') &&
                              off.find(std::string{","} + name + ",") == std::string::npos;
        REQUIRE(VideoCommon::Nx2Perf::opt_names[static_cast<size_t>(option)] == name);
        REQUIRE(VideoCommon::Nx2Perf::On(option) == expected);
    }
    NativeQmdScheduler scheduler;
    REQUIRE(scheduler.nx2_perf == VideoCommon::Nx2Perf::On(Opt::QmdScratch));
}

TEST_CASE("Nx2Perf compute: uninitialized QMD scratch matches the old scheduler",
          "[video_core][nx2_perf][compute_path]") {
    std::mt19937 rng{0x0400486024DEC000ULL & 0xffffffff};
    for (u32 round = 0; round < 64; ++round) {
        Run old_run{0x100000000ULL + u64{round} * 0x10000000ULL, false};
        Run fast_run{0x800000000ULL + u64{round} * 0x10000000ULL, true};
        for (u32 index = 0; index < NumQmds; ++index) {
            const Descriptor qmd = RandomQmd(rng, index);
            old_run.Store(index, qmd);
            fast_run.Store(index, qmd);
        }
        for (u32 step = 0; step < 400; ++step) {
            const u32 index = rng() % NumQmds;
            if (rng() % 8 == 0) {
                // The guest patches a waiting QMD's release payloads between requests.
                const Descriptor qmd = RandomQmd(rng, index);
                old_run.Store(index, qmd);
                fast_run.Store(index, qmd);
                continue;
            }
            static constexpr std::array<u32, 7> actions{3, 3, 3, 2, 7, 12, 1};
            const u32 count = rng() % 3;
            const u32 argument = actions[rng() % actions.size()] | (count << 14);
            REQUIRE(old_run.Signal(index, argument) == fast_run.Signal(index, argument));
        }
        REQUIRE(old_run.events == fast_run.events);
        REQUIRE(fast_run.reads == old_run.reads);
    }
}

TEST_CASE("Nx2Perf compute: QMD scratch handles the full dependency chain",
          "[video_core][nx2_perf][compute_path]") {
    Run old_run{0x2000000000ULL, false};
    Run fast_run{0x3000000000ULL, true};
    Descriptor descriptor{};
    descriptor[18] = 0x30;
    descriptor[12] = descriptor[13] = descriptor[14] = 1;
    descriptor[16] = 1 | (3U << 1);
    old_run.Store(0, descriptor);
    fast_run.Store(0, descriptor);
    REQUIRE(old_run.Signal(0, 3) == "QMD chain limit");
    REQUIRE(fast_run.Signal(0, 3) == "QMD chain limit");
    REQUIRE(old_run.events == fast_run.events);
    REQUIRE(fast_run.events.size() == 257);
    REQUIRE(fast_run.reads == 256);
    REQUIRE(old_run.reads == fast_run.reads);
}

namespace {

using Address = NativeQmdScheduler::Address;
using Word = NativeQmdScheduler::Word;

constexpr Word Enable = 1U << 23;
constexpr Word OneWord = 1U << 30;
constexpr Word Reduction = 1U << 26;

struct Released {
    Address address;
    Word payload;
    Word flags;
    bool operator==(const Released&) const = default;
};

struct Launch {
    std::string_view error;
    std::vector<Released> released;
};

Descriptor MakeQmd() {
    Descriptor qmd{};
    qmd[18] = 0x30;
    return qmd;
}

void SetRelease(Descriptor& qmd, Word base, Word address, Word flags, Word payload) {
    qmd[base] = address;
    qmd[base + 1] = flags;
    qmd[base + 2] = payload;
}

Launch Schedule(NativeQmdScheduler& scheduler, Address address, const std::map<Address, Descriptor>& memory) {
    Launch launch;
    launch.error = scheduler.Signal(
        address, 3,
        [&](Address qmd_address, Descriptor& descriptor) {
            const auto it = memory.find(qmd_address);
            if (it == memory.end()) return false;
            descriptor = it->second;
            return true;
        },
        [&](Address destination, Word payload, Word flags) {
            launch.released.push_back({destination, payload, flags});
            return true;
        });
    return launch;
}

} // Anonymous namespace

TEST_CASE("An undecodable QMD release slot keeps the other releases and the dependent QMD",
          "[video_core][native_qmd]") {
    // Orbitals (#400): both channels wait on 0x1b18871010, released by slot 0 of this QMD.
    constexpr Address first = 0x1b19635400;
    constexpr Address second = 0x1b19635500;
    auto qmd = MakeQmd();
    SetRelease(qmd, 24, 0x18871010, 0x4088001b, 6);
    SetRelease(qmd, 52, 0x18871020, Enable | OneWord | (1U << 29) | 0x1b, 7);
    qmd[15] = Word(second >> 8);
    qmd[16] = 1 | (1U << 1);
    auto dependent = MakeQmd();
    SetRelease(dependent, 24, 0x18871030, Enable | OneWord | 0x1b, 8);

    NativeQmdScheduler scheduler;
    const auto launch = Schedule(scheduler, first, {{first, qmd}, {second, dependent}});
    REQUIRE(launch.error == "unsupported QMD release format");
    REQUIRE(launch.released == std::vector<Released>{
                                   {0x1b18871010, 6, 0x4088001b},
                                   {0x1b18871030, 8, Enable | OneWord | 0x1b},
                               });
}

TEST_CASE("Timestamped, reduction, signed and interrupting QMD releases are performed",
          "[video_core][native_qmd]") {
    constexpr Address address = 0x4000000000ULL;
    constexpr Word four_words = Enable;
    constexpr Word increment = Enable | OneWord | Reduction | (3U << 20);
    constexpr Word signed_max_trap = Enable | OneWord | Reduction | (1U << 24) | (2U << 20) | (1U << 27);
    auto qmd = MakeQmd();
    SetRelease(qmd, 24, 0x1000, four_words, 1);
    SetRelease(qmd, 28, 0x1010, increment, 2);
    SetRelease(qmd, 52, 0x1020, signed_max_trap, 3);

    NativeQmdScheduler scheduler;
    const auto launch = Schedule(scheduler, address, {{address, qmd}});
    REQUIRE(launch.error.empty());
    REQUIRE(launch.released == std::vector<Released>{
                                   {0x1000, 1, four_words},
                                   {0x1010, 2, increment},
                                   {0x1020, 3, signed_max_trap},
                               });
}

TEST_CASE("Skipped QMD release slots are reported with their raw words", "[video_core][native_qmd]") {
    constexpr Address address = 0x5000000000ULL;
    constexpr Word two_words = Enable | (2U << 30);
    constexpr Word reserved_size = Enable | (3U << 30);
    constexpr Word timestamped_reduction = Enable | Reduction;
    auto qmd = MakeQmd();
    SetRelease(qmd, 24, 0x2000, two_words, 1);
    SetRelease(qmd, 28, 0x2010, reserved_size, 2);
    SetRelease(qmd, 52, 0x2020, timestamped_reduction, 3);

    NativeQmdScheduler scheduler;
    std::vector<std::array<Word, 6>> skipped;
    scheduler.skipped_release = [&](Address qmd_address, std::size_t slot, std::span<const Word, 4> words) {
        skipped.push_back({Word(qmd_address), Word(slot), words[0], words[1], words[2], words[3]});
    };
    const auto launch = Schedule(scheduler, address, {{address, qmd}});
    REQUIRE(launch.error == "unsupported QMD release format");
    REQUIRE(launch.released.empty());
    REQUIRE(skipped == std::vector<std::array<Word, 6>>{
                           {Word(address), 0, 0x2000, two_words, 1, 0},
                           {Word(address), 1, 0x2010, reserved_size, 2, 0},
                           {Word(address), 2, 0x2020, timestamped_reduction, 3, 0},
                       });
}

TEST_CASE("Semaphore reductions follow the documented ops", "[video_core][native_qmd]") {
    using Tegra::ReduceSemaphore;
    REQUIRE(ReduceSemaphore(0, false, 5, 3) == 8);
    REQUIRE(ReduceSemaphore(1, false, 0xffffffff, 1) == 1);
    REQUIRE(ReduceSemaphore(1, true, 0xffffffff, 1) == 0xffffffff);
    REQUIRE(ReduceSemaphore(2, false, 0xffffffff, 1) == 0xffffffff);
    REQUIRE(ReduceSemaphore(2, true, 0xffffffff, 1) == 1);
    REQUIRE(ReduceSemaphore(3, false, 4, 5) == 5);
    REQUIRE(ReduceSemaphore(3, false, 5, 5) == 0);
    REQUIRE(ReduceSemaphore(4, false, 3, 5) == 2);
    REQUIRE(ReduceSemaphore(4, false, 0, 5) == 5);
    REQUIRE(ReduceSemaphore(4, false, 7, 5) == 5);
    REQUIRE(ReduceSemaphore(5, false, 0b1100, 0b1010) == 0b1000);
    REQUIRE(ReduceSemaphore(6, false, 0b1100, 0b1010) == 0b1110);
    REQUIRE(ReduceSemaphore(7, false, 0b1100, 0b1010) == 0b0110);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstdint>
#include <map>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "video_core/native_qmd.h"

namespace {

using Scheduler = Tegra::NativeQmdScheduler;
using Descriptor = Scheduler::Descriptor;

constexpr std::uint64_t AddressOf(std::uint64_t base, std::size_t index) {
    return base + index * 0x100;
}

// Zero grid (nothing to dispatch), a valid release in slot 0 that writes `payload` to `target`.
Descriptor MakeDescriptor(std::uint64_t target, std::uint32_t payload) {
    Descriptor qmd{};
    qmd[18] = 0x30;                      // QMD 3.0
    qmd[24] = static_cast<std::uint32_t>(target);
    qmd[25] = 0x4080'0000u;              // release enabled, address bits 32..39 clear
    qmd[26] = payload;
    return qmd;
}

} // Anonymous namespace

TEST_CASE("Native QMD scheduler keeps releasing past the table bound", "[native-qmd]") {
    Scheduler scheduler;
    std::map<std::uint64_t, Descriptor> memory;
    std::vector<std::uint64_t> releases;
    const auto recycled_before = Scheduler::recycled;

    // Titles bump-allocate a QMD per launch, so the table fills up long before a session ends.
    // Every launch still has to perform its releases: queues park on those semaphores.
    constexpr std::size_t launches = Scheduler::StateLimit * 2;
    constexpr std::uint64_t base = 0x1000'0000;
    for (std::size_t i = 0; i < launches; ++i) {
        const std::uint64_t address = AddressOf(base, i);
        const std::uint64_t target = 0x2000'0000 + i * 4;
        memory[address] = MakeDescriptor(target, static_cast<std::uint32_t>(i));
        const auto error = scheduler.Signal(
            address, 3,
            [&](std::uint64_t read, Descriptor& out) {
                const auto it = memory.find(read);
                if (it == memory.end()) {
                    return false;
                }
                out = it->second;
                return true;
            },
            [&](std::uint64_t destination, std::uint32_t payload) {
                REQUIRE(destination == target);
                REQUIRE(payload == static_cast<std::uint32_t>(i));
                releases.push_back(destination);
                return true;
            });
        REQUIRE(error.empty());
    }
    REQUIRE(releases.size() == launches);
    REQUIRE(Scheduler::recycled > recycled_before);
}

TEST_CASE("Native QMD scheduler never recycles a launch still waiting", "[native-qmd]") {
    Scheduler scheduler;
    std::map<std::uint64_t, Descriptor> memory;
    std::size_t releases = 0;
    const auto read = [&](std::uint64_t address, Descriptor& out) {
        const auto it = memory.find(address);
        if (it == memory.end()) {
            return false;
        }
        out = it->second;
        return true;
    };
    const auto release = [&](std::uint64_t, std::uint32_t) {
        ++releases;
        return true;
    };

    // Two outstanding dependencies keep this QMD from running while the table churns.
    constexpr std::uint64_t base = 0x4000'0000;
    const std::uint64_t waiting = AddressOf(base, 0);
    memory[waiting] = MakeDescriptor(0x3000'0000, 7);
    memory[waiting][4] = 2U << 14;
    REQUIRE(scheduler.Signal(waiting, 3, read, release).empty());

    constexpr std::size_t launches = Scheduler::StateLimit * 2;
    for (std::size_t i = 1; i <= launches; ++i) {
        const std::uint64_t address = AddressOf(base, i);
        memory[address] = MakeDescriptor(0x2000'0000 + i * 4, 1);
        REQUIRE(scheduler.Signal(address, 3, read, release).empty());
    }
    REQUIRE(releases == launches);

    // Its last two decrements arrive after the table recycled everything it could.
    REQUIRE(scheduler.Signal(waiting, 7, read, release).empty());
    REQUIRE(releases == launches);
    REQUIRE(scheduler.Signal(waiting, 7, read, release).empty());
    REQUIRE(releases == launches + 1);
}

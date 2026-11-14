// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "core/file_sys/wonder_fps_patch.h"
#include "core/wonder_dynamic_fps.h"

namespace {
constexpr std::array<u32, 3> Offsets{0x1bbfb0, 0x1bbfcc, 0x1bc04c};
constexpr std::array<u32, 3> Original{0x1aca0d01, 0xb9404281, 0xb9405281};
std::vector<u8> MakeModule() {
    std::vector<u8> module(0x100 + Offsets.back() + 4);
    for (size_t i = 0; i < Offsets.size(); ++i) {
        std::memcpy(module.data() + 0x100 + Offsets[i], &Original[i], 4);
    }
    return module;
}
u32 ReadWord(const std::vector<u8>& module, size_t index) {
    u32 word{};
    std::memcpy(&word, module.data() + 0x100 + Offsets[index], 4);
    return word;
}
} // namespace

TEST_CASE("Wonder FPS patch keeps simulation and presentation together", "[wonder_fps]") {
    constexpr std::array targets{30U, 60U, 90U, 120U, 180U, 240U, 75U, 144U, 165U, 360U, 1U, 1000U};
    constexpr std::array<u32, 12> fps_values{30, 60, 90, 120, 180, 240, 75, 144, 165, 360, 1, 1000};
    for (size_t i = 0; i < targets.size(); ++i) {
        auto module = MakeModule();
        REQUIRE(FileSys::ApplyWonderFpsPatch(module, Settings::WonderSwitch2ProgramId,
                                             FileSys::WonderFpsBuildId, targets[i]));
        // Decode MOVZ W1 immediates, independently checking simulation Hz and NVN interval.
        for (size_t site = 0; site < 3; ++site) {
            CHECK((ReadWord(module, site) & 0xffe0001fU) == 0x52800001U);
        }
        CHECK(((ReadWord(module, 0) >> 5) & 0xffff) == fps_values[i]);
        CHECK(((ReadWord(module, 2) >> 5) & 0xffff) == fps_values[i]);
        CHECK(((ReadWord(module, 1) >> 5) & 0xffff) == (fps_values[i] == 30 ? 2 : 1));
    }
}

TEST_CASE("Wonder FPS patch rejects unknown games builds and altered or truncated code atomically",
          "[wonder_fps]") {
    auto module = MakeModule();
    const auto original = module;
    CHECK_FALSE(FileSys::ApplyWonderFpsPatch(module, u64{0x040074A01BF12000ULL},
                                             FileSys::WonderFpsBuildId, 60U));
    CHECK_FALSE(
        FileSys::ApplyWonderFpsPatch(module, Settings::WonderSwitch2ProgramId, "other build", 60U));
    CHECK(module == original);
    module.back() ^= 1;
    const auto altered = module;
    CHECK_FALSE(FileSys::ApplyWonderFpsPatch(module, Settings::WonderSwitch2ProgramId,
                                             FileSys::WonderFpsBuildId, 60U));
    CHECK(module == altered);
    module = original;
    module.pop_back();
    const auto truncated = module;
    CHECK_FALSE(FileSys::ApplyWonderFpsPatch(module, Settings::WonderSwitch2ProgramId,
                                             FileSys::WonderFpsBuildId, 60U));
    CHECK(module == truncated);
}

TEST_CASE("Wonder FPS selector supersedes standalone 60 FPS mod", "[wonder_fps]") {
    auto module = MakeModule();
    REQUIRE(FileSys::ApplyWonderFpsPatch(module, Settings::WonderSwitch2ProgramId,
                                         FileSys::WonderFpsBuildId, 60U));
    REQUIRE(FileSys::ApplyWonderFpsPatch(module, Settings::WonderSwitch2ProgramId,
                                         FileSys::WonderFpsBuildId, 240U));
    CHECK(((ReadWord(module, 0) >> 5) & 0xffff) == 240);
}

TEST_CASE("Game FPS controls are visible only in their own game's properties", "[wonder_fps]") {
    for (const auto id : {u64{0}, Settings::WonderSwitch2ProgramId, u64{0x040074A01BF12000ULL},
                          u64{0x010015100B514000}}) {
        CHECK_FALSE(Settings::ShowWonderFps(id, true));
        CHECK(Settings::ShowWonderFps(id, false) == (id == Settings::WonderSwitch2ProgramId));
    }
}

TEST_CASE("Wonder custom FPS rejects out-of-range targets without modifying code", "[wonder_fps]") {
    for (const u32 fps : {0U, 1001U, 65536U, 0xffffffffU}) {
        auto module = MakeModule();
        const auto original = module;
        CHECK_FALSE(FileSys::ApplyWonderFpsPatch(module, Settings::WonderSwitch2ProgramId,
                                                 FileSys::WonderFpsBuildId, fps));
        CHECK(module == original);
    }
}

TEST_CASE("Wonder dynamic patch installs timing hooks without changing fixed mode",
          "[wonder_fps]") {
    auto module = MakeModule();
    REQUIRE(FileSys::ApplyWonderFpsPatch(module, Settings::WonderSwitch2ProgramId,
                                         FileSys::WonderFpsBuildId, 120U, true));
    CHECK(ReadWord(module, 0) == (0xd4000001U | (0xf001U << 5)));
    CHECK(ReadWord(module, 2) == (0xd4000001U | (0xf002U << 5)));
    CHECK(((ReadWord(module, 1) >> 5) & 0xffff) == 1);
}

TEST_CASE("Wonder feedback bounds stalls and resets discontinuities", "[wonder_fps]") {
    Core::WonderDynamicFps timing;
    s64 now = 1'000'000'000;
    CHECK(timing.Sample(0, now, 120, 100) == 120);
    const auto dropped = timing.Sample(1, now += 16'666'667, 120, 100);
    CHECK(dropped >= 30);
    CHECK(dropped <= 60);
    CHECK(timing.Sample(1, now + 1, 120, 100) == dropped);
    CHECK(timing.Sample(2, now += 50'000'000, 120, 100) == 30);
    CHECK(timing.Sample(3, now += 500'000'000, 120, 100) == 120);
    CHECK(timing.Sample(8, now += 16'666'667, 120, 100) == 120);
    CHECK(timing.Sample(9, now += 8'333'333, 120, 200) == 120); // changed speed resets
    CHECK(timing.Sample(10, now += 16'666'667, 120, 0) == 120);
    CHECK(timing.Sample(11, now -= 1, 120, 100) == 120);
    CHECK(timing.Sample(12, now += 16'666'667, 120, 100, 0) == 120);
    CHECK(timing.Sample(13, now += 16'666'667, 60, 100) == 60);
}

TEST_CASE("Wonder feedback controls accumulated game time under jitter and fractional rates",
          "[wonder_fps]") {
    for (const bool jitter : {false, true}) {
        Core::WonderDynamicFps timing;
        s64 now{};
        double game{}, wall{}, step = 1.0 / 120;
        timing.Sample(0, now, 120, 100, step);
        for (u64 i = 1; i <= 3000; ++i) {
            const double fps = jitter ? std::array{120.0, 62.0, 95.0, 45.0}[i % 4] : 47.3;
            const s64 elapsed = static_cast<s64>(std::llround(1e9 / fps));
            now += elapsed;
            wall += elapsed / 1e9;
            game += step; // previous frame's independently applied game step
            const u32 rate = timing.Sample(i, now, 120, 100, step);
            REQUIRE(rate >= 30);
            REQUIRE(rate <= 150);
            step = 1.0 / rate;
        }
        CHECK(std::abs(game / wall - 1.0) < 0.001);
    }
}

TEST_CASE("Wonder feedback uses actual readback and recovers from its safety floor",
          "[wonder_fps]") {
    Core::WonderDynamicFps timing;
    s64 now{};
    double game{}, wall{}, step = 1.0 / 120;
    timing.Sample(0, now, 120, 100, step);
    for (u64 i = 1; i <= 2400; ++i) {
        const s64 elapsed = i <= 12 ? 50'000'000 : 16'666'667;
        now += elapsed;
        // Deliberately perturb actual application, not just requested rates.
        const double actual = step * 1.01;
        const auto rate = timing.Sample(i, now, 120, 100, actual);
        if (i > 240) {
            game += actual;
            wall += elapsed / 1e9;
        }
        step = 1.0 / rate;
    }
    CHECK(std::abs(game / wall - 1.0) < 0.001);
}

TEST_CASE("Wonder speed audit uses game scale and exposes lost coverage", "[wonder_fps]") {
    Core::WonderSpeedAudit audit;
    audit.Observe(0, 0, 1.0f);
    for (u64 i = 1; i <= 300; ++i)
        audit.Observe(i, i * 16'666'667, 1.0f);
    CHECK(audit.frames == 300);
    CHECK(std::abs(audit.game_seconds - 5.0) < 0.000001);
    CHECK(std::abs(audit.wall_seconds - 5.0) < 0.000001);
    audit.Observe(300, 5'000'000'101, 1.0f); // repeated query
    CHECK(audit.frames == 300);
    audit.Observe(301, 6'000'000'100, 1.0f); // long stall is explicitly uncovered
    CHECK(audit.discontinuities == 1);
    CHECK(audit.uncovered_seconds > 0.99);
    audit.Observe(303, 6'016'666'767, 1.0f); // missing frame
    CHECK(audit.discontinuities == 2);

    Core::WonderSpeedAudit slow;
    slow.Observe(0, 0, 0.5f);
    for (u64 i = 1; i <= 300; ++i)
        slow.Observe(i, i * 16'666'667, 0.5f);
    CHECK(std::abs(slow.game_seconds / slow.wall_seconds - 0.5) < 0.000001);
}

TEST_CASE("Wonder audit separates slow frames and settled continuous above-30 runs",
          "[wonder_fps]") {
    Core::WonderSpeedAudit audit;
    s64 now{};
    audit.Observe(0, now, 1.0f);
    for (u64 i = 1; i <= 300; ++i)
        audit.Observe(i, now += 16'666'667, 1.0f);
    CHECK(std::abs(audit.above30_wall_seconds - 5.0) < 0.000001);
    CHECK(audit.below30_wall_seconds == 0);
    CHECK(audit.steady30_wall_seconds > 2.99);
    CHECK(audit.steady30_wall_seconds < 3.02);
    const double before = audit.steady30_wall_seconds;
    audit.Observe(301, now += 50'000'000, 1.0f);
    CHECK(std::abs(audit.below30_wall_seconds - 0.05) < 0.000001);
    for (u64 i = 302; i < 400; ++i)
        audit.Observe(i, now += 16'666'667, 1.0f);
    CHECK(audit.steady30_wall_seconds == before); // settling after the dip is excluded
    audit.Observe(400, now += 500'000'000, 1.0f);
    CHECK(audit.discontinuities == 1);
}

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cmath>
#include <set>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "video_core/ocarina_upscale.h"

namespace {
namespace Ocarina = VideoCommon::OcarinaUpscale;

// The scene camera's projection in the game (rows), from a frame dump.
constexpr std::array<f32, 16> Projection{
    1.206f, 0.0f, 0.0f, 0.0f, 0.0f, 2.1445f, 0.0f, 0.0f,
    0.0f, 0.0f, -1.00001f, -0.2f, 0.0f, 0.0f, -1.0f, 0.0f,
};

/// A view looking along -z turned by yaw radians about y and moved to eye.
Ocarina::Matrix View(f64 yaw, std::array<f64, 3> eye) {
    const f64 c = std::cos(yaw);
    const f64 s = std::sin(yaw);
    // Rotation transposed, then the eye moved to the origin.
    const Ocarina::Matrix rotation{c, 0.0, -s, 0.0, 0.0, 1.0, 0.0, 0.0,
                                   s, 0.0, c,  0.0, 0.0, 0.0, 0.0, 1.0};
    const Ocarina::Matrix translation{1.0, 0.0, 0.0, -eye[0], 0.0, 1.0, 0.0, -eye[1],
                                      0.0, 0.0, 1.0, -eye[2], 0.0, 0.0, 0.0, 1.0};
    return Ocarina::Multiply(rotation, translation);
}

std::array<f32, 16> ViewProjection(f64 yaw, std::array<f64, 3> eye) {
    Ocarina::Matrix projection;
    for (size_t i = 0; i < 16; ++i) {
        projection[i] = Projection[i];
    }
    const Ocarina::Matrix product = Ocarina::Multiply(projection, View(yaw, eye));
    std::array<f32, 16> result;
    for (size_t i = 0; i < 16; ++i) {
        result[i] = static_cast<f32>(product[i]);
    }
    return result;
}

/// (pixel x, pixel y, device depth) of a world point.
std::array<f64, 3> Project(const Ocarina::Matrix& screen, std::array<f64, 3> point) {
    std::array<f64, 4> h{};
    for (size_t row = 0; row < 4; ++row) {
        h[row] = screen[row * 4 + 0] * point[0] + screen[row * 4 + 1] * point[1] +
                 screen[row * 4 + 2] * point[2] + screen[row * 4 + 3];
    }
    return {h[0] / h[3], h[1] / h[3], h[2] / h[3]};
}

/// What the motion vector shader computes: the offset to the previous frame's pixel.
std::array<f64, 2> Motion(const std::array<f32, 16>& columns, f64 x, f64 y, f64 depth) {
    std::array<f64, 4> h{};
    const std::array<f64, 4> v{x, y, depth, 1.0};
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            h[row] += columns[column * 4 + row] * v[column];
        }
    }
    return {h[0] / h[3] - x, h[1] / h[3] - y};
}

const Ocarina::Matrix Viewport = Ocarina::ViewportMatrix(0.0f, 0.0f, 1920.0f, 1080.0f, 1.0f,
                                                         0.5f, 0.5f);
} // Anonymous namespace

TEST_CASE("Ocarina upscale recognizes the scene camera", "[ocarina_upscale]") {
    REQUIRE(Ocarina::IsPerspective(Projection));
    std::array<f32, 16> orthographic{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, -0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    REQUIRE_FALSE(Ocarina::IsPerspective(orthographic));
    std::array<f32, 16> broken = Projection;
    broken[0] = std::nanf("");
    REQUIRE_FALSE(Ocarina::IsPerspective(broken));
    REQUIRE_FALSE(Ocarina::IsPerspective({}));
}

TEST_CASE("Ocarina upscale matches input and output passes across modes", "[ocarina_upscale]") {
    REQUIRE(Ocarina::IsInputPass(Ocarina::InputPassKey, 0));
    REQUIRE(Ocarina::IsInputPass(0, Ocarina::InputPassHash));
    REQUIRE(Ocarina::IsOutputPass(0, Ocarina::OutputPassHash));
    REQUIRE(Ocarina::IsOutputPass(Ocarina::OutputPassUpscaleKey, Ocarina::OutputPassUpscaleHash));
    REQUIRE(Ocarina::IsOutputPass(Ocarina::OutputPassDockedKey, Ocarina::OutputPassDockedHash));
    REQUIRE_FALSE(Ocarina::IsOutputPass(Ocarina::InputPassKey, Ocarina::InputPassHash));
}

TEST_CASE("Ocarina upscale jitter covers the pixel", "[ocarina_upscale]") {
    std::set<std::pair<f32, f32>> offsets;
    f32 sum_x = 0.0f;
    f32 sum_y = 0.0f;
    for (u32 frame = 0; frame < Ocarina::JitterPhases; ++frame) {
        const auto jitter = Ocarina::Jitter(frame);
        REQUIRE(jitter[0] >= -0.5f);
        REQUIRE(jitter[0] < 0.5f);
        REQUIRE(jitter[1] >= -0.5f);
        REQUIRE(jitter[1] < 0.5f);
        offsets.emplace(jitter[0], jitter[1]);
        sum_x += jitter[0];
        sum_y += jitter[1];
    }
    REQUIRE(offsets.size() == Ocarina::JitterPhases);
    REQUIRE(std::abs(sum_x / Ocarina::JitterPhases) < 0.05f);
    REQUIRE(std::abs(sum_y / Ocarina::JitterPhases) < 0.05f);
    REQUIRE(Ocarina::Jitter(3) == Ocarina::Jitter(3 + Ocarina::JitterPhases));
}

TEST_CASE("Ocarina upscale motion is zero for a still camera", "[ocarina_upscale]") {
    const auto screen = Ocarina::ScreenMatrix(Viewport, ViewProjection(0.3, {1.0, 2.0, 3.0}));
    const auto reprojection = Ocarina::Reprojection(screen, screen);
    REQUIRE(reprojection);
    for (const auto& [x, y, depth] : {std::array<f64, 3>{0.5, 0.5, 0.2},
                                      std::array<f64, 3>{960.5, 540.5, 0.999},
                                      std::array<f64, 3>{1919.5, 1079.5, 0.9999}}) {
        const auto motion = Motion(*reprojection, x, y, depth);
        REQUIRE(std::abs(motion[0]) < 1e-2);
        REQUIRE(std::abs(motion[1]) < 1e-2);
    }
}

TEST_CASE("Ocarina upscale motion follows the camera", "[ocarina_upscale]") {
    const auto previous = Ocarina::ScreenMatrix(Viewport, ViewProjection(0.30, {1.0, 2.0, 3.0}));
    const auto current = Ocarina::ScreenMatrix(Viewport, ViewProjection(0.32, {1.4, 2.0, 2.5}));
    const auto reprojection = Ocarina::Reprojection(previous, current);
    REQUIRE(reprojection);
    // Points in front of both cameras, near and far.
    for (const auto& point : {std::array<f64, 3>{-2.0, 1.0, -10.0},
                              std::array<f64, 3>{-30.0, -5.0, -200.0},
                              std::array<f64, 3>{3.0, 2.5, -4.0}}) {
        const auto now = Project(current, point);
        const auto before = Project(previous, point);
        const auto motion = Motion(*reprojection, now[0], now[1], now[2]);
        // f32 matrices and a far depth near 1: within a tenth of a pixel.
        REQUIRE(std::abs(now[0] + motion[0] - before[0]) < 0.1);
        REQUIRE(std::abs(now[1] + motion[1] - before[1]) < 0.1);
    }
}

TEST_CASE("Ocarina upscale rejects a singular camera", "[ocarina_upscale]") {
    const auto screen = Ocarina::ScreenMatrix(Viewport, ViewProjection(0.0, {0.0, 0.0, 0.0}));
    REQUIRE_FALSE(Ocarina::Reprojection(screen, Ocarina::Matrix{}));
    REQUIRE(Ocarina::Invert(Ocarina::Multiply(screen, *Ocarina::Invert(screen))));
}

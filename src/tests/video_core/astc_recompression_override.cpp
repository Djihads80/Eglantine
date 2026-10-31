// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"

namespace {

struct RecompressionRestore {
    Settings::AstcRecompression configured = Settings::values.astc_recompression.GetValue();
    ~RecompressionRestore() {
        Settings::values.astc_recompression.SetValue(configured);
        Settings::SetAstcRecompressionOverride(std::nullopt);
    }
};

} // Anonymous namespace

TEST_CASE("ASTC recompression override wins until cleared", "[video_core][astc]") {
    RecompressionRestore restore;
    Settings::values.astc_recompression.SetValue(Settings::AstcRecompression::Uncompressed);
    Settings::SetAstcRecompressionOverride(std::nullopt);
    REQUIRE(Settings::GetAstcRecompression() == Settings::AstcRecompression::Uncompressed);

    Settings::SetAstcRecompressionOverride(Settings::AstcRecompression::Bc3);
    REQUIRE(Settings::GetAstcRecompression() == Settings::AstcRecompression::Bc3);
    REQUIRE(Settings::values.astc_recompression.GetValue() ==
            Settings::AstcRecompression::Uncompressed);

    Settings::SetAstcRecompressionOverride(std::nullopt);
    REQUIRE(Settings::GetAstcRecompression() == Settings::AstcRecompression::Uncompressed);
}

TEST_CASE("ASTC recompression follows the configuration without an override",
          "[video_core][astc]") {
    RecompressionRestore restore;
    Settings::SetAstcRecompressionOverride(std::nullopt);
    Settings::values.astc_recompression.SetValue(Settings::AstcRecompression::Bc1);
    REQUIRE(Settings::GetAstcRecompression() == Settings::AstcRecompression::Bc1);
}

TEST_CASE("Switch 2 program flag is per load", "[video_core][astc]") {
    Settings::SetCurrentProgramIsSwitch2(true);
    REQUIRE(Settings::CurrentProgramIsSwitch2());
    Settings::SetCurrentProgramIsSwitch2(false);
    REQUIRE_FALSE(Settings::CurrentProgramIsSwitch2());
}

// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/native_upscale.h"
#include "video_core/host_shaders/blit_color_signed_square_frag_spv.h"

namespace {

// Execute the actual compiled fragment's straight-line arithmetic, substituting only the texture
// sample. Unknown function instructions fail closed. This checks emitted shader semantics rather
// than a separate C++ implementation of the transfer function. Vulkan validation is external.
std::vector<float> EvaluateFragment(const std::array<float, 4>& sample) {
    const std::span<const u32> code{BLIT_COLOR_SIGNED_SQUARE_FRAG_SPV};
    REQUIRE(code[0] == 0x07230203);
    std::map<u32, std::vector<float>> values;
    std::map<u32, u32> constants;
    std::map<u32, std::pair<u32, u32>> component_pointers;
    u32 output = 0;
    bool in_function = false;
    bool sampled = false;
    for (size_t cursor = 5; cursor < code.size();) {
        const u32 count = code[cursor] >> 16;
        const u32 op = code[cursor] & 0xffff;
        REQUIRE(count > 0);
        REQUIRE(cursor + count <= code.size());
        const auto w = code.subspan(cursor, count);
        cursor += count;
        switch (op) {
        case 43: // OpConstant (integer indices and float LOD)
            constants[w[2]] = w[3];
            values[w[2]] = {std::bit_cast<float>(w[3])};
            break;
        case 54: // OpFunction
            in_function = true;
            break;
        case 56: // OpFunctionEnd
            in_function = false;
            break;
        case 59: // OpVariable
            if (w[3] == 3) output = w[2]; // Output storage class
            break;
        case 61: { // OpLoad
            if (const auto it = component_pointers.find(w[3]); it != component_pointers.end()) {
                values[w[2]] = {values.at(it->second.first).at(it->second.second)};
            } else {
                values[w[2]] = values[w[3]]; // opaque sampler/coordinates need no numeric values
            }
            break;
        }
        case 62: // OpStore
            values[w[1]] = values.at(w[2]);
            break;
        case 65: // OpAccessChain, one vector component
            REQUIRE(count == 5);
            component_pointers[w[2]] = {w[3], constants.at(w[4])};
            break;
        case 79: { // OpVectorShuffle
            auto components = values.at(w[3]);
            const auto& second = values.at(w[4]);
            components.insert(components.end(), second.begin(), second.end());
            auto& result = values[w[2]];
            for (u32 i = 5; i < count; ++i) result.push_back(components.at(w[i]));
            break;
        }
        case 80: { // OpCompositeConstruct
            auto& result = values[w[2]];
            for (u32 i = 3; i < count; ++i) {
                const auto& operand = values.at(w[i]);
                result.insert(result.end(), operand.begin(), operand.end());
            }
            break;
        }
        case 81: // OpCompositeExtract
            REQUIRE(count == 5);
            values[w[2]] = {values.at(w[3]).at(w[4])};
            break;
        case 88: // OpImageSampleExplicitLod
            REQUIRE_FALSE(sampled);
            values[w[2]] = {sample.begin(), sample.end()};
            sampled = true;
            break;
        case 12: { // OpExtInst, GLSL.std.450 FAbs
            REQUIRE(in_function);
            REQUIRE(w[4] == 4);
            auto result = values.at(w[5]);
            for (auto& value : result) value = std::abs(value);
            values[w[2]] = std::move(result);
            break;
        }
        case 133: { // OpFMul
            REQUIRE(sampled);
            auto result = values.at(w[3]);
            const auto& rhs = values.at(w[4]);
            REQUIRE(result.size() == rhs.size());
            for (size_t i = 0; i < result.size(); ++i) result[i] *= rhs[i];
            values[w[2]] = std::move(result);
            break;
        }
        case 248: // OpLabel
        case 253: // OpReturn
            break;
        default:
            REQUIRE_FALSE(in_function); // declarations/debug records only; no unhandled arithmetic
            break;
        }
    }
    REQUIRE(sampled);
    REQUIRE(output != 0);
    REQUIRE(values.at(output).size() == 4);
    return values.at(output);
}

} // namespace

TEST_CASE("Native upscale output conversion has an exact program and format gate",
          "[video_core][native_upscale]") {
    using namespace VideoCommon::NativeUpscale;
    using Format = Tegra::Texture::TextureFormat;
    constexpr std::array<u8, 6> bytes{'f', 'o', 'o', 'b', 'a', 'r'};
    CHECK(ProgramKey({}) == 0xcbf29ce484222325ULL);
    CHECK(ProgramKey(bytes) == 0x85944171f73967e8ULL);
    CHECK(UsesSignedSquare(SignedSquareProgramKey, SignedSquareProgramBytes, Format::E5B9G9R9,
                           Format::B10G11R11));
    CHECK_FALSE(UsesSignedSquare(SignedSquareProgramKey ^ 1, SignedSquareProgramBytes,
                                Format::E5B9G9R9, Format::B10G11R11));
    CHECK_FALSE(UsesSignedSquare(SignedSquareProgramKey, SignedSquareProgramBytes - 16,
                                Format::E5B9G9R9, Format::B10G11R11));
    CHECK_FALSE(UsesSignedSquare(SignedSquareProgramKey, SignedSquareProgramBytes + 16,
                                Format::E5B9G9R9, Format::B10G11R11));
    CHECK_FALSE(UsesSignedSquare(SignedSquareProgramKey, SignedSquareProgramBytes,
                                Format::B10G11R11, Format::B10G11R11));
    CHECK_FALSE(UsesSignedSquare(SignedSquareProgramKey, SignedSquareProgramBytes,
                                Format::E5B9G9R9, Format::A8B8G8R8));
}

TEST_CASE("Compiled native upscale shader restores signed square after sampling and keeps alpha",
          "[video_core][native_upscale]") {
    // sqrt-encoded binary-exact inputs: the original linear values are 1/16, 1/4, 1.
    CHECK(EvaluateFragment({0.25f, 0.5f, 1.0f, 0.375f}) ==
          std::vector<float>{0.0625f, 0.25f, 1.0f, 0.375f});
    // Signed-square is not abs-square; HDR values are not clamped; alpha is not transformed.
    CHECK(EvaluateFragment({-0.5f, -2.0f, 4.0f, 0.125f}) ==
          std::vector<float>{-0.25f, -4.0f, 16.0f, 0.125f});
    // A bilinear midpoint between encoded 0 and 1 is sampled as 0.5, THEN becomes 0.25.
    CHECK(EvaluateFragment({0.5f, 0.5f, 0.5f, 1.0f}) ==
          std::vector<float>{0.25f, 0.25f, 0.25f, 1.0f});
    const auto zeros = EvaluateFragment({-0.0f, 0.0f, 0.0f, -0.0f});
    CHECK(std::signbit(zeros[0]));
    CHECK_FALSE(std::signbit(zeros[1]));
    CHECK(std::signbit(zeros[3]));

    if (const char* path = std::getenv("EDEN_TEST_SIGNED_SQUARE_SPV")) {
        std::ofstream file{path, std::ios::binary};
        REQUIRE(file.good());
        file.write(reinterpret_cast<const char*>(BLIT_COLOR_SIGNED_SQUARE_FRAG_SPV),
                   sizeof(BLIT_COLOR_SIGNED_SQUARE_FRAG_SPV));
        REQUIRE(file.good());
    }
}


TEST_CASE("Isolated tensor reconstruction requires the exact compute program and opt-in",
          "[video_core][native_upscale][oot_tensor_gate]") {
    using namespace VideoCommon::NativeUpscale;
    for (const std::string_view value : {"", "0", "true", "01", "1 ", "stub"}) {
        CHECK_FALSE(TensorGateEnabled(value, false));
    }
    REQUIRE(TensorGateEnabled("1", false));
    CHECK_FALSE(TensorGateEnabled("1", true)); // Preserve explicit global-mode precedence.
    REQUIRE(SelectTensorReconstruction(true, true, SignedSquareProgramKey,
                                       SignedSquareProgramBytes));
    CHECK_FALSE(SelectTensorReconstruction(false, true, SignedSquareProgramKey,
                                           SignedSquareProgramBytes));
    CHECK_FALSE(SelectTensorReconstruction(true, false, SignedSquareProgramKey,
                                           SignedSquareProgramBytes));
    for (const size_t bytes : {size_t{0}, SignedSquareProgramBytes - 16,
                               SignedSquareProgramBytes - 1, SignedSquareProgramBytes + 1,
                               SignedSquareProgramBytes + 16}) {
        CHECK_FALSE(SelectTensorReconstruction(true, true, SignedSquareProgramKey, bytes));
    }
    // Adjacent keys and other captured large networks must not inherit this opt-in.
    for (const u64 key : {SignedSquareProgramKey - 1, SignedSquareProgramKey + 1,
                          u64{0xe27336d38f1cc8f4}, u64{0x1fdb95ea2565f49c},
                          u64{0x0be2f024aea21a6c}}) {
        CHECK_FALSE(SelectTensorReconstruction(true, true, key, SignedSquareProgramBytes));
    }
}

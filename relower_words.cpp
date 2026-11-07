// Reads "w0 w1 w2 w3" hex lines on stdin; lowers each instruction followed by EXIT.
// Build: c++ -std=c++20 -O1 -w -I <repo>/src relower_words.cpp -lfmt -o relower
#include <cstdio>
#include <vector>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using namespace Shader::Ampere;

int main(int argc, char**) {
    const bool tensor = argc > 1; // any argument: emulate HMMA as EDEN_NX2_TENSOR would
    unsigned w0, w1, w2, w3;
    while (std::scanf("%x %x %x %x", &w0, &w1, &w2, &w3) == 4) {
        const std::vector<Instruction> code{Instruction{{w0, w1, w2, w3}},
                                            Instruction{{0x0000794d, 0, 0x03800000, 0}}};
        const auto lowered = LowerToMaxwell(code, LoweringOptions{.tensor = tensor});
        std::printf("%08x %08x %08x %08x: %s%.*s\n", w0, w1, w2, w3, lowered ? "OK" : "FAIL ",
                    static_cast<int>(lowered.error.size()), lowered.error.data());
    }
}

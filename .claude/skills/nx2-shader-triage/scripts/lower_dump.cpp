// Lowers every EDEN_NX2_SHADER_DUMP program given as arguments; one OK/REJECT line each.
// Build: c++ -std=c++20 -O1 -w -I <repo>/src lower_dump.cpp -lfmt -o lower_dump
// OK lines carry an FNV hash of the lowered code: build once against the committed header
// (-I <dir with `git show HEAD:` copies> first) and once against the tree, then join the OK lines
// to prove programs that already lowered are unchanged.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using namespace Shader::Ampere;

int main(int argc, char** argv) {
    for (int arg = 1; arg < argc; ++arg) {
        const std::string path = argv[arg];
        std::ifstream file(path, std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
        std::vector<Instruction> code(bytes.size() / sizeof(Instruction));
        std::memcpy(code.data(), bytes.data(), code.size() * sizeof(Instruction));
        // The dump name has no stage. Mesh programs (bound to the tessellation slot) lower plain,
        // geometry programs with geometry set: keep whichever gets furthest.
        auto lowered = LowerToMaxwell(code, LoweringOptions{.tensor = true});
        const char* stage = "plain";
        if (!lowered && path.find("-gfx-") != std::string::npos) {
            auto attempt = LowerToMaxwell(code, LoweringOptions{.geometry = true, .tensor = true});
            if (attempt || attempt.error_offset > lowered.error_offset) {
                lowered = std::move(attempt);
                stage = "geometry";
            }
        }
        const auto name = path.substr(path.find_last_of('/') + 1);
        if (lowered) {
            unsigned long long hash = 1469598103934665603ULL;
            for (const u64 word : lowered.instructions) {
                hash ^= word;
                hash *= 1099511628211ULL;
            }
            std::printf("OK %s %s %016llx\n", name.c_str(), stage, hash);
            continue;
        }
        const u32 index = lowered.error_offset / sizeof(Instruction);
        const auto words = index < code.size() ? code[index].words : std::array<u32, 4>{};
        std::printf("REJECT %s %s +%#x %.*s [op %03x %08x %08x %08x %08x]\n", name.c_str(), stage,
                    lowered.error_offset, static_cast<int>(lowered.error.size()), lowered.error.data(),
                    words[0] & 0xfff, words[0], words[1], words[2], words[3]);
    }
}

// Runs the Ampere -> Maxwell lowering over raw native shader dumps and lists the rejected ones.
//
// build (header-only, run from the repository root):
//   g++ -std=c++20 -O1 -w -I src -o lower_dump tools/nx2/lower_dump.cpp
// usage:
//   ./lower_dump <dump>.bin [<dump>.bin ...]
//   ./lower_dump /path/to/shaderdumps/*.bin
//
// Dumps come from EDEN_NX2_SHADER_DUMP=<dir> (<key>-gfx|cs-<address>.bin, 16 bytes per
// instruction, no header). One tab-separated line per rejected program:
//   REJECT <file> <reason> <byte offset> <opcode> <four instruction words>
// Only the lowering is checked: a program can still fail later in the recompiler or on a texture
// descriptor buffer conflict. Disassemble a dump with tools/nx2/nvd_cubin.py.
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using namespace Shader::Ampere;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <dump>.bin [...]\n", argv[0]);
        return 2;
    }
    size_t total = 0, rejected = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream file(argv[i], std::ios::binary | std::ios::ate);
        if (!file) {
            std::fprintf(stderr, "cannot read %s\n", argv[i]);
            continue;
        }
        const auto length = static_cast<size_t>(file.tellg());
        file.seekg(0);
        std::vector<Instruction> code(length / sizeof(Instruction));
        file.read(reinterpret_cast<char*>(code.data()), code.size() * sizeof(Instruction));
        ++total;
        const auto result = LowerToMaxwell(code);
        if (result) {
            continue;
        }
        ++rejected;
        const size_t index = result.error_offset / sizeof(Instruction);
        const std::array<u32, 4> words = index < code.size() ? code[index].words : std::array<u32, 4>{};
        std::printf("REJECT\t%s\t%s\t%#x\t%03x\t%08x %08x %08x %08x\n",
                    std::filesystem::path(argv[i]).filename().string().c_str(),
                    std::string(result.error).c_str(), static_cast<unsigned>(result.error_offset),
                    words[0] & 0xfff, words[0], words[1], words[2], words[3]);
    }
    std::fprintf(stderr, "%zu of %zu programs rejected\n", rejected, total);
    return rejected != 0 ? 1 : 0;
}

// Print the Maxwell words that given Ampere instructions of a dumped program lower to.
// Build outside the repo: c++ -std=c++20 -O1 -w -I <repo>/src lowered_for.cpp -lfmt -o lowered_for
// Usage: lowered_for <dump.bin> <ampere byte offset hex>...   (tensor lowering on, so HMMA programs lower)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
using namespace Shader::Ampere;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <dump.bin> <offset>...\n", argv[0]);
        return 1;
    }
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
    std::vector<Instruction> code(bytes.size() / sizeof(Instruction));
    std::memcpy(code.data(), bytes.data(), code.size() * sizeof(Instruction));
    const auto lowered = LowerToMaxwell(code, LoweringOptions{.tensor = true});
    const auto& offsets = lowered.source_instruction_offsets;
    if (offsets.empty()) {
        std::printf("program did not lower\n");
        return 1;
    }
    for (int arg = 2; arg < argc; ++arg) {
        const u32 index = static_cast<u32>(std::strtoul(argv[arg], nullptr, 16)) / 16;
        if (index >= offsets.size()) continue;
        const size_t begin = offsets[index];
        const size_t end = index + 1 < offsets.size() ? offsets[index + 1] : lowered.instructions.size();
        std::printf("ampere %#x (%08x %08x %08x %08x) -> maxwell [%zu, %zu)\n", index * 16, code[index].words[0],
                    code[index].words[1], code[index].words[2], code[index].words[3], begin, end);
        for (size_t i = begin; i < end; ++i)
            std::printf("   %016llx\n", static_cast<unsigned long long>(lowered.instructions[i]));
    }
}

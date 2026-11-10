// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shader_recompiler/frontend/ampere/native_program_image.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace Shader::Ampere {

// native_programs.bin keeps what these scans found: a change to what they find needs a new
// CacheMagic in core/file_sys/native_shader_programs.cpp, or old lists stay in use.

// A native program found in a shader container from the game's files (BFSHA, SARC, effect and
// layout archives): the same instruction range the runtime capture reads
// once the program is bound, so its NativeProgramKey matches.
struct ScannedProgram {
    size_t block_offset; ///< 256-byte block: the program header (graphics) or the code (compute)
    size_t code_offset;
    size_t code_size;
    bool graphics;
};

namespace ProgramScanDetail {
// Program blocks are looked for at 256-byte offsets from the start of the scanned file. Every
// container in Mario Kart World's archives sits at such an offset; one that does not is missed.
constexpr size_t BlockSize = 0x100;
constexpr size_t GraphicsCodeOffset = 0x80;
constexpr size_t MaximumBytes = ProgramCaptureState::MaximumInstructions * sizeof(Instruction);
// Every program ends with an unconditional branch to itself (BRA with a -16 offset).
constexpr u64 SelfBranchLow = 0xfffffff000007947ULL;

inline Instruction ReadInstruction(std::span<const u8> data, size_t offset) {
    Instruction instruction;
    std::memcpy(&instruction, data.data() + offset, sizeof(instruction));
    return instruction;
}

inline bool IsSelfBranch(std::span<const u8> data, size_t offset) {
    u64 low;
    std::memcpy(&low, data.data() + offset, sizeof(low));
    return low == SelfBranchLow;
}

inline bool IsZero(const Instruction& instruction) {
    return (instruction.words[0] | instruction.words[1] | instruction.words[2] |
            instruction.words[3]) == 0;
}

inline bool IsPadding(const Instruction& instruction) {
    return IsZero(instruction) || instruction.Bits(0, 12) == 0x918; // NOP
}

// Program header type in the low five bits of its first word (1 vertex/geometry, 2 pixel), and
// the zero tail between the 0x50-byte header and the code.
inline bool IsGraphicsHeader(std::span<const u8> data, size_t offset) {
    u32 word;
    std::memcpy(&word, data.data() + offset, sizeof(word));
    const u32 type = word & 0x1f;
    if (type != 1 && type != 2) {
        return false;
    }
    for (size_t tail = 0x50; tail < GraphicsCodeOffset; tail += sizeof(Instruction)) {
        if (!IsZero(ReadInstruction(data, offset + tail))) {
            return false;
        }
    }
    return true;
}

// Runs the runtime capture from code_offset; the program must end at the self-branch.
inline std::optional<size_t> CaptureSize(std::span<const u8> data, size_t code_offset,
                                         size_t branch_offset) {
    if (code_offset >= branch_offset || branch_offset - code_offset > MaximumBytes) {
        return std::nullopt;
    }
    ProgramCaptureState capture;
    const size_t limit = (branch_offset - code_offset) / sizeof(Instruction) + 1;
    for (size_t index = 0; index < limit; ++index) {
        const auto instruction = ReadInstruction(data, code_offset + index * sizeof(Instruction));
        switch (capture.Observe(instruction, static_cast<u32>(index))) {
        case ProgramCaptureState::Result::Continue:
            break;
        case ProgramCaptureState::Result::InvalidTarget:
            return std::nullopt;
        case ProgramCaptureState::Result::Complete: {
            const size_t end = code_offset + (index + 1) * sizeof(Instruction);
            if (end == branch_offset || end == branch_offset + sizeof(Instruction)) {
                return end - code_offset;
            }
            return std::nullopt;
        }
        }
    }
    return std::nullopt;
}
} // namespace ProgramScanDetail

// Finds native programs by their closing self-branch, then walks back to the first block after
// the previous program (or after a zero run, for the first one) and confirms the start by
// capturing forward, as the shader cache does, up to that self-branch.
inline std::vector<ScannedProgram> ScanNativePrograms(std::span<const u8> data) {
    using namespace ProgramScanDetail;
    std::vector<ScannedProgram> programs;
    size_t previous_end = 0;
    bool has_previous = false;
    for (size_t branch = 0; branch + sizeof(Instruction) <= data.size();
         branch += sizeof(Instruction)) {
        if (!IsSelfBranch(data, branch)) {
            continue;
        }
        size_t lower = branch > MaximumBytes ? branch - MaximumBytes : 0;
        if (has_previous && previous_end > lower) {
            lower = previous_end;
        } else {
            // First program in reach: walk back over its code to the zero run before it.
            size_t offset = branch;
            while (offset >= lower + sizeof(Instruction) &&
                   !IsZero(ReadInstruction(data, offset - sizeof(Instruction)))) {
                offset -= sizeof(Instruction);
            }
            lower = offset;
        }
        size_t first = lower;
        while (first < branch && IsPadding(ReadInstruction(data, first))) {
            first += sizeof(Instruction);
        }
        has_previous = true;
        previous_end = branch + sizeof(Instruction);

        // Capturing from too early a block also reaches the self-branch. Code holds no all-zero
        // instruction while headers and data between programs do, so the code starts after the
        // last zero slot; a block holding a graphics header wins over compute code.
        size_t code_floor = first;
        for (size_t offset = branch; offset > first; offset -= sizeof(Instruction)) {
            if (IsZero(ReadInstruction(data, offset - sizeof(Instruction)))) {
                code_floor = offset;
                break;
            }
        }
        const auto add = [&](size_t block, size_t size, bool graphics) {
            programs.push_back({
                .block_offset = block,
                .code_offset = block + (graphics ? GraphicsCodeOffset : 0),
                .code_size = size,
                .graphics = graphics,
            });
        };
        const size_t down = first / BlockSize * BlockSize;
        bool found = false;
        for (size_t block = down; block + GraphicsCodeOffset < branch; block += BlockSize) {
            if (block + GraphicsCodeOffset < code_floor || !IsGraphicsHeader(data, block)) {
                continue;
            }
            if (const auto size = CaptureSize(data, block + GraphicsCodeOffset, branch)) {
                add(block, *size, true);
                found = true;
                break;
            }
        }
        const size_t up = (code_floor + BlockSize - 1) / BlockSize * BlockSize;
        for (size_t block = up; !found && block < branch; block += BlockSize) {
            if (IsPadding(ReadInstruction(data, block))) {
                continue;
            }
            if (const auto size = CaptureSize(data, block, branch)) {
                add(block, *size, false);
                found = true;
            }
        }
    }
    return programs;
}

// The vertex and pixel programs combined by the shader program tables of BNSH containers: a
// stage entry holds the code block's offset from the container header and a size word tagged
// 0xd8 in its upper half; the pixel entry follows the vertex one 0x40 bytes later. Entries can lie
// anywhere after their container's header (in an archive, past later containers), so the tagged
// words are found once and each container is matched against those. agl BAHS archives keep their
// entries ahead of the code section they count from (u32 at +0x1c), with the tag in the size
// word's lower half; a program's pixel entry is the next tagged entry after its vertex one (Mario
// Kart World: up to 0x300 bytes later).
inline std::vector<std::pair<size_t, size_t>> ScanProgramPairs(
    std::span<const u8> data, std::span<const ScannedProgram> programs) {
    using namespace ProgramScanDetail;
    constexpr size_t PixelEntryOffset = 0x40;
    constexpr size_t ArchivePixelReach = 0x400;
    constexpr u64 SizeTag = 0xd8;
    std::unordered_map<size_t, size_t> blocks; ///< Graphics block offset -> program index
    for (size_t index = 0; index < programs.size(); ++index) {
        if (programs[index].graphics) {
            blocks.emplace(programs[index].block_offset, index);
        }
    }
    const auto read_u32 = [&](size_t offset) {
        u32 value;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    };
    const auto read_u64 = [&](size_t offset) {
        u64 value;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    };
    const auto stage_type = [&](size_t index) { return data[programs[index].block_offset] & 0x1f; };
    std::vector<std::pair<size_t, size_t>> pairs;
    if (blocks.empty() || data.size() < PixelEntryOffset + 2 * sizeof(u64)) {
        return pairs;
    }
    constexpr std::array<u8, 4> Magic{'B', 'N', 'S', 'H'};
    std::vector<size_t> tagged; ///< Entry offsets whose size word holds the tag
    for (size_t entry = 0; entry + PixelEntryOffset + sizeof(u64) <= data.size();
         entry += sizeof(u32)) {
        if ((read_u64(entry + sizeof(u64)) >> 32) == SizeTag) {
            tagged.push_back(entry);
        }
    }
    for (size_t base = 0; base + Magic.size() <= data.size(); base += sizeof(u32)) {
        if (std::memcmp(data.data() + base, Magic.data(), Magic.size()) != 0) {
            continue;
        }
        for (auto it = std::ranges::lower_bound(tagged, base); it != tagged.end(); ++it) {
            const size_t entry = *it;
            if ((entry - base) % sizeof(u64) != 0) {
                continue;
            }
            const auto vertex = blocks.find(base + read_u64(entry));
            if (vertex == blocks.end()) {
                continue;
            }
            const auto pixel = blocks.find(base + read_u64(entry + PixelEntryOffset));
            if (pixel != blocks.end() && stage_type(vertex->second) == 1 &&
                stage_type(pixel->second) == 2) {
                pairs.emplace_back(vertex->second, pixel->second);
            }
        }
    }
    std::vector<size_t> archive_tagged; ///< Entry offsets whose size word's lower half is the tag
    for (size_t entry = 0; entry + 2 * sizeof(u64) <= data.size(); entry += sizeof(u32)) {
        if (read_u32(entry + sizeof(u64)) == SizeTag) {
            archive_tagged.push_back(entry);
        }
    }
    for (size_t start = 0; start + 0x20 <= data.size(); start += sizeof(u32)) {
        if (std::memcmp(data.data() + start, "BAHS", 4) != 0) {
            continue;
        }
        // Entries are aligned to their archive, which may not be to the file.
        const auto aligned = [start](size_t entry) { return (entry - start) % sizeof(u64) == 0; };
        const size_t base = start + read_u32(start + 0x1c);
        const auto last = std::ranges::lower_bound(archive_tagged, base);
        for (auto it = std::ranges::lower_bound(archive_tagged, start); it != last; ++it) {
            if (!aligned(*it)) {
                continue;
            }
            const auto next_it = std::find_if(std::next(it), archive_tagged.end(), aligned);
            if (next_it == archive_tagged.end()) {
                break;
            }
            const auto vertex = blocks.find(base + read_u64(*it));
            const size_t next = *next_it;
            if (vertex == blocks.end() || stage_type(vertex->second) != 1 ||
                next - *it > ArchivePixelReach) {
                continue;
            }
            const auto pixel = blocks.find(base + read_u64(next));
            if (pixel != blocks.end() && stage_type(pixel->second) == 2) {
                pairs.emplace_back(vertex->second, pixel->second);
            }
        }
    }
    return pairs;
}

struct ScannedDispatch {
    size_t program;
    std::array<u32, 3> workgroup;
    u32 shared_memory;
};

// The workgroup and shared memory of compute programs, from the NVN shader control sections their
// stage entries lead to. An entry holds the code block's offset from its container (a BNSH header,
// or the code section of an agl BAHS archive) and a 0xd8 tag; the control section (magic
// 0x98761234) follows within 0x100 bytes, with the workgroup at +0x58 and the shared memory at
// +0x64. As with program pairs, the tagged entries are found once. Programs whose code several
// entries share may get more than one dispatch.
inline std::vector<ScannedDispatch> ScanComputeDispatches(
    std::span<const u8> data, std::span<const ScannedProgram> programs) {
    constexpr u32 ControlMagic = 0x98761234;
    constexpr size_t ControlReach = 0x100;
    constexpr size_t WorkgroupOffset = 0x58;
    constexpr size_t SharedMemoryOffset = 0x64;
    constexpr u32 SharedMemoryGranularity = 0x100;
    std::unordered_map<size_t, size_t> blocks; ///< Compute code offset -> program index
    for (size_t index = 0; index < programs.size(); ++index) {
        if (!programs[index].graphics) {
            blocks.emplace(programs[index].code_offset, index);
        }
    }
    std::vector<ScannedDispatch> dispatches;
    if (blocks.empty()) {
        return dispatches;
    }
    const auto read_u32 = [&](size_t offset) {
        u32 value;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    };
    const auto read_u64 = [&](size_t offset) {
        u64 value;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    };
    // (where the container's entries start, what their code offsets count from): a BAHS archive
    // keeps its entries ahead of its code section.
    std::vector<std::pair<size_t, size_t>> containers;
    for (size_t offset = 0; offset + 0x20 <= data.size(); offset += sizeof(u32)) {
        if (std::memcmp(data.data() + offset, "BNSH", 4) == 0) {
            containers.emplace_back(offset, offset);
        } else if (std::memcmp(data.data() + offset, "BAHS", 4) == 0) {
            containers.emplace_back(offset, offset + read_u32(offset + 0x1c));
        }
    }
    std::vector<size_t> tagged; ///< Entry offsets whose size word holds the tag
    for (size_t entry = 0; entry + ControlReach + 0x70 <= data.size(); entry += sizeof(u32)) {
        if (read_u32(entry + 8) == 0xd8 || (read_u64(entry + 8) >> 32) == 0xd8) {
            tagged.push_back(entry);
        }
    }
    for (const auto& [start, base] : containers) {
        for (auto it = std::ranges::lower_bound(tagged, start); it != tagged.end(); ++it) {
            const size_t entry = *it;
            if ((entry - start) % sizeof(u64) != 0) {
                continue; // Entries are aligned to their container, which may not be to the file
            }
            const auto block = blocks.find(base + read_u64(entry));
            if (block == blocks.end()) {
                continue;
            }
            for (size_t control = entry; control < entry + ControlReach; control += sizeof(u32)) {
                if (read_u32(control) != ControlMagic) {
                    continue;
                }
                const std::array<u32, 3> workgroup{read_u32(control + WorkgroupOffset),
                                                   read_u32(control + WorkgroupOffset + 4),
                                                   read_u32(control + WorkgroupOffset + 8)};
                const u64 invocations = u64{workgroup[0]} * workgroup[1] * workgroup[2];
                if (workgroup[0] == 0 || workgroup[1] == 0 || workgroup[2] == 0 ||
                    invocations > 1024) {
                    break;
                }
                const u32 shared = read_u32(control + SharedMemoryOffset);
                dispatches.push_back({
                    .program = block->second,
                    .workgroup = workgroup,
                    .shared_memory = (shared + SharedMemoryGranularity - 1) /
                                     SharedMemoryGranularity * SharedMemoryGranularity,
                });
                break;
            }
        }
    }
    return dispatches;
}

inline NativeProgramImage CopyNativeProgram(std::span<const u8> data,
                                            const ScannedProgram& program) {
    constexpr size_t HeaderSize = 0x50;
    NativeProgramImage image;
    if (program.graphics) {
        image.header.assign(data.begin() + program.block_offset,
                            data.begin() + program.block_offset + HeaderSize);
    }
    image.code.assign(data.begin() + program.code_offset,
                      data.begin() + program.code_offset + program.code_size);
    return image;
}

} // namespace Shader::Ampere

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/memory_manager.h"

namespace VideoCommon {

/// The address of storage buffer `binding` of a shader as its descriptor in a constant buffer gives
/// it; 0 when it has none. storage_alignment is the host storage buffer offset alignment: storage
/// offsets in a handle program are relative to the buffer address aligned down to it
/// (StorageOffset), which is also where the buffer cache binds the buffer.
template <typename Memory, typename CbufAddress>
GPUVAddr StorageBufferBase(const Shader::Info& info, u32 binding, Memory& gpu_memory,
                           CbufAddress& cbuf_address, u64 storage_alignment) {
    if (binding >= info.storage_buffers_descriptors.size()) {
        return 0;
    }
    const auto& descriptor{info.storage_buffers_descriptors[binding]};
    const GPUVAddr cbuf{cbuf_address(descriptor.cbuf_index)};
    if (cbuf == 0) {
        return 0;
    }
    GPUVAddr base{gpu_memory.template Read<u64>(cbuf + descriptor.cbuf_offset)};
    if (base != 0 && descriptor.indirect_offset != ~0U) {
        // The table entry is read at its exact address. Only the buffer address it holds is
        // aligned down, as the shader does.
        base = gpu_memory.template Read<u64>(base + descriptor.indirect_offset);
    }
    return base & ~(GPUVAddr{(std::max)(storage_alignment, u64{1})} - 1);
}

/// The texture handle a shader reads from a storage buffer, computed from the current constant
/// buffer and storage buffer words (Shader::Info::handle_programs). cbuf_address(index) is the
/// bound constant buffer's address, 0 when none is bound. storage_alignment is as for
/// StorageBufferBase. element is the descriptor array element, for a program computing one handle
/// per element.
template <typename Memory, typename CbufAddress>
u32 EvaluateTextureHandle(const Shader::Info& info, u32 program, Memory& gpu_memory,
                          CbufAddress&& cbuf_address, u64 storage_alignment, u32 element = 0) {
    return Shader::EvaluateHandleProgram(
               info.handle_programs[program],
               [&](u32 index, u32 offset) -> u32 {
                   const GPUVAddr base{cbuf_address(index)};
                   return base != 0 ? gpu_memory.template Read<u32>(base + offset) : 0U;
               },
               [&](u32 binding, u32 offset) -> u32 {
                   const GPUVAddr base{StorageBufferBase(info, binding, gpu_memory, cbuf_address,
                                                         storage_alignment)};
                   return base != 0 ? gpu_memory.template Read<u32>(base + offset) : 0U;
               },
               element)
        .value_or(0U);
}

/// The handles of elements 0 to count - 1 of a descriptor array of handle program results, as
/// EvaluateTextureHandle computes each, passed to output(element, handle). The constant buffer
/// words and storage buffer addresses the program reads are read once for all the elements.
template <typename Memory, typename CbufAddress, typename Output>
void EvaluateTextureHandles(const Shader::Info& info, u32 program, Memory& gpu_memory,
                            CbufAddress&& cbuf_address, u64 storage_alignment, u32 count,
                            Output&& output) {
    struct CbufWord {
        u32 index;
        u32 offset;
        u32 value;
    };
    std::array<CbufWord, 16> words;
    size_t num_words{};
    std::array<GPUVAddr, Shader::Info::MAX_SSBOS> bases;
    // One bit per binding read so far: native programs bind up to MAX_SSBOS storage buffers
    // (Ocarina of Time binds 37), so the mask needs all 64 bits.
    static_assert(Shader::Info::MAX_SSBOS <= 64);
    u64 known_bases{};
    const auto read_cbuf = [&](u32 index, u32 offset) -> u32 {
        for (size_t word = 0; word < num_words; ++word) {
            if (words[word].index == index && words[word].offset == offset) {
                return words[word].value;
            }
        }
        const GPUVAddr base{cbuf_address(index)};
        const u32 value{base != 0 ? gpu_memory.template Read<u32>(base + offset) : 0U};
        if (num_words < words.size()) {
            words[num_words++] = {index, offset, value};
        }
        return value;
    };
    const auto read_storage = [&](u32 binding, u32 offset) -> u32 {
        if (binding >= bases.size()) {
            return 0U;
        }
        if ((known_bases & (u64{1} << binding)) == 0) {
            bases[binding] =
                StorageBufferBase(info, binding, gpu_memory, cbuf_address, storage_alignment);
            known_bases |= u64{1} << binding;
        }
        return bases[binding] != 0 ? gpu_memory.template Read<u32>(bases[binding] + offset) : 0U;
    };
    for (u32 element = 0; element < count; ++element) {
        output(element, Shader::EvaluateHandleProgram(info.handle_programs[program], read_cbuf,
                                                      read_storage, element)
                            .value_or(0U));
    }
}

/// Logs what the elements of a descriptor array of handle program results (a table of per-light
/// shadow maps, say) hold, whenever that changes, for the first lines: how many distinct textures
/// and which, by TIC index with the first element holding each. Off unless
/// EDEN_NX2_HANDLE_TABLE_NOTES is set, so draws and dispatches don't gather the tables for it.
class HandleTableNotes {
public:
    [[nodiscard]] static bool Wanted() noexcept {
        return enabled && lines.load(std::memory_order_relaxed) < MAX_LINES;
    }

    static void Note(u64 shader, u32 texture, std::span<const u32> tics) {
        std::vector<std::pair<u32, u32>> distinct;
        for (u32 element = 0; element < tics.size(); ++element) {
            if (std::ranges::none_of(distinct, [&](const auto& entry) {
                    return entry.first == tics[element];
                })) {
                distinct.emplace_back(tics[element], element);
            }
        }
        std::vector<u32> signature(distinct.size());
        std::ranges::transform(distinct, signature.begin(), &std::pair<u32, u32>::first);
        {
            std::scoped_lock lock{mutex};
            auto& last{seen[shader ^ (u64{texture} << 56)]};
            if (last == signature) {
                return;
            }
            last = std::move(signature);
        }
        if (lines.fetch_add(1, std::memory_order_relaxed) >= MAX_LINES) {
            return;
        }
        std::string list;
        for (size_t i = 0; i < distinct.size() && i < 16; ++i) {
            list += fmt::format(" tic{}@{}", distinct[i].first, distinct[i].second);
        }
        LOG_INFO(Render_Vulkan, "Handle table: shader {:016x} texture {}: {} elements, {} distinct:{}{}",
                 shader, texture, tics.size(), distinct.size(), list,
                 distinct.size() > 16 ? " ..." : "");
    }

private:
    static constexpr int MAX_LINES = 64;
    inline static const bool enabled{std::getenv("EDEN_NX2_HANDLE_TABLE_NOTES") != nullptr};
    inline static std::atomic<int> lines{};
    inline static std::mutex mutex;
    inline static std::unordered_map<u64, std::vector<u32>> seen;
};

/// Whether texture descriptor `desc`'s handle word (element `index`) is where a storage buffer of
/// the same program keeps its address. The translation falls back to such a word when the shader
/// loads the handle per invocation, which no draw-time evaluation can give (a light's texture
/// read in a clustered light loop, Ocarina of Time's waterfall). Its low bits then name an
/// unrelated texture, a different one whenever the game moves the buffer: every frame for a
/// double-buffered one, so the surface flickers. Bind no texture for it instead.
template <typename Descriptor>
[[nodiscard]] bool HandleIsStoragePointer(const Shader::Info& info, const Descriptor& desc,
                                          u32 index) {
    if (desc.handle_program != ~0U || desc.has_secondary) {
        return false;
    }
    const u32 offset = desc.cbuf_offset + (index << desc.size_shift);
    return std::ranges::any_of(info.storage_buffers_descriptors, [&](const auto& storage) {
        return storage.cbuf_index == desc.cbuf_index && offset >= storage.cbuf_offset &&
               offset - storage.cbuf_offset < sizeof(u64);
    });
}

/// A texture handle no table holds: the texture cache binds its null view and sampler.
inline constexpr std::pair<u32, u32> NULL_TEXTURE_HANDLE{~0U, ~0U};

} // namespace VideoCommon

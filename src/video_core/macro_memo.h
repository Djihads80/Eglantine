// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Tegra {

using MacroMemoPairs = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

/// The memo's keys are already well mixed hashes: use them as they are (std::hash<u64> is a
/// byte-wise FNV-1a on MSVC, ~20% of a lookup).
struct MacroMemoHash {
    using is_avalanching = std::true_type;
    std::size_t operator()(std::uint64_t key) const noexcept {
        return static_cast<std::size_t>(key);
    }
};

// The effects of native macro calls that depend on nothing but their start address, their
// parameters and data RAM words they read (no register or memory reads), so that a repeated
// call replays its data RAM and method writes instead of being interpreted. In title 1's 3D
// scenes ~96% of the ~1,000,000 calls a second repeat an earlier one exactly, and they run 7
// instructions on average: the per-call interpreter cost, not the instructions, was the time.
// `Map` maps a u64 (the call's hash) to a u32 (its record's offset in the arena).
template <typename Map>
class BasicMacroMemo {
public:
    /// A remembered call's effects: (address, value) pairs, flattened.
    struct Effects {
        std::span<const std::uint32_t> data_ram_writes;
        std::span<const std::uint32_t> writes; ///< (method, value) in order
    };

    struct Statistics {
        std::uint64_t hits{}, slot_hits{}, misses{}, guard_failures{}, stores{}, overwrites{},
            compactions{}, clears{};
    };

    BasicMacroMemo() {
        last_by_start.fill({0, NoRecord});
    }

    /// An earlier call with this start and these parameters whose data RAM reads still hold.
    bool Find(std::uint32_t start, std::span<const std::uint32_t> parameters,
              std::span<const std::uint32_t> data_ram, Effects& effects) {
        // Nearly half the calls repeat the previous call of the same macro: try its record before
        // hashing (no probe, and the record is still in the cache).
        auto& last = last_by_start[start % last_by_start.size()];
        if (last.first == start && last.second != NoRecord &&
            Match(last.second, start, parameters, data_ram, effects)) {
            ++statistics.hits;
            ++statistics.slot_hits;
            return true;
        }
        const auto it = entries.find(Hash(start, parameters));
        if (it == entries.end()) {
            ++statistics.misses;
            return false;
        }
        if (!Match(it->second, start, parameters, data_ram, effects)) {
            ++statistics.guard_failures; // or another call with the same hash
            return false;
        }
        last = {start, it->second};
        ++statistics.hits;
        return true;
    }

private:
    static constexpr std::uint32_t NoRecord = ~0U;

    /// Whether the record at `offset` is this call with its data RAM reads holding; fills
    /// `effects` if so.
    bool Match(std::uint32_t offset, std::uint32_t start, std::span<const std::uint32_t> parameters,
               std::span<const std::uint32_t> data_ram, Effects& effects) {
        // Record: start, parameter count, read count, data RAM write count, write count, then
        // the parameters and the (address, value) pairs.
        const std::uint32_t* record = arena.data() + offset;
        const std::uint32_t num_parameters = record[1], num_reads = record[2];
        const std::uint32_t num_data_ram_writes = record[3], num_writes = record[4];
        if (record[0] != start || num_parameters != parameters.size() ||
            !std::equal(parameters.begin(), parameters.end(), record + HeaderWords)) {
            return false; // another call
        }
        const std::uint32_t* reads = record + HeaderWords + num_parameters;
        for (std::uint32_t i = 0; i < num_reads; ++i) {
            if (data_ram[reads[i * 2]] != reads[i * 2 + 1]) {
                return false;
            }
        }
        const std::uint32_t* data_ram_writes = reads + num_reads * 2;
        effects.data_ram_writes = {data_ram_writes, num_data_ram_writes * 2};
        effects.writes = {data_ram_writes + num_data_ram_writes * 2, num_writes * 2};
        return true;
    }

public:
    void Store(std::uint32_t start, std::span<const std::uint32_t> parameters,
               const MacroMemoPairs& data_ram_reads, const MacroMemoPairs& data_ram_writes,
               const MacroMemoPairs& writes) {
        ++statistics.stores;
        const std::uint64_t hash = Hash(start, parameters);
        // A call stored again (its data RAM reads no longer held) usually has the same shape:
        // rewrite its record in place, or the arena fills with stale copies.
        if (const auto it = entries.find(hash); it != entries.end()) {
            std::uint32_t* record = arena.data() + it->second;
            if (record[0] == start && record[1] == parameters.size() &&
                record[2] == data_ram_reads.size() && record[3] == data_ram_writes.size() &&
                record[4] == writes.size() &&
                std::equal(parameters.begin(), parameters.end(), record + HeaderWords)) {
                std::uint32_t* out = record + HeaderWords + parameters.size();
                for (const MacroMemoPairs* pairs : {&data_ram_reads, &data_ram_writes, &writes}) {
                    for (const auto& [address, value] : *pairs) {
                        *out++ = address;
                        *out++ = value;
                    }
                }
                ++statistics.overwrites;
                return;
            }
        }
        if (arena.size() >= MaxArenaWords) {
            Compact();
        }
        const auto offset = static_cast<std::uint32_t>(arena.size());
        arena.push_back(start);
        arena.push_back(static_cast<std::uint32_t>(parameters.size()));
        arena.push_back(static_cast<std::uint32_t>(data_ram_reads.size()));
        arena.push_back(static_cast<std::uint32_t>(data_ram_writes.size()));
        arena.push_back(static_cast<std::uint32_t>(writes.size()));
        arena.insert(arena.end(), parameters.begin(), parameters.end());
        for (const MacroMemoPairs* pairs : {&data_ram_reads, &data_ram_writes, &writes}) {
            for (const auto& [address, value] : *pairs) {
                arena.push_back(address);
                arena.push_back(value);
            }
        }
        entries.insert_or_assign(hash, offset);
        last_by_start[start % last_by_start.size()] = {start, offset};
    }

    const Statistics& GetStatistics() const {
        return statistics;
    }

    std::size_t Size() const {
        return entries.size();
    }

    std::size_t ArenaWords() const {
        return arena.size();
    }

    /// Forgets every call when the macro code is no longer the code they ran.
    void Synchronize(std::uint64_t code_generation) {
        if (code_generation != generation) {
            generation = code_generation;
            Clear();
        }
    }

private:
    static constexpr std::size_t HeaderWords = 5;
    static constexpr std::size_t MaxArenaWords = std::size_t{1} << 22; // 16 MiB

    static std::uint64_t Hash(std::uint32_t start, std::span<const std::uint32_t> parameters) {
        std::uint64_t hash = 0x9e3779b97f4a7c15ULL ^ (std::uint64_t{start} << 32) ^ parameters.size();
        for (const std::uint32_t word : parameters) {
            hash = (hash ^ word) * 0xff51afd7ed558ccdULL;
            hash ^= hash >> 32;
        }
        return hash;
    }

    void Clear() {
        arena.clear();
        entries.clear();
        last_by_start.fill({0, NoRecord});
    }

    /// Copies the records still in the map to a new arena. A call stored again with another
    /// shape leaves its old record behind: title 1 filled the arena with them every ~2 s, and
    /// clearing it made the next ~20,000 calls interpret again. Clears only when the live
    /// records alone would fill most of it.
    void Compact() {
        compacted.clear();
        for (auto& entry : entries) {
            const std::uint32_t* record = arena.data() + entry.second;
            const std::size_t words =
                HeaderWords + record[1] + std::size_t{2} * (record[2] + record[3] + record[4]);
            entry.second = static_cast<std::uint32_t>(compacted.size());
            compacted.insert(compacted.end(), record, record + words);
        }
        arena.swap(compacted);
        last_by_start.fill({0, NoRecord});
        ++statistics.compactions;
        if (arena.size() >= MaxArenaWords / 4 * 3) {
            Clear();
            ++statistics.clears;
        }
    }

    Map entries;
    std::vector<std::uint32_t> arena;
    std::vector<std::uint32_t> compacted; ///< Compact's new arena (kept for its capacity)
    /// The last record found or stored per macro start (direct mapped).
    std::array<std::pair<std::uint32_t, std::uint32_t>, 256> last_by_start{};
    std::uint64_t generation{};
    Statistics statistics;
};

} // namespace Tegra

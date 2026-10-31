// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Service::Nvidia::Devices {

class Nx2GpuMappingTable {
public:
    struct Range {
        u64 gpu;
        u64 cpu;
        u64 size;
    };
    struct Change {
        Range range;
        bool map;
        bool native;
    };
    struct Update {
        bool success{};
        u64 lease{};
        std::vector<Change> changes;
    };

    static bool Valid(u64 address, u64 size) {
        return size != 0 && size <= std::numeric_limits<u64>::max() - address;
    }

    static std::optional<Range> ImageRange(u64 address, u64 bound, u64 offset, u64 size) {
        if (address == 0 || size == 0 || offset > bound || size > bound - offset ||
            !Valid(address, bound)) {
            return std::nullopt;
        }
        const u64 first = address + offset;
        const u64 last = first + size - 1;
        if (last > std::numeric_limits<u64>::max() - 0x1000) {
            return std::nullopt;
        }
        const u64 start = first & ~u64{0xfff};
        const u64 end = (last + 0x1000) & ~u64{0xfff};
        if (start < address || end > address + bound) {
            return std::nullopt;
        }
        return Range{start, start, end - start};
    }

    bool HasNativeOverlap(u64 gpu, u64 size) const {
        for (const auto& [id, range] : leases) {
            if (Overlaps(gpu, size, range.gpu, range.size)) {
                return true;
            }
        }
        return false;
    }

    Update Map(u64 gpu, u64 cpu, u64 size) {
        if (!Valid(gpu, size) || !Valid(cpu, size)) {
            return {};
        }
        for (const auto& [id, lease] : leases) {
            if (Overlaps(gpu, size, lease.gpu, lease.size) && cpu != gpu) {
                return {};
            }
        }
        const auto before = Effective();
        Trim(gpu, size);
        const auto position = std::upper_bound(mappings.begin(), mappings.end(), gpu,
                                               [](u64 address, const Range& range) {
            return address < range.gpu;
        });
        mappings.insert(position, {gpu, cpu, size});
        return {true, 0, Difference(before)};
    }

    Update Unmap(u64 gpu, u64 size) {
        if (!Valid(gpu, size)) {
            return {};
        }
        const auto before = Effective();
        Trim(gpu, size);
        return {true, 0, Difference(before)};
    }

    Update Acquire(u64 address, u64 size) {
        if (!Valid(address, size)) {
            return {};
        }
        for (const auto& range : mappings) {
            if (Overlaps(address, size, range.gpu, range.size) && range.cpu != range.gpu) {
                return {};
            }
        }
        const auto before = Effective();
        const u64 id = next_lease++;
        leases.emplace(id, Range{address, address, size});
        return {true, id, Difference(before)};
    }

    Update Release(u64 id) {
        if (!leases.contains(id)) {
            return {};
        }
        const auto before = Effective();
        leases.erase(id);
        return {true, 0, Difference(before)};
    }

    bool UsesBacking(u64 cpu, u64 size) const {
        for (const auto& range : mappings) {
            if (Overlaps(cpu, size, range.cpu, range.size)) {
                return true;
            }
        }
        for (const auto& [id, range] : leases) {
            if (Overlaps(cpu, size, range.cpu, range.size)) {
                return true;
            }
        }
        return false;
    }

    std::vector<Range> Effective() const {
        if (leases.empty()) {
            return mappings;
        }
        std::vector<u64> boundaries;
        for (const auto& range : mappings) {
            boundaries.push_back(range.gpu);
            boundaries.push_back(range.gpu + range.size);
        }
        for (const auto& [id, range] : leases) {
            boundaries.push_back(range.gpu);
            boundaries.push_back(range.gpu + range.size);
        }
        Sort(boundaries);
        std::vector<Range> result;
        for (size_t i = 1; i < boundaries.size(); ++i) {
            const u64 start = boundaries[i - 1];
            if (const auto cpu = Lookup(start)) {
                const u64 size = boundaries[i] - start;
                if (!result.empty() && result.back().gpu + result.back().size == start &&
                    result.back().cpu + result.back().size == *cpu) {
                    result.back().size += size;
                } else {
                    result.push_back({start, *cpu, size});
                }
            }
        }
        return result;
    }

    void Reset() {
        mappings.clear();
        leases.clear();
    }

private:
    static bool Overlaps(u64 a, u64 size_a, u64 b, u64 size_b) {
        return a < b + size_b && b < a + size_a;
    }

    static void Sort(std::vector<u64>& boundaries) {
        std::ranges::sort(boundaries);
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    }

    std::optional<u64> Lookup(u64 gpu) const {
        if (const auto cpu = Lookup(mappings, gpu)) {
            return cpu;
        }
        for (const auto& [id, range] : leases) {
            if (gpu >= range.gpu && gpu - range.gpu < range.size) {
                return gpu;
            }
        }
        return std::nullopt;
    }

    static std::optional<u64> Lookup(const std::vector<Range>& ranges, u64 gpu) {
        auto it = std::upper_bound(ranges.begin(), ranges.end(), gpu,
                                   [](u64 address, const Range& range) {
            return address < range.gpu;
        });
        if (it == ranges.begin()) {
            return std::nullopt;
        }
        --it;
        if (gpu - it->gpu < it->size) {
            return it->cpu + (gpu - it->gpu);
        }
        return std::nullopt;
    }

    std::vector<Change> Difference(const std::vector<Range>& before) const {
        const auto after = Effective();
        std::vector<u64> boundaries;
        for (const auto* ranges : {&before, &after}) {
            for (const auto& range : *ranges) {
                boundaries.push_back(range.gpu);
                boundaries.push_back(range.gpu + range.size);
            }
        }
        Sort(boundaries);
        std::vector<Change> changes;
        for (size_t i = 1; i < boundaries.size(); ++i) {
            const u64 start = boundaries[i - 1];
            const auto old_cpu = Lookup(before, start);
            const auto new_cpu = Lookup(after, start);
            if (old_cpu == new_cpu) {
                continue;
            }
            const bool native = new_cpu && !Lookup(mappings, start);
            const bool map = new_cpu.has_value();
            const u64 cpu = new_cpu.value_or(old_cpu.value_or(0));
            const u64 size = boundaries[i] - start;
            if (!changes.empty() && changes.back().map == map && changes.back().native == native &&
                changes.back().range.gpu + changes.back().range.size == start &&
                changes.back().range.cpu + changes.back().range.size == cpu) {
                changes.back().range.size += size;
            } else {
                changes.push_back({{start, cpu, size}, map, native});
            }
        }
        return changes;
    }

    void Trim(u64 gpu, u64 size) {
        std::vector<Range> kept;
        const u64 end = gpu + size;
        for (const auto& range : mappings) {
            const u64 range_end = range.gpu + range.size;
            if (!Overlaps(gpu, size, range.gpu, range.size)) {
                kept.push_back(range);
                continue;
            }
            if (range.gpu < gpu) {
                kept.push_back({range.gpu, range.cpu, gpu - range.gpu});
            }
            if (range_end > end) {
                kept.push_back({end, range.cpu + (end - range.gpu), range_end - end});
            }
        }
        mappings = std::move(kept);
    }

    std::vector<Range> mappings;
    std::map<u64, Range> leases;
    u64 next_lease{1};
};

} // namespace Service::Nvidia::Devices

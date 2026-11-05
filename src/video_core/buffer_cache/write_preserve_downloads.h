// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <map>

#include "common/common_types.h"

namespace VideoCommon {

/// Optional eager-download hints learned from partial CPU writes. These are not CPU-read
/// (Preflushable) marks: hinted pages must retain their ordinary read traps and dirty state.
/// One sparse 128-byte mask covers 4 MiB; the 36-bit device space bounds all masks to 2 MiB.
/// Ownership follows the buffer-cache instance. Forget removes stale hints on unmap/deletion.
class WritePreserveDownloadHints {
public:
    static constexpr u64 AddressLimit = 1ULL << 36;
    static constexpr u64 PageSize = 4096;
    static constexpr u64 BlockSize = 4 * 1024 * 1024;

    /// Learn only pages with GPU-written bytes outside this CPU write. Even a large write can
    /// contribute at most its two partial boundary pages. The GPU range set is never modified.
    template <typename WrittenRanges>
    void LearnFromWrite(DAddr address, u64 size, const WrittenRanges& gpu_written) {
        if (!ValidRange(address, size)) {
            return;
        }
        const DAddr end = address + size;
        const DAddr first_page = address & ~(PageSize - 1);
        if (address != first_page && gpu_written.Intersects(first_page, address - first_page)) {
            MarkPage(first_page);
        }
        if (end % PageSize != 0) {
            const DAddr last_page = end & ~(PageSize - 1);
            if (gpu_written.Intersects(end, last_page + PageSize - end)) {
                MarkPage(last_page);
            }
        }
    }

    /// Ordered, coalesced hints clipped to the exact requested byte range. No allocation or
    /// mutation occurs during iteration, including queries spanning unallocated blocks.
    template <typename Func>
    void ForEachRange(DAddr address, u64 size, Func&& func) const {
        if (!ValidRange(address, size)) {
            return;
        }
        const DAddr end = address + size;
        const u64 first_page = address / PageSize;
        const u64 end_page = (end - 1) / PageSize + 1;
        DAddr pending_begin = 0;
        DAddr pending_end = 0;
        bool pending = false;
        for (auto it = blocks.lower_bound(address / BlockSize);
             it != blocks.end() && it->first <= (end - 1) / BlockSize; ++it) {
            for (size_t word = 0; word < WordsPerBlock; ++word) {
                const u64 word_page = it->first * PagesPerBlock + word * 64;
                u64 bits = it->second[word] & WordMask(word_page, first_page, end_page);
                while (bits != 0) {
                    const DAddr page = (word_page + std::countr_zero(bits)) * PageSize;
                    bits &= bits - 1;
                    const DAddr begin = (std::max)(page, address);
                    const DAddr page_end = (std::min)(page + PageSize, end);
                    if (pending && pending_end != begin) {
                        func(pending_begin, pending_end - pending_begin);
                        pending = false;
                    }
                    if (!pending) {
                        pending_begin = begin;
                        pending = true;
                    }
                    pending_end = page_end;
                }
            }
        }
        if (pending) {
            func(pending_begin, pending_end - pending_begin);
        }
    }

    /// Forget whole overlapping pages. Dropping a hint only loses an optimization.
    void Forget(DAddr address, u64 size) {
        if (!ValidRange(address, size)) {
            return;
        }
        const DAddr end = address + size;
        const u64 first_page = address / PageSize;
        const u64 end_page = (end - 1) / PageSize + 1;
        for (auto it = blocks.lower_bound(address / BlockSize);
             it != blocks.end() && it->first <= (end - 1) / BlockSize;) {
            for (size_t word = 0; word < WordsPerBlock; ++word) {
                const u64 word_page = it->first * PagesPerBlock + word * 64;
                it->second[word] &= ~WordMask(word_page, first_page, end_page);
            }
            if (std::ranges::all_of(it->second, [](u64 bits) { return bits == 0; })) {
                it = blocks.erase(it);
            } else {
                ++it;
            }
        }
    }

    void Clear() { blocks.clear(); }
    [[nodiscard]] size_t BlockCount() const { return blocks.size(); }

private:
    static constexpr size_t PagesPerBlock = BlockSize / PageSize;
    static constexpr size_t WordsPerBlock = PagesPerBlock / 64;
    using Mask = std::array<u64, WordsPerBlock>;

    static bool ValidRange(DAddr address, u64 size) {
        return size != 0 && address < AddressLimit && size <= AddressLimit - address;
    }

    static u64 WordMask(u64 word_page, u64 first_page, u64 end_page) {
        const u64 begin = (std::max)(word_page, first_page);
        const u64 end = (std::min)(word_page + 64, end_page);
        if (begin >= end) {
            return 0;
        }
        const u64 low = ~u64{0} << (begin - word_page);
        const u64 high = end - word_page == 64 ? ~u64{0} : (u64{1} << (end - word_page)) - 1;
        return low & high;
    }

    void MarkPage(DAddr address) {
        const u64 page = (address % BlockSize) / PageSize;
        blocks[address / BlockSize][page / 64] |= u64{1} << (page % 64);
    }

    std::map<u64, Mask> blocks;
};

/// Combine historical CPU-read pages with optional eager-download hints. Only the former may
/// omit read traps: hinted pages still need a reactive flush if their download is pending.
template <typename Tracker, typename Download, typename Trap>
void ForEachWrittenDownloadRange(Tracker& tracker, const WritePreserveDownloadHints& hints,
                                DAddr address, u64 size, Download&& download, Trap&& trap) {
    if (size == 0 || address >= WritePreserveDownloadHints::AddressLimit ||
        size > WritePreserveDownloadHints::AddressLimit - address) {
        return;
    }
    const auto unknown = [&](DAddr begin, u64 length) {
        hints.ForEachRange(begin, length, download);
        trap(begin, length);
    };
    DAddr cursor = address;
    tracker.ForEachPreflushableRange(address, size, [&](DAddr begin, u64 length) {
        if (cursor < begin) {
            unknown(cursor, begin - cursor);
        }
        download(begin, length);
        cursor = begin + length;
    });
    const DAddr end = address + size;
    if (cursor < end) {
        unknown(cursor, end - cursor);
    }
}

} // namespace VideoCommon

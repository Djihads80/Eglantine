// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <type_traits>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

/// Device pages whose guest memory lost the GPU's data (TextureCache::MarkStale), one bit each.
/// Mark and Clear call on_run(addr, size) for every run of pages whose state they changed.
class StalePages {
public:
    /// Device memory pages (Core::Memory::YUZU_PAGEBITS), not the texture cache's lookup pages.
    static constexpr u64 PAGE_BITS = 12;
    static constexpr u64 PAGE_SIZE = u64{1} << PAGE_BITS;

    /// Marks the pages wholly inside [addr, addr + size): a page shared with other data keeps
    /// that data's state.
    template <typename OnRun>
    void Mark(DAddr addr, u64 size, OnRun&& on_run) {
        const u64 first = (addr + PAGE_SIZE - 1) >> PAGE_BITS;
        const u64 end = (addr + size) >> PAGE_BITS;
        if (end <= first || end > MAX_PAGES) {
            return;
        }
        if (bits.size() * 64 < end) {
            bits.resize((end + 63) / 64);
        }
        Run<std::remove_reference_t<OnRun>> run{on_run};
        for (u64 page = first; page < end; ++page) {
            u64& word = bits[page / 64];
            const u64 bit = u64{1} << (page % 64);
            if ((word & bit) != 0) {
                run.Flush();
                continue;
            }
            word |= bit;
            ++count;
            run.Add(page);
        }
        run.Flush();
    }

    /// Clears every page [addr, addr + size) touches.
    template <typename OnRun>
    void Clear(DAddr addr, u64 size, OnRun&& on_run) {
        if (count == 0 || size == 0) {
            return;
        }
        const u64 first = addr >> PAGE_BITS;
        const u64 end = (std::min<u64>)((addr + size + PAGE_SIZE - 1) >> PAGE_BITS, bits.size() * 64);
        Run<std::remove_reference_t<OnRun>> run{on_run};
        for (u64 page = first; page < end;) {
            u64& word = bits[page / 64];
            if (word == 0 && page % 64 == 0) {
                run.Flush();
                page += 64;
                continue;
            }
            const u64 bit = u64{1} << (page % 64);
            if ((word & bit) != 0) {
                word &= ~bit;
                --count;
                run.Add(page);
            } else {
                run.Flush();
            }
            ++page;
        }
        run.Flush();
    }

    /// Whether every page [addr, addr + size) touches is marked.
    [[nodiscard]] bool IsAll(DAddr addr, u64 size) const {
        if (count == 0 || size == 0) {
            return false;
        }
        const u64 first = addr >> PAGE_BITS;
        const u64 end = (addr + size + PAGE_SIZE - 1) >> PAGE_BITS;
        if (end > bits.size() * 64) {
            return false;
        }
        for (u64 page = first; page < end;) {
            const u64 word = bits[page / 64];
            if (page % 64 == 0 && page + 64 <= end) {
                if (word != ~u64{0}) {
                    return false;
                }
                page += 64;
                continue;
            }
            if (((word >> (page % 64)) & 1) == 0) {
                return false;
            }
            ++page;
        }
        return true;
    }

    [[nodiscard]] u64 Count() const {
        return count;
    }

private:
    /// Bounds the bitmap (1 TiB of device memory); the stand-in addresses of images without CPU
    /// backing lie far past it.
    static constexpr u64 MAX_PAGES = u64{1} << 28;

    template <typename OnRun>
    struct Run {
        OnRun& on_run;
        u64 begin{};
        u64 pages{};

        void Add(u64 page) {
            if (pages == 0) {
                begin = page;
            }
            ++pages;
        }
        void Flush() {
            if (pages != 0) {
                on_run(begin << PAGE_BITS, pages << PAGE_BITS);
                pages = 0;
            }
        }
    };

    std::vector<u64> bits;
    u64 count{};
};

} // namespace VideoCommon

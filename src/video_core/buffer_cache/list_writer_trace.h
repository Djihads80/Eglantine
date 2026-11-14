// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <map>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

// Diagnostic only. CompleteCopy may read bytes only after the existing GPU retirement check.
class ListWriterTrace {
public:
    static constexpr size_t MaxPageBytes = 4096;
    struct Limits {
        size_t pages = 4096;
        size_t pending_copies = 8192;
        size_t shaders = 256;
        size_t page_bytes = 4096;
    };
    struct Stats {
        u64 completed{}, baselines{}, unchanged{}, changed{}, adjacent_changed{}, recent_changed{};
        u64 mixed_changed{}, incomparable{}, out_of_order{};
        u64 read_copies{}, read_changed{}, adjacent_read_changed{}, recent_read_changed{};
    };

    ListWriterTrace() = default;
    explicit ListWriterTrace(Limits limits_) : limits{limits_} {}

    void BeginDispatch(u64 shader) {
        active_scope = ++scope_serial;
        active_shader = shader;
        scope_hits = 0;
        recent_input = false;
    }
    void EndDispatch() {
        active_scope = active_shader = scope_hits = 0;
        recent_input = false;
    }
    void SetRecentInput(bool recent) {
        recent_input = recent;
    }
    [[nodiscard]] bool HasScopeWrites() const {
        return scope_hits != 0;
    }

    void NoteGpuWrite(DAddr page, u64 previous_version, bool collecting) {
        Page* state = FindPage(page);
        if (collecting && active_scope != 0) {
            ++scope_hits;
            if (state && state->scope != active_scope) {
                state->scope = active_scope;
                state->begin_version = previous_version;
            }
        } else if (state) {
            state->scope = 0;
        }
    }
    void NoteHostWrite(DAddr page) {
        if (auto it = pages.find(page); it != pages.end()) {
            it->second.scope = 0;
        }
    }

    void RegisterCopy(uintptr_t key, DAddr page, u64 version, u32 begin, u32 end) {
        if (copies.erase(key) != 0 || copies.size() >= limits.pending_copies || begin >= end ||
            end > limits.page_bytes || end > MaxPageBytes) {
            ++dropped;
            return;
        }
        Page* state = FindPage(page);
        if (!state) {
            return;
        }
        const bool scoped = active_scope != 0 && state->scope == active_scope;
        copies.emplace(key, Copy{page, version, version, scoped ? state->begin_version : 0,
                                 scoped ? active_shader : 0, begin, end, scoped, recent_input});
    }
    // Host writes can update a completed staging copy in place before it is retired.
    void FreezeCopy(uintptr_t key, u64 final_version) {
        if (auto it = copies.find(key); it != copies.end()) {
            it->second.version = final_version;
        }
    }

    void NoteFrontRead(uintptr_t key, size_t begin, size_t size) {
        if (auto it = copies.find(key); it != copies.end()) {
            Copy& copy = it->second;
            if (begin < copy.begin || begin > copy.end || size > copy.end - begin) {
                ++dropped;
                return;
            }
            for (size_t offset = begin; offset < begin + size; ++offset) {
                copy.read_mask[offset / 64] |= u64{1} << (offset % 64);
            }
        }
    }

    bool CompleteCopy(uintptr_t key, std::span<const u8> bytes) {
        const auto it = copies.find(key);
        if (it == copies.end()) {
            return false;
        }
        const Copy copy = it->second;
        copies.erase(it);
        if (copy.end > bytes.size()) {
            ++dropped;
            return false;
        }
        Page& state = pages.at(copy.page);
        if (!stats.contains(copy.shader) && stats.size() >= limits.shaders) {
            ++dropped;
            state.bytes.clear();
            return false;
        }
        Stats& count = stats[copy.shader];
        ++count.completed;
        count.read_copies +=
            std::ranges::any_of(copy.read_mask, [](u64 word) { return word != 0; });
        ++completed;
        const auto payload = bytes.subspan(copy.begin, copy.end - copy.begin);
        if (!state.bytes.empty() && copy.version < state.version) {
            ++count.out_of_order;
            return completed % 2000 == 0;
        }
        if (state.bytes.empty()) {
            ++count.baselines;
        } else if (state.begin != copy.begin || state.bytes.size() != payload.size()) {
            ++count.incomparable;
        } else if (std::equal(payload.begin(), payload.end(), state.bytes.begin())) {
            ++count.unchanged;
        } else {
            ++count.changed;
            const bool adjacent = copy.scoped && copy.version == copy.recorded_version &&
                                  state.version == copy.begin_version;
            bool read_changed = false;
            for (size_t index = 0; index < payload.size(); ++index) {
                const size_t offset = copy.begin + index;
                read_changed |= payload[index] != state.bytes[index] &&
                                (copy.read_mask[offset / 64] & (u64{1} << (offset % 64))) != 0;
            }
            read_changed &= copy.version == copy.recorded_version;
            count.read_changed += read_changed;
            count.adjacent_read_changed += read_changed && adjacent;
            count.recent_read_changed += read_changed && adjacent && copy.recent_input;
            if (adjacent) {
                ++count.adjacent_changed;
                count.recent_changed += copy.recent_input;
            } else {
                ++count.mixed_changed;
            }
        }
        state.bytes.assign(payload.begin(), payload.end());
        state.begin = copy.begin;
        state.version = copy.version;
        return completed % 2000 == 0;
    }

    [[nodiscard]] const std::map<u64, Stats>& ShaderStats() const {
        return stats;
    }
    [[nodiscard]] u64 Dropped() const {
        return dropped;
    }
    [[nodiscard]] size_t PageCount() const {
        return pages.size();
    }
    [[nodiscard]] size_t PendingCount() const {
        return copies.size();
    }

private:
    struct Page {
        u64 scope{}, begin_version{}, version{};
        u32 begin{};
        std::vector<u8> bytes;
    };
    struct Copy {
        DAddr page;
        u64 recorded_version, version, begin_version, shader;
        u32 begin, end;
        bool scoped, recent_input;
        std::array<u64, MaxPageBytes / 64> read_mask{};
    };
    Page* FindPage(DAddr page) {
        if (auto it = pages.find(page); it != pages.end()) {
            return &it->second;
        }
        if (pages.size() >= limits.pages) {
            ++dropped;
            return nullptr;
        }
        return &pages.try_emplace(page).first->second;
    }

    Limits limits;
    std::unordered_map<DAddr, Page> pages;
    std::unordered_map<uintptr_t, Copy> copies;
    std::map<u64, Stats> stats;
    u64 scope_serial{}, active_scope{}, active_shader{}, scope_hits{}, completed{}, dropped{};
    bool recent_input{};
};

} // namespace VideoCommon

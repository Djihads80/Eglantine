// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string_view>
#include <type_traits>
#include <unordered_map>

namespace Tegra {

// Native QMD 3.0 dependency scheduling; nonempty grids require a dispatch backend.
class NativeQmdScheduler {
public:
    using Word = std::uint32_t;
    using Address = std::uint64_t;
    using Descriptor = std::array<Word, 64>;
    struct RejectShaderDispatch { bool operator()(const Descriptor&) const { return false; } };
    // Optional diagnostics: called with "load"/"count"/"defer"/"run" events.
    std::function<void(std::string_view, Address, std::int32_t)> trace;
    template <typename Read, typename Release>
    std::string_view Signal(Address address, Word argument, Read read, Release release) {
        return Signal(address, argument, read, release, RejectShaderDispatch{});
    }
    template <typename Read, typename Release, typename Dispatch>
    std::string_view Signal(Address address, Word argument, Read read, Release release, Dispatch dispatch) {
        struct Pending { Address address; Word action; Word count; };
        std::string_view error;
        std::array<Pending, 256> pending{};
        std::size_t count = 1;
        pending[0] = {address, argument & 15, ((argument >> 14) & 0x3ff) + 1};
        for (std::size_t i = 0; i < count; ++i) {
            const auto request = pending[i];
            if (!Reserve(request.address)) return "QMD cache limit";
            const auto [slot, inserted] = states.try_emplace(request.address);
            if (inserted) {
                order.push_back(request.address);
            }
            auto& state = slot->second;
            if (request.action == 3 || request.action == 2) {
                if (request.action == 3 || !state.loaded) {
                    if (!read(request.address, state.descriptor)) return "unmapped QMD";
                    const auto& qmd = state.descriptor;
                    if ((qmd[18] & 0xff) != 0x30 || (qmd[4] & 0x100)) return "unsupported QMD version or queue";
                    if constexpr (std::is_same_v<Dispatch, RejectShaderDispatch>) {
                        if (qmd[12] && (qmd[13] & 0xffff) && (qmd[14] & 0xffff))
                            return "native shader dispatch required";
                    }
                    state.dependencies = std::int32_t((qmd[4] >> 14) & 0xffff) + state.deferred;
                    if (trace) trace("base", request.address, std::int32_t((qmd[4] >> 14) & 0xffff));
                    if (trace) trace("load", request.address, state.dependencies);
                    state.deferred = 0;
                    state.loaded = true;
                    state.completed = false;
                }
                state.scheduled = true;
            } else if (request.action == 7 || request.action == 12) {
                const auto delta = request.action == 7 ? -std::int32_t(request.count) : std::int32_t(request.count);
                if (!state.loaded || state.completed) {
                    state.deferred += delta;
                    if (trace) trace("defer", request.address, state.deferred);
                    continue;
                }
                state.dependencies += delta;
                if (trace) trace("count", request.address, state.dependencies);
            } else if (request.action == 0 || request.action == 1) {
                continue;
            } else {
                return "unsupported PCAS action";
            }
            if (!state.loaded || !state.scheduled || state.completed || state.dependencies > 0) continue;
            if (state.dependencies < 0) return "QMD dependency underflow";
            // The SDK patches release payloads while a scheduled QMD is
            // waiting for its remaining dependencies. Fetch them at execution.
            if (!read(request.address, state.descriptor)) return "unmapped QMD";
            const auto qmd = state.descriptor;
            if ((qmd[18] & 0xff) != 0x30 || (qmd[4] & 0x100)) return "unsupported QMD version or queue";
            // Validate the release slots before making any visible.
            for (const Word base : ReleaseSlots) {
                const Word flags = qmd[base + 1];
                if (!(flags & (1U << 23))) continue;
                const bool add = (flags & (1U << 26)) != 0;
                if ((flags & (0x3bU << 24)) || (flags >> 30) != 1 ||
                    (add && (flags & (7U << 20)))) return "unsupported QMD release format";
                if (add) {
                    if constexpr (!std::is_invocable_r_v<bool, Release, Address, Word, Word>)
                        return "unsupported QMD reduction callback";
                }
            }
            // A program Eden can't run is skipped, but the QMD still completes: its releases
            // are what other queues and the CPU wait on (the console never fails a launch).
            const bool dispatched = !(qmd[12] && (qmd[13] & 0xffff) && (qmd[14] & 0xffff)) || dispatch(qmd);
            if (!dispatched) error = "native shader dispatch required";
            if (trace) trace("run", request.address, dispatched ? 1 : 0);
            state.completed = true;
            for (const Word base : ReleaseSlots) {
                const Word flags = qmd[base + 1];
                if (!(flags & (1U << 23))) continue;
                const Address destination = (Address{flags & 0xff} << 32) | qmd[base];
                bool released{};
                if constexpr (std::is_invocable_r_v<bool, Release, Address, Word, Word>) {
                    released = release(destination, qmd[base + 2], flags);
                } else {
                    released = release(destination, qmd[base + 2]);
                }
                if (!released) return "unmapped QMD release";
            }
            if (qmd[16] & 1) {
                const Word action = (qmd[16] >> 1) & 7;
                if (action != 4 && action != 3 && action != 1) return "unsupported dependent QMD action";
                if (count == pending.size()) return "QMD chain limit";
                pending[count++] = {Address{qmd[15]} << 8, action == 4 ? 7U : action == 3 ? 3U : 2U, 1};
            }
            if (qmd[16] & (1U << 5)) return "second dependent QMD unsupported";
        }
        return error;
    }
public:
    // Bound on the per-QMD dependency table. Reached by bump-allocated launches within seconds of
    // gameplay, so it is a cache size, not a launch limit: finished entries are recycled.
    static constexpr std::size_t StateLimit = 4096;
    // Finished entries recycled since start; only used by diagnostics and tests.
    static inline std::size_t recycled{};

private:
    // Release slots (address low, flags | address high, payload). NVN2 patches a third one at
    // word 52 for queue syncs (e.g. a compute queue releasing a semaphore another queue waits on).
    static constexpr std::array<Word, 3> ReleaseSlots{24, 28, 52};

    struct State {
        Descriptor descriptor{};
        std::int32_t dependencies{};
        std::int32_t deferred{};
        bool loaded{};
        bool scheduled{};
        bool completed{};
    };
    // One table for every channel: a QMD's dependency counter is state of the QMD itself, and
    // title 1 schedules a QMD on one channel while another decrements it (a decrement that
    // arrived first was kept by the other channel's scheduler and the QMD never launched).
    static inline std::unordered_map<Address, State> states;
    // Insertion order of `states`, oldest first, so finished entries can be recycled.
    static inline std::deque<Address> order;

    // Titles bump-allocate a fresh QMD for every compute launch, so the table reaches its bound
    // within seconds of gameplay. Refusing a launch there is not an option: a QMD's releases are
    // what other queues and the CPU wait on, and dropping them parked a queue on a semaphore that
    // never advanced (Kirby Air Riders froze on its first dialog). Recycle the finished entries
    // instead, which is what the console's own launch slots do. A recycled address that comes back
    // simply starts unloaded, so a later decrement stays deferred exactly as it did before.
    static bool Reserve(Address address) {
        if (states.contains(address) || states.size() < StateLimit) {
            return true;
        }
        std::size_t examined = order.size();
        while (examined-- > 0 && states.size() >= StateLimit) {
            const Address candidate = order.front();
            order.pop_front();
            const auto entry = states.find(candidate);
            if (entry == states.end()) {
                continue;
            }
            if (!entry->second.completed || entry->second.deferred != 0) {
                order.push_back(candidate);
                continue;
            }
            states.erase(entry);
            ++recycled;
        }
        return states.size() < StateLimit;
    }
};
} // namespace Tegra

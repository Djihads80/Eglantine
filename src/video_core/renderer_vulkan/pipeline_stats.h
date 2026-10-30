// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Measurement only: where the time goes when pipelines are created. Cheap atomics, one log line
// per active (thread role, pipeline kind) every 10 s plus the totals since boot.
// Disabled with the environment variable EDEN_NX2_PIPELINE_STATS=0.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <utility>

#include "common/logging.h"

namespace Vulkan::PipelineStats {

enum class Role : unsigned {
    Gpu,   // any thread not listed below (the GPU thread)
    Xlate, // translation_workers (async native graphics pipelines)
    Par,   // `workers` threads running the queued driver compile (build_in_parallel)
    Boot,  // disk cache replay
    Count,
};
enum class Kind : unsigned { Graphics, Compute, Count };
enum class Stage : unsigned { Lower, Translate, Emit, Module, Driver, Count };

inline constexpr size_t NumRoles = static_cast<size_t>(Role::Count);
inline constexpr size_t NumKinds = static_cast<size_t>(Kind::Count);
inline constexpr size_t NumStages = static_cast<size_t>(Stage::Count);

inline bool Enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("EDEN_NX2_PIPELINE_STATS");
        return !(text && text[0] == '0');
    }();
    return enabled;
}

inline Role& CurrentRole() {
    thread_local Role role = Role::Gpu;
    return role;
}

// Marks the current thread's role for the lifetime of the object.
class ScopedRole {
public:
    explicit ScopedRole(Role role) : previous{CurrentRole()} {
        CurrentRole() = role;
    }
    ~ScopedRole() {
        CurrentRole() = previous;
    }
    ScopedRole(const ScopedRole&) = delete;
    ScopedRole& operator=(const ScopedRole&) = delete;

private:
    Role previous;
};

class Registry {
public:
    struct Cell {
        std::atomic<uint64_t> pipelines{};
        std::atomic<uint64_t> repeat_combos{};
        std::atomic<uint64_t> translations{};
        std::atomic<uint64_t> new_programs{};
        std::array<std::atomic<uint64_t>, NumStages> count{};
        std::array<std::atomic<uint64_t>, NumStages> nanos{};
    };
    struct Snapshot {
        uint64_t pipelines{}, repeat_combos{}, translations{}, new_programs{};
        std::array<uint64_t, NumStages> count{};
        std::array<uint64_t, NumStages> nanos{};
        bool Active() const {
            return pipelines || translations || count != std::array<uint64_t, NumStages>{};
        }
    };

    static Registry& Get() {
        static Registry registry;
        return registry;
    }

    void AddTime(Role role, Kind kind, Stage stage, int64_t nanos) {
        Cell& c = CellOf(role, kind);
        c.count[static_cast<size_t>(stage)].fetch_add(1, std::memory_order_relaxed);
        c.nanos[static_cast<size_t>(stage)].fetch_add(static_cast<uint64_t>(nanos),
                                                       std::memory_order_relaxed);
        MaybeReport();
    }

    // One pipeline creation; `combo` identifies its shader-hash combination.
    void NotePipeline(Kind kind, uint64_t combo) {
        const Role role = CurrentRole();
        Cell& c = CellOf(role, kind);
        c.pipelines.fetch_add(1, std::memory_order_relaxed);
        bool inserted;
        {
            std::scoped_lock lock{sets_mutex};
            inserted = combos[role == Role::Boot][static_cast<size_t>(kind)].insert(combo).second;
        }
        if (!inserted) {
            c.repeat_combos.fetch_add(1, std::memory_order_relaxed);
        }
        MaybeReport();
    }

    // One program translation; `stage_slot` is the pipeline stage index (5 for compute).
    void NoteTranslation(Kind kind, unsigned stage_slot, uint64_t hash) {
        const Role role = CurrentRole();
        Cell& c = CellOf(role, kind);
        c.translations.fetch_add(1, std::memory_order_relaxed);
        bool inserted;
        {
            std::scoped_lock lock{sets_mutex};
            inserted = programs[role == Role::Boot][static_cast<size_t>(kind)]
                           .insert({stage_slot, hash})
                           .second;
        }
        if (inserted) {
            c.new_programs.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    static constexpr int64_t WindowNanos = 10'000'000'000;

    Registry() : last_report{Now()} {}

    static int64_t Now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    Cell& CellOf(Role role, Kind kind) {
        return cells[static_cast<size_t>(role)][static_cast<size_t>(kind)];
    }

    static Snapshot Load(const Cell& c) {
        Snapshot s;
        s.pipelines = c.pipelines.load(std::memory_order_relaxed);
        s.repeat_combos = c.repeat_combos.load(std::memory_order_relaxed);
        s.translations = c.translations.load(std::memory_order_relaxed);
        s.new_programs = c.new_programs.load(std::memory_order_relaxed);
        for (size_t i = 0; i < NumStages; ++i) {
            s.count[i] = c.count[i].load(std::memory_order_relaxed);
            s.nanos[i] = c.nanos[i].load(std::memory_order_relaxed);
        }
        return s;
    }

    static void Print(const char* scope, Role role, Kind kind, const Snapshot& s) {
        static constexpr std::array role_names{"gpu", "xlate", "par", "boot"};
        static constexpr std::array kind_names{"graphics", "compute"};
        const auto ms = [&](Stage stage) {
            return static_cast<double>(s.nanos[static_cast<size_t>(stage)]) / 1e6;
        };
        const auto n = [&](Stage stage) { return s.count[static_cast<size_t>(stage)]; };
        LOG_INFO(Render_Vulkan,
                 "PipelineStats [{}] role={} kind={} pipelines={} repeat_combos={} translations={} "
                 "new_programs={} | lower {}x{:.1f}ms translate {}x{:.1f}ms emit {}x{:.1f}ms "
                 "module {}x{:.1f}ms driver {}x{:.1f}ms",
                 scope, role_names[static_cast<size_t>(role)], kind_names[static_cast<size_t>(kind)],
                 s.pipelines, s.repeat_combos, s.translations, s.new_programs, n(Stage::Lower),
                 ms(Stage::Lower), n(Stage::Translate), ms(Stage::Translate), n(Stage::Emit),
                 ms(Stage::Emit), n(Stage::Module), ms(Stage::Module), n(Stage::Driver),
                 ms(Stage::Driver));
    }

    void MaybeReport() {
        int64_t last = last_report.load(std::memory_order_relaxed);
        const int64_t now = Now();
        if (now - last < WindowNanos ||
            !last_report.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            return;
        }
        std::scoped_lock lock{report_mutex};
        bool any = false;
        for (size_t r = 0; r < NumRoles; ++r) {
            for (size_t k = 0; k < NumKinds; ++k) {
                const Snapshot current = Load(cells[r][k]);
                Snapshot& before = reported[r][k];
                Snapshot delta;
                delta.pipelines = current.pipelines - before.pipelines;
                delta.repeat_combos = current.repeat_combos - before.repeat_combos;
                delta.translations = current.translations - before.translations;
                delta.new_programs = current.new_programs - before.new_programs;
                for (size_t i = 0; i < NumStages; ++i) {
                    delta.count[i] = current.count[i] - before.count[i];
                    delta.nanos[i] = current.nanos[i] - before.nanos[i];
                }
                before = current;
                if (delta.Active()) {
                    any = true;
                    Print("10s", static_cast<Role>(r), static_cast<Kind>(k), delta);
                }
            }
        }
        if (!any) {
            return;
        }
        for (size_t r = 0; r < NumRoles; ++r) {
            for (size_t k = 0; k < NumKinds; ++k) {
                const Snapshot& total = reported[r][k];
                if (total.Active()) {
                    Print("total", static_cast<Role>(r), static_cast<Kind>(k), total);
                }
            }
        }
    }

    std::array<std::array<Cell, NumKinds>, NumRoles> cells;
    std::array<std::array<Snapshot, NumKinds>, NumRoles> reported;
    std::atomic<int64_t> last_report;
    std::mutex report_mutex;
    std::mutex sets_mutex;
    // [is_boot][kind]
    std::array<std::array<std::set<uint64_t>, NumKinds>, 2> combos;
    std::array<std::array<std::set<std::pair<unsigned, uint64_t>>, NumKinds>, 2> programs;
};

// Times a scope and charges it to the current thread's role.
class ScopedTimer {
public:
    ScopedTimer(Kind kind_, Stage stage_) : kind{kind_}, stage{stage_}, active{Enabled()} {
        if (active) {
            role = CurrentRole();
            begin = std::chrono::steady_clock::now();
        }
    }
    ~ScopedTimer() {
        if (active) {
            const auto elapsed = std::chrono::steady_clock::now() - begin;
            Registry::Get().AddTime(
                role, kind, stage,
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        }
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    Kind kind;
    Stage stage;
    bool active;
    Role role{};
    std::chrono::steady_clock::time_point begin{};
};

inline void NotePipeline(Kind kind, uint64_t combo) {
    if (Enabled()) {
        Registry::Get().NotePipeline(kind, combo);
    }
}

inline void NoteTranslation(Kind kind, unsigned stage_slot, uint64_t hash) {
    if (Enabled()) {
        Registry::Get().NoteTranslation(kind, stage_slot, hash);
    }
}

} // namespace Vulkan::PipelineStats

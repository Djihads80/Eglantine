// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "common/common_types.h"
#include "common/logging.h"

// Switch 2 draw and dispatch diagnostics, all off unless one of these is set:
//
//   EDEN_NX2_DIAG=1          per-shader outcome counters: a summary line each second and a line for
//                            every distinct failure (draw: vs/ps pair, dispatch: code address) the
//                            first time and each time its count doubles.
//   EDEN_NX2_DIAG_DIR=<dir>  a control directory (see tools/nx2/shader_lab.py):
//                            - diag-state.json: every program seen (vertex, pixel, compute) with its
//                              draw/dispatch counts, key and shader hash, and the program id
//                              ("title"), written once a second;
//                            - ablate.txt: programs to skip, re-read when it changes. One entry per
//                              line: "key <hex16>" (the shader dump key, stable across runs), or
//                              "vs|ps|cs <hex>" (an address as logged); "#" starts a comment;
//                            - inspect.txt: one program key per line; for those the constant buffers
//                              bound at each draw are watched and the words that change from frame
//                              to frame are reported in diag-state.json;
//                            - draw-list: create it and the next frame's draws and dispatches are
//                              written to draw-list-<n>.tsv (the file is removed).
//   EDEN_NX2_DIAG_SAMPLES=1  with a draw-list capture, count the samples each draw passes (an occlusion
//                            query per draw, up to MaxProbeQueries) and add them to the draw list as
//                            the "samples" column and to diag-state.json as "probe". A draw that
//                            reaches the screen with real geometry but passes 0 samples is drawing
//                            nothing visible. Draws made while the game itself counts occlusion
//                            samples are not probed ("-").
//   EDEN_NX2_LAB=1|open      start tools/nx2/shader_lab.py for <dir> (Linux/macOS; "open" also opens the
//                            browser). EDEN_NX2_SHADER_DUMP and EDEN_NX2_FRAME_CAPTURE are passed on.
//                            The script is looked up as tools/nx2/shader_lab.py from the working
//                            directory, or EDEN_NX2_LAB_SCRIPT; EDEN_NX2_LAB_PYTHON picks the
//                            interpreter (default python3). The address goes to <dir>/lab-url.txt
//                            and the log; the lab exits with the emulator.
//   EDEN_NX2_ABLATE_FILE=<f> use that file instead of <dir>/ablate.txt.
namespace VideoCommon::Nx2Diag {

inline const char* Env(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? value : nullptr;
}

inline bool CountersEnabled() {
    static const bool enabled = Env("EDEN_NX2_DIAG") != nullptr;
    return enabled;
}

inline const std::filesystem::path& DrawListDir() {
    static const std::filesystem::path dir = [] {
        const char* value = Env("EDEN_NX2_DIAG_DIR");
        return value ? std::filesystem::path{value} : std::filesystem::path{};
    }();
    return dir;
}

inline const std::filesystem::path& AblateFile() {
    static const std::filesystem::path file = [] {
        if (const char* value = Env("EDEN_NX2_ABLATE_FILE")) {
            return std::filesystem::path{value};
        }
        return DrawListDir().empty() ? std::filesystem::path{} : DrawListDir() / "ablate.txt";
    }();
    return file;
}

inline bool SamplesEnabled() {
    static const bool enabled = Env("EDEN_NX2_DIAG_SAMPLES") != nullptr;
    return enabled;
}

inline constexpr u32 MaxProbeQueries = 8192;

inline std::string Hex16(u64 value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

// EDEN_NX2_LAB: starts the shader lab beside the emulator. Runs once at program start, before logging
// is up, so the outcome is kept in LabStatus() for the log and also printed to stderr.
inline std::string& LabStatus() {
    static std::string status;
    return status;
}

inline bool StartLab() {
    const char* mode = Env("EDEN_NX2_LAB");
    if (!mode) {
        return false;
    }
    std::string& status = LabStatus();
    if (DrawListDir().empty()) {
        status = "EDEN_NX2_LAB needs EDEN_NX2_DIAG_DIR";
    } else {
#ifdef _WIN32
        status = "EDEN_NX2_LAB is not supported on Windows, run tools/nx2/shader_lab.py yourself";
#else
        const auto script = std::filesystem::path{Env("EDEN_NX2_LAB_SCRIPT") ? Env("EDEN_NX2_LAB_SCRIPT")
                                                                              : "tools/nx2/shader_lab.py"};
        if (!std::filesystem::exists(script)) {
            status = "shader lab script " + script.string() +
                     " not found (run from the repository or set EDEN_NX2_LAB_SCRIPT)";
        } else {
            const auto quote = [](const std::string& text) {
                std::string quoted{'\''};
                for (const char c : text) {
                    quoted += c == '\'' ? std::string{"'\\''"} : std::string{c};
                }
                return quoted + '\'';
            };
            std::filesystem::create_directories(DrawListDir());
            // An address left by an earlier run would point at a lab that is gone.
            std::error_code ignored;
            std::filesystem::remove(DrawListDir() / "lab-url.txt", ignored);
            std::string command = std::string{Env("EDEN_NX2_LAB_PYTHON") ? Env("EDEN_NX2_LAB_PYTHON") : "python3"} +
                                  " -I " + quote(script.string()) + " --diag-dir " + quote(DrawListDir().string()) +
                                  " --parent-pid " + std::to_string(getpid());
            if (const char* dumps = Env("EDEN_NX2_SHADER_DUMP")) {
                command += " --dump-dir " + quote(dumps);
            }
            if (const char* frames = Env("EDEN_NX2_FRAME_CAPTURE")) {
                command += " --frames " + quote(frames);
            }
            if (std::string_view{mode} == "open") {
                command += " --open";
            }
            command += " >" + quote((DrawListDir() / "lab.log").string()) + " 2>&1 &";
            const int result = std::system(command.c_str());
            status = std::string{"shader lab "} + (result == 0 ? "launched" : "failed to launch") +
                     " (address in " + (DrawListDir() / "lab-url.txt").string() + ", log in " +
                     (DrawListDir() / "lab.log").string() + ")";
        }
#endif
    }
    std::fprintf(stderr, "diag: %s\n", status.c_str());
    return true;
}

// Starts the lab when the process starts, not at the first draw: with only the game list open there
// are no draws.
inline const bool g_lab_autostart = StartLab();

// diag_draw_lite: a drawing thread's cached answers per (vs, ps) and its draw counts per
// (vs, ps, outcome), folded into Diagnostics under one lock (at a present, before the 1 s
// summary, a full table or every 16 ms). Outcomes are string literals, compared by pointer.
struct LiteAnswer {
    u64 vs{};
    u64 ps{};
    u64 generation{}; ///< 0: empty (Diagnostics generations start at 1)
    u64 vkey{};
    u64 pkey{};
    bool ablated_v{};
    bool ablated_p{};
    bool inspect_v{};
    bool inspect_p{};
};

struct LitePending {
    u64 vs{};
    u64 ps{};
    const char* outcome{}; ///< nullptr: empty
    u64 vkey{};
    u64 pkey{};
    u64 n{};
    u64 instances{};
    u64 vertices{};
    u64 last_seq{};
    u64 last_frame{};
    u32 width{};
    u32 height{};
};

struct LiteTable {
    static constexpr u32 Size = 1024;
    static constexpr u32 FoldAt = Size * 3 / 4;
    std::array<LiteAnswer, Size> answers{};
    std::array<LitePending, Size> pending{};
    std::array<u16, Size> used_index{};
    u32 used{};
    u32 adds_since_check{};
    u64 generation{}; ///< The generation the pending slots' keys belong to
    u64 seq{};
    std::chrono::steady_clock::time_point last_fold{};
};

inline u32 LiteHash(u64 vs, u64 ps, const void* outcome) {
    u64 h = vs * 0x9E3779B97F4A7C15ULL;
    h ^= (ps + 0x632BE59BD9B4E019ULL) * 0xC2B2AE3D27D4EB4FULL;
    h ^= static_cast<u64>(reinterpret_cast<std::uintptr_t>(outcome)) * 0x165667B19E3779F9ULL;
    return static_cast<u32>(h ^ (h >> 32));
}

// The calling thread's table (created by the first lite draw on that thread).
inline std::unique_ptr<LiteTable>& ThreadLite() {
    thread_local std::unique_ptr<LiteTable> table;
    return table;
}

class Diagnostics {
public:
    struct Quiet {};

    static Diagnostics& Get() {
        static Diagnostics instance;
        return instance;
    }

    // diag_draw_lite verify: receives every draw synchronously, logs nothing.
    static Diagnostics& Shadow() {
        static Diagnostics instance{Quiet{}};
        return instance;
    }

    // A separate instance that logs nothing (verify shadow, tests). `counters`: folds apply the
    // Note counts as with EDEN_NX2_DIAG=1.
    explicit Diagnostics(Quiet, bool counters = false) : quiet{true}, force_counters{counters} {}

    // A draw (shader addresses as logged) or dispatch (vs = code address, ps = 0) and what became of it.
    void Note(bool dispatch, u64 vs, u64 ps, std::string_view outcome, u64 instances, u64 vertices,
              std::string_view detail = {}) {
        std::scoped_lock lock{mutex};
        NoteLocked(dispatch, vs, ps, outcome, 1, instances, vertices, detail);
        // diag_draw_lite: fold only when the summary is due, not at every dispatch.
        if (lite_used.load(std::memory_order_relaxed) &&
            std::chrono::steady_clock::now() - last_summary >= std::chrono::seconds{1}) {
            FoldOwnLocked();
        }
        SummaryLocked();
    }

    // Registry of the programs seen, for diag-state.json. kind: 'v' vertex, 'p' pixel, 'c' compute.
    void Touch(char kind, u64 address, std::string_view outcome, u64 instances, u64 vertices,
               u32 width, u32 height) {
        std::scoped_lock lock{mutex};
        TouchLocked(kind, address, KeyOf(address), outcome, 1, instances, vertices, width, height,
                    frame, nullptr);
    }

    // The shader dump's key for the program at that address, so draw lists and dumps from different
    // runs can be matched (addresses change between runs, the key is a hash of the code). `hash` is
    // its shader hash (frame dump "shaders") once it is lowered, 0 until then.
    void RegisterProgram(u64 address, u64 key, u64 hash = 0) {
        std::scoped_lock lock{mutex};
        const auto [it, inserted] = program_keys.try_emplace(address, key);
        if (inserted || it->second != key) {
            it->second = key;
            generation.fetch_add(1, std::memory_order_relaxed);
        }
        program_hashes[address] = hash;
    }

    // The running program id, written to diag-state.json as "title".
    void SetTitle(u64 program_id) {
        std::scoped_lock lock{mutex};
        title = program_id;
    }

    // diag_draw_lite: bumped whenever an answer LiteResolve gives may change (control files
    // re-read, a program key registered or changed, a capture starting or ending).
    u64 Generation() const {
        return generation.load(std::memory_order_relaxed);
    }

    u64 CurrentFrame() const {
        return frame_now.load(std::memory_order_relaxed);
    }

    // The answers for a draw's programs as the synchronous calls would give them now.
    LiteAnswer Resolve(u64 vs, u64 ps, bool poll) {
        std::scoped_lock lock{mutex};
        if (poll) {
            Poll();
        }
        LiteAnswer answer{.vs = vs, .ps = ps, .generation = Generation(), .vkey = KeyOf(vs),
                          .pkey = KeyOf(ps)};
        answer.ablated_v = ablated.contains({'v', vs}) ||
                           (answer.vkey != 0 && ablated_keys.contains(answer.vkey));
        answer.ablated_p = ablated.contains({'p', ps}) ||
                           (answer.pkey != 0 && ablated_keys.contains(answer.pkey));
        const bool dir = !DrawListDir().empty();
        answer.inspect_v = dir && answer.vkey != 0 && inspected.contains(answer.vkey);
        answer.inspect_p = dir && answer.pkey != 0 && inspected.contains(answer.pkey);
        return answer;
    }

    // The table's cached answer, refilled under the lock when the generation moved.
    const LiteAnswer& LiteLookup(LiteTable& table, u64 vs, u64 ps) {
        LiteAnswer& entry = table.answers[LiteHash(vs, ps, nullptr) & (LiteTable::Size - 1)];
        if (entry.generation == Generation() && entry.vs == vs && entry.ps == ps) [[likely]] {
            return entry;
        }
        if (table.used != 0 &&
            std::chrono::steady_clock::now() - table.last_fold >= std::chrono::milliseconds{16}) {
            FoldLite(table);
        }
        entry = Resolve(vs, ps, true);
        return entry;
    }

    // Counts one draw in the table (no lock unless it folds).
    void LiteAdd(LiteTable& table, const LiteAnswer& answer, const char* outcome, u64 instances,
                 u64 vertices, u32 width, u32 height) {
        if (answer.generation != table.generation) {
            // The pending slots carry the keys of their generation: fold them first.
            if (table.used != 0) {
                FoldLite(table);
            }
            table.generation = answer.generation;
        }
        u32 index = LiteHash(answer.vs, answer.ps, outcome) & (LiteTable::Size - 1);
        LitePending* slot = &table.pending[index];
        while (slot->outcome != nullptr &&
               (slot->outcome != outcome || slot->vs != answer.vs || slot->ps != answer.ps)) {
            index = (index + 1) & (LiteTable::Size - 1);
            slot = &table.pending[index];
        }
        if (slot->outcome == nullptr) {
            *slot = {.vs = answer.vs, .ps = answer.ps, .outcome = outcome, .vkey = answer.vkey,
                     .pkey = answer.pkey};
            table.used_index[table.used++] = static_cast<u16>(index);
        }
        ++slot->n;
        slot->instances += instances;
        slot->vertices += vertices;
        slot->last_seq = ++table.seq;
        slot->last_frame = CurrentFrame();
        slot->width = width;
        slot->height = height;
        if (table.used >= LiteTable::FoldAt) {
            FoldLite(table);
        } else if (++table.adds_since_check >= 1024) {
            // A safety net for a present on another thread: folds at least every 16 ms.
            table.adds_since_check = 0;
            if (std::chrono::steady_clock::now() - table.last_fold >= std::chrono::milliseconds{16}) {
                FoldLite(table);
            }
        }
    }

    // Applies the table's pending draws as their Touch/Note calls would have.
    void FoldLite(LiteTable& table) {
        std::scoped_lock lock{mutex};
        FoldLocked(table, force_counters || CountersEnabled());
    }

    // Marks that a lite table exists, so Note and TickFrame fold the calling thread's one.
    void EnableLite() {
        lite_used.store(true, std::memory_order_relaxed);
    }

    // Verify: one draw applied synchronously, with the keys and frame the lite path used.
    void ShadowDraw(u64 frame_value, const LiteAnswer& answer, const char* outcome, u64 instances,
                    u64 vertices, u32 width, u32 height, bool counters) {
        std::scoped_lock lock{mutex};
        TouchLocked('v', answer.vs, answer.vkey, outcome, 1, instances, vertices, width, height,
                    frame_value, nullptr);
        TouchLocked('p', answer.ps, answer.pkey, outcome, 1, instances, vertices, width, height,
                    frame_value, nullptr);
        if (counters) {
            NoteLocked(false, answer.vs, answer.ps, outcome, 1, instances, vertices, {});
        }
    }

    // Verify: per-key differences of the draw state ('v'/'p' programs, draw counts) between two
    // instances. Logs up to `lines` of them.
    u64 DrawDifferences(Diagnostics& other, u32& lines) {
        std::scoped_lock lock{mutex, other.mutex};
        u64 differences = 0;
        const auto report = [&](std::string_view what, char kind, u64 id) {
            ++differences;
            if (lines > 0) {
                --lines;
                LOG_ERROR(HW_GPU, "Nx2Perf diag_draw_lite: {} differs for {} {:#x}", what, kind, id);
            }
        };
        const auto programs_of = [](Diagnostics& diag) {
            std::vector<std::pair<std::pair<char, u64>, const Program*>> list;
            for (const auto& [id, program] : diag.programs) {
                if (id.first == 'v' || id.first == 'p') {
                    list.emplace_back(id, &program);
                }
            }
            return list;
        };
        const auto mine = programs_of(*this);
        const auto theirs = programs_of(other);
        if (mine.size() != theirs.size()) {
            report("program count", '-', mine.size() > theirs.size() ? mine.size() - theirs.size()
                                                                     : theirs.size() - mine.size());
        }
        for (size_t i = 0; i < std::min(mine.size(), theirs.size()); ++i) {
            const auto& [id, a] = mine[i];
            const auto& b = theirs[i].second;
            if (id != theirs[i].first) {
                report("program set", id.first, id.second);
                continue;
            }
            if (a->draws != b->draws || a->instances != b->instances || a->vertices != b->vertices ||
                a->outcomes != b->outcomes) {
                report("program counts", id.first, id.second);
            }
            if (a->address != b->address || a->key != b->key || a->last_frame != b->last_frame ||
                a->width != b->width || a->height != b->height) {
                report("program last draw", id.first, id.second);
            }
        }
        const auto counts_of = [](Diagnostics& diag) {
            std::vector<std::pair<const std::tuple<bool, u64, u64, std::string>*, const Count*>> list;
            for (const auto& [key, count] : diag.counts) {
                if (!std::get<0>(key)) {
                    list.emplace_back(&key, &count);
                }
            }
            return list;
        };
        const auto my_counts = counts_of(*this);
        const auto their_counts = counts_of(other);
        if (my_counts.size() != their_counts.size()) {
            report("draw count set", '-', my_counts.size());
        }
        for (size_t i = 0; i < std::min(my_counts.size(), their_counts.size()); ++i) {
            const auto& [key, a] = my_counts[i];
            const auto& [their_key, b] = their_counts[i];
            if (*key != *their_key || a->n != b->n || a->instances != b->instances ||
                a->vertices != b->vertices) {
                report("draw count", 'd', std::get<1>(*key));
            }
        }
        return differences;
    }

    u64 ProgramKey(u64 address) {
        std::scoped_lock lock{mutex};
        return KeyOf(address);
    }

    bool Capturing() const {
        return capturing;
    }

    // One tab separated row of the frame being captured (only call while Capturing()). `query` is the
    // probe query of the draw (NextQuery()) and vs/ps its programs, for the samples column.
    void Row(const std::string& row, int query = -1, u64 vs = 0, u64 ps = 0) {
        std::scoped_lock lock{mutex};
        if (capturing) {
            rows.push_back({row, query, vs, ps});
        }
    }

    // The next probe query of the capture in progress, or -1 (not capturing or out of queries).
    int NextQuery() {
        std::scoped_lock lock{mutex};
        return capturing && query_count < MaxProbeQueries ? static_cast<int>(query_count++) : -1;
    }

    u32 QueryCount() {
        std::scoped_lock lock{mutex};
        return query_count;
    }

    // The samples passed by each probe query of the capture that is ending.
    void SetSamples(std::vector<u64> values) {
        std::scoped_lock lock{mutex};
        samples = std::move(values);
    }

    // Frame boundary: exports the state, starts a capture when <dir>/draw-list exists and ends the
    // running one.
    void TickFrame() {
        const auto& dir = DrawListDir();
        if (dir.empty()) {
            if (lite_used.load(std::memory_order_relaxed)) {
                std::scoped_lock lock{mutex};
                FoldOwnLocked();
            }
            return;
        }
        std::scoped_lock lock{mutex};
        FoldOwnLocked();
        ++frame;
        frame_now.store(frame, std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (now - last_export >= std::chrono::seconds{1}) {
            Export(dir, std::chrono::duration<double>(now - last_export).count());
            last_export = now;
        }
        if (capturing) {
            capturing = false;
            const auto path = dir / ("draw-list-" + std::to_string(next_capture++) + ".tsv");
            if (std::ofstream out{path}) {
                out << "seq\tkind\tvs\tps\toutcome\tvertices\tinstances\ttopology\trt0\twidth\theight\t"
                       "format\tzeta\tblend\tcolor_mask\textra\tvs_key\tps_key\tsamples\n";
                for (auto& [id, program] : programs) {
                    program.probe = {};
                }
                for (const auto& entry : rows) {
                    const bool known = entry.query >= 0 && static_cast<size_t>(entry.query) < samples.size();
                    out << entry.text << '\t' << (known ? std::to_string(samples[entry.query]) : "-") << '\n';
                    if (known) {
                        for (const auto& [kind, address] : {std::pair{'v', entry.vs}, std::pair{'p', entry.ps}}) {
                            const u64 key = KeyOf(address);
                            if (const auto it = programs.find({kind, key ? key : address}); it != programs.end()) {
                                ++it->second.probe.draws;
                                it->second.probe.zero += samples[entry.query] == 0;
                                it->second.probe.samples += samples[entry.query];
                            }
                        }
                    }
                }
                LOG_INFO(HW_GPU, "diag: wrote {} ({} rows, {} probed)", path.string(), rows.size(),
                         samples.size());
            } else {
                LOG_ERROR(HW_GPU, "diag: cannot write {}", path.string());
            }
            rows.clear();
            samples.clear();
            query_count = 0;
            generation.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::error_code error;
        if (std::filesystem::remove(dir / "draw-list", error)) {
            capturing = true;
            generation.fetch_add(1, std::memory_order_relaxed);
            screenshot_request = next_capture;
            seq = 0;
            rows.clear();
            samples.clear();
            query_count = 0;
        }
    }

    // The id of a capture that just began and wants a screenshot (capture-<id>.tga in the diag
    // folder), taken once by the present code.
    std::optional<u32> TakeScreenshotRequest() {
        std::scoped_lock lock{mutex};
        return std::exchange(screenshot_request, std::nullopt);
    }

    u32 NextSeq() {
        std::scoped_lock lock{mutex};
        return seq++;
    }

    // True when the ablation file names this program for that kind ('v' vertex, 'p' pixel,
    // 'c' compute), by address or by its dump key.
    bool Ablated(char kind, u64 address) {
        std::scoped_lock lock{mutex};
        Poll();
        const u64 key = KeyOf(address);
        return ablated.contains({kind, address}) || (key != 0 && ablated_keys.contains(key));
    }

    // True when the program with this key is being inspected (inspect.txt).
    bool Inspecting(u64 key) {
        if (key == 0 || DrawListDir().empty()) {
            return false;
        }
        std::scoped_lock lock{mutex};
        Poll();
        return inspected.contains(key);
    }

    // The words of constant buffer `slot` bound for that program at a draw or dispatch. The first call
    // in each frame is compared with the same slot's first call in the previous frame.
    void Observe(u64 key, u32 slot, const std::vector<u32>& words) {
        std::scoped_lock lock{mutex};
        auto& watch = watches[{key, slot}];
        if (watch.last_frame == frame && !watch.first.empty()) {
            // Later draws of the frame: words that differ from the frame's first draw vary per draw.
            for (size_t i = 0; i < words.size() && i < watch.first.size(); ++i) {
                if (words[i] != watch.first[i]) {
                    watch.per_draw.insert(static_cast<u32>(i));
                }
            }
            return;
        }
        if (!watch.first.empty()) {
            for (size_t i = 0; i < words.size() && i < watch.first.size(); ++i) {
                if (words[i] != watch.first[i]) {
                    ++watch.changes[static_cast<u32>(i)];
                }
            }
            ++watch.frames_compared;
        }
        watch.first = words;
        watch.last_frame = frame;
    }

private:
    Diagnostics() {
        if (!LabStatus().empty()) {
            LOG_INFO(HW_GPU, "diag: {}", LabStatus());
        }
    }

    struct Count {
        u64 n{};
        u64 instances{};
        u64 vertices{};
        std::string detail;
    };

    struct Program {
        u64 address{};
        u64 key{};
        u64 draws{};
        u64 instances{};
        u64 vertices{};
        u64 last_frame{};
        u32 width{};
        u32 height{};
        u64 exported_draws{};
        u64 lite_seq{};
        double draws_per_second{};
        std::map<std::string, u64> outcomes;
        struct Probe {
            u64 draws{};
            u64 zero{};
            u64 samples{};
        } probe;
    };

    struct RowEntry {
        std::string text;
        int query;
        u64 vs;
        u64 ps;
    };

    struct Watch {
        std::vector<u32> first;
        u64 last_frame{~u64{}};
        u64 frames_compared{};
        std::map<u32, u64> changes;
        std::set<u32> per_draw;
    };

    u64 KeyOf(u64 address) const {
        const auto it = program_keys.find(address);
        return it == program_keys.end() ? 0 : it->second;
    }

    u64 HashOf(u64 address) const {
        const auto it = program_hashes.find(address);
        return it == program_hashes.end() ? 0 : it->second;
    }

    static bool Changed(const std::filesystem::path& path, std::filesystem::file_time_type& stamp) {
        std::error_code error;
        const auto now = std::filesystem::last_write_time(path, error);
        if (error) {
            const bool had = stamp != std::filesystem::file_time_type{};
            stamp = {};
            return had;
        }
        if (now == stamp) {
            return false;
        }
        stamp = now;
        return true;
    }

    // Re-reads the control files when they changed (checked at most every 250 ms).
    void Poll() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_poll < std::chrono::milliseconds{250}) {
            return;
        }
        last_poll = now;
        if (!AblateFile().empty() && Changed(AblateFile(), ablate_stamp)) {
            ablated.clear();
            ablated_keys.clear();
            std::ifstream in{AblateFile()};
            std::string line;
            while (std::getline(in, line)) {
                line = line.substr(0, line.find('#'));
                std::istringstream words{line};
                std::string kind, address;
                if (!(words >> kind >> address)) {
                    continue;
                }
                const u64 value = std::strtoull(address.c_str(), nullptr, 16);
                if (kind == "key") {
                    ablated_keys.insert(value);
                } else if (kind == "vs" || kind == "ps" || kind == "cs") {
                    ablated.emplace(kind == "vs" ? 'v' : kind == "ps" ? 'p' : 'c', value);
                }
            }
            LOG_INFO(HW_GPU, "diag: ablating {} program(s) from {}", ablated.size() + ablated_keys.size(),
                     AblateFile().string());
            generation.fetch_add(1, std::memory_order_relaxed);
        }
        if (const auto path = DrawListDir() / "inspect.txt";
            !DrawListDir().empty() && Changed(path, inspect_stamp)) {
            inspected.clear();
            watches.clear();
            std::ifstream in{path};
            std::string line;
            while (std::getline(in, line)) {
                line = line.substr(0, line.find('#'));
                if (const u64 value = std::strtoull(line.c_str(), nullptr, 16); value != 0) {
                    inspected.insert(value);
                }
            }
            LOG_INFO(HW_GPU, "diag: inspecting {} program(s)", inspected.size());
            generation.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void NoteLocked(bool dispatch, u64 vs, u64 ps, std::string_view outcome, u64 n, u64 instances,
                    u64 vertices, std::string_view detail) {
        const bool good = outcome == "drawn" || outcome == "ok";
        auto& count = counts[{dispatch, vs, ps, std::string{outcome}}];
        const u64 before = count.n;
        count.n += n;
        count.instances += instances;
        count.vertices += vertices;
        if (count.detail.empty()) {
            count.detail = detail;
        }
        window[{dispatch, std::string{outcome}}] += n;
        if (good || quiet) {
            return;
        }
        // A line each time the count reaches a power of two (several when n draws were folded).
        for (u64 reached = std::bit_ceil(before + 1); reached != 0 && reached <= count.n; reached <<= 1) {
            if (dispatch) {
                LOG_WARNING(HW_GPU, "diag: dispatch cs={:#x} {} x{}{}{}", vs, outcome, reached,
                            count.detail.empty() ? "" : ": ", count.detail);
            } else {
                LOG_WARNING(HW_GPU, "diag: draw vs={:#x} ps={:#x} {} x{}", vs, ps, outcome, reached);
            }
        }
    }

    void SummaryLocked() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_summary < std::chrono::seconds{1}) {
            return;
        }
        last_summary = now;
        if (quiet) {
            window.clear();
            return;
        }
        std::ostringstream text;
        for (const auto& [key, n] : window) {
            text << ' ' << (key.first ? "cs:" : "draw:") << key.second << '=' << n;
        }
        window.clear();
        LOG_INFO(HW_GPU, "diag 1s:{}", text.str());
    }

    // `lite_seq`: a lite fold's sequence of the slot's last draw; the last-draw fields (address, frame,
    // render target) follow only a newer draw. nullptr: a synchronous call, always newer.
    void TouchLocked(char kind, u64 address, u64 key, std::string_view outcome, u64 n, u64 instances,
                     u64 vertices, u32 width, u32 height, u64 last_frame, const u64* lite_seq) {
        auto& program = programs[{kind, key ? key : address}];
        program.draws += n;
        program.instances += instances;
        program.vertices += vertices;
        program.outcomes[std::string{outcome}] += n;
        if (lite_seq != nullptr) {
            if (*lite_seq < program.lite_seq) {
                return;
            }
            program.lite_seq = *lite_seq;
        }
        program.address = address;
        program.key = key;
        program.last_frame = last_frame;
        program.width = width;
        program.height = height;
    }

    void FoldLocked(LiteTable& table, bool counters) {
        // Keeps the control files' 250 ms poll running without the per-draw calls.
        Poll();
        for (u32 i = 0; i < table.used; ++i) {
            LitePending& slot = table.pending[table.used_index[i]];
            TouchLocked('v', slot.vs, slot.vkey, slot.outcome, slot.n, slot.instances, slot.vertices,
                        slot.width, slot.height, slot.last_frame, &slot.last_seq);
            TouchLocked('p', slot.ps, slot.pkey, slot.outcome, slot.n, slot.instances, slot.vertices,
                        slot.width, slot.height, slot.last_frame, &slot.last_seq);
            if (counters) {
                NoteLocked(false, slot.vs, slot.ps, slot.outcome, slot.n, slot.instances,
                           slot.vertices, {});
            }
            slot = {};
        }
        table.used = 0;
        table.adds_since_check = 0;
        table.last_fold = std::chrono::steady_clock::now();
        if (counters) {
            SummaryLocked();
        }
    }

    // Folds the calling thread's lite table (Get() only: summaries and exports see every draw).
    void FoldOwnLocked() {
        if (!lite_used.load(std::memory_order_relaxed) || this != &Get()) {
            return;
        }
        if (const auto& table = ThreadLite(); table && table->used != 0) {
            FoldLocked(*table, force_counters || CountersEnabled());
        }
    }

    // diag-state.json, replaced atomically so a reader never sees half of it.
    void Export(const std::filesystem::path& dir, double seconds) {
        Poll();
        std::ostringstream out;
        char title_text[17] = "";
        if (title != 0) {
            std::snprintf(title_text, sizeof(title_text), "%016llX",
                          static_cast<unsigned long long>(title));
        }
        out << "{\"frame\":" << frame << ",\"title\":\"" << title_text << "\",\"programs\":[";
        bool first = true;
        for (auto& [id, program] : programs) {
            program.draws_per_second = seconds > 0 ? (program.draws - program.exported_draws) / seconds : 0;
            program.exported_draws = program.draws;
            const u64 hash = HashOf(program.address);
            out << (first ? "" : ",") << "{\"kind\":\"" << id.first << "\",\"key\":\"" << Hex16(program.key)
                << "\",\"hash\":\"" << (hash != 0 ? Hex16(hash) : std::string{})
                << "\",\"addr\":\"" << std::hex << program.address << std::dec << "\",\"draws\":"
                << program.draws << ",\"per_s\":" << static_cast<u64>(program.draws_per_second)
                << ",\"instances\":" << program.instances << ",\"vertices\":" << program.vertices
                << ",\"last_frame\":" << program.last_frame << ",\"rt\":\"" << program.width << 'x'
                << program.height << "\",\"outcomes\":{";
            bool first_outcome = true;
            for (const auto& [name, n] : program.outcomes) {
                out << (first_outcome ? "" : ",") << '"' << name << "\":" << n;
                first_outcome = false;
            }
            out << '}';
            if (program.probe.draws > 0) {
                out << ",\"probe\":{\"draws\":" << program.probe.draws << ",\"zero\":" << program.probe.zero
                    << ",\"samples\":" << program.probe.samples << '}';
            }
            out << '}';
            first = false;
        }
        out << "],\"ablated\":[";
        first = true;
        for (const u64 key : ablated_keys) {
            out << (first ? "" : ",") << '"' << Hex16(key) << '"';
            first = false;
        }
        out << "],\"inspect\":[";
        first = true;
        for (const u64 key : inspected) {
            out << (first ? "" : ",") << "{\"key\":\"" << Hex16(key) << "\",\"slots\":[";
            bool first_slot = true;
            for (const auto& [id, watch] : watches) {
                if (id.first != key) {
                    continue;
                }
                out << (first_slot ? "" : ",") << "{\"slot\":" << id.second << ",\"words\":"
                    << watch.first.size() << ",\"frames\":" << watch.frames_compared << ",\"changing\":[";
                first_slot = false;
                size_t listed = 0;
                for (const auto& [index, n] : watch.changes) {
                    if (listed++ >= 96) {
                        break;
                    }
                    out << (listed > 1 ? "," : "") << "{\"off\":" << index * 4 << ",\"changes\":" << n
                        << ",\"value\":" << watch.first[index] << '}';
                }
                out << "],\"per_draw\":[";
                listed = 0;
                for (const u32 index : watch.per_draw) {
                    if (listed++ >= 96) {
                        break;
                    }
                    out << (listed > 1 ? "," : "") << index * 4;
                }
                out << "]}";
            }
            out << "]}";
            first = false;
        }
        out << "]}\n";
        const auto temporary = dir / "diag-state.json.tmp";
        if (std::ofstream file{temporary, std::ios::trunc}) {
            file << out.str();
        }
        std::error_code error;
        std::filesystem::rename(temporary, dir / "diag-state.json", error);
    }

    std::mutex mutex;
    std::map<std::tuple<bool, u64, u64, std::string>, Count> counts;
    std::map<std::pair<bool, std::string>, u64> window;
    std::map<std::pair<char, u64>, Program> programs;
    std::map<u64, u64> program_keys;
    std::map<u64, u64> program_hashes;
    u64 title{};
    std::map<std::pair<u64, u32>, Watch> watches;
    std::chrono::steady_clock::time_point last_summary{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_export{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_poll{};
    std::set<std::pair<char, u64>> ablated;
    std::set<u64> ablated_keys;
    std::set<u64> inspected;
    std::filesystem::file_time_type ablate_stamp{};
    std::filesystem::file_time_type inspect_stamp{};
    std::vector<RowEntry> rows;
    std::vector<u64> samples;
    u32 query_count{};
    bool capturing{};
    u32 seq{};
    u32 next_capture{};
    std::optional<u32> screenshot_request;
    u64 frame{};
    std::atomic<u64> frame_now{};       ///< frame, readable without the lock
    std::atomic<u64> generation{1};     ///< See Generation()
    std::atomic<bool> lite_used{};      ///< A diag_draw_lite table exists
    bool quiet{};
    bool force_counters{};
};

inline bool Active() {
    static const bool active =
        CountersEnabled() || !DrawListDir().empty() || !AblateFile().empty();
    return active;
}

inline bool Ablates() {
    return !AblateFile().empty();
}

} // namespace VideoCommon::Nx2Diag

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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

class Diagnostics {
public:
    static Diagnostics& Get() {
        static Diagnostics instance;
        return instance;
    }

    // A draw (shader addresses as logged) or dispatch (vs = code address, ps = 0) and what became of it.
    void Note(bool dispatch, u64 vs, u64 ps, std::string_view outcome, u64 instances, u64 vertices,
              std::string_view detail = {}) {
        std::scoped_lock lock{mutex};
        const bool good = outcome == "drawn" || outcome == "ok";
        auto& count = counts[{dispatch, vs, ps, std::string{outcome}}];
        ++count.n;
        count.instances += instances;
        count.vertices += vertices;
        if (count.detail.empty()) {
            count.detail = detail;
        }
        ++window[{dispatch, std::string{outcome}}];
        if (!good && (count.n & (count.n - 1)) == 0) {
            if (dispatch) {
                LOG_WARNING(HW_GPU, "diag: dispatch cs={:#x} {} x{}{}{}", vs, outcome, count.n,
                            count.detail.empty() ? "" : ": ", count.detail);
            } else {
                LOG_WARNING(HW_GPU, "diag: draw vs={:#x} ps={:#x} {} x{}", vs, ps, outcome, count.n);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_summary >= std::chrono::seconds{1}) {
            last_summary = now;
            std::ostringstream text;
            for (const auto& [key, n] : window) {
                text << ' ' << (key.first ? "cs:" : "draw:") << key.second << '=' << n;
            }
            window.clear();
            LOG_INFO(HW_GPU, "diag 1s:{}", text.str());
        }
    }

    // Registry of the programs seen, for diag-state.json. kind: 'v' vertex, 'p' pixel, 'c' compute.
    void Touch(char kind, u64 address, std::string_view outcome, u64 instances, u64 vertices,
               u32 width, u32 height) {
        std::scoped_lock lock{mutex};
        const u64 key = KeyOf(address);
        auto& program = programs[{kind, key ? key : address}];
        program.address = address;
        program.key = key;
        ++program.draws;
        program.instances += instances;
        program.vertices += vertices;
        ++program.outcomes[std::string{outcome}];
        program.last_frame = frame;
        program.width = width;
        program.height = height;
    }

    // The shader dump's key for the program at that address, so draw lists and dumps from different
    // runs can be matched (addresses change between runs, the key is a hash of the code). `hash` is
    // its shader hash (frame dump "shaders") once it is lowered, 0 until then.
    void RegisterProgram(u64 address, u64 key, u64 hash = 0) {
        std::scoped_lock lock{mutex};
        program_keys[address] = key;
        program_hashes[address] = hash;
    }

    // The running program id, written to diag-state.json as "title".
    void SetTitle(u64 program_id) {
        std::scoped_lock lock{mutex};
        title = program_id;
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
            return;
        }
        std::scoped_lock lock{mutex};
        ++frame;
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
            return;
        }
        std::error_code error;
        if (std::filesystem::remove(dir / "draw-list", error)) {
            capturing = true;
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

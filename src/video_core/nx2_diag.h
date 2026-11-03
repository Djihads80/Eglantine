// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>

#include "common/common_types.h"
#include "common/logging.h"

// Switch 2 draw and dispatch diagnostics, all off unless one of these is set:
//
//   EDEN_NX2_DIAG=1          per-shader outcome counters: a summary line each second and a line for
//                            every distinct failure (draw: vs/ps pair, dispatch: code address) the
//                            first time and each time its count doubles.
//   EDEN_NX2_DIAG_DIR=<dir>  create <dir>/draw-list while running and the next frame's draws and
//                            dispatches are written to <dir>/draw-list-<n>.tsv (the file is removed).
//   EDEN_NX2_ABLATE_FILE=<f> skip draws and dispatches named in that file, re-read when it changes.
//                            One entry per line: "vs <hex>", "ps <hex>" or "cs <hex>" (addresses as
//                            logged and as in the shader dump names); "#" starts a comment.
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
        const char* value = Env("EDEN_NX2_ABLATE_FILE");
        return value ? std::filesystem::path{value} : std::filesystem::path{};
    }();
    return file;
}

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

    bool Capturing() const {
        return capturing;
    }

    // One tab separated row of the frame being captured (only call while Capturing()).
    void Row(const std::string& row) {
        std::scoped_lock lock{mutex};
        if (capturing) {
            rows << row << '\n';
        }
    }

    // Frame boundary: starts a capture when <dir>/draw-list exists and ends the running one.
    void TickFrame() {
        const auto& dir = DrawListDir();
        if (dir.empty()) {
            return;
        }
        std::scoped_lock lock{mutex};
        if (capturing) {
            capturing = false;
            const auto path = dir / ("draw-list-" + std::to_string(next_capture++) + ".tsv");
            if (std::ofstream out{path}) {
                out << "seq\tkind\tvs\tps\toutcome\tvertices\tinstances\ttopology\trt0\twidth\theight\t"
                       "format\tzeta\tblend\tcolor_mask\textra\n"
                    << rows.str();
                LOG_INFO(HW_GPU, "diag: wrote {} ({} rows)", path.string(), seq);
            } else {
                LOG_ERROR(HW_GPU, "diag: cannot write {}", path.string());
            }
            rows.str({});
            return;
        }
        std::error_code error;
        if (std::filesystem::remove(dir / "draw-list", error)) {
            capturing = true;
            seq = 0;
            rows.str({});
        }
    }

    u32 NextSeq() {
        std::scoped_lock lock{mutex};
        return seq++;
    }

    // True when the ablation file names this address for that kind ('v' vertex, 'p' pixel, 'c' compute).
    bool Ablated(char kind, u64 address) {
        std::scoped_lock lock{mutex};
        const auto now = std::chrono::steady_clock::now();
        if (now - last_ablate_check >= std::chrono::milliseconds{250}) {
            last_ablate_check = now;
            std::error_code error;
            const auto stamp = std::filesystem::last_write_time(AblateFile(), error);
            if (!error && stamp != ablate_stamp) {
                ablate_stamp = stamp;
                ReloadAblation();
            } else if (error && (!ablated.empty())) {
                ablated.clear();
                LOG_INFO(HW_GPU, "diag: ablation file gone, nothing ablated");
            }
        }
        return ablated.contains({kind, address});
    }

private:
    struct Count {
        u64 n{};
        u64 instances{};
        u64 vertices{};
        std::string detail;
    };

    void ReloadAblation() {
        ablated.clear();
        std::ifstream in{AblateFile()};
        std::string line;
        while (std::getline(in, line)) {
            line = line.substr(0, line.find('#'));
            std::istringstream words{line};
            std::string kind, address;
            if (!(words >> kind >> address)) {
                continue;
            }
            const char letter = kind == "vs" ? 'v' : kind == "ps" ? 'p' : kind == "cs" ? 'c' : 0;
            if (letter) {
                ablated.emplace(letter, std::strtoull(address.c_str(), nullptr, 16));
            }
        }
        LOG_INFO(HW_GPU, "diag: ablating {} shader(s) from {}", ablated.size(), AblateFile().string());
    }

    std::mutex mutex;
    std::map<std::tuple<bool, u64, u64, std::string>, Count> counts;
    std::map<std::pair<bool, std::string>, u64> window;
    std::chrono::steady_clock::time_point last_summary{std::chrono::steady_clock::now()};
    std::set<std::pair<char, u64>> ablated;
    std::filesystem::file_time_type ablate_stamp{};
    std::chrono::steady_clock::time_point last_ablate_check{};
    std::ostringstream rows;
    bool capturing{};
    u32 seq{};
    u32 next_capture{};
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

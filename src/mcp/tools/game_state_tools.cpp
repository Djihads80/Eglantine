// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Knowing where a game is without reading screenshots: screen fingerprints (labelled thumbnails
// matched server-side), memory probes (named addresses, module-relative or pointer chains), and
// routes (saved per-game recipes of input + waits for a screen or probe), all stored per game.

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/stb.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "mcp/activity.h"
#include "mcp/context.h"
#include "mcp/control.h"
#include "mcp/host.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"
#include "video_core/capture.h"
#include "video_core/gpu.h"
#include "video_core/textures/decoders.h"

namespace Mcp {

namespace {

using namespace std::chrono;

// ---- per-game storage ---------------------------------------------------------------------

/// Guards load-modify-save of the per-game JSON store files.
std::mutex g_store_mutex;

/// <config>/mcp/<kind>/<program id>.json.
std::filesystem::path StorePath(std::string_view kind, u64 program_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "mcp" / std::string{kind} /
           fmt::format("{:016X}.json", program_id);
}

/// Reads a store file; an empty object if it is missing or not valid JSON. Callers hold
/// g_store_mutex.
json LoadStore(std::string_view kind, u64 program_id) {
    const std::string text =
        Common::FS::ReadStringFromFile(StorePath(kind, program_id), Common::FS::FileType::TextFile);
    if (text.empty()) {
        return json::object();
    }
    try {
        return json::parse(text);
    } catch (...) {
        return json::object();
    }
}

/// Writes a store file, creating its directory. A failed write is only logged.
void SaveStore(std::string_view kind, u64 program_id, const json& data) {
    const auto path = StorePath(kind, program_id);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::string text = data.dump(1);
    if (Common::FS::WriteStringToFile(path, Common::FS::FileType::TextFile, text) != text.size()) {
        LOG_WARNING(Debug_MCP, "Could not write {}", Common::FS::PathToUTF8String(path));
    }
}

u64 ProgramId(ToolContext& ctx) {
    return ctx.system->GetApplicationProcessProgramID();
}

// ---- frames -------------------------------------------------------------------------------

/// The last frame of the applet capture layer: 1280x720, no post-processing, works with the
/// window hidden or the game paused.
std::optional<Frame> RawCapture(ToolContext& ctx, std::string& error) {
    namespace Cap = VideoCore::Capture;
    // The capture is a sync operation on the GPU thread, which no longer runs once the game has
    // stopped: it would block forever.
    if (!ctx.GameRunning()) {
        error = "no game is running";
        return std::nullopt;
    }
    const std::vector<u8> tiled = ctx.system->GPU().GetAppletCaptureBuffer();
    if (tiled.size() < Cap::TiledSize) {
        error = "applet capture buffer unavailable";
        return std::nullopt;
    }
    Frame frame{.width = Cap::LinearWidth, .height = Cap::LinearHeight};
    frame.bgra.resize(size_t{frame.width} * frame.height * Cap::BytesPerPixel);
    Tegra::Texture::UnswizzleTexture(frame.bgra, tiled, Cap::BytesPerPixel, Cap::LinearWidth,
                                     Cap::LinearHeight, Cap::LinearDepth, Cap::BlockHeight,
                                     Cap::BlockDepth);
    if (std::all_of(frame.bgra.begin(), frame.bgra.end(), [](u8 b) { return b == 0; })) {
        error = "applet capture buffer is empty";
        return std::nullopt;
    }
    // The layer holds R, G, B, A bytes (despite Capture::PixelFormat): swap to B, G, R, A.
    for (size_t i = 0; i + 3 < frame.bgra.size(); i += 4) {
        std::swap(frame.bgra[i], frame.bgra[i + 2]);
    }
    return frame;
}

/// Where the last AnalysisFrame came from, for diagnostics.
thread_local std::string t_frame_source;

/// A frame for analysis: the presented display if the window can deliver one, else raw.
std::optional<Frame> AnalysisFrame(ToolContext& ctx, std::string& error) {
    if (ctx.host) {
        if (auto frame = ctx.host->CaptureFrame(384, milliseconds{1500}, error)) {
            t_frame_source = "display";
            return frame;
        }
    }
    const std::string display_error = error;
    auto frame = RawCapture(ctx, error);
    t_frame_source = "raw (display: " + display_error + ")";
    return frame;
}

/// PNG of a BGRA frame scaled to at most max_width.
std::vector<u8> EncodePng(const Frame& frame, u32 max_width) {
    u32 w = frame.width;
    u32 h = frame.height;
    std::vector<u8> rgba(frame.bgra.size());
    for (size_t i = 0; i + 3 < frame.bgra.size(); i += 4) {
        rgba[i + 0] = frame.bgra[i + 2];
        rgba[i + 1] = frame.bgra[i + 1];
        rgba[i + 2] = frame.bgra[i + 0];
        rgba[i + 3] = 255;
    }
    if (max_width < w) {
        const u32 nh = h * max_width / w;
        std::vector<u8> scaled(size_t{max_width} * nh * 4);
        stbir_resize_uint8(rgba.data(), static_cast<int>(w), static_cast<int>(h), 0,
                           scaled.data(), static_cast<int>(max_width), static_cast<int>(nh), 0, 4);
        rgba = std::move(scaled);
        w = max_width;
        h = nh;
    }
    std::vector<u8> png;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<u8>*>(context);
            const auto* bytes = static_cast<const u8*>(data);
            out->insert(out->end(), bytes, bytes + size);
        },
        &png, static_cast<int>(w), static_cast<int>(h), 4, rgba.data(), static_cast<int>(w * 4));
    return png;
}

// ---- fingerprints -------------------------------------------------------------------------

constexpr u32 kThumbW = 48;
constexpr u32 kThumbH = 27;

/// Part of the screen in normalized 0..1 coordinates; the default is the whole screen.
struct Region {
    double x = 0, y = 0, w = 1, h = 1;
};

/// Region from an [x, y, w, h] array; the whole screen if `v` is not a 4-element array.
Region ParseRegion(const json& v) {
    if (!v.is_array() || v.size() != 4) {
        return {};
    }
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>(), v[3].get<double>()};
}

/// Luma thumbnail (tw x th) of a region (normalized coordinates) by box averaging.
std::vector<u8> ThumbSized(const Frame& f, const Region& r, u32 tw, u32 th) {
    std::vector<u8> out(size_t{tw} * th);
    const double x0 = r.x * f.width, y0 = r.y * f.height;
    const double cw = r.w * f.width / tw, ch = r.h * f.height / th;
    for (u32 ty = 0; ty < th; ++ty) {
        for (u32 tx = 0; tx < tw; ++tx) {
            const u32 sx0 = static_cast<u32>(x0 + tx * cw), sy0 = static_cast<u32>(y0 + ty * ch);
            const u32 sx1 = std::min<u32>(f.width, std::max(sx0 + 1, static_cast<u32>(x0 + (tx + 1) * cw)));
            const u32 sy1 = std::min<u32>(f.height, std::max(sy0 + 1, static_cast<u32>(y0 + (ty + 1) * ch)));
            u64 sum = 0, n = 0;
            for (u32 y = sy0; y < sy1; y += 2) {
                const u8* row = f.bgra.data() + (size_t{y} * f.width) * 4;
                for (u32 x = sx0; x < sx1; x += 2) {
                    const u8* p = row + size_t{x} * 4;
                    sum += (p[2] * 77u + p[1] * 150u + p[0] * 29u) >> 8;
                    ++n;
                }
            }
            out[size_t{ty} * tw + tx] = static_cast<u8>(n ? sum / n : 0);
        }
    }
    return out;
}

std::vector<u8> Thumb(const Frame& f, const Region& r) {
    return ThumbSized(f, r, kThumbW, kThumbH);
}

/// Normalized cross-correlation of two thumbnails, clamped to 0..1: compares the pattern, not the
/// brightness (a mean-difference score rated any two dark screens as similar). Flat (featureless)
/// thumbnails only match other flat ones of similar brightness.
/// `weights` (optional, same size) emphasise pixels that are stable across a label's samples.
double Similarity(const std::vector<u8>& a, const std::vector<u8>& b,
                  const std::vector<double>* weights = nullptr) {
    if (a.size() != b.size() || a.empty()) {
        return 0.0;
    }
    const auto w = [&](size_t i) { return weights ? (*weights)[i] : 1.0; };
    double n = 0, ma = 0, mb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        n += w(i);
        ma += w(i) * a[i];
        mb += w(i) * b[i];
    }
    ma /= n;
    mb /= n;
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double da = a[i] - ma, db = b[i] - mb;
        sab += w(i) * da * db;
        saa += w(i) * da * da;
        sbb += w(i) * db * db;
    }
    constexpr double kFlat = 16.0; // std dev below ~4 levels: featureless
    const bool flat_a = saa / n < kFlat, flat_b = sbb / n < kFlat;
    if (flat_a || flat_b) {
        return flat_a && flat_b ? 1.0 - std::abs(ma - mb) / 255.0 : 0.0;
    }
    return std::max(0.0, sab / std::sqrt(saa * sbb));
}

/// Lowercase hex string of the bytes, two digits each.
std::string ToHex(const std::vector<u8>& v) {
    std::string s;
    s.reserve(v.size() * 2);
    for (const u8 b : v) {
        s += fmt::format("{:02x}", b);
    }
    return s;
}

/// Empty on malformed input (routes and labels come from editable JSON files).
std::vector<u8> FromHex(const std::string& s) {
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::vector<u8> v;
    v.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = nibble(s[i]);
        const int lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) {
            return {};
        }
        v.push_back(static_cast<u8>(hi << 4 | lo));
    }
    return v;
}

/// Whether `v` is a non-empty string of an even number of hex digits.
bool IsHexString(const json& v) {
    if (!v.is_string()) {
        return false;
    }
    const std::string& s = v.get_ref<const std::string&>();
    return !s.empty() && s.size() % 2 == 0 && !FromHex(s).empty();
}

/// How well the current frame matches one screen label.
struct ScreenMatch {
    std::string label;
    double score = 0.0;
    double threshold = 0.9;
};

/// Scores the frame against every label of the game (best sample per label), best first.
std::vector<ScreenMatch> MatchScreens(const Frame& frame, const json& labels) {
    std::vector<ScreenMatch> out;
    for (const auto& [name, entry] : labels.items()) {
        const auto thumb = Thumb(frame, ParseRegion(entry.value("region", json{})));
        std::vector<std::vector<u8>> samples;
        for (const auto& sample : entry.value("samples", json::array())) {
            samples.push_back(FromHex(sample.get<std::string>()));
        }
        // With several samples, pixels that vary between them (scenery behind a HUD) count
        // less: weight = 1 / (1 + variance / 64).
        std::vector<double> weights;
        if (samples.size() >= 2) {
            weights.assign(thumb.size(), 1.0);
            for (size_t i = 0; i < thumb.size(); ++i) {
                double mean = 0, var = 0;
                for (const auto& s : samples) {
                    mean += s.size() == thumb.size() ? s[i] : 0;
                }
                mean /= static_cast<double>(samples.size());
                for (const auto& s : samples) {
                    const double d = (s.size() == thumb.size() ? s[i] : 0) - mean;
                    var += d * d;
                }
                var /= static_cast<double>(samples.size());
                weights[i] = 1.0 / (1.0 + var / 64.0);
            }
        }
        double best = 0.0;
        for (const auto& sample : samples) {
            best = std::max(best, Similarity(thumb, sample, weights.empty() ? nullptr : &weights));
        }
        out.push_back({name, best, entry.value("threshold", 0.9)});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.score - a.threshold > b.score - b.threshold;
    });
    return out;
}

/// {screen, scores (top 5), source} for the current frame; screen is "unknown" when no label
/// reaches its threshold. Null with `error` set if there is no frame or no labels. Can block up
/// to 1.5 s waiting for a display frame.
json ScreenStateJson(ToolContext& ctx, std::string& error) {
    const auto frame = AnalysisFrame(ctx, error);
    if (!frame) {
        return nullptr;
    }
    json labels;
    {
        std::scoped_lock lock{g_store_mutex};
        labels = LoadStore("screens", ProgramId(ctx)).value("labels", json::object());
    }
    if (labels.empty()) {
        error = "no screen labels for this game yet (screen_label)";
        return nullptr;
    }
    const auto matches = MatchScreens(*frame, labels);
    json scores = json::array();
    for (size_t i = 0; i < std::min<size_t>(matches.size(), 5); ++i) {
        scores.push_back({{"label", matches[i].label},
                          {"score", std::round(matches[i].score * 1000) / 1000},
                          {"threshold", matches[i].threshold}});
    }
    const bool hit = !matches.empty() && matches[0].score >= matches[0].threshold;
    return json{{"screen", hit ? matches[0].label : "unknown"},
                {"scores", std::move(scores)},
                {"source", t_frame_source}};
}

// ---- probes -------------------------------------------------------------------------------

/// Resolves "Module+0x10->0x8->0x20" (module-relative, then pointer derefs), "0x1234" absolute,
/// or "0x1234->0x8". Returns nullopt with error.
std::optional<u64> ResolveAddress(ToolContext& ctx, const std::string& expr, std::string& error) {
    auto* process = ctx.system->ApplicationProcess();
    auto& memory = ctx.system->ApplicationMemory();
    std::vector<std::string> parts;
    for (size_t start = 0;;) {
        const size_t arrow = expr.find("->", start);
        parts.push_back(expr.substr(start, arrow == std::string::npos ? std::string::npos : arrow - start));
        if (arrow == std::string::npos) {
            break;
        }
        start = arrow + 2;
    }
    u64 address = 0;
    const std::string& first = parts[0];
    try {
        if (const auto plus = first.find('+'); plus != std::string::npos) {
            const std::string module = first.substr(0, plus);
            const u64 offset = std::stoull(first.substr(plus + 1), nullptr, 0);
            bool found = false;
            for (const auto& [base, name] : Core::FindModules(process)) {
                if (name == module) {
                    address = base + offset;
                    found = true;
                }
            }
            if (!found) {
                error = "module not found: " + module + " (process_info lists them)";
                return std::nullopt;
            }
        } else {
            address = std::stoull(first, nullptr, 0);
        }
        for (size_t i = 1; i < parts.size(); ++i) {
            if (!memory.IsValidVirtualAddressRange(address, 8)) {
                error = fmt::format("pointer at {:#x} unmapped (step {})", address, i);
                return std::nullopt;
            }
            address = memory.Read64(address) + std::stoull(parts[i], nullptr, 0);
        }
    } catch (...) {
        error = "bad address expression: " + expr;
        return std::nullopt;
    }
    return address;
}

/// Reads one probe's value from guest memory as its `type` (u32 by default), adding its
/// `meaning` if one is defined. Errors (bad address, unmapped memory) go in an "error" field.
json ReadProbe(ToolContext& ctx, const std::string& name, const json& probe) {
    std::string error;
    const auto address = ResolveAddress(ctx, probe.at("address").get<std::string>(), error);
    if (!address) {
        return json{{"name", name}, {"error", error}};
    }
    const std::string type = probe.value("type", "u32");
    const size_t size = type == "u8" || type == "s8"     ? 1
                        : type == "u16" || type == "s16" ? 2
                        : type == "u64" || type == "s64" || type == "f64" ? 8
                                                                         : 4;
    auto& memory = ctx.system->ApplicationMemory();
    if (!memory.IsValidVirtualAddressRange(*address, size)) {
        return json{{"name", name}, {"error", fmt::format("{:#x} unmapped", *address)}};
    }
    u64 bits = 0;
    memory.ReadBlockUnsafe(*address, &bits, size);
    json value;
    if (type == "f32") {
        value = std::bit_cast<f32>(static_cast<u32>(bits));
    } else if (type == "f64") {
        value = std::bit_cast<f64>(bits);
    } else if (type.starts_with("s")) {
        const int shift = 64 - static_cast<int>(size) * 8;
        value = static_cast<s64>(bits << shift) >> shift;
    } else {
        value = bits;
    }
    json out{{"name", name}, {"value", value}, {"address", fmt::format("{:#x}", *address)}};
    if (probe.contains("meanings") && probe["meanings"].contains(value.dump())) {
        out["meaning"] = probe["meanings"][value.dump()];
    }
    return out;
}

/// Current values of the game's saved probes keyed by name; all of them when `names` is empty.
json ReadAllProbes(ToolContext& ctx, const json& names) {
    json probes;
    {
        std::scoped_lock lock{g_store_mutex};
        probes = LoadStore("probes", ProgramId(ctx)).value("probes", json::object());
    }
    json out = json::object();
    for (const auto& [name, probe] : probes.items()) {
        if (!names.empty() && std::find(names.begin(), names.end(), name) == names.end()) {
            continue;
        }
        json r = ReadProbe(ctx, name, probe);
        r.erase("name");
        out[name] = std::move(r);
    }
    return out;
}

/// Compares a probe value with `target` using `op` (equals, not_equals, greater, less). False
/// for a non-numeric value or an unknown op.
bool ProbeCondition(const json& value, const std::string& op, const json& target) {
    if (!value.is_number()) {
        return false;
    }
    const double v = value.get<double>(), t = target.get<double>();
    if (op == "equals") return v == t;
    if (op == "not_equals") return v != t;
    if (op == "greater") return v > t;
    if (op == "less") return v < t;
    return false;
}

// ---- waits shared by act and routes -------------------------------------------------------

/// Polls until the screen matches one of `labels` or timeout. Returns the matched label or "".
std::string WaitScreen(ToolContext& ctx, const json& labels, milliseconds timeout, json& state) {
    const auto deadline = steady_clock::now() + timeout;
    do {
        std::string error;
        state = ScreenStateJson(ctx, error);
        if (!state.is_null()) {
            const std::string screen = state["screen"].get<std::string>();
            for (const auto& l : labels) {
                if (l.get<std::string>() == screen) {
                    return screen;
                }
            }
        }
        std::this_thread::sleep_for(milliseconds{250}); // each check captures a frame
    } while (steady_clock::now() < deadline && !ServerStopping() && !ctx.stop.stop_requested());
    return {};
}

/// Polls the probe named in `cond` about every 16 ms until its condition holds. Returns false on
/// timeout or server stop; `values` holds the last read.
bool WaitProbe(ToolContext& ctx, const json& cond, milliseconds timeout, json& values) {
    const auto deadline = steady_clock::now() + timeout;
    const std::string name = cond.at("name").get<std::string>();
    do {
        values = ReadAllProbes(ctx, json::array({name}));
        if (values.contains(name) &&
            ProbeCondition(values[name].value("value", json{}), cond.value("op", "equals"),
                           cond.at("value"))) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds{16});
    } while (steady_clock::now() < deadline && !ServerStopping() && !ctx.stop.stop_requested());
    return false;
}

// ---- tools --------------------------------------------------------------------------------

/// screen_label tool: adds a fingerprint of the current frame (or `region` of it) to a label,
/// or replaces its samples, and returns a PNG of the stored region. Holds g_store_mutex while
/// saving.
ToolResult ScreenLabel(ToolContext& ctx, const json& args) {
    std::string error;
    const auto frame = AnalysisFrame(ctx, error);
    if (!frame) {
        return ToolResult::Error("no frame: " + error);
    }
    const std::string name = args.at("name").get<std::string>();
    std::scoped_lock lock{g_store_mutex};
    json store = LoadStore("screens", ProgramId(ctx));
    json& entry = store["labels"][name];
    if (args.value("replace", false) || !entry.contains("samples")) {
        entry["samples"] = json::array();
    }
    if (args.contains("region")) {
        entry["region"] = args["region"];
    }
    entry["threshold"] = args.value("threshold", entry.value("threshold", 0.9));
    const Region region = ParseRegion(entry.value("region", json{}));
    entry["samples"].push_back(ToHex(Thumb(*frame, region)));
    SaveStore("screens", ProgramId(ctx), store);
    ToolResult result = ToolResult::Structured(json{{"label", name},
                                                    {"samples", entry["samples"].size()},
                                                    {"threshold", entry["threshold"]},
                                                    {"labels", store["labels"].size()},
                                                    {"source", t_frame_source},
                                                    {"note", "The image is the exact region "
                                                             "stored: check it shows this screen."}});
    // Preview of what was stored: the region crop of the analysed frame.
    const u32 x0 = std::min(static_cast<u32>(region.x * frame->width), frame->width - 1);
    const u32 y0 = std::min(static_cast<u32>(region.y * frame->height), frame->height - 1);
    const u32 cw = std::max<u32>(1, std::min<u32>(frame->width - x0, static_cast<u32>(region.w * frame->width)));
    const u32 ch = std::max<u32>(1, std::min<u32>(frame->height - y0, static_cast<u32>(region.h * frame->height)));
    Frame crop{.width = cw, .height = ch};
    crop.bgra.resize(size_t{cw} * ch * 4);
    for (u32 y = 0; y < ch; ++y) {
        std::memcpy(crop.bgra.data() + size_t{y} * cw * 4,
                    frame->bgra.data() + (size_t{y0 + y} * frame->width + x0) * 4, size_t{cw} * 4);
    }
    result.AddImage(EncodePng(crop, 320), "image/png");
    return result;
}

/// screen_state tool: the best matching screen label and scores for the current frame.
ToolResult ScreenState(ToolContext& ctx, const json&) {
    std::string error;
    json state = ScreenStateJson(ctx, error);
    if (state.is_null()) {
        return ToolResult::Error(error);
    }
    return ToolResult::Structured(std::move(state));
}

/// screen_labels tool: lists the game's labels, first removing `remove` if given.
ToolResult ScreenLabels(ToolContext& ctx, const json& args) {
    std::scoped_lock lock{g_store_mutex};
    json store = LoadStore("screens", ProgramId(ctx));
    if (args.contains("remove") && store.contains("labels")) {
        store["labels"].erase(args["remove"].get<std::string>());
        SaveStore("screens", ProgramId(ctx), store);
    }
    json list = json::array();
    // Named local: iterating items() of the temporary value() returns would dangle.
    const json labels = store.value("labels", json::object());
    for (const auto& [name, entry] : labels.items()) {
        list.push_back({{"label", name},
                        {"samples", entry.value("samples", json::array()).size()},
                        {"threshold", entry.value("threshold", 0.9)},
                        {"region", entry.value("region", json{})}});
    }
    return ToolResult::Structured(json{{"labels", std::move(list)}});
}

/// screenshot_raw tool: PNG of the capture layer plus the guest frame number.
ToolResult ScreenshotRaw(ToolContext& ctx, const json& args) {
    std::string error;
    const auto frame = RawCapture(ctx, error);
    if (!frame) {
        return ToolResult::Error(error);
    }
    ToolResult r;
    r.AddImage(EncodePng(*frame, args.value("max_width", 1280U)), "image/png");
    r.AddText(fmt::format("raw 1280x720 capture layer, guest frame {}", PresentedFrames()));
    return r;
}

/// probe_save tool: stores the probe and returns its current value. The probe is saved even if
/// that read fails.
ToolResult ProbeSave(ToolContext& ctx, const json& args) {
    const std::string name = args.at("name").get<std::string>();
    json probe{{"address", args.at("address")}, {"type", args.value("type", "u32")}};
    if (args.contains("meanings")) {
        probe["meanings"] = args["meanings"];
    }
    if (args.contains("note")) {
        probe["note"] = args["note"];
    }
    json check = ReadProbe(ctx, name, probe);
    std::scoped_lock lock{g_store_mutex};
    json store = LoadStore("probes", ProgramId(ctx));
    store["probes"][name] = probe;
    SaveStore("probes", ProgramId(ctx), store);
    return ToolResult::Structured(json{{"saved", name}, {"now", std::move(check)}});
}

/// probe_read tool: reads the probes, then deletes `remove` if given (it still appears in this
/// result).
ToolResult ProbeRead(ToolContext& ctx, const json& args) {
    json values = ReadAllProbes(ctx, args.value("names", json::array()));
    if (args.value("remove", std::string{}) != "") {
        std::scoped_lock lock{g_store_mutex};
        json store = LoadStore("probes", ProgramId(ctx));
        store["probes"].erase(args["remove"].get<std::string>());
        SaveStore("probes", ProgramId(ctx), store);
    }
    return ToolResult::Structured(json{{"probes", std::move(values)}});
}

/// Asked before every input a route sends: returns why the route may no longer drive the game, or
/// "" while it may. Routes can run for minutes, so permissions are re-checked as they go.
using RouteAuthority = std::function<std::string()>;

/// Runs one route step; returns ok and fills `result`. `authority` is checked before every attempt;
/// when it refuses, result["stopped"] is set and false is returned.
bool RunStep(ToolContext& ctx, const json& step, json& result, const RouteAuthority& authority) {
    const auto timeout = milliseconds{step.value("timeout_ms", 30000)};
    const size_t npad = NpadIndex(step);
    // Repeat: press the input until the wait condition holds (e.g. A until 'continue_menu').
    const int repeats = step.value("repeat_until", false) ? step.value("max_repeats", 20) : 1;
    for (int attempt = 0; attempt < repeats; ++attempt) {
        if (std::string reason = authority(); !reason.empty()) {
            result["stopped"] = reason;
            return false;
        }
        if (step.contains("input")) {
            if (std::string error = RunInputSteps(ctx, npad, step["input"]); !error.empty()) {
                result["error"] = error;
                return false;
            }
        }
        if (step.contains("wait_frames")) {
            WaitFrames(step["wait_frames"].get<u64>(), timeout, ctx.stop);
        }
        if (step.contains("wait_ms")) {
            SleepUnlessStopping(milliseconds{step["wait_ms"].get<int>()}, ctx.stop);
        }
        const auto per_try = repeats > 1 ? milliseconds{step.value("repeat_interval_ms", 1500)} : timeout;
        bool ok = true;
        if (step.contains("wait_screen")) {
            json state;
            const std::string hit = WaitScreen(ctx, step["wait_screen"], per_try, state);
            result["screen"] = state.is_null() ? json("unknown") : state["screen"];
            ok = !hit.empty();
        }
        if (ok && step.contains("wait_not_screen")) {
            // Until the screen is none of these (e.g. loading finished).
            const auto deadline = steady_clock::now() + per_try;
            ok = false;
            do {
                std::string error;
                const json state = ScreenStateJson(ctx, error);
                const std::string screen = state.is_null() ? "" : state["screen"].get<std::string>();
                const auto& labels = step["wait_not_screen"];
                if (!screen.empty() &&
                    std::none_of(labels.begin(), labels.end(),
                                 [&](const json& l) { return l.get<std::string>() == screen; })) {
                    result["screen"] = screen;
                    ok = true;
                    break;
                }
                std::this_thread::sleep_for(milliseconds{250});
            } while (steady_clock::now() < deadline && !ServerStopping() &&
                     !ctx.stop.stop_requested());
        }
        if (ok && step.contains("wait_probe")) {
            json values;
            ok = WaitProbe(ctx, step["wait_probe"], per_try, values);
            result["probe"] = values;
        }
        if (ok) {
            result["attempts"] = attempt + 1;
            return true;
        }
    }
    result["timed_out"] = true;
    return !step.value("required", true);
}

ToolResult RouteSave(ToolContext& ctx, const json& args) {
    std::scoped_lock lock{g_store_mutex};
    json store = LoadStore("routes", ProgramId(ctx));
    store["routes"][args.at("name").get<std::string>()] = {
        {"steps", args.at("steps")}, {"description", args.value("description", "")}};
    SaveStore("routes", ProgramId(ctx), store);
    return ToolResult::Structured(json{{"saved", args["name"]}, {"steps", args["steps"].size()}});
}

/// route_list tool: removes `remove` if given, then lists the game's routes (full steps, or only
/// step counts with `summary`) and the store file path.
ToolResult RouteList(ToolContext& ctx, const json& args) {
    std::scoped_lock lock{g_store_mutex};
    json store = LoadStore("routes", ProgramId(ctx));
    if (args.contains("remove") && store.contains("routes")) {
        store["routes"].erase(args["remove"].get<std::string>());
        SaveStore("routes", ProgramId(ctx), store);
    }
    const bool summary = args.value("summary", false);
    json list = json::array();
    const json routes = store.value("routes", json::object());
    for (const auto& [name, r] : routes.items()) {
        json entry{{"name", name},
                   {"description", r.value("description", "")},
                   {"recorded", r.value("recorded", false)}};
        const json steps = r.value("steps", json::array());
        if (summary) {
            entry["step_count"] = steps.size();
        } else {
            entry["steps"] = steps;
        }
        list.push_back(std::move(entry));
    }
    return ToolResult::Structured(
        json{{"routes", std::move(list)},
             {"file", Common::FS::PathToUTF8String(StorePath("routes", ProgramId(ctx)))}});
}

// ---- checkpoints (recorded fingerprints) --------------------------------------------------

/// Similarity (0..1) of the frame to a recorded checkpoint's thumbnail.
double CheckpointScore(const Frame& frame, const json& checkpoint) {
    const auto thumb = Thumb(frame, ParseRegion(checkpoint.value("region", json{})));
    return Similarity(thumb, FromHex(checkpoint.at("thumb").get<std::string>()));
}

/// Waits until the screen matches the checkpoint of step `index` or of one of the next few
/// checkpoint steps (a recording can hold presses the replay doesn't need, e.g. a second A
/// where the first one was ignored). While waiting, if the screen still matches the previous
/// checkpoint long after its input, that input is fired again (the replay's press was ignored).
/// Returns the matched step index, or -1 on timeout/stop. `authority` is checked on every poll
/// (which also renews the control lease while waiting); when it refuses, result["stopped"] is set.
int WaitCheckpoint(ToolContext& ctx, const json& steps, size_t index, int refire_from,
                   std::stop_token stop, json& result, const RouteAuthority& authority) {
    const auto& step = steps[index];
    const auto began = steady_clock::now();
    const auto deadline = began + milliseconds{step.value("timeout_ms", 120000)};
    auto last_fire = began;
    int refires = 0;
    int consecutive = 0;
    double best_score = 0.0;
    // A later checkpoint may only be skipped to if its screen differs from the one just left
    // (e.g. "back in gameplay" looks like "in gameplay" before the menu even opened), and not
    // while the game is still reacting to the last input.
    const auto can_skip_to = [&](size_t j) {
        if (steady_clock::now() - began < milliseconds{1500}) {
            return false;
        }
        if (refire_from < 0 || !steps[refire_from].contains("checkpoint")) {
            return true;
        }
        const auto prev = FromHex(steps[refire_from]["checkpoint"]["thumb"].get<std::string>());
        const auto later = FromHex(steps[j]["checkpoint"]["thumb"].get<std::string>());
        return Similarity(prev, later) < 0.9;
    };
    while (!stop.stop_requested() && !ServerStopping() && steady_clock::now() < deadline) {
        if (std::string reason = authority(); !reason.empty()) {
            result["stopped"] = reason;
            return -1;
        }
        std::string error;
        if (const auto frame = AnalysisFrame(ctx, error)) {
            for (size_t j = index; j < steps.size() && j < index + 4; ++j) {
                if (!steps[j].contains("checkpoint") || (j != index && !can_skip_to(j))) {
                    continue;
                }
                const auto& cp = steps[j]["checkpoint"];
                const double score = CheckpointScore(*frame, cp);
                if (j == index) {
                    best_score = std::max(best_score, score);
                }
                if (score < cp.value("threshold", 0.85)) {
                    if (j == index) {
                        consecutive = 0;
                    }
                    continue;
                }
                // Fire only once the screen has settled (two checks in a row): menus that are
                // still animating in ignore input.
                if (j == index && ++consecutive < 2) {
                    continue;
                }
                {
                    result["checkpoint_score"] = std::round(score * 1000) / 1000;
                    if (j != index) {
                        result["skipped_to"] = j;
                    }
                    return static_cast<int>(j);
                }
            }
            // Still on the previous checkpoint's screen well after its input: press again.
            if (refire_from >= 0 && refires < 5 && steady_clock::now() - last_fire > seconds{3} &&
                steps[refire_from].contains("input") &&
                CheckpointScore(*frame, steps[refire_from]["checkpoint"]) >=
                    steps[refire_from]["checkpoint"].value("threshold", 0.85)) {
                RunInputSteps(ctx, NpadIndex(steps[refire_from]), steps[refire_from]["input"]);
                last_fire = steady_clock::now();
                ++refires;
                result["refired_previous"] = refires;
            }
        }
        std::this_thread::sleep_for(milliseconds{250}); // ~4 captures a second while waiting
    }
    result["best_score"] = std::round(best_score * 1000) / 1000;
    return -1;
}

// ---- route runner (blocking and background) -----------------------------------------------

/// Runs steps; reports progress to Activity and the timeline. Returns whether it completed.
bool ExecuteRoute(ToolContext ctx, const std::string& name, const json& steps,
                  std::stop_token stop, json& results) {
    const auto set_progress = [&](size_t done, std::string state, std::string detail) {
        Activity::Get().Update([&](ActivityState& s) {
            s.route = name;
            s.route_step = done;
            s.route_total = steps.size();
            s.route_state = std::move(state);
            s.route_detail = std::move(detail);
            s.route_changed = steady_clock::now();
        });
    };
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "route", "route " + name + " started");
    }
    set_progress(0, "running", "starting");
    // Every route stops when asked to or when the game stops. An agent's route also keeps the
    // control lease while it runs (each check renews it, never re-takes it) and stops if it lost
    // the lease or the user turned Allow Writes off. Routes the user starts from the menu have no
    // session, so only the first two apply to them.
    const RouteAuthority authority = [&]() -> std::string {
        if (stop.stop_requested() || ServerStopping()) {
            return "stopped by request";
        }
        if (!ctx.GameRunning()) {
            return "the game stopped";
        }
        if (ctx.session_id.empty()) {
            return {};
        }
        if (ctx.live_writes != nullptr && !ctx.live_writes->load()) {
            return "Allow Writes was turned off";
        }
        if (ctx.control != nullptr && !ctx.control->Renew(ctx.session_id)) {
            return "lost the control lease";
        }
        return {};
    };
    const auto stop_route = [&](size_t step, const std::string& reason) {
        set_progress(step, "stopped", reason);
        if (ctx.timeline) {
            ctx.timeline->Push(EventKind::State, 0, "route",
                               "route " + name + " stopped: " + reason);
        }
        return false;
    };
    int last_checkpoint = -1;
    bool ok = true;
    for (size_t i = 0; i < steps.size() && ok; ++i) {
        if (std::string reason = authority(); !reason.empty()) {
            return stop_route(i, reason);
        }
        json r{{"step", i}};
        if (steps[i].contains("label")) {
            r["label"] = steps[i]["label"];
        }
        if (steps[i].contains("checkpoint")) {
            set_progress(i, "running", fmt::format("waiting for checkpoint {}", i));
            const int matched = WaitCheckpoint(ctx, steps, i, last_checkpoint, stop, r, authority);
            if (r.contains("stopped")) {
                return stop_route(i, r["stopped"].get<std::string>());
            }
            if (matched < 0) {
                r["timed_out"] = true;
                ok = !steps[i].value("required", true);
                results.push_back(std::move(r));
                continue;
            }
            i = static_cast<size_t>(matched);
            r["step"] = i;
            last_checkpoint = static_cast<int>(i);
            // Measure here (settled screen, before the input): benchmark routes.
            if (steps[i].contains("sample")) {
                set_progress(i, "running", fmt::format("measuring at step {}", i));
                r["sample"] = RunPerfSample(ctx, steps[i]["sample"]);
            }
            if (steps[i].contains("input")) {
                // The checkpoint wait and sample can be long: check again before sending input.
                if (std::string reason = authority(); !reason.empty()) {
                    return stop_route(i, reason);
                }
                if (std::string error = RunInputSteps(ctx, NpadIndex(steps[i]), steps[i]["input"]);
                    !error.empty()) {
                    r["error"] = error;
                    ok = false;
                }
            }
            r["attempts"] = 1;
        } else {
            ok = RunStep(ctx, steps[i], r, authority);
            if (r.contains("stopped")) {
                return stop_route(i, r["stopped"].get<std::string>());
            }
            // Timed steps measure after their input and waits.
            if (ok && steps[i].contains("sample")) {
                set_progress(i, "running", fmt::format("measuring at step {}", i));
                r["sample"] = RunPerfSample(ctx, steps[i]["sample"]);
            }
        }
        if (ctx.timeline) {
            ctx.timeline->Push(EventKind::State, 0, "route",
                               fmt::format("route {}: step {}/{} {}", name, i + 1, steps.size(),
                                           ok ? "done" : "FAILED"));
        }
        results.push_back(std::move(r));
        set_progress(i + 1, "running", "");
    }
    set_progress(ok ? steps.size() : results.size(), ok ? "done" : "failed",
                 ok ? "" : "stopped at step " + std::to_string(results.size() - 1));
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "route",
                           "route " + name + (ok ? " completed" : " failed"));
    }
    return ok;
}

/// The one route run with background:true and its results; fields are guarded by `mutex`.
struct BackgroundRoute {
    std::mutex mutex;
    std::jthread thread;
    std::string name;
    json results = json::array();
    std::string state; ///< running / done / failed / stopped
    steady_clock::time_point started{};
    std::string session; ///< MCP session that started it (empty from the frontend's menu)
};

/// The process-wide background route slot.
BackgroundRoute& Background() {
    static BackgroundRoute bg;
    return bg;
}

/// Inline `steps` if given, else the steps of the saved route `name`; nullopt with `error` set
/// when no such route exists.
std::optional<json> LoadRouteSteps(ToolContext& ctx, const json& args, std::string& error) {
    if (args.contains("steps")) {
        return args["steps"];
    }
    std::scoped_lock lock{g_store_mutex};
    const json store = LoadStore("routes", ProgramId(ctx));
    const std::string name = args.value("name", "");
    if (!store.contains("routes") || !store["routes"].contains(name)) {
        error = "no route '" + name + "' for this game (route_list)";
        return std::nullopt;
    }
    return store["routes"][name]["steps"];
}

/// route_run tool: validates the steps, then either runs them on the calling thread (blocking
/// until done; is_error set if the route failed) or, with `background`, starts them on the
/// background route thread and returns at once. Refuses if a background route is running.
ToolResult RouteRun(ToolContext& ctx, const json& args) {
    std::string error;
    const auto steps_opt = LoadRouteSteps(ctx, args, error);
    if (!steps_opt) {
        return ToolResult::Error(error);
    }
    const json steps = *steps_opt;
    if (!steps.is_array()) {
        return ToolResult::Error("route steps must be an array");
    }
    for (size_t i = 0; i < steps.size(); ++i) {
        if (!steps[i].is_object()) {
            return ToolResult::Error(fmt::format("route step {} is not an object", i));
        }
        if (steps[i].contains("checkpoint") &&
            !(steps[i]["checkpoint"].is_object() &&
              IsHexString(steps[i]["checkpoint"].value("thumb", json{})))) {
            return ToolResult::Error(fmt::format(
                "route step {}: checkpoint.thumb must be a hex fingerprint (record the route with "
                "route_record_start/stop)", i));
        }
    }
    const std::string name = args.value("name", std::string{"(inline)"});
    const u64 since_seq = ctx.timeline ? ctx.timeline->LastSeq() : 0;

    if (args.value("background", false)) {
        auto& bg = Background();
        std::scoped_lock lock{bg.mutex};
        // Checked under the lock the shutdown path takes, so no route starts after it.
        if (ServerStopping()) {
            return ToolResult::Error("the MCP server is stopping");
        }
        if (bg.thread.joinable() && bg.state == "running") {
            return ToolResult::Error("route '" + bg.name + "' is already running (route_status / "
                                     "route_stop)");
        }
        if (bg.thread.joinable()) {
            bg.thread.request_stop();
            bg.thread.join();
        }
        bg.name = name;
        bg.results = json::array();
        bg.state = "running";
        bg.started = steady_clock::now();
        bg.session = ctx.session_id;
        bg.thread = std::jthread([copy = ctx, name, steps](std::stop_token stop) mutable {
            json results = json::array();
            bool ok = false;
            copy.stop = stop; // input steps end early when the route is stopped
            // An exception escaping this thread would terminate the emulator.
            try {
                ok = ExecuteRoute(copy, name, steps, stop, results);
            } catch (const std::exception& e) {
                LOG_ERROR(Debug_MCP, "Route {} failed: {}", name, e.what());
                results.push_back({{"error", e.what()}});
                Activity::Get().Update([&](ActivityState& s) {
                    s.route_state = "failed";
                    s.route_detail = e.what();
                    s.route_changed = steady_clock::now();
                });
            }
            auto& b = Background();
            std::scoped_lock l{b.mutex};
            b.results = std::move(results);
            b.state = stop.stop_requested() ? "stopped" : ok ? "done" : "failed";
        });
        return ToolResult::Structured(
            json{{"started", name},
                 {"steps", steps.size()},
                 {"since_seq", since_seq},
                 {"hint", "Progress: route_status, the status bar, or wait_for {source_prefix: "
                          "'route', contains: 'completed'} / /events/wait."}});
    }

    const auto started = steady_clock::now();
    json results = json::array();
    const bool ok = ExecuteRoute(ctx, name, steps, std::stop_token{}, results);
    json out{{"completed", ok},
             {"steps", std::move(results)},
             {"elapsed_ms", duration_cast<milliseconds>(steady_clock::now() - started).count()},
             {"since_seq", since_seq}};
    ToolResult result = ToolResult::Structured(std::move(out));
    if (args.value("screenshot", true) && ctx.host) {
        std::string shot_error;
        if (const auto png = ctx.host->CaptureScreenshotPng(640, milliseconds{3000}, shot_error)) {
            result.AddImage(*png, "image/png");
        }
    }
    if (!ok) {
        result.is_error = true;
    }
    return result;
}

/// route_status tool: progress from Activity (covers blocking and menu-started routes), plus
/// elapsed time and per-step results when a background route has run.
ToolResult RouteStatus(ToolContext&, const json&) {
    auto& bg = Background();
    std::scoped_lock lock{bg.mutex};
    const auto a = Activity::Get().Snapshot();
    json out{{"route", bg.name.empty() ? a.route : bg.name},
             {"state", bg.state.empty() ? a.route_state : bg.state},
             {"step", a.route_step},
             {"total", a.route_total},
             {"detail", a.route_detail}};
    if (!bg.name.empty()) {
        out["elapsed_ms"] = duration_cast<milliseconds>(steady_clock::now() - bg.started).count();
        out["results"] = bg.results;
    }
    return ToolResult::Structured(std::move(out));
}

/// Stops the background route (only if `session` started it, when given) and waits for it.
/// Returns whether one was stopped.
bool StopBackgroundRoute(const std::string* session = nullptr) {
    auto& bg = Background();
    std::jthread thread;
    {
        std::scoped_lock lock{bg.mutex};
        if (session != nullptr && bg.session != *session) {
            return false;
        }
        thread = std::move(bg.thread);
    }
    if (!thread.joinable()) {
        return false;
    }
    thread.request_stop();
    thread.join();
    return true;
}

/// route_stop tool: stops the background route, if one is running.
ToolResult RouteStop(ToolContext&, const json&) {
    return ToolResult::Text(StopBackgroundRoute() ? "route stopped" : "no background route");
}

// ---- recording ----------------------------------------------------------------------------

/// Records player input every presented frame plus screen fingerprints whenever input starts
/// (and a few times a second, to tell a settled screen from a moving one). route_record_stop
/// turns it into a route: input after a settled screen becomes "wait for that screen, then
/// press"; input during motion becomes timed input.
class Recorder {
public:
    /// Controller state at one presented frame; t_ms is from the start of the recording, sticks
    /// are -1..1.
    struct InputFrame {
        u64 t_ms;
        u64 buttons;
        float lx, ly, rx, ry;
    };
    /// Full-screen luma thumbnail taken at t_ms.
    struct Snapshot {
        u64 t_ms;
        std::vector<u8> thumb;
    };

    static Recorder& Get() {
        static Recorder instance;
        return instance;
    }

    /// Starts the recording thread. Returns false if a recording is already running.
    bool Start(ToolContext ctx, std::string name) {
        std::scoped_lock lock{mutex};
        if (thread.joinable()) {
            return false;
        }
        frames.clear();
        snapshots.clear();
        record_name = std::move(name);
        // Taken now: the game may have closed by the time the recording is saved.
        record_program_id = ProgramId(ctx);
        thread = std::jthread([this, ctx](std::stop_token stop) { Run(stop, ctx); });
        return true;
    }

    /// The program that was running when the last recording started.
    [[nodiscard]] u64 RecordedProgramId() {
        std::scoped_lock lock{mutex};
        return record_program_id;
    }

    /// Stops and returns what was recorded.
    bool Stop(std::vector<InputFrame>& out_frames, std::vector<Snapshot>& out_snapshots,
              std::string& out_name) {
        std::jthread t;
        {
            std::scoped_lock lock{mutex};
            if (!thread.joinable()) {
                return false;
            }
            t = std::move(thread);
        }
        t.request_stop();
        t.join();
        std::scoped_lock lock{mutex};
        out_frames = std::move(frames);
        out_snapshots = std::move(snapshots);
        out_name = record_name;
        return true;
    }

    [[nodiscard]] bool Recording() {
        std::scoped_lock lock{mutex};
        return thread.joinable();
    }

    /// Stops and joins the recording thread if it is still running.
    ~Recorder() {
        if (thread.joinable()) {
            thread.request_stop();
            thread.join();
        }
    }

private:
    /// No buttons held and every stick inside the 0.25 dead zone.
    static bool Neutral(const InputFrame& f) {
        constexpr float dead = 0.25f;
        return f.buttons == 0 && std::abs(f.lx) < dead && std::abs(f.ly) < dead &&
               std::abs(f.rx) < dead && std::abs(f.ry) < dead;
    }

    /// Recording thread: polls every 2 ms and, per new presented frame, stores player 1's (or
    /// handheld's) input. Takes a snapshot when input starts and at most every 500 ms otherwise.
    /// Ends on stop request or when the system powers off.
    void Run(std::stop_token stop, ToolContext ctx) {
        const auto start = steady_clock::now();
        const auto now_ms = [&] {
            return static_cast<u64>(duration_cast<milliseconds>(steady_clock::now() - start).count());
        };
        const auto snapshot = [&] {
            std::string error;
            if (const auto frame = AnalysisFrame(ctx, error)) {
                std::scoped_lock lock{mutex};
                snapshots.push_back({now_ms(), Thumb(*frame, Region{})});
            }
        };
        Activity::Get().Update([&](ActivityState& s) {
            s.recording = true;
            s.record_name = record_name;
            s.record_frames = 0;
            s.record_checkpoints = 0;
        });
        snapshot();
        u64 last_frame = PresentedFrames();
        u64 last_snapshot_ms = 0;
        bool was_neutral = true;
        u64 presses = 0;
        while (!stop.stop_requested() && ctx.system->IsPoweredOn()) {
            std::this_thread::sleep_for(milliseconds{2});
            const u64 frame = PresentedFrames();
            if (frame == last_frame) {
                continue;
            }
            last_frame = frame;
            // Player 1, or the handheld controller when player 1 isn't connected.
            auto& hid = ctx.system->HIDCore();
            auto* controller = hid.GetEmulatedControllerByIndex(0);
            if (!controller || !controller->IsConnected()) {
                controller = hid.GetEmulatedControllerByIndex(8);
            }
            if (!controller) {
                continue;
            }
            const auto sticks = controller->GetSticks();
            InputFrame f{now_ms(), static_cast<u64>(controller->GetNpadButtons().raw),
                         sticks.left.x / 32767.0f, sticks.left.y / 32767.0f,
                         sticks.right.x / 32767.0f, sticks.right.y / 32767.0f};
            const bool neutral = Neutral(f);
            {
                std::scoped_lock lock{mutex};
                frames.push_back(f);
            }
            // The screen as it was when an input started: the checkpoint candidate.
            if (was_neutral && !neutral) {
                snapshot();
                last_snapshot_ms = f.t_ms;
                ++presses;
            } else if (f.t_ms - last_snapshot_ms >= 500) {
                // Each capture costs the renderer a frame copy: keep background ones sparse.
                snapshot();
                last_snapshot_ms = f.t_ms;
            }
            was_neutral = neutral;
            Activity::Get().Update([&](ActivityState& s) {
                s.record_frames = frames.size();
                s.record_checkpoints = presses;
            });
        }
        snapshot(); // the screen it ended on
        Activity::Get().Update([&](ActivityState& s) { s.recording = false; });
    }

    std::mutex mutex;
    std::jthread thread;
    std::string record_name;
    u64 record_program_id = 0;
    std::vector<InputFrame> frames;
    std::vector<Snapshot> snapshots;
};

/// input_sequence button names by bit position in the npad button mask.
constexpr std::array<std::string_view, 16> kNpadButtonNames{
    "A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R",
    "ZL", "ZR", "PLUS", "MINUS", "DLEFT", "DUP", "DRIGHT", "DDOWN"};

/// Input frames [begin, end) as input_sequence steps (runs of equal input, wall-clock holds).
json FramesToInput(const std::vector<Recorder::InputFrame>& frames, size_t begin, size_t end) {
    const auto quant = [](float v) { return std::round(v * 10.0f) / 10.0f; };
    json steps = json::array();
    size_t run = begin;
    for (size_t i = begin + 1; i <= end; ++i) {
        const auto& a = frames[run];
        const bool same = i < end && frames[i].buttons == a.buttons &&
                          quant(frames[i].lx) == quant(a.lx) && quant(frames[i].ly) == quant(a.ly) &&
                          quant(frames[i].rx) == quant(a.rx) && quant(frames[i].ry) == quant(a.ry);
        if (same) {
            continue;
        }
        const u64 until = i < frames.size() ? frames[i].t_ms : a.t_ms + 16;
        json step{{"hold_ms", static_cast<int>(std::max<u64>(16, until - a.t_ms))}};
        json buttons = json::array();
        for (u32 b = 0; b < kNpadButtonNames.size(); ++b) {
            if ((a.buttons >> b) & 1) {
                buttons.push_back(kNpadButtonNames[b]);
            }
        }
        if (!buttons.empty()) {
            step["buttons"] = std::move(buttons);
        }
        if (std::abs(a.lx) >= 0.25f || std::abs(a.ly) >= 0.25f) {
            step["left"] = {quant(a.lx), quant(a.ly)};
        }
        if (std::abs(a.rx) >= 0.25f || std::abs(a.ry) >= 0.25f) {
            step["right"] = {quant(a.rx), quant(a.ry)};
        }
        steps.push_back(std::move(step));
        run = i;
    }
    return steps;
}

/// The last snapshot taken at or before t_ms (20 ms slack), or null if none.
const Recorder::Snapshot* SnapshotAt(const std::vector<Recorder::Snapshot>& snaps, u64 t_ms) {
    const Recorder::Snapshot* best = nullptr;
    for (const auto& s : snaps) {
        if (s.t_ms <= t_ms + 20) {
            best = &s;
        }
    }
    return best;
}

/// Turns a recording into route steps.
/// Each run of non-neutral input becomes one step: a checkpoint step if the screen was settled
/// before it, else a timed (wait_ms) step. Ends with an optional end-screen checkpoint.
/// `summary` gets checkpoint/timed counts, frame count and duration.
json BuildRecordedRoute(const std::vector<Recorder::InputFrame>& frames,
                        const std::vector<Recorder::Snapshot>& snaps, double threshold,
                        json& summary) {
    const auto neutral = [](const Recorder::InputFrame& f) {
        return f.buttons == 0 && std::abs(f.lx) < 0.25f && std::abs(f.ly) < 0.25f &&
               std::abs(f.rx) < 0.25f && std::abs(f.ry) < 0.25f;
    };
    json steps = json::array();
    size_t checkpoints = 0, timed = 0;
    u64 prev_end_ms = 0;
    for (size_t i = 0; i < frames.size();) {
        if (neutral(frames[i])) {
            ++i;
            continue;
        }
        size_t end = i;
        while (end < frames.size() && !neutral(frames[end])) {
            ++end;
        }
        const u64 t0 = frames[i].t_ms;
        json step;
        // Settled screen before the input: it was there 400 ms earlier too.
        const auto* at = SnapshotAt(snaps, t0);
        const auto* before = SnapshotAt(snaps, t0 > 400 ? t0 - 400 : 0);
        const bool settled = at && before && at != before && t0 - prev_end_ms >= 300 &&
                             Similarity(at->thumb, before->thumb) >= 0.92;
        if (settled) {
            step["label"] = fmt::format("checkpoint at {:.1f}s", t0 / 1000.0);
            step["checkpoint"] = {{"thumb", ToHex(at->thumb)}, {"threshold", threshold}};
            step["timeout_ms"] = 120000;
            ++checkpoints;
        } else {
            step["label"] = fmt::format("timed at {:.1f}s", t0 / 1000.0);
            step["wait_ms"] = static_cast<int>(std::min<u64>(t0 - prev_end_ms, 600000));
            ++timed;
        }
        step["input"] = FramesToInput(frames, i, end);
        steps.push_back(std::move(step));
        prev_end_ms = end < frames.size() ? frames[end].t_ms : frames.back().t_ms;
        i = end;
    }
    // Finish by checking the screen the recording ended on.
    if (!snaps.empty()) {
        steps.push_back({{"label", "end screen"},
                         {"checkpoint", {{"thumb", ToHex(snaps.back().thumb)}, {"threshold", threshold}}},
                         {"timeout_ms", 120000},
                         {"required", false}});
    }
    summary = {{"checkpoints", checkpoints},
               {"timed_steps", timed},
               {"frames", frames.size()},
               {"duration_s", frames.empty() ? 0.0 : frames.back().t_ms / 1000.0}};
    return steps;
}

/// route_record_start tool: starts the Recorder thread; error if one is already recording.
ToolResult RouteRecordStart(ToolContext& ctx, const json& args) {
    const std::string name = args.at("name").get<std::string>();
    if (!Recorder::Get().Start(ctx, name)) {
        return ToolResult::Error("already recording (route_record_stop first)");
    }
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::Input, 0, "route", "recording route " + name);
    }
    return ToolResult::Structured(json{
        {"recording", name},
        {"note", "Play normally (controller, keyboard or the input tools). Pause briefly on each "
                 "screen before pressing: input after a settled screen becomes a checkpoint. "
                 "Then call route_record_stop."}});
}

/// route_record_stop tool: stops the Recorder (joins its thread), builds the route and saves it
/// under the program that was running when recording started. Error if not recording or no
/// input was recorded.
ToolResult RouteRecordStop(ToolContext& ctx, const json& args) {
    std::vector<Recorder::InputFrame> frames;
    std::vector<Recorder::Snapshot> snaps;
    std::string name;
    if (!Recorder::Get().Stop(frames, snaps, name)) {
        return ToolResult::Error("not recording");
    }
    json summary;
    const json steps = BuildRecordedRoute(frames, snaps, args.value("threshold", 0.85), summary);
    if (steps.empty()) {
        return ToolResult::Error("nothing recorded (no input?)");
    }
    if (args.value("save", true)) {
        std::scoped_lock lock{g_store_mutex};
        const u64 program_id = Recorder::Get().RecordedProgramId();
        json store = LoadStore("routes", program_id);
        store["routes"][name] = {{"steps", steps},
                                 {"description", args.value("description", "recorded")},
                                 {"recorded", true}};
        SaveStore("routes", program_id, store);
    }
    json outline = json::array();
    for (const auto& s : steps) {
        outline.push_back(fmt::format("{} ({} input steps)", s.value("label", ""),
                                      s.value("input", json::array()).size()));
    }
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::Input, 0, "route", "recorded route " + name);
    }
    summary["route"] = name;
    summary["steps"] = std::move(outline);
    summary["saved"] = args.value("save", true);
    return ToolResult::Structured(std::move(summary));
}

json RegionSchema() {
    return json{{"type", "array"},
                {"items", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                {"minItems", 4},
                {"maxItems", 4},
                {"description", "[x, y, w, h] in 0..1 of the screen: match only this part (e.g. "
                                "a menu box), so changing backgrounds don't matter."}};
}

/// Schema of a route's step list, shared by route_save and route_run.
json RouteStepsSchema() {
    return json{
        {"type", "array"},
        {"minItems", 1},
        {"maxItems", 100},
        {"items",
         {{"type", "object"},
          {"properties",
           {{"label", {{"type", "string"}}},
            {"input", InputStepsSchema()},
            {"player", PlayerSchema()},
            {"wait_frames", {{"type", "integer"}, {"minimum", 0}, {"maximum", 36000}}},
            {"wait_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 600000}}},
            {"wait_screen", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            {"wait_not_screen", {{"type", "array"}, {"items", {{"type", "string"}}},
                                 {"description", "Wait until the screen is none of these."}}},
            {"sample", {{"type", "object"},
                        {"description", "perf_sample arguments: measure when this checkpoint "
                                        "matches, before its input (benchmark routes)."}}},
            {"checkpoint", {{"type", "object"},
                            {"description", "Recorded fingerprint {thumb, threshold, region?}: wait "
                                            "for it BEFORE the input (route_record_stop makes these)."}}},
            {"wait_probe", {{"type", "object"},
                            {"properties",
                             {{"name", {{"type", "string"}}},
                              {"op", {{"type", "string"}, {"enum", {"equals", "not_equals", "greater", "less"}}}},
                              {"value", {{"type", "number"}}}}},
                            {"required", {"name", "value"}}}},
            {"repeat_until", {{"type", "boolean"},
                              {"description", "Repeat input until the wait holds."}}},
            {"max_repeats", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}},
            {"repeat_interval_ms", {{"type", "integer"}, {"minimum", 100}, {"maximum", 60000}}},
            {"timeout_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 600000}}},
            {"required", {{"type", "boolean"}, {"description", "Stop the route on timeout (default true)."}}}}},
          {"additionalProperties", false}}}};
}

} // namespace

std::optional<u64> ResolveGuestAddress(ToolContext& ctx, const std::string& expr,
                                       std::string& error) {
    if (!ctx.GameRunning()) {
        error = "no game is running";
        return std::nullopt;
    }
    return ResolveAddress(ctx, expr, error);
}

json GameScreenState(ToolContext& ctx, std::string& error) {
    return ScreenStateJson(ctx, error);
}

std::string GameWaitScreen(ToolContext& ctx, const json& labels, milliseconds timeout,
                           json& state) {
    return WaitScreen(ctx, labels, timeout, state);
}

json GameProbes(ToolContext& ctx) {
    return ReadAllProbes(ctx, json::array());
}

json RunRouteForTool(ToolContext& ctx, const std::string& name, std::stop_token stop,
                     std::string& error) {
    const auto steps = LoadRouteSteps(ctx, json{{"name", name}}, error);
    if (!steps) {
        return nullptr;
    }
    if (!steps->is_array()) {
        error = "route '" + name + "' has no step list (route_list)";
        return nullptr;
    }
    const auto started = steady_clock::now();
    json results = json::array();
    const bool ok = ExecuteRoute(ctx, name, *steps, stop, results);
    return json{{"ok", ok},
                {"results", std::move(results)},
                {"elapsed_ms", duration_cast<milliseconds>(steady_clock::now() - started).count()}};
}

// ---- screenshot baselines (visual regression checks) --------------------------------------

namespace {
constexpr u32 kBaseW = 96;
constexpr u32 kBaseH = 54;

/// <config>/mcp/baselines/<program id>/<name>.png.
std::filesystem::path BaselinePng(u64 program_id, const std::string& name) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "mcp" / "baselines" /
           fmt::format("{:016X}", program_id) / (name + ".png");
}

/// Baseline names become file names: 1-64 letters, digits, '-' or '_'.
bool ValidName(const std::string& name) {
    return !name.empty() && name.size() <= 64 &&
           std::all_of(name.begin(), name.end(), [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
           });
}
} // namespace

/// Writes the current frame's PNG and stores its fingerprint under `name` for this game.
json BaselineSave(ToolContext& ctx, const std::string& name, std::string& error) {
    if (!ValidName(name)) {
        error = "name: letters, digits, '-' or '_'";
        return nullptr;
    }
    const auto frame = AnalysisFrame(ctx, error);
    if (!frame) {
        return nullptr;
    }
    const u64 pid = ProgramId(ctx);
    const auto png_path = BaselinePng(pid, name);
    std::error_code ec;
    std::filesystem::create_directories(png_path.parent_path(), ec);
    const auto png = EncodePng(*frame, 640);
    if (Common::FS::WriteStringToFile(png_path, Common::FS::FileType::BinaryFile,
                                      std::string{png.begin(), png.end()}) != png.size()) {
        error = "could not write " + Common::FS::PathToUTF8String(png_path);
        return nullptr;
    }
    std::scoped_lock lock{g_store_mutex};
    json store = LoadStore("baselines", pid);
    store["baselines"][name] = {{"thumb", ToHex(ThumbSized(*frame, Region{}, kBaseW, kBaseH))},
                                {"png", Common::FS::PathToUTF8String(png_path)},
                                {"frame", PresentedFrames()}};
    SaveStore("baselines", pid, store);
    return json{{"saved", name}, {"png", Common::FS::PathToUTF8String(png_path)}};
}

/// Scores the current frame's fingerprint against the stored baseline and the threshold.
json BaselineCheck(ToolContext& ctx, const std::string& name, double threshold, std::string& error,
                   std::vector<u8>* current_png) {
    json entry;
    {
        std::scoped_lock lock{g_store_mutex};
        const json store = LoadStore("baselines", ProgramId(ctx));
        if (!store.contains("baselines") || !store["baselines"].contains(name)) {
            error = "no baseline '" + name + "' for this game (baseline_save)";
            return nullptr;
        }
        entry = store["baselines"][name];
    }
    const auto frame = AnalysisFrame(ctx, error);
    if (!frame) {
        return nullptr;
    }
    const double score = Similarity(ThumbSized(*frame, Region{}, kBaseW, kBaseH),
                                    FromHex(entry.at("thumb").get<std::string>()));
    if (current_png) {
        *current_png = EncodePng(*frame, 480);
    }
    return json{{"baseline", name},
                {"score", std::round(score * 1000) / 1000},
                {"threshold", threshold},
                {"pass", score >= threshold},
                {"baseline_png", entry.value("png", "")}};
}

void StopSessionRoutes(const std::string& session) {
    StopBackgroundRoute(&session);
}

void StopGameStateWork() {
    StopBackgroundRoute();
    std::vector<Recorder::InputFrame> frames;
    std::vector<Recorder::Snapshot> snaps;
    std::string name;
    Recorder::Get().Stop(frames, snaps, name);
}

/// PNG of the raw capture layer scaled to max_width, or nullopt with `error` set.
std::optional<std::vector<u8>> RawCapturePng(ToolContext& ctx, u32 max_width, std::string& error) {
    const auto frame = RawCapture(ctx, error);
    if (!frame) {
        return std::nullopt;
    }
    return EncodePng(*frame, max_width);
}

/// Registers the screen label, probe, raw screenshot and route tools.
void RegisterGameStateTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "screen_label",
        .title = "Label the current screen",
        .description =
            "Teach the server what the current screen is (e.g. 'title', 'continue_menu', "
            "'loading', 'gameplay') for this game: stores a small fingerprint (more calls add "
            "samples to the same label). Afterwards screen_state / act / routes recognise it "
            "without screenshots. region limits matching to part of the screen.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                           {"region", RegionSchema()},
                           {"threshold", {{"type", "number"}, {"minimum", 0.5}, {"maximum", 1}}},
                           {"replace", {{"type", "boolean"}}}}},
                         {"required", {"name"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .handler = ScreenLabel,
    });
    registry.Add(Tool{
        .name = "screen_state",
        .title = "Which screen is this?",
        .description = "Matches the current frame against this game's labelled screens and returns "
                       "the best label (or 'unknown') with scores. A few words instead of an image.",
        .annotations = {.read_only = true},
        .requires_game = true,
        .handler = ScreenState,
    });
    registry.Add(Tool{
        .name = "screen_labels",
        .title = "Screen labels",
        .description = "This game's screen labels (samples, threshold, region); remove one by name.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"remove", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .handler = ScreenLabels,
    });
    registry.Add(Tool{
        .name = "screenshot_raw",
        .title = "Raw capture-layer screenshot",
        .description = "The game's last frame from the capture layer: 1280x720, without "
                       "post-processing/upscaling (for comparisons), and it works when the window "
                       "is hidden or the game paused.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"max_width", {{"type", "integer"}, {"minimum", 160}, {"maximum", 1280}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .requires_game = true,
        .handler = ScreenshotRaw,
    });
    registry.Add(Tool{
        .name = "probe_save",
        .title = "Save a memory probe",
        .description =
            "Name a game variable so it can be read in one call: address is 'Module+0xOFF' "
            "(survives ASLR), a pointer chain 'Module+0xOFF->0x10->0x8', or absolute '0x...' "
            "(changes every boot). meanings maps values to names (e.g. {\"3\": \"title\"}). Find "
            "candidates with scan_start/scan_next and watch_add.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                           {"address", {{"type", "string"}}},
                           {"type", {{"type", "string"},
                                     {"enum", {"u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64", "f32", "f64"}}}},
                           {"meanings", {{"type", "object"}}},
                           {"note", {{"type", "string"}}}}},
                         {"required", {"name", "address"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .handler = ProbeSave,
    });
    registry.Add(Tool{
        .name = "probe_read",
        .title = "Read memory probes",
        .description = "Current values (and meanings) of this game's saved probes, all or by name; "
                       "remove deletes one.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"names", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                           {"remove", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .requires_game = true,
        .handler = ProbeRead,
    });
    registry.Add(Tool{
        .name = "route_save",
        .title = "Save a route",
        .description =
            "Save a named recipe for this game, e.g. 'to_gameplay': steps of input, waits for "
            "frames/ms, for a labelled screen (wait_screen) or a probe value (wait_probe), with "
            "repeat_until to press until the wait holds. Run it with route_run.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                           {"description", {{"type", "string"}}},
                           {"steps", RouteStepsSchema()}}},
                         {"required", {"name", "steps"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .handler = RouteSave,
    });
    registry.Add(Tool{
        .name = "route_list",
        .title = "List routes",
        .description = "This game's saved routes (with their steps, or only counts with "
                       "summary:true) and the file they live in; remove deletes one. Routes are "
                       "stored per game under <config>/mcp/routes/<program id>.json.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"summary", {{"type", "boolean"}}}, {"remove", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .handler = RouteList,
    });
    registry.Add(Tool{
        .name = "route_run",
        .title = "Run a route",
        .description = "Run a saved route by name (or inline steps) and report each step (attempts, "
                       "screen reached, checkpoint scores, skips/re-presses), plus a final "
                       "screenshot unless screenshot:false. background:true returns at once: follow "
                       "it with route_status, the status bar, or timeline events from source "
                       "'route' ('... completed' / '... failed').",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"name", {{"type", "string"}}},
                           {"steps", RouteStepsSchema()},
                           {"screenshot", {{"type", "boolean"}}},
                           {"background", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = RouteRun,
    });
    registry.Add(Tool{
        .name = "route_status",
        .title = "Route progress",
        .description = "Progress of the running (or last) route: state, step/total, what it is "
                       "waiting for, and per-step results of a background run.",
        .annotations = {.read_only = true},
        .handler = RouteStatus,
    });
    registry.Add(Tool{
        .name = "route_stop",
        .title = "Stop the background route",
        .description = "Stop a route started with background:true.",
        .requires_control = true,
        .handler = RouteStop,
    });
    registry.Add(Tool{
        .name = "route_record_start",
        .title = "Record a route",
        .description =
            "Start recording what is played (any input source) into a route named name. Every "
            "frame's input is kept, plus a screen fingerprint whenever input starts. Pause briefly "
            "on each screen before pressing: input after a settled screen becomes a checkpoint "
            "(replay waits for that screen, then presses), input during motion is replayed on "
            "timing. Stop with route_record_stop.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}}}},
                         {"required", {"name"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .handler = RouteRecordStart,
    });
    registry.Add(Tool{
        .name = "route_record_stop",
        .title = "Stop recording and save the route",
        .description = "Stop recording, turn it into route steps (checkpoints + timed input + an "
                       "end-screen check) and save it for this game (unless save:false). Returns "
                       "the outline. Replay with route_run.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"save", {{"type", "boolean"}}},
                           {"description", {{"type", "string"}}},
                           {"threshold", {{"type", "number"}, {"minimum", 0.5}, {"maximum", 1},
                                          {"description", "Checkpoint match threshold (default 0.85)."}}}}},
                         {"additionalProperties", false}},
        .handler = RouteRecordStop,
    });
}

} // namespace Mcp

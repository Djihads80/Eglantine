// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Server-side reductions of the timeline (agents pay per token): a clustered summary, JSONL
// export of a run, and a comparison of two exported runs (before/after a change). Plus the
// installed firmware version.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <sstream>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "core/core.h"
#include "frontend_common/firmware_manager.h"
#include "mcp/context.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

/// Message with numbers and hex collapsed to '#', so repeats of one log line cluster together.
std::string Normalize(std::string_view msg) {
    std::string out;
    out.reserve(std::min<size_t>(msg.size(), 200));
    for (size_t i = 0; i < msg.size() && out.size() < 200; ++i) {
        const char c = msg[i];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            // Swallow the whole number, including 0x prefixes and hex digits.
            size_t j = i;
            if (c == '0' && j + 1 < msg.size() && (msg[j + 1] == 'x' || msg[j + 1] == 'X')) {
                j += 2;
            }
            while (j < msg.size() && std::isxdigit(static_cast<unsigned char>(msg[j]))) {
                ++j;
            }
            out += '#';
            i = j - 1;
        } else {
            out += c;
        }
    }
    return out;
}

/// Events sharing kind, source, level and normalized message, with the first message as example.
struct Cluster {
    std::string kind, source, level, pattern, example;
    u64 count = 0, first_frame = 0, last_frame = 0, first_seq = 0;
};

/// Groups EventJson-shaped events into clusters, most frequent first. Expects events oldest first:
/// first_frame/first_seq come from the first event seen and last_frame from the last.
std::vector<Cluster> ClusterEvents(const std::vector<json>& events) {
    std::map<std::string, Cluster> clusters;
    for (const auto& e : events) {
        const std::string kind = e.value("kind", "");
        const std::string source = e.value("source", "");
        const std::string level = e.value("level", "");
        const std::string msg = e.value("msg", "");
        const std::string pattern = Normalize(msg);
        auto& c = clusters[kind + '\x1f' + source + '\x1f' + level + '\x1f' + pattern];
        if (c.count == 0) {
            c = {kind, source, level, pattern, msg.substr(0, 300), 0,
                 e.value("frame", u64{0}), 0, e.value("seq", u64{0})};
        }
        ++c.count;
        c.last_frame = e.value("frame", u64{0});
    }
    std::vector<Cluster> out;
    for (auto& [key, c] : clusters) {
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.count > b.count; });
    return out;
}

/// A cluster as JSON with its frame range; "level" only for log clusters.
json ClusterJson(const Cluster& c) {
    json j{{"kind", c.kind}, {"source", c.source}, {"count", c.count},
           {"frames", fmt::format("{}..{}", c.first_frame, c.last_frame)}, {"example", c.example}};
    if (!c.level.empty()) {
        j["level"] = c.level;
    }
    return j;
}

/// Reads every event after since_seq at min_level or above, in pages of 5000, as EventJson.
/// Stops early once more than 200000 events are collected. ctx.timeline must not be null.
std::vector<json> ReadEvents(ToolContext& ctx, u64 since_seq, int min_level) {
    std::vector<json> events;
    Timeline::Query q;
    q.since_seq = since_seq;
    q.min_level = min_level;
    q.limit = 5000;
    while (true) {
        const auto r = ctx.timeline->Read(q);
        for (const auto& e : r.events) {
            events.push_back(EventJson(e));
        }
        if (!r.truncated || events.size() > 200000) {
            break;
        }
        q.since_seq = r.next_seq;
    }
    return events;
}

/// summarize_since: counts per kind, the "top" largest clusters, up to 50 crash and unimpl events
/// verbatim, current perf, and next_seq. Logs default to warning and above.
ToolResult SummarizeSince(ToolContext& ctx, const json& args) {
    if (!ctx.timeline) {
        return ToolResult::Error("timeline unavailable");
    }
    const u64 since = args.value("since_seq", u64{0});
    const int min_level = args.contains("min_level")
                              ? ParseQuery(json{{"min_level", args["min_level"]}}).min_level
                              : 3; // warning
    const auto events = ReadEvents(ctx, since, min_level);
    std::map<std::string, u64> per_kind;
    json important = json::array();
    for (const auto& e : events) {
        const std::string kind = e.value("kind", "");
        ++per_kind[kind];
        if ((kind == "crash" || kind == "unimpl") && important.size() < 50) {
            important.push_back(e);
        }
    }
    const auto clusters = ClusterEvents(events);
    const size_t top = args.value("top", size_t{25});
    json list = json::array();
    for (size_t i = 0; i < std::min(top, clusters.size()); ++i) {
        list.push_back(ClusterJson(clusters[i]));
    }
    json out{{"events", events.size()},
             {"per_kind", per_kind},
             {"clusters", clusters.size()},
             {"top_clusters", std::move(list)},
             {"crashes_and_unimplemented", std::move(important)},
             {"next_seq", ctx.timeline->LastSeq()},
             {"note", "Logs below min_level (default warning) are not included; numbers in "
                      "messages are collapsed to # when clustering."}};
    if (json perf = PerfJson(ctx); !perf.is_null()) {
        out["perf_now"] = std::move(perf);
    }
    return ToolResult::Structured(std::move(out));
}

/// Exports live under <log dir>/mcp; names only (no paths), so no tool writes or reads elsewhere.
std::optional<std::filesystem::path> ExportPath(const std::string& name) {
    if (name.empty() || name.size() > 64 ||
        !std::all_of(name.begin(), name.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
        }) ||
        name.find("..") != std::string::npos) {
        return std::nullopt;
    }
    return Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "mcp" / (name + ".jsonl");
}

/// timeline_export: writes the events after since_seq (all log levels by default) as one JSON
/// object per line, overwriting any export of the same name. Fails on a bad name, when no events
/// match, or on a short write.
ToolResult TimelineExport(ToolContext& ctx, const json& args) {
    if (!ctx.timeline) {
        return ToolResult::Error("timeline unavailable");
    }
    const auto path = ExportPath(args.at("name").get<std::string>());
    if (!path) {
        return ToolResult::Error("name: letters, digits, '-', '_' or '.', up to 64 characters");
    }
    const int min_level = args.contains("min_level")
                              ? ParseQuery(json{{"min_level", args["min_level"]}}).min_level
                              : 0;
    const auto events = ReadEvents(ctx, args.value("since_seq", u64{0}), min_level);
    if (events.empty()) {
        // Also keeps the write check below meaningful: an empty write cannot be told apart from
        // a file that failed to open.
        return ToolResult::Error("no events match; nothing was exported");
    }
    std::string text;
    for (const auto& e : events) {
        text += e.dump();
        text += '\n';
    }
    std::error_code ec;
    std::filesystem::create_directories(path->parent_path(), ec);
    if (Common::FS::WriteStringToFile(*path, Common::FS::FileType::TextFile, text) != text.size()) {
        return ToolResult::Error("could not write " + Common::FS::PathToUTF8String(*path));
    }
    return ToolResult::Structured(json{{"path", Common::FS::PathToUTF8String(*path)},
                                       {"events", events.size()},
                                       {"bytes", text.size()}});
}

/// Parses a timeline_export file back into events, skipping lines that are not valid JSON.
/// Returns nullopt with `error` set for a bad name or a missing or empty file.
std::optional<std::vector<json>> LoadExport(const std::string& name, std::string& error) {
    const auto path = ExportPath(name);
    if (!path) {
        error = "bad run name: " + name;
        return std::nullopt;
    }
    const std::string text = Common::FS::ReadStringFromFile(*path, Common::FS::FileType::TextFile);
    if (text.empty()) {
        error = "no export named " + name + " (timeline_export first)";
        return std::nullopt;
    }
    std::vector<json> events;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty()) {
            try {
                events.push_back(json::parse(line));
            } catch (...) {
            }
        }
    }
    return events;
}

/// compare_runs: clusters both exports and lists clusters only in a, only in b, and in both with
/// a count ratio above 2x either way. Each list is sorted by count and cut to "limit", with the
/// untrimmed size in "total".
ToolResult CompareRuns(ToolContext&, const json& args) {
    std::string error;
    const auto a = LoadExport(args.at("a").get<std::string>(), error);
    if (!a) {
        return ToolResult::Error(error);
    }
    const auto b = LoadExport(args.at("b").get<std::string>(), error);
    if (!b) {
        return ToolResult::Error(error);
    }
    const auto key = [](const Cluster& c) {
        return c.kind + '\x1f' + c.source + '\x1f' + c.level + '\x1f' + c.pattern;
    };
    std::map<std::string, Cluster> ca, cb;
    for (auto& c : ClusterEvents(*a)) {
        ca[key(c)] = c;
    }
    for (auto& c : ClusterEvents(*b)) {
        cb[key(c)] = c;
    }
    json only_a = json::array(), only_b = json::array(), changed = json::array();
    for (const auto& [k, c] : ca) {
        if (!cb.contains(k)) {
            only_a.push_back(ClusterJson(c));
        } else {
            const double ratio = static_cast<double>(cb[k].count) / static_cast<double>(c.count);
            if (ratio > 2.0 || ratio < 0.5) {
                json j = ClusterJson(c);
                j["count_a"] = c.count;
                j["count_b"] = cb[k].count;
                j.erase("count");
                changed.push_back(std::move(j));
            }
        }
    }
    for (const auto& [k, c] : cb) {
        if (!ca.contains(k)) {
            only_b.push_back(ClusterJson(c));
        }
    }
    const size_t limit = args.value("limit", size_t{30});
    const auto trim = [limit](json& arr) {
        std::sort(arr.begin(), arr.end(), [](const json& x, const json& y) {
            return x.value("count", x.value("count_a", u64{0})) >
                   y.value("count", y.value("count_a", u64{0}));
        });
        if (arr.size() > limit) {
            arr.erase(arr.begin() + static_cast<std::ptrdiff_t>(limit), arr.end());
        }
    };
    const size_t n_only_a = only_a.size(), n_only_b = only_b.size(), n_changed = changed.size();
    trim(only_a);
    trim(only_b);
    trim(changed);
    return ToolResult::Structured(json{
        {"events", {{"a", a->size()}, {"b", b->size()}}},
        {"only_in_a", {{"total", n_only_a}, {"clusters", std::move(only_a)}}},
        {"only_in_b", {{"total", n_only_b}, {"clusters", std::move(only_b)}}},
        {"count_changed_over_2x", {{"total", n_changed}, {"clusters", std::move(changed)}}},
        {"note", "Clusters = event kind + source + level + message with numbers collapsed. "
                 "only_in_b after a fix = new behaviour (new warnings, new unimplemented calls); "
                 "only_in_a = what the change removed."}});
}

/// firmware_status: whether firmware is installed and, if its version can be read, the version
/// and display strings.
ToolResult FirmwareStatus(ToolContext& ctx, const json&) {
    const bool present = FirmwareManager::CheckFirmwarePresence(*ctx.system);
    json out{{"installed", present}};
    if (present) {
        const auto [version, result] = FirmwareManager::GetFirmwareVersion(*ctx.system);
        if (result.IsSuccess()) {
            out["version"] = fmt::format("{}.{}.{}", version.major, version.minor, version.micro);
            out["display_version"] = std::string{version.display_version.data()};
            out["display_title"] = std::string{version.display_title.data()};
        }
    }
    return ToolResult::Structured(std::move(out));
}

json LevelSchema() {
    return json{{"type", "string"},
                {"enum", {"trace", "debug", "info", "warning", "error", "critical"}}};
}

} // namespace

/// Registers summarize_since, timeline_export, compare_runs and firmware_status.
void RegisterTimelineTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "summarize_since",
        .title = "Summarize the timeline",
        .description = "Digest of what happened since since_seq: event counts per kind, the most "
                       "frequent clusters of similar events (numbers collapsed, so thousands of "
                       "repeats are one row with a count and frame range), every crash and newly "
                       "seen unimplemented command, and current perf. Logs at warning+ by default.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"since_seq", {{"type", "integer"}, {"minimum", 0}}},
                           {"min_level", LevelSchema()},
                           {"top", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .handler = SummarizeSince,
    });
    registry.Add(Tool{
        .name = "timeline_export",
        .title = "Export the timeline",
        .description = "Write the timeline since since_seq as JSONL to <log dir>/mcp/<name>.jsonl, "
                       "for bug reports and for compare_runs (e.g. export 'before', change code or "
                       "a knob, replay the same route/TAS, export 'after').",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                           {"since_seq", {{"type", "integer"}, {"minimum", 0}}},
                           {"min_level", LevelSchema()}}},
                         {"required", {"name"}},
                         {"additionalProperties", false}},
        .handler = TimelineExport,
    });
    registry.Add(Tool{
        .name = "compare_runs",
        .title = "Compare two runs",
        .description = "Diff two exported timelines by event cluster: what appears only in a, only "
                       "in b, and what changed more than 2x in frequency. The way to show what a "
                       "code change or setting did to the emulator's behaviour.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"a", {{"type", "string"}}},
                           {"b", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}}}},
                         {"required", {"a", "b"}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = CompareRuns,
    });
    registry.Add(Tool{
        .name = "firmware_status",
        .title = "Firmware status",
        .description = "Whether system firmware is installed and its version.",
        .annotations = {.read_only = true, .idempotent = true},
        .handler = FirmwareStatus,
    });
}

} // namespace Mcp

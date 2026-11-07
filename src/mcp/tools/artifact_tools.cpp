// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Artifacts the emulator writes to disk: shader and macro dumps (dump directory), the reporter's
// JSON reports (<log dir>/<type>/) and the FS access log games can emit. Every path is resolved
// and checked to stay inside those folders; the keys, config and NAND folders are never listed or
// read, even when the dump directory is configured to contain them. Reads are size-bounded.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/settings.h"
#include "mcp/context.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"

namespace Mcp {

namespace {

namespace fs = std::filesystem;

/// Report folders the reporter (core/reporter.cpp) writes under the log directory.
constexpr std::array<std::string_view, 7> kReportTypes{
    "crash_report", "svc_break_report", "unimpl_func_report", "unimpl_applet_report",
    "error_report", "play_report",      "user_report",
};

/// Upper bound on directory entries visited by one listing, so a huge dump folder (e.g. RomFS
/// dumps) cannot stall the call.
constexpr size_t kMaxScannedEntries = 200000;

/// Longest FS access log line returned; longer lines are cut.
constexpr size_t kMaxLineLength = 1000;

/// ASCII lower-case copy of s, for case-insensitive matching.
std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// p with symlinks and dot segments resolved (the missing tail is kept lexically); empty on error.
fs::path Canonical(const fs::path& p) {
    std::error_code ec;
    fs::path out = fs::weakly_canonical(p, ec);
    return ec ? fs::path{} : out;
}

/// Whether the canonical path `child` is `base` itself or below it.
bool IsWithin(const fs::path& child, const fs::path& base) {
    if (child.empty() || base.empty()) {
        return false;
    }
    const fs::path rel = child.lexically_relative(base);
    if (rel.empty()) {
        return false;
    }
    const auto first = rel.begin();
    return first == rel.end() || *first != "..";
}

/// Canonical folders that must never be exposed: keys, config (credentials) and the emulated NAND.
std::vector<fs::path> SensitiveDirs() {
    using Common::FS::EdenPath;
    std::vector<fs::path> dirs;
    for (const EdenPath p : {EdenPath::KeysDir, EdenPath::ConfigDir, EdenPath::NANDDir}) {
        if (fs::path c = Canonical(Common::FS::GetEdenPath(p)); !c.empty()) {
            dirs.push_back(std::move(c));
        }
    }
    return dirs;
}

/// Whether a canonical path is in a sensitive folder or is a key file.
bool IsSensitive(const fs::path& path, const std::vector<fs::path>& sensitive) {
    if (Lower(Common::FS::PathToUTF8String(path.extension())) == ".keys") {
        return true;
    }
    return std::any_of(sensitive.begin(), sensitive.end(),
                       [&](const fs::path& dir) { return IsWithin(path, dir); });
}

/// Canonical dump directory, or empty if it cannot be resolved.
fs::path DumpRoot() {
    return Canonical(Common::FS::GetEdenPath(Common::FS::EdenPath::DumpDir));
}

/// A path relative to `root` as UTF-8 with forward slashes.
std::string RelativeString(const fs::path& path, const fs::path& root) {
    std::string s = Common::FS::PathToUTF8String(path.lexically_relative(root));
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

/**
 * Resolves a caller-supplied relative path inside `root` (canonical).
 * @return The canonical path, or empty with `error` set when the path is absolute, contains '..',
 *         resolves outside root (e.g. through a link) or into a sensitive folder.
 */
fs::path ResolveInside(const fs::path& root, const std::string& rel, std::string& error) {
    if (root.empty()) {
        error = "the dump directory could not be resolved";
        return {};
    }
    const fs::path user{Common::FS::ToU8String(rel)};
    if (user.is_absolute() || user.has_root_name() || user.has_root_directory()) {
        error = "path must be relative to the dump directory (as dump_list returns it)";
        return {};
    }
    for (const auto& part : user) {
        if (part == "..") {
            error = "path must not contain '..'";
            return {};
        }
    }
    const fs::path resolved = Canonical(root / user);
    if (!IsWithin(resolved, root) || IsSensitive(resolved, SensitiveDirs())) {
        error = "path is outside the dump directory";
        return {};
    }
    return resolved;
}

/// Milliseconds since a file's last write (0 if in the future or unreadable).
s64 AgeMs(fs::file_time_type written) {
    const auto age = fs::file_time_type::clock::now() - written;
    return std::max<s64>(0, std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
}

/// Length of the valid UTF-8 sequence at the start of `d`, or 0 if invalid or cut off.
size_t Utf8SequenceLength(std::span<const u8> d) {
    const u8 c = d[0];
    if (c < 0x80) {
        return 1;
    }
    size_t len = 0;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
    } else {
        return 0;
    }
    if (d.size() < len) {
        return 0;
    }
    u32 cp = c & (0x7F >> len);
    for (size_t k = 1; k < len; ++k) {
        if ((d[k] & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (d[k] & 0x3F);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return len;
}

/**
 * Whether bytes are text: valid UTF-8 with no control characters besides tab, CR and LF.
 * @param text_len Set to the length of the text, which excludes a multi-byte character cut off
 *                 at the end of the buffer (only allowed when `cut` is true).
 */
bool LooksLikeText(std::span<const u8> d, bool cut, size_t& text_len) {
    size_t i = 0;
    while (i < d.size()) {
        const u8 c = d[i];
        if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7F) {
            return false;
        }
        const size_t n = Utf8SequenceLength(d.subspan(i));
        if (n == 0) {
            // A lead byte whose continuation lies past the end of this read.
            const size_t rest = d.size() - i;
            if (cut && rest < 4 && c >= 0xC2 && c <= 0xF4 &&
                std::all_of(d.begin() + static_cast<std::ptrdiff_t>(i) + 1, d.end(),
                            [](u8 b) { return (b & 0xC0) == 0x80; })) {
                break;
            }
            return false;
        }
        i += n;
    }
    text_len = i;
    return true;
}

/// Copy of bytes as valid UTF-8, with invalid bytes replaced by U+FFFD (JSON needs valid UTF-8).
std::string SanitizeUtf8(std::span<const u8> d) {
    std::string out;
    out.reserve(d.size());
    size_t i = 0;
    while (i < d.size()) {
        const size_t n = Utf8SequenceLength(d.subspan(i));
        if (n == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(reinterpret_cast<const char*>(d.data() + i), n);
            i += n;
        }
    }
    return out;
}

/// SanitizeUtf8 for a string.
std::string SanitizeUtf8(std::string_view s) {
    return SanitizeUtf8(std::span<const u8>{reinterpret_cast<const u8*>(s.data()), s.size()});
}

/// The three dump settings and the dump directory, as dump_set reports them.
json DumpStateJson() {
    const fs::path dir = Common::FS::GetEdenPath(Common::FS::EdenPath::DumpDir);
    std::error_code ec;
    return json{{"shaders", Settings::values.dump_guest_shaders.GetValue()},
                {"spirv", Settings::values.gpu_log_shader_dumps.GetValue()},
                {"macros", Settings::values.dump_macros.GetValue()},
                {"dump_dir", Common::FS::PathToUTF8String(dir)},
                {"dump_dir_exists", fs::is_directory(dir, ec)}};
}

/// dump_set: turns guest shader, SPIR-V and macro dumping on or off. All settings are read at
/// compile time, so a change applies to the next shader or macro compiled.
ToolResult DumpSet(ToolContext& ctx, const json& args) {
    const auto set = [&](const char* key, Settings::Setting<bool>& setting, const char* label) {
        if (!args.contains(key)) {
            return;
        }
        const bool before = setting.GetValue();
        const bool after = args[key].get<bool>();
        setting.SetValue(after);
        if (ctx.timeline && before != after) {
            ctx.timeline->Push(EventKind::State, 0, "mcp",
                               fmt::format("setting {}: {} -> {}", label, before, after));
        }
    };
    set("shaders", Settings::values.dump_guest_shaders, "dump_guest_shaders");
    set("spirv", Settings::values.gpu_log_shader_dumps, "gpu_log_shader_dumps");
    set("macros", Settings::values.dump_macros, "dump_macros");
    json out = DumpStateJson();
    out["note"] = "Shaders and macros are dumped only when they are compiled: clear the shader "
                  "cache or reach new content to see dumps. Guest shaders are <program>_<hash>_"
                  "<stage>.ash, SPIR-V (Vulkan only) <program>_<hash>.spv, macros "
                  "<program>_<hash>_raw|jit.macro, all in dump_dir. Browse with dump_list.";
    return ToolResult::Structured(std::move(out));
}

/// dump_list: regular files under the dump directory (or a subfolder), newest first, filtered by
/// age and a case-insensitive path substring. Links and sensitive folders are skipped.
ToolResult DumpList(ToolContext&, const json& args) {
    const fs::path root = DumpRoot();
    fs::path dir = root;
    if (const std::string sub = args.value("subdir", ""); !sub.empty()) {
        std::string error;
        dir = ResolveInside(root, sub, error);
        if (dir.empty()) {
            return ToolResult::Error("subdir: " + error);
        }
    }
    json out{{"dump_dir", Common::FS::PathToUTF8String(root)}};
    std::error_code ec;
    if (root.empty() || !fs::is_directory(dir, ec)) {
        out["files"] = json::array();
        out["total"] = 0;
        out["note"] = "No such directory yet: nothing has been dumped (turn dumping on with "
                      "dump_set).";
        return ToolResult::Structured(std::move(out));
    }

    const bool has_since = args.contains("since_ms_ago");
    const s64 since = args.value("since_ms_ago", s64{0});
    const std::string contains = Lower(args.value("contains", ""));
    const size_t limit = args.value("limit", size_t{100});
    const auto sensitive = SensitiveDirs();

    struct Entry {
        std::string path;
        u64 bytes;
        s64 age_ms;
    };
    std::vector<Entry> files;
    size_t scanned = 0;
    bool scan_truncated = false;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (++scanned > kMaxScannedEntries) {
            scan_truncated = true;
            break;
        }
        const fs::directory_entry& entry = *it;
        std::error_code entry_ec;
        const fs::file_status st = entry.symlink_status(entry_ec);
        if (entry_ec || fs::is_symlink(st)) {
            continue;
        }
        if (fs::is_directory(st)) {
            if (IsSensitive(entry.path(), sensitive)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!fs::is_regular_file(st) || IsSensitive(entry.path(), sensitive)) {
            continue;
        }
        const s64 age = AgeMs(entry.last_write_time(entry_ec));
        if (entry_ec || (has_since && age > since)) {
            continue;
        }
        std::string rel = RelativeString(entry.path(), root);
        if (!contains.empty() && Lower(rel).find(contains) == std::string::npos) {
            continue;
        }
        const u64 size = entry.file_size(entry_ec);
        files.push_back({std::move(rel), entry_ec ? 0 : size, age});
    }
    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b) { return a.age_ms < b.age_ms; });

    json list = json::array();
    for (size_t i = 0; i < std::min(limit, files.size()); ++i) {
        list.push_back({{"path", files[i].path},
                        {"bytes", files[i].bytes},
                        {"modified_ms_ago", files[i].age_ms}});
    }
    out["files"] = std::move(list);
    out["total"] = files.size();
    if (files.size() > limit) {
        out["truncated"] = true;
    }
    if (scan_truncated) {
        out["scan_truncated"] = true;
        out["note"] = "Stopped after visiting 200000 entries: narrow with subdir.";
    }
    return ToolResult::Structured(std::move(out));
}

/// dump_read: up to `length` bytes of one dump file from `offset`, as text (valid UTF-8 without
/// control characters) or lowercase hex. The path must resolve inside the dump directory.
ToolResult DumpRead(ToolContext&, const json& args) {
    const std::string rel = args.at("path").get<std::string>();
    std::string error;
    const fs::path path = ResolveInside(DumpRoot(), rel, error);
    if (path.empty()) {
        return ToolResult::Error("path: " + error);
    }
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return ToolResult::Error("no file '" + rel + "' in the dump directory (see dump_list)");
    }
    const u64 size = fs::file_size(path, ec);
    if (ec) {
        return ToolResult::Error("could not read the size of '" + rel + "'");
    }
    const u64 offset = args.value("offset", u64{0});
    if (offset > size) {
        return ToolResult::Error(fmt::format("offset {} is past the end of the file ({} bytes)",
                                             offset, size));
    }
    const u64 want = args.value("length", u64{4096});
    std::vector<u8> bytes(static_cast<size_t>(std::min(want, size - offset)));
    if (!bytes.empty()) {
        Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read,
                                Common::FS::FileType::BinaryFile,
                                Common::FS::FileShareFlag::ShareReadWrite};
        if (!file.IsOpen() || !file.Seek(static_cast<s64>(offset))) {
            return ToolResult::Error("could not open '" + rel + "'");
        }
        bytes.resize(file.ReadSpan(std::span<u8>{bytes}));
    }

    std::string format = args.value("format", "auto");
    size_t text_len = bytes.size();
    const bool cut = offset + bytes.size() < size;
    if (format == "auto") {
        format = LooksLikeText(bytes, cut, text_len) ? "text" : "hex";
    }
    std::string data;
    if (format == "text") {
        bytes.resize(text_len);
        data = SanitizeUtf8(bytes);
    } else {
        data = Common::HexToString(bytes, false);
    }
    json out{{"path", rel},          {"size", size},     {"offset", offset},
             {"length", bytes.size()}, {"format", format}, {"data", std::move(data)}};
    if (offset + bytes.size() < size) {
        out["next_offset"] = offset + bytes.size();
    }
    return ToolResult::Structured(std::move(out));
}

/// Whether `name` is one of kReportTypes.
bool IsReportType(std::string_view name) {
    return std::find(kReportTypes.begin(), kReportTypes.end(), name) != kReportTypes.end();
}

/// Reporter settings and the hint to turn it on, shared by the report tools.
void AddReportingState(json& out) {
    out["reporting_enabled"] = Settings::values.reporting_services.GetValue();
    out["hint"] = "Reports are written only while reporting_services is on (settings_set "
                  "{values: {\"reporting_services\": true}}); turn it on before reproducing.";
}

/// report_list: the reporter's JSON files, newest first, from one type folder or all of them.
/// File names are <TITLEID>_<timestamp>.json; title_id and timestamp are parsed from them.
ToolResult ReportList(ToolContext&, const json& args) {
    const fs::path log_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    std::vector<std::string_view> types(kReportTypes.begin(), kReportTypes.end());
    if (args.contains("type")) {
        const std::string type = args["type"].get<std::string>();
        if (!IsReportType(type)) {
            return ToolResult::Error("unknown report type '" + type + "'");
        }
        types = {*std::find(kReportTypes.begin(), kReportTypes.end(), type)};
    }
    const bool has_since = args.contains("since_ms_ago");
    const s64 since = args.value("since_ms_ago", s64{0});
    const size_t limit = args.value("limit", size_t{100});

    std::vector<json> reports;
    for (const std::string_view type : types) {
        const fs::path dir = log_dir / fs::path{Common::FS::ToU8String(type)};
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            continue;
        }
        size_t scanned = 0;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::directory_iterator{} && ++scanned <= kMaxScannedEntries;
             it.increment(ec)) {
            std::error_code entry_ec;
            if (!fs::is_regular_file(it->symlink_status(entry_ec)) || entry_ec) {
                continue;
            }
            const std::string file = Common::FS::PathToUTF8String(it->path().filename());
            if (!file.ends_with(".json")) {
                continue;
            }
            const s64 age = AgeMs(it->last_write_time(entry_ec));
            if (entry_ec || (has_since && age > since)) {
                continue;
            }
            const std::string stem = file.substr(0, file.size() - 5);
            const size_t sep = stem.find('_');
            const bool named = sep != std::string::npos;
            const u64 bytes = it->file_size(entry_ec);
            reports.push_back({{"type", std::string{type}},
                               {"file", file},
                               {"title_id", named ? stem.substr(0, sep) : std::string{}},
                               {"timestamp", named ? stem.substr(sep + 1) : stem},
                               {"modified_ms_ago", age},
                               {"bytes", entry_ec ? 0 : bytes}});
        }
    }
    std::sort(reports.begin(), reports.end(), [](const json& a, const json& b) {
        return a["modified_ms_ago"].get<s64>() < b["modified_ms_ago"].get<s64>();
    });
    json list = json::array();
    for (size_t i = 0; i < std::min(limit, reports.size()); ++i) {
        list.push_back(std::move(reports[i]));
    }
    json out{{"reports", std::move(list)}, {"total", reports.size()}};
    if (reports.size() > limit) {
        out["truncated"] = true;
    }
    AddReportingState(out);
    return ToolResult::Structured(std::move(out));
}

/// report_read: one report parsed as JSON. The file must be a plain <name>.json in the type's
/// folder and no larger than max_bytes.
ToolResult ReportRead(ToolContext&, const json& args) {
    const std::string type = args.at("type").get<std::string>();
    const std::string file = args.at("file").get<std::string>();
    if (!IsReportType(type)) {
        return ToolResult::Error("unknown report type '" + type + "'");
    }
    if (!file.ends_with(".json") || file.find("..") != std::string::npos ||
        !std::all_of(file.begin(), file.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
        })) {
        return ToolResult::Error("file: a report file name as report_list returns it (no path)");
    }
    const fs::path dir = Canonical(Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                                   fs::path{Common::FS::ToU8String(type)});
    const fs::path path = Canonical(dir / fs::path{Common::FS::ToU8String(file)});
    std::error_code ec;
    if (dir.empty() || path.empty() || path.parent_path() != dir ||
        !fs::is_regular_file(fs::symlink_status(path, ec)) || ec) {
        return ToolResult::Error("no report " + type + "/" + file + " (see report_list)");
    }
    const u64 size = fs::file_size(path, ec);
    const u64 max_bytes = args.value("max_bytes", u64{262144});
    if (ec || size > max_bytes) {
        return ToolResult::Error(fmt::format(
            "report is {} bytes, over max_bytes {}: raise max_bytes (up to 4194304) or pick a "
            "smaller report",
            size, max_bytes));
    }
    const std::string text =
        Common::FS::ReadStringFromFile(path, Common::FS::FileType::BinaryFile);
    json report = json::parse(SanitizeUtf8(text), nullptr, false);
    if (report.is_discarded()) {
        return ToolResult::Error("report " + type + "/" + file + " is not valid JSON");
    }
    json out{{"type", type}, {"file", file}, {"bytes", size}, {"report", std::move(report)}};
    AddReportingState(out);
    return ToolResult::Structured(std::move(out));
}

/// fs_access_log: lines of <sdmc>/FsAccessLog.txt after since_line (1-based numbering), filtered
/// by a case-insensitive substring. A missing file is an empty result, not an error.
ToolResult FsAccessLog(ToolContext&, const json& args) {
    const fs::path path =
        Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir) / "FsAccessLog.txt";
    const u64 since_line = args.value("since_line", u64{0});
    const std::string contains = Lower(args.value("contains", ""));
    const size_t limit = args.value("limit", size_t{200});

    json lines = json::array();
    u64 n = 0;
    u64 next_line = since_line;
    bool truncated = false;
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        std::string line;
        while (in && std::getline(in, line)) {
            ++n;
            if (n <= since_line) {
                continue;
            }
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!contains.empty() && Lower(line).find(contains) == std::string::npos) {
                next_line = n;
                continue;
            }
            if (lines.size() >= limit) {
                truncated = true;
                break;
            }
            if (line.size() > kMaxLineLength) {
                line.resize(kMaxLineLength);
            }
            lines.push_back({{"n", n}, {"text", SanitizeUtf8(line)}});
            next_line = n;
        }
    }
    json out{{"lines", std::move(lines)},
             {"next_line", next_line},
             {"file", Common::FS::PathToUTF8String(path)},
             {"enabled", Settings::values.enable_fs_access_log.GetValue()},
             {"note", "Only games that call OutputAccessLogToSdCard write entries, and only while "
                      "enable_fs_access_log is on; set it before booting the game. The log is "
                      "cleared at each boot. Pass next_line as since_line to read on."}};
    if (truncated) {
        out["truncated"] = true;
    }
    return ToolResult::Structured(std::move(out));
}

/// The `type` argument schema: one of kReportTypes.
json ReportTypeSchema() {
    json values = json::array();
    for (const auto t : kReportTypes) {
        values.push_back(std::string{t});
    }
    return json{{"type", "string"}, {"enum", std::move(values)}};
}

} // namespace

/// Registers dump_set, dump_list, dump_read, report_list, report_read and fs_access_log.
void RegisterArtifactTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "dump_set",
        .title = "Shader and macro dumping",
        .description = "Turn dumping of guest shaders (.ash, for nvdisasm), SPIR-V (.spv, Vulkan) "
                       "and GPU macros on or off. Returns the current state and dump directory. "
                       "Dumps happen when shaders compile, so clear the shader cache or reach new "
                       "content; then dump_list / dump_read.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"shaders", {{"type", "boolean"}}},
                           {"spirv", {{"type", "boolean"}}},
                           {"macros", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .annotations = {.idempotent = true},
        .requires_write = true,
        .requires_control = true,
        .handler = DumpSet,
    });
    registry.Add(Tool{
        .name = "dump_list",
        .title = "List dump files",
        .description = "Files in the dump directory (recursive), newest first, with size and age. "
                       "since_ms_ago keeps recent ones (e.g. what the last scene compiled); "
                       "contains filters the path (e.g. a program id or '.spv').",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"since_ms_ago", {{"type", "integer"}, {"minimum", 0}}},
                           {"subdir", {{"type", "string"}, {"maxLength", 512}}},
                           {"contains", {{"type", "string"}, {"maxLength", 256}}},
                           {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = DumpList,
    });
    registry.Add(Tool{
        .name = "dump_read",
        .title = "Read a dump file",
        .description = "Read part of one dump file by its dump_list path, as text or hex (auto "
                       "picks text for printable UTF-8). Page with offset / next_offset.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 512}}},
                           {"offset", {{"type", "integer"}, {"minimum", 0}}},
                           {"length", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65536}}},
                           {"format", {{"type", "string"}, {"enum", {"auto", "text", "hex"}}}}}},
                         {"required", {"path"}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = DumpRead,
    });
    registry.Add(Tool{
        .name = "report_list",
        .title = "List reports",
        .description = "Crash, svc break, unimplemented function/applet, error, play and user "
                       "reports the emulator saved as JSON, newest first. Written only while "
                       "reporting_services is on.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"type", ReportTypeSchema()},
                           {"since_ms_ago", {{"type", "integer"}, {"minimum", 0}}},
                           {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = ReportList,
    });
    registry.Add(Tool{
        .name = "report_read",
        .title = "Read a report",
        .description = "One saved report as parsed JSON (registers, backtrace, IPC command "
                       "buffer, error text...), by type and file name from report_list.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"type", ReportTypeSchema()},
                           {"file", {{"type", "string"}, {"minLength", 6}, {"maxLength", 128}}},
                           {"max_bytes",
                            {{"type", "integer"}, {"minimum", 1024}, {"maximum", 4194304}}}}},
                         {"required", {"type", "file"}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = ReportRead,
    });
    registry.Add(Tool{
        .name = "fs_access_log",
        .title = "FS access log",
        .description = "Lines of the filesystem access log a game writes via "
                       "OutputAccessLogToSdCard (needs enable_fs_access_log before boot). Page "
                       "with since_line = next_line; contains filters.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"since_line", {{"type", "integer"}, {"minimum", 0}}},
                           {"contains", {{"type", "string"}, {"maxLength", 256}}},
                           {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = FsAccessLog,
    });
}

} // namespace Mcp

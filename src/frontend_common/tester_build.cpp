// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend_common/tester_build.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <ranges>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace TesterBuild {

namespace {

std::string_view Trim(std::string_view text) {
    const auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

bool IsAlnum(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

std::optional<u64> ParsePrNumber(std::string_view text) {
    if (text.empty() || text.size() > 10 ||
        !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    u64 value = 0;
    for (const char c : text) {
        value = value * 10 + static_cast<u64>(c - '0');
    }
    if (value < 1 || value > 1'000'000'000) {
        return std::nullopt;
    }
    return value;
}

bool IsValidBranch(std::string_view name) {
    if (name.empty() || name.size() > 200 || name == "@") {
        return false;
    }
    const auto allowed = [](char c) {
        return IsAlnum(c) || c == '/' || c == '.' || c == '_' || c == '-' || c == '+' || c == '@';
    };
    if (!std::ranges::all_of(name, allowed)) {
        return false;
    }
    if (name.front() == '-' || name.front() == '/' || name.front() == '.' || name.back() == '/' ||
        name.back() == '.' || name.ends_with(".lock")) {
        return false;
    }
    return name.find("..") == std::string_view::npos && name.find("//") == std::string_view::npos &&
           name.find("@{") == std::string_view::npos;
}

std::optional<Source> MakeBranch(std::string_view name) {
    if (!IsValidBranch(name)) {
        return std::nullopt;
    }
    return Source{Source::Kind::Branch, std::string{name}, 0};
}

std::optional<Source> MakePullRequest(std::string_view number) {
    const auto pr = ParsePrNumber(number);
    if (!pr) {
        return std::nullopt;
    }
    return Source{Source::Kind::PullRequest, {}, *pr};
}

std::optional<Source> ParsePullRequestUrl(std::string_view input, std::string_view repo) {
    std::string prefix = "https://github.com/";
    prefix += repo;
    prefix += "/pull/";
    if (repo.empty() || input.size() <= prefix.size() ||
        !EqualsIgnoreCase(input.substr(0, prefix.size()), prefix)) {
        return std::nullopt;
    }
    auto rest = input.substr(prefix.size());
    const auto digits = rest.find_first_not_of("0123456789");
    if (digits != std::string_view::npos && rest[digits] != '/' && rest[digits] != '?' &&
        rest[digits] != '#') {
        return std::nullopt;
    }
    return MakePullRequest(rest.substr(0, digits));
}

std::string ReadFile(const fs::path& file) {
    std::ifstream stream{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::optional<std::string> EnvVar(const char* name) {
#ifdef _WIN32
    const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
    const wchar_t* value = _wgetenv(wide_name.c_str());
    if (value == nullptr || *value == L'\0') {
        return std::nullopt;
    }
    return PathToUtf8(fs::path{value});
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string{value};
#endif
}

bool SameComponent(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    return std::ranges::equal(a.native(), b.native(), [](wchar_t x, wchar_t y) {
        return std::towlower(x) == std::towlower(y);
    });
#else
    return a == b;
#endif
}

std::vector<fs::path> Components(const fs::path& path) {
    std::vector<fs::path> parts;
    for (const auto& part : path.lexically_normal()) {
        if (!part.empty()) {
            parts.push_back(part);
        }
    }
    return parts;
}

bool SamePath(const fs::path& a, const fs::path& b) {
    const auto left = Components(a);
    const auto right = Components(b);
    return left.size() == right.size() &&
           std::ranges::equal(left, right, [](const fs::path& x, const fs::path& y) {
               return SameComponent(x, y);
           });
}

} // namespace

std::string Source::ToString() const {
    return kind == Kind::Branch ? "branch:" + branch : "pr:" + std::to_string(pr);
}

std::string Source::DisplayName() const {
    return kind == Kind::Branch ? branch : "PR #" + std::to_string(pr);
}

std::string Source::RefSpec() const {
    return kind == Kind::Branch ? branch : "pr-" + std::to_string(pr);
}

std::string Source::FetchRef() const {
    return kind == Kind::Branch ? "refs/heads/" + branch
                                : "refs/pull/" + std::to_string(pr) + "/head";
}

std::optional<Source> ParseSource(std::string_view input, std::string_view repo) {
    input = Trim(input);
    if (input.starts_with("branch:")) {
        return MakeBranch(input.substr(7));
    }
    if (input.starts_with("pr:")) {
        return MakePullRequest(input.substr(3));
    }
    if (input.starts_with('#')) {
        return MakePullRequest(input.substr(1));
    }
    if (!input.empty() && std::ranges::all_of(input, [](char c) { return c >= '0' && c <= '9'; })) {
        return MakePullRequest(input);
    }
    if (input.starts_with("https://")) {
        return ParsePullRequestUrl(input, repo);
    }
    return MakeBranch(input);
}

std::string PathToUtf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

fs::path PathFromUtf8(std::string_view text) {
    return fs::path{std::u8string{text.begin(), text.end()}};
}

fs::path StatePath(const fs::path& config_dir) {
    return config_dir / "tester.json";
}

State LoadState(const fs::path& file) {
    State state;
    const auto parsed = json::parse(ReadFile(file), nullptr, false);
    if (!parsed.is_object()) {
        return state;
    }
    const auto text = [&parsed](const char* key) {
        const auto it = parsed.find(key);
        return it != parsed.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    if (const auto source = text("source"); !source.empty()) {
        state.source = ParseSource(source, {});
    }
    state.built_sha = text("built_sha");
    state.current_install = text("current_install");
    state.previous_install = text("previous_install");
    state.release_exe = text("release_exe");
    if (const auto it = parsed.find("built_at"); it != parsed.end() && it->is_number_integer()) {
        state.built_at = it->get<s64>();
    }
    if (const auto it = parsed.find("pr_warning_shown"); it != parsed.end() && it->is_boolean()) {
        state.pr_warning_shown = it->get<bool>();
    }
    return state;
}

bool SaveState(const fs::path& file, const State& state, std::error_code& ec) {
    json out = {
        {"source", state.source ? state.source->ToString() : std::string{}},
        {"built_sha", state.built_sha},
        {"current_install", state.current_install},
        {"previous_install", state.previous_install},
        {"release_exe", state.release_exe},
        {"built_at", state.built_at},
        {"pr_warning_shown", state.pr_warning_shown},
    };
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        return false;
    }
    const auto temp = fs::path{file}.concat(".tmp");
    {
        std::ofstream stream{temp, std::ios::binary};
        stream << out.dump(2) << '\n';
        stream.flush();
        if (!stream.good()) {
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        return false;
    }
    return true;
}

fs::path Layout::InstallDir(std::string_view sha) const {
    return installs / std::string{sha.substr(0, 10)};
}

fs::path DefaultRoot() {
    if (const auto root = EnvVar("EGLANTINE_TESTER_ROOT")) {
        return PathFromUtf8(*root);
    }
#ifdef _WIN32
    if (const auto base = EnvVar("LOCALAPPDATA")) {
        return PathFromUtf8(*base) / "eglantine-tester";
    }
#else
    if (const auto base = EnvVar("XDG_DATA_HOME")) {
        return PathFromUtf8(*base) / "eglantine-tester";
    }
    if (const auto home = EnvVar("HOME")) {
        return PathFromUtf8(*home) / ".local" / "share" / "eglantine-tester";
    }
#endif
    return {};
}

Layout MakeLayout(const fs::path& root) {
    return {root, root / "tools", root / "src", root / "build", root / "installs", root / "logs"};
}

bool IsTesterInstall(const fs::path& exe, const fs::path& root) {
    const auto installs = Components(root / "installs");
    const auto parts = Components(exe);
    return parts.size() > installs.size() &&
           std::ranges::equal(installs, parts | std::views::take(installs.size()),
                              [](const fs::path& a, const fs::path& b) {
                                  return SameComponent(a, b);
                              });
}

Platform CurrentPlatform() {
#ifdef _WIN32
    return Platform::Windows;
#else
    return Platform::Linux;
#endif
}

namespace {

bool IsName(std::string_view text, bool allow_extra) {
    if (text.empty() || !IsAlnum(text.front())) {
        return false;
    }
    return std::ranges::all_of(text, [allow_extra](char c) {
        return IsAlnum(c) || c == '.' || c == '_' || c == '-' ||
               (allow_extra && (c == '+' || c == ':'));
    });
}

// Package names and winget ids.
bool IsPackageName(std::string_view text) {
    return IsName(text, true);
}

bool IsTarget(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) {
        return IsAlnum(c) || c == '_' || c == '.' || c == '-';
    });
}

bool HasControlCharacter(std::string_view text) {
    return std::ranges::any_of(text, [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
    });
}

bool IsConfigureFlag(std::string_view text) {
    return (text.starts_with("-D") || text.starts_with("-G")) && !HasControlCharacter(text);
}

// Digits and dots, as vswhere takes them.
bool IsVersion(std::string_view text) {
    return !text.empty() && text.size() <= 20 && IsDigit(text.front()) &&
           std::ranges::all_of(text, [](char c) { return IsDigit(c) || c == '.'; });
}

bool IsSha256(std::string_view text) {
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    });
}

// Missing is fine and gives nothing; anything but an array of strings that pass `valid` is not.
bool ReadList(const json& object, const char* key, bool (*valid)(std::string_view),
              std::vector<std::string>& out) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->is_array()) {
        return false;
    }
    for (const auto& item : *it) {
        if (!item.is_string() || !valid(item.get_ref<const std::string&>())) {
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

bool ReadString(const json& object, const char* key, std::string& out) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return false;
    }
    out = it->get<std::string>();
    return true;
}

// A path inside an archive: relative, without "..", and empty is the archive root.
bool IsArchivePath(std::string_view text) {
    if (HasControlCharacter(text) || text.starts_with('/') || text.starts_with('\\') ||
        text.find(':') != std::string_view::npos) {
        return false;
    }
    return std::ranges::none_of(fs::path{text}, [](const fs::path& part) { return part == ".."; });
}

bool ParseTools(const json& tools, Platform platform, std::vector<ToolDownload>& out) {
    if (!tools.is_object()) {
        return false;
    }
    const char* key = platform == Platform::Windows ? "windows-x86_64" : "linux-x86_64";
    for (const auto& [name, tool] : tools.items()) {
        if (!IsName(name, false) || !tool.is_object()) {
            return false;
        }
        const auto entry = tool.find(key);
        if (entry == tool.end()) {
            continue;
        }
        ToolDownload download;
        download.name = name;
        if (const auto version = tool.find("version"); version != tool.end()) {
            if (!version->is_string() || HasControlCharacter(version->get<std::string>())) {
                return false;
            }
            download.version = version->get<std::string>();
        }
        if (!entry->is_object() || !ReadString(*entry, "url", download.url) ||
            !download.url.starts_with("https://") || HasControlCharacter(download.url) ||
            !ReadString(*entry, "sha256", download.sha256) || !IsSha256(download.sha256) ||
            !ReadString(*entry, "bin", download.bin) || !IsArchivePath(download.bin)) {
            return false;
        }
        std::ranges::transform(download.sha256, download.sha256.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        out.push_back(std::move(download));
    }
    return true;
}

bool ParseWinget(const json& root, std::vector<WingetPackage>& out) {
    const auto windows = root.find("windows");
    if (windows == root.end()) {
        return true;
    }
    if (!windows->is_object()) {
        return false;
    }
    const auto list = windows->find("winget");
    if (list == windows->end()) {
        return true;
    }
    if (!list->is_array()) {
        return false;
    }
    for (const auto& item : *list) {
        WingetPackage package;
        if (!item.is_object() || !ReadString(item, "provides", package.provides) ||
            !IsPackageName(package.provides) || !ReadString(item, "id", package.id) ||
            !IsPackageName(package.id)) {
            return false;
        }
        if (item.contains("override")) {
            if (!ReadString(item, "override", package.override_args) ||
                HasControlCharacter(package.override_args)) {
                return false;
            }
        }
        if (item.contains("min_version") &&
            (!ReadString(item, "min_version", package.min_version) ||
             !IsVersion(package.min_version))) {
            return false;
        }
        out.push_back(std::move(package));
    }
    return true;
}

bool ParseLinuxPackages(const json& root, std::map<PackageManager, PackageList>& out) {
    const auto linux_entry = root.find("linux");
    if (linux_entry == root.end()) {
        return true;
    }
    if (!linux_entry->is_object()) {
        return false;
    }
    constexpr std::pair<const char*, PackageManager> managers[] = {
        {"apt", PackageManager::Apt},
        {"pacman", PackageManager::Pacman},
        {"dnf", PackageManager::Dnf},
    };
    for (const auto& [key, manager] : managers) {
        const auto it = linux_entry->find(key);
        if (it == linux_entry->end()) {
            continue;
        }
        if (!it->is_object()) {
            return false;
        }
        PackageList list;
        if (const auto verified = it->find("verified"); verified != it->end()) {
            if (!verified->is_boolean()) {
                return false;
            }
            list.verified = verified->get<bool>();
        }
        if (!ReadList(*it, "packages", IsPackageName, list.packages)) {
            return false;
        }
        out[manager] = std::move(list);
    }
    return true;
}

} // namespace

std::optional<Recipe> ParseRecipe(std::string_view text, Platform platform) {
    const auto root = json::parse(text, nullptr, false);
    if (!root.is_object()) {
        return std::nullopt;
    }
    const auto version = root.find("version");
    if (version == root.end() || !version->is_number_integer() || version->get<s64>() < 1 ||
        version->get<s64>() > 1) {
        return std::nullopt;
    }

    Recipe recipe;
    if (const auto tools = root.find("tools"); tools != root.end()) {
        if (!ParseTools(*tools, platform, recipe.tools)) {
            return std::nullopt;
        }
    }
    if (const auto configure = root.find("configure"); configure != root.end()) {
        if (!configure->is_object() ||
            !ReadList(*configure, "common", IsConfigureFlag, recipe.configure) ||
            !ReadList(*configure, platform == Platform::Windows ? "windows" : "linux",
                      IsConfigureFlag, recipe.configure)) {
            return std::nullopt;
        }
    }
    if (!ReadList(root, "targets", IsTarget, recipe.targets) || !ParseWinget(root, recipe.winget) ||
        !ParseLinuxPackages(root, recipe.linux_packages)) {
        return std::nullopt;
    }
    return recipe;
}

Recipe LoadRecipe(const fs::path& src_dir, Platform platform, bool trust_packages) {
    auto embedded = ParseRecipe(EmbeddedRecipe(), platform).value_or(Recipe{});
    const auto file = src_dir / "tools" / "tester-build" / "recipe.json";
    auto recipe = ParseRecipe(ReadFile(file), platform);
    if (!recipe) {
        return embedded;
    }
    if (!trust_packages) {
        recipe->winget = std::move(embedded.winget);
        recipe->linux_packages = std::move(embedded.linux_packages);
    }
    return *recipe;
}

fs::path FindProgram(std::string_view name, std::string_view path_env) {
    if (name.empty()) {
        return {};
    }
#ifdef _WIN32
    constexpr char Separator = ';';
    std::vector<std::string> names;
    if (!EqualsIgnoreCase(name.substr(name.size() >= 4 ? name.size() - 4 : 0), ".exe")) {
        names.push_back(std::string{name} + ".exe");
    }
    names.emplace_back(name);
#else
    constexpr char Separator = ':';
    const std::vector<std::string> names{std::string{name}};
#endif
    while (!path_env.empty()) {
        const auto end = path_env.find(Separator);
        auto dir = path_env.substr(0, end);
        path_env = end == std::string_view::npos ? std::string_view{} : path_env.substr(end + 1);
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') {
            dir = dir.substr(1, dir.size() - 2);
        }
        if (dir.empty()) {
            continue;
        }
        for (const auto& candidate : names) {
            const auto file = PathFromUtf8(dir) / PathFromUtf8(candidate);
            std::error_code ec;
            // symlink_status: status fails on an app execution alias such as winget.
            if (fs::is_regular_file(file, ec) ||
                fs::is_regular_file(fs::symlink_status(file, ec))) {
                return file;
            }
        }
    }
    return {};
}

fs::path VsWherePath() {
    const auto base = EnvVar("ProgramFiles(x86)");
    return (base ? PathFromUtf8(*base) : fs::path{"C:\\Program Files (x86)"}) /
           "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
}

Step VsWhereStep(std::string_view min_version) {
    std::vector<std::string> args = {"-latest", "-products", "*", "-requires",
                                     "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"};
    if (!min_version.empty()) {
        args.push_back("-version");
        args.push_back("[" + std::string{min_version} + ",)");
    }
    args.push_back("-property");
    args.push_back("installationPath");
    return {"vswhere", "Locating Visual Studio", VsWherePath(), std::move(args), {}, false, true};
}

fs::path ParseVsWhere(std::string_view output) {
    std::istringstream stream{std::string{output}};
    for (std::string line; std::getline(stream, line);) {
        const auto trimmed = Trim(line);
        if (!trimmed.empty()) {
            return PathFromUtf8(trimmed);
        }
    }
    return {};
}

Step VcVarsStep(const fs::path& vs_install) {
    const auto bat = vs_install / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat";
    return {"vcvars",
            "Loading the Visual Studio environment",
            "cmd.exe",
            {"/d", "/s", "/c", "\"\"" + PathToUtf8(bat) + "\" x64 >nul && set\""},
            {},
            false,
            true};
}

std::vector<std::pair<std::string, std::string>> ParseEnvironment(std::string_view output) {
    std::vector<std::pair<std::string, std::string>> environment;
    std::istringstream stream{std::string{output}};
    for (std::string line; std::getline(stream, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos || line.starts_with('=')) {
            continue;
        }
        environment.emplace_back(line.substr(0, equals), line.substr(equals + 1));
    }
    return environment;
}

PackageManager ParseOsRelease(std::string_view text) {
    std::string id;
    std::string id_like;
    std::istringstream stream{std::string{text}};
    for (std::string line; std::getline(stream, line);) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const auto key = Trim(std::string_view{line}.substr(0, equals));
        auto value = Trim(std::string_view{line}.substr(equals + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        if (key == "ID") {
            id = value;
        } else if (key == "ID_LIKE") {
            id_like = value;
        }
    }

    const auto classify = [](std::string_view name) {
        constexpr std::pair<std::string_view, PackageManager> known[] = {
            {"debian", PackageManager::Apt},       {"ubuntu", PackageManager::Apt},
            {"linuxmint", PackageManager::Apt},    {"pop", PackageManager::Apt},
            {"arch", PackageManager::Pacman},      {"manjaro", PackageManager::Pacman},
            {"endeavouros", PackageManager::Pacman}, {"cachyos", PackageManager::Pacman},
            {"fedora", PackageManager::Dnf},       {"rhel", PackageManager::Dnf},
            {"centos", PackageManager::Dnf},       {"almalinux", PackageManager::Dnf},
            {"rocky", PackageManager::Dnf},
        };
        for (const auto& [candidate, manager] : known) {
            if (EqualsIgnoreCase(name, candidate)) {
                return manager;
            }
        }
        return PackageManager::None;
    };

    if (const auto manager = classify(id); manager != PackageManager::None) {
        return manager;
    }
    std::istringstream like{id_like};
    for (std::string word; like >> word;) {
        if (const auto manager = classify(word); manager != PackageManager::None) {
            return manager;
        }
    }
    return PackageManager::None;
}

Step InstalledQueryStep(PackageManager manager, const std::vector<std::string>& packages) {
    Step step{"query-packages", "Checking installed packages", {}, {}, {}, false, true};
    switch (manager) {
    case PackageManager::Apt:
        step.program = "dpkg-query";
        step.args = {"-W", "-f=${Package}\\t${db:Status-Abbrev}\\n"};
        break;
    case PackageManager::Pacman:
        step.program = "pacman";
        step.args = {"-T"};
        break;
    case PackageManager::Dnf:
        step.program = "rpm";
        step.args = {"-q", "--qf", "%{NAME}\\n"};
        break;
    case PackageManager::None:
        return step;
    }
    step.args.insert(step.args.end(), packages.begin(), packages.end());
    return step;
}

std::vector<std::string> ParseMissing(PackageManager manager, std::string_view output,
                                      const std::vector<std::string>& packages) {
    std::set<std::string> installed;
    std::vector<std::string> missing;
    std::istringstream stream{std::string{output}};
    for (std::string line; std::getline(stream, line);) {
        const std::string text{Trim(line)};
        if (text.empty()) {
            continue;
        }
        switch (manager) {
        case PackageManager::Apt: {
            const auto tab = text.find('\t');
            if (tab == std::string::npos || !Trim(std::string_view{text}.substr(tab + 1))
                                                 .starts_with("ii")) {
                break;
            }
            auto name = text.substr(0, tab);
            name.resize(name.find(':') == std::string::npos ? name.size() : name.find(':'));
            installed.insert(name);
            break;
        }
        case PackageManager::Pacman:
            missing.push_back(text);
            break;
        case PackageManager::Dnf:
            if (text.find("is not installed") == std::string::npos) {
                installed.insert(text);
            }
            break;
        case PackageManager::None:
            break;
        }
    }
    if (manager == PackageManager::Pacman) {
        return missing;
    }
    for (const auto& package : packages) {
        if (!installed.contains(package)) {
            missing.push_back(package);
        }
    }
    return missing;
}

Step InstallPackagesStep(PackageManager manager, const std::vector<std::string>& missing) {
    Step step{"install-packages", "Installing packages", {}, {}, {}, true, false};
    switch (manager) {
    case PackageManager::Apt:
        step.program = "apt-get";
        step.args = {"install", "-y", "--no-install-recommends"};
        break;
    case PackageManager::Pacman:
        step.program = "pacman";
        step.args = {"-S", "--needed", "--noconfirm"};
        break;
    case PackageManager::Dnf:
        step.program = "dnf";
        step.args = {"install", "-y"};
        break;
    case PackageManager::None:
        return step;
    }
    step.args.insert(step.args.end(), missing.begin(), missing.end());
    return step;
}

std::string InstallCommandText(const Step& step) {
    const auto quote = [](const std::string& arg) {
        const bool plain = !arg.empty() && std::ranges::all_of(arg, [](char c) {
            return IsAlnum(c) || std::string_view{"._+:/=@%-"}.find(c) != std::string_view::npos;
        });
        if (plain) {
            return arg;
        }
        std::string quoted = "'";
        for (const char c : arg) {
            quoted += c == '\'' ? std::string{"'\\''"} : std::string{c};
        }
        return quoted + "'";
    };
    std::string text = "sudo " + PathToUtf8(step.program);
    for (const auto& arg : step.args) {
        text += ' ' + quote(arg);
    }
    return text;
}

std::vector<Step> WingetSteps(const Recipe& recipe,
                              const std::vector<std::string>& missing_provides) {
    std::vector<Step> steps;
    for (const auto& package : recipe.winget) {
        if (std::ranges::find(missing_provides, package.provides) == missing_provides.end()) {
            continue;
        }
        Step step{"winget-" + package.provides, "Installing " + package.provides, "winget",
                  {"install", "--id", package.id, "-e", "--accept-source-agreements",
                   "--accept-package-agreements", "--disable-interactivity"},
                  {}, false, false};
        if (!package.override_args.empty()) {
            step.args.push_back("--override");
            step.args.push_back(package.override_args);
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

std::vector<std::string> MissingProvides(const Toolchain& tools) {
    std::vector<std::string> missing;
    if (!tools.compiler) {
        missing.emplace_back("msvc");
    }
    if (tools.git.empty()) {
        missing.emplace_back("git");
    }
    if (tools.glslang.empty()) {
        missing.emplace_back("glslang");
    }
    return missing;
}

std::vector<Step> FetchSteps(const BuildRequest& request) {
    const auto& layout = request.layout;
    const auto& git = request.tools.git;
    const auto src = PathToUtf8(layout.src);
    std::vector<Step> steps;
    std::error_code ec;
    if (!fs::exists(layout.src / ".git", ec)) {
        steps.push_back({"clone",
                         "Cloning the repository",
                         git,
                         {"clone", "--filter=blob:none", "--no-checkout", request.clone_url, src},
                         {},
                         false,
                         false});
    }
    steps.push_back({"fetch",
                     "Fetching " + request.source.DisplayName(),
                     git,
                     {"-C", src, "fetch", "--filter=blob:none", "--force", "origin",
                      "+" + request.source.FetchRef() + ":refs/tester/head"},
                     {},
                     false,
                     false});
    steps.push_back({"checkout",
                     "Checking out the source",
                     git,
                     {"-C", src, "checkout", "--force", "--detach", "refs/tester/head"},
                     {},
                     false,
                     false});
    steps.push_back({"rev-parse", "Reading the commit", git, {"-C", src, "rev-parse", "HEAD"}, {},
                     false, true});
    return steps;
}

std::optional<std::string> ParseRevParse(std::string_view output) {
    const auto sha = Trim(output);
    if (sha.size() != 40 || !std::ranges::all_of(sha, [](char c) {
            return std::isxdigit(static_cast<unsigned char>(c)) != 0;
        })) {
        return std::nullopt;
    }
    std::string lower{sha};
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower;
}

bool WriteStamp(const Layout& layout, const Source& source, std::string_view sha,
                std::error_code& ec) {
    if (!ParseRevParse(sha)) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    const auto write = [&](const char* name, std::string_view content) {
        std::ofstream stream{layout.src / name, std::ios::binary};
        stream << content << '\n';
        stream.flush();
        return stream.good();
    };
    const auto label = source.RefSpec();
    if (!write("GIT-COMMIT", sha) || !write("GIT-REFSPEC", label) || !write("GIT-TAG", label)) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    // A stale release stamp would make the build report itself as a release.
    fs::remove(layout.src / "GIT-RELEASE", ec);
    return !ec;
}

std::vector<Step> BuildSteps(const BuildRequest& request) {
    const auto& layout = request.layout;
    std::vector<std::string> configure = {"-S",
                                          PathToUtf8(layout.src),
                                          "-B",
                                          PathToUtf8(layout.build),
                                          "-G",
                                          "Ninja",
                                          "-DCMAKE_MAKE_PROGRAM=" + PathToUtf8(request.tools.ninja)};
    // The recipe cannot pick the generator, the make program or the repository updates come from.
    for (const auto& flag : request.recipe.configure) {
        if (!flag.starts_with("-G") && !flag.starts_with("-DEGLANTINE_UPDATE_REPO") &&
            !flag.starts_with("-DCMAKE_MAKE_PROGRAM")) {
            configure.push_back(flag);
        }
    }
    configure.push_back("-DEGLANTINE_UPDATE_REPO=" + request.repo);

    std::vector<std::string> build = {"--build", PathToUtf8(layout.build)};
    if (!request.recipe.targets.empty()) {
        build.emplace_back("--target");
        build.insert(build.end(), request.recipe.targets.begin(), request.recipe.targets.end());
    }
    return {
        {"configure", "Configuring", request.tools.cmake, std::move(configure), {}, false, false},
        {"build", "Building", request.tools.cmake, std::move(build), {}, false, false},
    };
}

std::optional<std::pair<u32, u32>> ParseNinjaProgress(std::string_view line) {
    if (!line.starts_with('[')) {
        return std::nullopt;
    }
    line.remove_prefix(1);
    const auto number = [&line](u32& out) {
        std::size_t digits = 0;
        u64 value = 0;
        while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') {
            value = value * 10 + static_cast<u64>(line[digits] - '0');
            if (value > 0xFFFFFFFF) {
                return false;
            }
            ++digits;
        }
        if (digits == 0) {
            return false;
        }
        out = static_cast<u32>(value);
        line.remove_prefix(digits);
        return true;
    };
    u32 done = 0;
    u32 total = 0;
    if (!number(done) || !line.starts_with('/')) {
        return std::nullopt;
    }
    line.remove_prefix(1);
    if (!number(total) || !line.starts_with(']')) {
        return std::nullopt;
    }
    return std::pair{done, total};
}

std::optional<u64> EstimateRemaining(u32 done, u32 total, u64 elapsed_seconds) {
    if (done == 0 || done > total || elapsed_seconds < 15 || u64{done} * 50 < total) {
        return std::nullopt;
    }
    return elapsed_seconds * (total - done) / done;
}

fs::path InstalledExe(const fs::path& install_dir) {
#ifdef _WIN32
    return install_dir / "eglantine.exe";
#else
    return install_dir / "eglantine";
#endif
}

bool Promote(const Layout& layout, std::string_view sha, State& state, std::error_code& ec) {
    const auto bin = layout.build / "bin";
    if (!fs::is_regular_file(InstalledExe(bin), ec)) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }
    const auto destination = layout.InstallDir(sha);
    const auto staging = fs::path{destination}.concat(".tmp");
    if (!state.current_install.empty() && SamePath(PathFromUtf8(state.current_install), destination)) {
        ec = std::make_error_code(std::errc::device_or_resource_busy);
        return false;
    }

    std::error_code ignored;
    fs::remove_all(staging, ec);
    if (!ec) {
        fs::remove_all(destination, ec);
    }
    if (!ec) {
        fs::create_directories(layout.installs, ec);
    }
    if (!ec) {
        fs::copy(bin, staging, fs::copy_options::recursive, ec);
    }
    if (!ec) {
        fs::rename(staging, destination, ec);
    }
    if (ec) {
        fs::remove_all(staging, ignored);
        return false;
    }

    state.previous_install = state.current_install;
    state.current_install = PathToUtf8(destination);
    state.built_sha = std::string{sha};
    state.built_at = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return true;
}

void PruneInstalls(const Layout& layout, const State& state) {
    std::error_code ec;
    std::vector<fs::path> doomed;
    for (const auto& entry : fs::directory_iterator{layout.installs, ec}) {
        std::error_code type_ec;
        if (!entry.is_directory(type_ec)) {
            continue;
        }
        const bool keep = (!state.current_install.empty() &&
                           SamePath(entry.path(), PathFromUtf8(state.current_install))) ||
                          (!state.previous_install.empty() &&
                           SamePath(entry.path(), PathFromUtf8(state.previous_install)));
        if (!keep) {
            doomed.push_back(entry.path());
        }
    }
    for (const auto& path : doomed) {
        fs::remove_all(path, ec);
    }
}

bool HasInstall(const State& state) {
    std::error_code ec;
    return !state.current_install.empty() &&
           fs::is_regular_file(InstalledExe(PathFromUtf8(state.current_install)), ec);
}

std::string ShortSha(std::string_view sha) {
    return std::string{sha.substr(0, 10)};
}

LaunchAction DecideLaunch(const State& state, bool self_is_tester, bool release_flag,
                          bool install_exists, const std::optional<RemoteInfo>& remote) {
    if (!state.source || self_is_tester || release_flag) {
        return LaunchAction::RunRelease;
    }
    if (!install_exists) {
        return LaunchAction::OfferFirstBuild;
    }
    if (remote) {
        if (state.source->kind == Source::Kind::PullRequest &&
            (!remote->pr_open || remote->pr_merged)) {
            return LaunchAction::OfferClosedPullRequest;
        }
        if (!remote->sha.empty() && remote->sha != state.built_sha) {
            return LaunchAction::OfferUpdate;
        }
    }
    return LaunchAction::StartTester;
}

u64 DiskUsage(const fs::path& dir) {
    std::error_code ec;
    u64 total = 0;
    for (fs::recursive_directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            total += it->file_size(ec);
        }
    }
    return ec ? 0 : total;
}

bool RemoveAll(const Layout& layout, std::error_code& ec) {
    ec.clear();
    if (layout.root.empty()) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    std::error_code first_error;
    for (const auto* dir : {&layout.src, &layout.build, &layout.installs, &layout.tools,
                            &layout.logs}) {
        std::error_code current;
        fs::remove_all(*dir, current);
        if (current && !first_error) {
            first_error = current;
        }
    }
    ec = first_error;
    return !ec;
}

} // namespace TesterBuild

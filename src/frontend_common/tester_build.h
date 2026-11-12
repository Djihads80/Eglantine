// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "common/common_types.h"

// Tester builds: the release build clones a branch or pull request of the official repository,
// builds it and runs it beside the release install. This library parses, plans and does the file
// bookkeeping. It never starts a process and never uses the network: whatever needs a process is
// returned as a Step for the caller to run, and whatever needs its output has a Parse function.
namespace TesterBuild {

struct Source {
    enum class Kind { Branch, PullRequest };

    Kind kind{Kind::Branch};
    std::string branch;
    u64 pr{};

    // The form stored in tester.json: "branch:name" or "pr:838".
    std::string ToString() const;
    // "name" or "PR #838".
    std::string DisplayName() const;
    // The branch name, or "pr-838". It becomes the build's branch label.
    std::string RefSpec() const;
    // "refs/heads/name" or "refs/pull/838/head".
    std::string FetchRef() const;

    bool operator==(const Source&) const = default;
};

// Accepts "branch:name", "pr:838", "#838", a bare number, a pull request URL of `repo`
// ("owner/name", compared case-insensitively) and otherwise a branch name. Branch names end up as
// git arguments, so they are validated strictly.
std::optional<Source> ParseSource(std::string_view input, std::string_view repo);

struct State {
    std::optional<Source> source;
    std::string built_sha;
    std::string current_install;
    std::string previous_install;
    std::string release_exe;
    s64 built_at{}; // Unix seconds
    bool pr_warning_shown{};
};

// Paths in State are UTF-8 strings.
std::string PathToUtf8(const std::filesystem::path& path);
std::filesystem::path PathFromUtf8(std::string_view text);

std::filesystem::path StatePath(const std::filesystem::path& config_dir);

// A missing or corrupt file gives a default State.
State LoadState(const std::filesystem::path& file);

// Written beside the file and renamed over it, so a crash cannot leave half a file.
bool SaveState(const std::filesystem::path& file, const State& state, std::error_code& ec);

struct Layout {
    std::filesystem::path root, tools, src, build, installs, logs;

    // The first 10 characters of `sha` name the folder.
    std::filesystem::path InstallDir(std::string_view sha) const;
};

// EGLANTINE_TESTER_ROOT, else the per-user data folder; empty when neither can be found.
std::filesystem::path DefaultRoot();

Layout MakeLayout(const std::filesystem::path& root);

// Whether `exe` lies under root/installs. Compared lexically, ignoring case on Windows.
bool IsTesterInstall(const std::filesystem::path& exe, const std::filesystem::path& root);

enum class Platform { Windows, Linux };

Platform CurrentPlatform();

struct ToolDownload {
    std::string name, version, url, sha256, bin;
};

struct WingetPackage {
    std::string provides, id, override_args;
    // Oldest acceptable version of what it provides, "18.0"; empty accepts any.
    std::string min_version;
};

enum class PackageManager { None, Apt, Pacman, Dnf };

struct PackageList {
    bool verified{};
    std::vector<std::string> packages;
};

// `tools` and `configure` are resolved for the platform given to ParseRecipe.
struct Recipe {
    std::vector<ToolDownload> tools;
    std::vector<std::string> configure;
    std::vector<std::string> targets;
    std::vector<WingetPackage> winget;
    std::map<PackageManager, PackageList> linux_packages;
};

// Fails on a missing or newer `version`, and on any entry that is not safe to hand to a package
// manager, downloader or cmake, because a pull request can carry its own recipe.
std::optional<Recipe> ParseRecipe(std::string_view json, Platform platform);

// tools/tester-build/recipe.json as it was when this build was made.
std::string_view EmbeddedRecipe();

// The checkout's recipe if it parses, else the embedded one. Packages are installed with
// administrator rights, so without `trust_packages` (a pull request) they always come from the
// embedded recipe.
Recipe LoadRecipe(const std::filesystem::path& src_dir, Platform platform, bool trust_packages);

struct Step {
    std::string id;
    std::string label;
    std::filesystem::path program;
    std::vector<std::string> args;
    std::filesystem::path cwd;
    bool elevated{};
    // The runner hands the step's stdout to the matching Parse function.
    bool capture{};
};

// The first match in a PATH-style list (';' on Windows, ':' elsewhere); empty when absent.
std::filesystem::path FindProgram(std::string_view name, std::string_view path_env);

std::filesystem::path VsWherePath();
// A non-empty `min_version` limits the answer to installs of at least that version.
Step VsWhereStep(std::string_view min_version = {});
std::filesystem::path ParseVsWhere(std::string_view output);

// Runs vcvarsall through cmd.exe and prints the resulting environment. `args` holds the single
// command line for cmd, quotes included, which has to be passed to CreateProcess unmodified.
Step VcVarsStep(const std::filesystem::path& vs_install);
std::vector<std::pair<std::string, std::string>> ParseEnvironment(std::string_view output);

PackageManager ParseOsRelease(std::string_view text);

// These exit non-zero when something is missing, which is normal.
Step InstalledQueryStep(PackageManager manager, const std::vector<std::string>& packages);
std::vector<std::string> ParseMissing(PackageManager manager, std::string_view output,
                                      const std::vector<std::string>& packages);

// `program` is the package manager itself; the runner adds pkexec or sudo for `elevated`.
Step InstallPackagesStep(PackageManager manager, const std::vector<std::string>& missing);

// The step as one copy-pasteable line starting with "sudo ".
std::string InstallCommandText(const Step& step);

std::vector<Step> WingetSteps(const Recipe& recipe,
                              const std::vector<std::string>& missing_provides);

struct Toolchain {
    std::filesystem::path git, cmake, ninja, glslang;
    bool compiler{};
};

// The subset of "msvc", "git" and "glslang" that is absent.
std::vector<std::string> MissingProvides(const Toolchain& tools);

struct BuildRequest {
    Layout layout;
    Source source;
    std::string clone_url;
    std::string repo;
    Recipe recipe;
    Toolchain tools;
};

// Clone when needed, fetch the source, check it out and read back the commit (capture).
std::vector<Step> FetchSteps(const BuildRequest& request);
std::optional<std::string> ParseRevParse(std::string_view output);

// A detached checkout has no branch name, so this writes the files GetSCMRev.cmake reads
// (GIT-COMMIT, GIT-REFSPEC, GIT-TAG) into the checkout, and removes GIT-RELEASE.
bool WriteStamp(const Layout& layout, const Source& source, std::string_view sha,
                std::error_code& ec);

std::vector<Step> BuildSteps(const BuildRequest& request);

// "[12/345] ..." gives {12, 345}.
std::optional<std::pair<u32, u32>> ParseNinjaProgress(std::string_view line);

std::filesystem::path InstalledExe(const std::filesystem::path& install_dir);

// Copy build/bin to installs/<sha10> and make it the current install, the old one the previous.
// Does not save `state`.
bool Promote(const Layout& layout, std::string_view sha, State& state, std::error_code& ec);

// Delete every folder under installs that is neither current nor previous. Errors are ignored: a
// running exe cannot be deleted on Windows and goes next time.
void PruneInstalls(const Layout& layout, const State& state);

// Whether state.current_install holds a runnable exe.
bool HasInstall(const State& state);

// The first 10 characters, which name an install folder.
std::string ShortSha(std::string_view sha);

// What a check of the followed branch or pull request found.
struct RemoteInfo {
    std::string sha;
    // Pull requests only.
    std::string title;
    std::string author;
    bool pr_open{true};
    bool pr_merged{};
};

enum class LaunchAction {
    RunRelease,
    StartTester,
    OfferFirstBuild,
    OfferUpdate,
    OfferClosedPullRequest,
};

// What the release exe does at startup. `remote` is empty when the check failed, timed out or was
// skipped, and an unknown remote never blocks starting the installed tester build.
LaunchAction DecideLaunch(const State& state, bool self_is_tester, bool release_flag,
                          bool install_exists, const std::optional<RemoteInfo>& remote);

// Recursive size in bytes, 0 on error.
u64 DiskUsage(const std::filesystem::path& dir);

// Delete everything under the root (not the root itself).
bool RemoveAll(const Layout& layout, std::error_code& ec);

} // namespace TesterBuild

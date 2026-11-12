// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "frontend_common/tester_build.h"

namespace fs = std::filesystem;
using namespace TesterBuild;

namespace {

struct TempDir {
    TempDir() {
        path = fs::temp_directory_path() /
               ("eglantine-tester-build-" + std::to_string(std::rand()) + "-" +
                std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
    static inline int counter = 0;
};

void Write(const fs::path& file, const std::string& content) {
    fs::create_directories(file.parent_path());
    std::ofstream{file, std::ios::binary} << content;
}

std::string Read(const fs::path& file) {
    std::ifstream stream{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

using Strings = std::vector<std::string>;

constexpr const char* Repo = "aidankinzett/Eglantine";
const std::string Sha1 = "0123456789abcdef0123456789abcdef01234567";
const std::string Sha2 = "fedcba9876543210fedcba9876543210fedcba98";
const std::string Sha3 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

const std::string Hash(64, 'a');

std::string RecipeJson(const std::string& version = "1", const std::string& extra = "") {
    return R"({"version": )" + version + R"(,
  "tools": {
    "cmake": {"version": "3.30.5",
      "windows-x86_64": {"url": "https://example.com/cmake.zip", "sha256": ")" + Hash + R"(", "bin": "cmake/bin"},
      "linux-x86_64": {"url": "https://example.com/cmake.tgz", "sha256": ")" + Hash + R"(", "bin": "cmake-l/bin"}},
    "ninja": {"version": "1.12.1",
      "windows-x86_64": {"url": "https://example.com/ninja.zip", "sha256": ")" + Hash + R"(", "bin": ""}}
  },
  "configure": {"common": ["-DCMAKE_BUILD_TYPE=Release"], "windows": ["-DWIN=1"], "linux": ["-DLIN=1"]},
  "targets": ["yuzu", "eden-cli"],
  "windows": {"winget": [{"provides": "msvc", "id": "Microsoft.VisualStudio.2022.BuildTools", "override": "--passive --add X"},
                         {"provides": "git", "id": "Git.Git"}]},
  "linux": {"apt": {"verified": true, "packages": ["build-essential", "libssl-dev"]},
            "pacman": {"verified": false, "packages": ["base-devel"]}}
  )" + extra + "}";
}

// A recipe with one value replaced, for the validation cases.
std::string Replace(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

BuildRequest MakeRequest(const fs::path& root) {
    BuildRequest request;
    request.layout = MakeLayout(root);
    request.source = {Source::Kind::PullRequest, {}, 838};
    request.clone_url = "https://github.com/aidankinzett/Eglantine.git";
    request.repo = Repo;
    request.recipe.configure = {"-DCMAKE_BUILD_TYPE=Release"};
    request.recipe.targets = {"yuzu"};
    request.tools.git = "git";
    request.tools.cmake = "cmake";
    request.tools.ninja = "ninja";
    return request;
}

} // namespace

TEST_CASE("ParseSource accepts every form", "[tester]") {
    const Source branch{Source::Kind::Branch, "feature/x-1.2_y", 0};
    const Source pr{Source::Kind::PullRequest, {}, 838};

    CHECK(ParseSource("branch:feature/x-1.2_y", Repo) == branch);
    CHECK(ParseSource("  feature/x-1.2_y\n", Repo) == branch);
    CHECK(ParseSource("pr:838", Repo) == pr);
    CHECK(ParseSource("#838", Repo) == pr);
    CHECK(ParseSource(" 838 ", Repo) == pr);
    CHECK(ParseSource("https://github.com/aidankinzett/Eglantine/pull/838", Repo) == pr);
    CHECK(ParseSource("https://github.com/AIDANKINZETT/eglantine/pull/838/files", Repo) == pr);
    CHECK(ParseSource("https://github.com/aidankinzett/Eglantine/pull/838?diff=split", Repo) == pr);
    CHECK(ParseSource("https://github.com/aidankinzett/Eglantine/pull/838#issuecomment-1", Repo) ==
          pr);
    CHECK(ParseSource("a+b@c", Repo).has_value());
    CHECK(ParseSource("1000000000", Repo)->pr == 1'000'000'000);
}

TEST_CASE("ParseSource rejects everything else", "[tester]") {
    for (const char* input :
         {"", "   ", "--upload-pack=x", "-x", "a..b", "a b", "x;rm", "a//b", "/a", ".a", "a/", "a.",
          "a.lock", "a@{b", "a\\b", "a:b", "branch:", "branch:-x", "pr:", "pr:0", "#0", "0", "pr:abc",
          "pr:1000000001", "99999999999", "pr:-5", "#", "$(x)", "a`b`", "a\nb", "@"}) {
        INFO(input);
        CHECK_FALSE(ParseSource(input, Repo).has_value());
    }
    CHECK_FALSE(ParseSource(std::string(201, 'a'), Repo).has_value());
    CHECK(ParseSource(std::string(200, 'a'), Repo).has_value());
    CHECK_FALSE(ParseSource("https://github.com/someone/else/pull/838", Repo).has_value());
    CHECK_FALSE(ParseSource("https://github.com/aidankinzett/Eglantine/issues/838", Repo));
    CHECK_FALSE(ParseSource("https://github.com/aidankinzett/Eglantine/pull/838x", Repo));
    CHECK_FALSE(ParseSource("https://github.com/aidankinzett/Eglantine/pull/", Repo));
    CHECK_FALSE(ParseSource("https://evil.com/aidankinzett/Eglantine/pull/838", Repo));
    CHECK_FALSE(ParseSource("http://github.com/aidankinzett/Eglantine/pull/838", Repo));
}

TEST_CASE("Source string forms", "[tester]") {
    const Source branch{Source::Kind::Branch, "feature/x", 0};
    const Source pr{Source::Kind::PullRequest, {}, 838};
    CHECK(branch.ToString() == "branch:feature/x");
    CHECK(branch.DisplayName() == "feature/x");
    CHECK(branch.RefSpec() == "feature/x");
    CHECK(branch.FetchRef() == "refs/heads/feature/x");
    CHECK(pr.ToString() == "pr:838");
    CHECK(pr.DisplayName() == "PR #838");
    CHECK(pr.RefSpec() == "pr-838");
    CHECK(pr.FetchRef() == "refs/pull/838/head");
    CHECK(ParseSource(pr.ToString(), "") == pr);
    CHECK(ParseSource(branch.ToString(), "") == branch);
}

TEST_CASE("State round trips", "[tester]") {
    TempDir dir;
    const auto file = StatePath(dir.path / "config");
    CHECK(file.filename() == "tester.json");

    State state;
    state.source = Source{Source::Kind::PullRequest, {}, 838};
    state.built_sha = Sha1;
    state.current_install = "C:/x/installs/0123456789";
    state.previous_install = "C:/x/installs/fedcba9876";
    state.release_exe = "C:/Program Files/Eglantine/eglantine.exe";
    state.built_at = 1'700'000'000;
    state.pr_warning_shown = true;

    std::error_code ec;
    REQUIRE(SaveState(file, state, ec));
    CHECK_FALSE(ec);
    CHECK_FALSE(fs::exists(fs::path{file}.concat(".tmp")));

    const auto loaded = LoadState(file);
    CHECK(loaded.source == state.source);
    CHECK(loaded.built_sha == state.built_sha);
    CHECK(loaded.current_install == state.current_install);
    CHECK(loaded.previous_install == state.previous_install);
    CHECK(loaded.release_exe == state.release_exe);
    CHECK(loaded.built_at == state.built_at);
    CHECK(loaded.pr_warning_shown);

    state.source = Source{Source::Kind::Branch, "dev", 0};
    REQUIRE(SaveState(file, state, ec));
    CHECK(LoadState(file).source == state.source);
}

TEST_CASE("State load tolerates a missing or corrupt file", "[tester]") {
    TempDir dir;
    const auto file = dir.path / "tester.json";
    CHECK_FALSE(LoadState(file).source.has_value());

    for (const char* text : {"", "{not json", "[1, 2]", "42", R"({"source": 5, "built_at": "x"})"}) {
        Write(file, text);
        const auto state = LoadState(file);
        CHECK_FALSE(state.source.has_value());
        CHECK(state.built_sha.empty());
        CHECK(state.built_at == 0);
    }

    Write(file, R"({"source": "branch:--bad", "built_sha": "abc"})");
    const auto partial = LoadState(file);
    CHECK_FALSE(partial.source.has_value());
    CHECK(partial.built_sha == "abc");
}

TEST_CASE("Layout folders", "[tester]") {
    const fs::path root = fs::path{"/work"} / "eglantine-tester";
    const auto layout = MakeLayout(root);
    CHECK(layout.root == root);
    CHECK(layout.tools == root / "tools");
    CHECK(layout.src == root / "src");
    CHECK(layout.build == root / "build");
    CHECK(layout.installs == root / "installs");
    CHECK(layout.logs == root / "logs");
    CHECK(layout.InstallDir(Sha1) == root / "installs" / "0123456789");
    CHECK(layout.InstallDir("abc") == root / "installs" / "abc");
}

TEST_CASE("DefaultRoot honours the override", "[tester]") {
    const auto root = (fs::temp_directory_path() / "eglantine-tester-root-test").string();
#ifdef _WIN32
    _putenv_s("EGLANTINE_TESTER_ROOT", root.c_str());
#else
    setenv("EGLANTINE_TESTER_ROOT", root.c_str(), 1);
#endif
    CHECK(DefaultRoot() == fs::path{root});
#ifdef _WIN32
    _putenv_s("EGLANTINE_TESTER_ROOT", "");
#else
    unsetenv("EGLANTINE_TESTER_ROOT");
#endif
    const auto fallback = DefaultRoot();
    CHECK((fallback.empty() || fallback.filename() == "eglantine-tester"));
}

TEST_CASE("IsTesterInstall is true only below installs", "[tester]") {
    const fs::path root = fs::path{"/data"} / "eglantine-tester";
    CHECK(IsTesterInstall(root / "installs" / "0123456789" / "eglantine", root));
    CHECK(IsTesterInstall(root / "installs" / "x" / ".." / "y" / "eglantine", root));
    CHECK_FALSE(IsTesterInstall(root / "installs", root));
    CHECK_FALSE(IsTesterInstall(root / "installs-evil" / "0123456789" / "eglantine", root));
    CHECK_FALSE(IsTesterInstall(root / "build" / "bin" / "eglantine", root));
    CHECK_FALSE(IsTesterInstall(root / "installs" / ".." / "build" / "eglantine", root));
    CHECK_FALSE(IsTesterInstall(fs::path{"/opt"} / "eglantine" / "eglantine", root));
    CHECK_FALSE(IsTesterInstall({}, root));
#ifdef _WIN32
    CHECK(IsTesterInstall(fs::path{"C:\\Data\\Eglantine-Tester\\INSTALLS\\abc\\eglantine.exe"},
                          fs::path{"c:\\data\\eglantine-tester"}));
#endif
}

TEST_CASE("ParseRecipe resolves the platform", "[tester]") {
    const auto windows = ParseRecipe(RecipeJson(), Platform::Windows);
    REQUIRE(windows.has_value());
    REQUIRE(windows->tools.size() == 2);
    CHECK(windows->tools[0].name == "cmake");
    CHECK(windows->tools[0].version == "3.30.5");
    CHECK(windows->tools[0].url == "https://example.com/cmake.zip");
    CHECK(windows->tools[0].sha256 == Hash);
    CHECK(windows->tools[0].bin == "cmake/bin");
    CHECK(windows->tools[1].name == "ninja");
    CHECK(windows->tools[1].bin.empty());
    CHECK(windows->configure == Strings{"-DCMAKE_BUILD_TYPE=Release", "-DWIN=1"});
    CHECK(windows->targets == Strings{"yuzu", "eden-cli"});
    REQUIRE(windows->winget.size() == 2);
    CHECK(windows->winget[0].provides == "msvc");
    CHECK(windows->winget[0].id == "Microsoft.VisualStudio.2022.BuildTools");
    CHECK(windows->winget[0].override_args == "--passive --add X");
    CHECK(windows->winget[1].override_args.empty());

    const auto linux_recipe = ParseRecipe(RecipeJson(), Platform::Linux);
    REQUIRE(linux_recipe.has_value());
    REQUIRE(linux_recipe->tools.size() == 1); // ninja has no Linux entry
    CHECK(linux_recipe->tools[0].url == "https://example.com/cmake.tgz");
    CHECK(linux_recipe->configure == Strings{"-DCMAKE_BUILD_TYPE=Release", "-DLIN=1"});
    REQUIRE(linux_recipe->linux_packages.contains(PackageManager::Apt));
    CHECK(linux_recipe->linux_packages.at(PackageManager::Apt).verified);
    CHECK(linux_recipe->linux_packages.at(PackageManager::Apt).packages ==
          Strings{"build-essential", "libssl-dev"});
    CHECK_FALSE(linux_recipe->linux_packages.at(PackageManager::Pacman).verified);
    CHECK_FALSE(linux_recipe->linux_packages.contains(PackageManager::Dnf));
}

TEST_CASE("ParseRecipe checks the version and ignores unknown keys", "[tester]") {
    CHECK_FALSE(ParseRecipe(RecipeJson("2"), Platform::Linux).has_value());
    CHECK_FALSE(ParseRecipe(RecipeJson("0"), Platform::Linux).has_value());
    CHECK_FALSE(ParseRecipe(RecipeJson("\"1\""), Platform::Linux).has_value());
    CHECK_FALSE(ParseRecipe(R"({"targets": ["yuzu"]})", Platform::Linux).has_value());
    CHECK_FALSE(ParseRecipe("not json", Platform::Linux).has_value());
    CHECK_FALSE(ParseRecipe("[]", Platform::Linux).has_value());
    CHECK(ParseRecipe(R"({"version": 1})", Platform::Linux).has_value());
    CHECK(ParseRecipe(RecipeJson("1", R"(, "future": {"x": [1, 2]})"), Platform::Linux).has_value());
}

TEST_CASE("ParseRecipe rejects unsafe entries", "[tester]") {
    const auto base = RecipeJson();
    // Entries that only one platform uses are checked for that platform.
    const auto rejected = [&](const std::string& from, const std::string& to,
                              std::vector<Platform> platforms = {Platform::Windows,
                                                                 Platform::Linux}) {
        INFO(from << " -> " << to);
        for (const auto platform : platforms) {
            CHECK_FALSE(ParseRecipe(Replace(base, from, to), platform).has_value());
        }
    };
    const std::vector<Platform> win{Platform::Windows};
    const std::vector<Platform> lin{Platform::Linux};
    rejected("\"libssl-dev\"", "\"--evil\"");
    rejected("\"libssl-dev\"", "\"a b\"");
    rejected("\"libssl-dev\"", "\"a;rm\"");
    rejected("\"libssl-dev\"", "\"\"");
    rejected("\"libssl-dev\"", "5");
    rejected("\"Git.Git\"", "\"-e\"");
    rejected("\"Git.Git\"", "\"Git.Git && calc\"");
    rejected("\"override\": \"--passive --add X\"", "\"override\": \"a\\nb\"");
    rejected("\"yuzu\"", "\"yuzu;x\"");
    rejected("\"yuzu\"", "\"\"");
    rejected("\"-DWIN=1\"", "\"-Xfoo\"", win);
    rejected("\"-DLIN=1\"", "\"-DA=\\u0001\"", lin);
    rejected("\"-DLIN=1\"", "\"--version\"", lin);
    rejected("\"https://example.com/cmake.zip\"", "\"http://example.com/cmake.zip\"", win);
    rejected("\"https://example.com/cmake.tgz\"", "\"ftp://example.com/cmake.tgz\"", lin);
    rejected("\"" + Hash + "\", \"bin\": \"cmake/bin\"", "\"abc\", \"bin\": \"cmake/bin\"", win);
    rejected("\"" + Hash + "\", \"bin\": \"cmake-l/bin\"",
             "\"" + std::string(64, 'z') + "\", \"bin\": \"cmake-l/bin\"", lin);
    rejected("\"cmake/bin\"", "\"../bin\"", win);
    rejected("\"cmake-l/bin\"", "\"/usr/bin\"", lin);
    rejected("\"verified\": true", "\"verified\": \"yes\"");
}

TEST_CASE("ParseRecipe reads and validates min_version", "[tester]") {
    const auto base = RecipeJson();
    const std::string msvc = "\"id\": \"Microsoft.VisualStudio.2022.BuildTools\"";
    const auto with = [&](const std::string& value) {
        return Replace(base, msvc, msvc + ", \"min_version\": " + value);
    };
    const auto parsed = ParseRecipe(with("\"18.0\""), Platform::Windows);
    REQUIRE(parsed.has_value());
    CHECK(parsed->winget[0].min_version == "18.0");
    CHECK(parsed->winget[1].min_version.empty());
    CHECK(ParseRecipe(base, Platform::Windows)->winget[0].min_version.empty());
    for (const auto* bad : {"\"\"", "\"18.0 \"", "\"v18\"", "\".5\"", "\"18.0)\"", "18",
                            "\"1111111111.2222222222.3\""}) {
        CAPTURE(bad);
        CHECK_FALSE(ParseRecipe(with(bad), Platform::Windows).has_value());
    }
}

TEST_CASE("ParseRecipe accepts mixed-case hashes and lowercases them", "[tester]") {
    const auto text = Replace(RecipeJson(), "\"" + Hash + "\", \"bin\": \"cmake/bin\"",
                              "\"" + std::string(64, 'A') + "\", \"bin\": \"cmake/bin\"");
    const auto recipe = ParseRecipe(text, Platform::Windows);
    REQUIRE(recipe.has_value());
    CHECK(recipe->tools[0].sha256 == Hash);
}

TEST_CASE("The embedded recipe parses for both platforms", "[tester]") {
    CHECK_FALSE(EmbeddedRecipe().empty());
    for (const auto platform : {Platform::Windows, Platform::Linux}) {
        const auto recipe = ParseRecipe(EmbeddedRecipe(), platform);
        REQUIRE(recipe.has_value());
        CHECK_FALSE(recipe->tools.empty());
        CHECK_FALSE(recipe->targets.empty());
    }
}

TEST_CASE("LoadRecipe prefers the checkout and falls back to the embedded recipe", "[tester]") {
    TempDir dir;
    const auto embedded = ParseRecipe(EmbeddedRecipe(), Platform::Linux);
    REQUIRE(embedded.has_value());

    CHECK(LoadRecipe(dir.path, Platform::Linux, true).targets == embedded->targets);

    Write(dir.path / "tools" / "tester-build" / "recipe.json", R"({"version": 1, "targets": ["only"]})");
    CHECK(LoadRecipe(dir.path, Platform::Linux, true).targets == Strings{"only"});

    Write(dir.path / "tools" / "tester-build" / "recipe.json", R"({"version": 9, "targets": ["only"]})");
    CHECK(LoadRecipe(dir.path, Platform::Linux, true).targets == embedded->targets);

    Write(dir.path / "tools" / "tester-build" / "recipe.json",
          R"({"version": 1, "targets": ["only"], "linux": {"apt": {"packages": ["evil"]}}})");
    CHECK(LoadRecipe(dir.path, Platform::Linux, true).linux_packages[PackageManager::Apt].packages ==
          Strings{"evil"});
    const auto untrusted = LoadRecipe(dir.path, Platform::Linux, false);
    CHECK(untrusted.targets == Strings{"only"});
    CHECK(untrusted.linux_packages.size() == embedded->linux_packages.size());
    for (const auto& [manager, list] : untrusted.linux_packages) {
        CHECK(list.packages == embedded->linux_packages.at(manager).packages);
    }

    Write(dir.path / "tools" / "tester-build" / "recipe.json", "garbage");
    CHECK(LoadRecipe(dir.path, Platform::Linux, true).targets == embedded->targets);
}

TEST_CASE("FindProgram searches a PATH list", "[tester]") {
    TempDir dir;
#ifdef _WIN32
    constexpr char Separator = ';';
    const auto file = dir.path / "git.exe";
#else
    constexpr char Separator = ':';
    const auto file = dir.path / "git";
#endif
    Write(file, "x");
    fs::create_directories(dir.path / "empty");
    const auto list = (dir.path / "empty").string() + Separator + Separator +
                      (dir.path / "missing").string() + Separator + dir.path.string();

    CHECK(FindProgram("git", list) == file);
    CHECK(FindProgram("git", dir.path.string()) == file);
    CHECK(FindProgram("cmake", list).empty());
    CHECK(FindProgram("git", "").empty());
    CHECK(FindProgram("", list).empty());
    CHECK(FindProgram("empty", list).empty()); // a directory is not a program
}

TEST_CASE("vswhere output and the vcvars step", "[tester]") {
    const auto step = VsWhereStep();
    CHECK(step.capture);
    CHECK(step.program == VsWherePath());
    CHECK(VsWherePath().filename() == "vswhere.exe");
    CHECK(step.args == Strings{"-latest", "-products", "*", "-requires",
                               "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                               "installationPath"});
    CHECK(VsWhereStep("").args == step.args);
    CHECK(VsWhereStep("18.0").args ==
          Strings{"-latest", "-products", "*", "-requires",
                  "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-version", "[18.0,)",
                  "-property", "installationPath"});

    CHECK(ParseVsWhere("C:\\VS\\2022\\Community\r\n") == fs::path{"C:\\VS\\2022\\Community"});
    CHECK(ParseVsWhere("\r\n  \r\nC:\\VS\r\nD:\\Other\r\n") == fs::path{"C:\\VS"});
    CHECK(ParseVsWhere("").empty());
    CHECK(ParseVsWhere("\r\n\r\n").empty());

    const auto vcvars = VcVarsStep(fs::path{"C:\\VS"});
    CHECK(vcvars.capture);
    CHECK(vcvars.program == fs::path{"cmd.exe"});
    REQUIRE(vcvars.args.size() == 4);
    CHECK(vcvars.args[0] == "/d");
    CHECK(vcvars.args[1] == "/s");
    CHECK(vcvars.args[2] == "/c");
    CHECK(vcvars.args[3].starts_with("\"\"") );
    CHECK(vcvars.args[3].ends_with("\" x64 >nul && set\""));
    CHECK(vcvars.args[3].find("vcvarsall.bat") != std::string::npos);
}

TEST_CASE("ParseEnvironment reads NAME=VALUE lines", "[tester]") {
    const auto environment = ParseEnvironment(
        "**** Visual Studio ****\r\nPATH=C:\\a;C:\\b\r\n=C:=C:\\hidden\r\nINCLUDE=x=y\r\nEMPTY=\r\n"
        "no equals here\r\n");
    REQUIRE(environment.size() == 3);
    CHECK(environment[0] == std::pair<std::string, std::string>{"PATH", "C:\\a;C:\\b"});
    CHECK(environment[1] == std::pair<std::string, std::string>{"INCLUDE", "x=y"});
    CHECK(environment[2] == std::pair<std::string, std::string>{"EMPTY", ""});
    CHECK(ParseEnvironment("").empty());
}

TEST_CASE("ParseOsRelease picks the package manager", "[tester]") {
    CHECK(ParseOsRelease("NAME=\"Ubuntu\"\nID=ubuntu\nID_LIKE=debian\nVERSION_ID=\"24.04\"\n") ==
          PackageManager::Apt);
    CHECK(ParseOsRelease("NAME=\"Arch Linux\"\nID=arch\n") == PackageManager::Pacman);
    CHECK(ParseOsRelease("NAME=\"Fedora Linux\"\nID=fedora\nVERSION_ID=40\n") ==
          PackageManager::Dnf);
    CHECK(ParseOsRelease("ID=garuda\nID_LIKE=\"arch\"\n") == PackageManager::Pacman);
    CHECK(ParseOsRelease("ID=\"nobara\"\r\nID_LIKE=\"rhel centos fedora\"\r\n") ==
          PackageManager::Dnf);
    CHECK(ParseOsRelease("ID='linuxmint'\n") == PackageManager::Apt);
    CHECK(ParseOsRelease("ID=cachyos\nID_LIKE=arch\n") == PackageManager::Pacman);
    CHECK(ParseOsRelease("ID=opensuse-tumbleweed\nID_LIKE=\"opensuse suse\"\n") ==
          PackageManager::None);
    CHECK(ParseOsRelease("") == PackageManager::None);
}

TEST_CASE("Installed package queries", "[tester]") {
    const Strings packages{"build-essential", "git"};
    const auto apt = InstalledQueryStep(PackageManager::Apt, packages);
    CHECK(apt.capture);
    CHECK(apt.program == fs::path{"dpkg-query"});
    CHECK(apt.args == Strings{"-W", "-f=${Package}\\t${db:Status-Abbrev}\\n", "build-essential", "git"});
    const auto pacman = InstalledQueryStep(PackageManager::Pacman, packages);
    CHECK(pacman.program == fs::path{"pacman"});
    CHECK(pacman.args == Strings{"-T", "build-essential", "git"});
    const auto dnf = InstalledQueryStep(PackageManager::Dnf, packages);
    CHECK(dnf.program == fs::path{"rpm"});
    CHECK(dnf.args == Strings{"-q", "--qf", "%{NAME}\\n", "build-essential", "git"});
    CHECK_FALSE(apt.elevated);
}

TEST_CASE("ParseMissing for each package manager", "[tester]") {
    const Strings packages{"build-essential", "libssl-dev", "git", "cmake"};
    // git is unknown to dpkg and prints nothing on stdout; cmake was removed but not purged.
    CHECK(ParseMissing(PackageManager::Apt,
                       "build-essential\tii \nlibssl-dev:amd64\tii \ncmake\trc \n", packages) ==
          Strings{"git", "cmake"});
    CHECK(ParseMissing(PackageManager::Apt, "", packages) == packages);
    CHECK(ParseMissing(PackageManager::Apt,
                       "build-essential\tii \r\nlibssl-dev\tii \r\ngit\tii \r\ncmake\tii \r\n",
                       packages)
              .empty());

    CHECK(ParseMissing(PackageManager::Pacman, "git\ncmake\n", packages) ==
          Strings{"git", "cmake"});
    CHECK(ParseMissing(PackageManager::Pacman, "", packages).empty());

    CHECK(ParseMissing(PackageManager::Dnf,
                       "git\npackage libssl-dev is not installed\ncmake\n"
                       "package build-essential is not installed\n",
                       packages) == Strings{"build-essential", "libssl-dev"});
    CHECK(ParseMissing(PackageManager::Dnf, "", packages) == packages);
}

TEST_CASE("InstallPackagesStep and InstallCommandText", "[tester]") {
    const Strings missing{"git", "libssl-dev"};
    const auto apt = InstallPackagesStep(PackageManager::Apt, missing);
    CHECK(apt.elevated);
    CHECK_FALSE(apt.capture);
    CHECK(apt.id == "install-packages");
    CHECK(apt.program == fs::path{"apt-get"});
    CHECK(apt.args == Strings{"install", "-y", "--no-install-recommends", "git", "libssl-dev"});
    CHECK(InstallCommandText(apt) == "sudo apt-get install -y --no-install-recommends git libssl-dev");

    const auto pacman = InstallPackagesStep(PackageManager::Pacman, missing);
    CHECK(pacman.args == Strings{"-S", "--needed", "--noconfirm", "git", "libssl-dev"});
    CHECK(InstallCommandText(pacman) == "sudo pacman -S --needed --noconfirm git libssl-dev");

    const auto dnf = InstallPackagesStep(PackageManager::Dnf, missing);
    CHECK(dnf.args == Strings{"install", "-y", "git", "libssl-dev"});
    CHECK(InstallCommandText(dnf) == "sudo dnf install -y git libssl-dev");

    Step odd;
    odd.program = "tool";
    odd.args = {"a b", "it's"};
    CHECK(InstallCommandText(odd) == "sudo tool 'a b' 'it'\\''s'");
}

TEST_CASE("WingetSteps installs only what is missing, in recipe order", "[tester]") {
    const auto recipe = ParseRecipe(RecipeJson(), Platform::Windows);
    REQUIRE(recipe.has_value());

    const auto both = WingetSteps(*recipe, {"git", "msvc", "glslang"});
    REQUIRE(both.size() == 2);
    CHECK(both[0].program == fs::path{"winget"});
    CHECK(both[0].args ==
          Strings{"install", "--id", "Microsoft.VisualStudio.2022.BuildTools", "-e",
                  "--accept-source-agreements", "--accept-package-agreements",
                  "--disable-interactivity", "--override", "--passive --add X"});
    CHECK(both[1].args == Strings{"install", "--id", "Git.Git", "-e", "--accept-source-agreements",
                                  "--accept-package-agreements", "--disable-interactivity"});
    CHECK_FALSE(both[0].elevated);
    CHECK(both[0].id != both[1].id);

    const auto git = WingetSteps(*recipe, {"git"});
    REQUIRE(git.size() == 1);
    CHECK(git[0].args[2] == "Git.Git");
    CHECK(WingetSteps(*recipe, {}).empty());
}

TEST_CASE("MissingProvides", "[tester]") {
    Toolchain tools;
    CHECK(MissingProvides(tools) == Strings{"msvc", "git", "glslang"});
    tools.compiler = true;
    tools.git = "git";
    CHECK(MissingProvides(tools) == Strings{"glslang"});
    tools.glslang = "glslang";
    CHECK(MissingProvides(tools).empty());
}

TEST_CASE("FetchSteps clones only without an existing clone", "[tester]") {
    TempDir dir;
    auto request = MakeRequest(dir.path);
    const auto src = PathToUtf8(request.layout.src);

    auto steps = FetchSteps(request);
    REQUIRE(steps.size() == 4);
    CHECK(steps[0].id == "clone");
    CHECK(steps[0].program == fs::path{"git"});
    CHECK(steps[0].args == Strings{"clone", "--filter=blob:none", "--no-checkout",
                                   "https://github.com/aidankinzett/Eglantine.git", src});
    CHECK(steps[1].id == "fetch");
    CHECK(steps[1].args == Strings{"-C", src, "fetch", "--filter=blob:none", "--force", "origin",
                                   "+refs/pull/838/head:refs/tester/head"});
    CHECK(steps[2].id == "checkout");
    CHECK(steps[2].args == Strings{"-C", src, "checkout", "--force", "--detach", "refs/tester/head"});
    CHECK(steps[3].id == "rev-parse");
    CHECK(steps[3].args == Strings{"-C", src, "rev-parse", "HEAD"});
    CHECK(steps[3].capture);
    CHECK_FALSE(steps[0].capture);
    CHECK_FALSE(steps[1].capture);

    fs::create_directories(request.layout.src / ".git");
    request.source = {Source::Kind::Branch, "dev/x", 0};
    steps = FetchSteps(request);
    REQUIRE(steps.size() == 3);
    CHECK(steps[0].id == "fetch");
    CHECK(steps[0].args.back() == "+refs/heads/dev/x:refs/tester/head");
}

TEST_CASE("ParseRevParse", "[tester]") {
    CHECK(ParseRevParse(Sha1 + "\n") == Sha1);
    CHECK(ParseRevParse("  " + Sha1 + "\r\n") == Sha1);
    CHECK(ParseRevParse("0123456789ABCDEF0123456789ABCDEF01234567\n") == Sha1);
    CHECK_FALSE(ParseRevParse("").has_value());
    CHECK_FALSE(ParseRevParse("abc123").has_value());
    CHECK_FALSE(ParseRevParse(Sha1 + "0").has_value());
    CHECK_FALSE(ParseRevParse(std::string(40, 'g')).has_value());
    CHECK_FALSE(ParseRevParse("fatal: not a git repository").has_value());
}

TEST_CASE("WriteStamp writes the files GetSCMRev reads", "[tester]") {
    TempDir dir;
    const auto layout = MakeLayout(dir.path);
    fs::create_directories(layout.src);
    Write(layout.src / "GIT-RELEASE", "v1.0.0\n");

    std::error_code ec;
    const Source pr{Source::Kind::PullRequest, {}, 838};
    REQUIRE(WriteStamp(layout, pr, Sha1, ec));
    CHECK_FALSE(ec);
    CHECK(Read(layout.src / "GIT-COMMIT") == Sha1 + "\n");
    CHECK(Read(layout.src / "GIT-REFSPEC") == "pr-838\n");
    CHECK(Read(layout.src / "GIT-TAG") == "pr-838\n");
    CHECK_FALSE(fs::exists(layout.src / "GIT-RELEASE"));

    const Source branch{Source::Kind::Branch, "feature/x", 0};
    REQUIRE(WriteStamp(layout, branch, Sha2, ec));
    CHECK(Read(layout.src / "GIT-COMMIT") == Sha2 + "\n");
    CHECK(Read(layout.src / "GIT-REFSPEC") == "feature/x\n");

    CHECK_FALSE(WriteStamp(layout, branch, "nonsense", ec));
    CHECK(ec);
    ec.clear();
    CHECK_FALSE(WriteStamp(MakeLayout(dir.path / "missing"), branch, Sha1, ec));
    CHECK(ec);
}

TEST_CASE("BuildSteps configure and build", "[tester]") {
    TempDir dir;
    auto request = MakeRequest(dir.path);
    request.tools.ninja = dir.path / "tools" / "ninja";
    request.recipe.configure = {"-DCMAKE_BUILD_TYPE=Release", "-GMake", "-DEGLANTINE_UPDATE_REPO=evil/repo",
                                "-DEGLANTINE_UPDATE_REPO:STRING=evil/repo", "-DCMAKE_MAKE_PROGRAM=evil",
                                "-DFOO=1"};
    request.recipe.targets = {"yuzu", "eden-cli"};
    const auto layout = request.layout;

    const auto steps = BuildSteps(request);
    REQUIRE(steps.size() == 2);
    CHECK(steps[0].id == "configure");
    CHECK(steps[0].program == fs::path{"cmake"});
    CHECK(steps[0].args ==
          Strings{"-S", PathToUtf8(layout.src), "-B", PathToUtf8(layout.build), "-G", "Ninja",
                  "-DCMAKE_MAKE_PROGRAM=" + PathToUtf8(request.tools.ninja),
                  "-DCMAKE_BUILD_TYPE=Release", "-DFOO=1",
                  "-DEGLANTINE_UPDATE_REPO=aidankinzett/Eglantine"});
    CHECK(steps[1].id == "build");
    CHECK(steps[1].program == fs::path{"cmake"});
    CHECK(steps[1].args == Strings{"--build", PathToUtf8(layout.build), "--target", "yuzu", "eden-cli"});

    request.recipe.targets.clear();
    CHECK(BuildSteps(request)[1].args == Strings{"--build", PathToUtf8(layout.build)});
}

TEST_CASE("ParseNinjaProgress", "[tester]") {
    using Pair = std::pair<u32, u32>;
    CHECK(ParseNinjaProgress("[12/345] Building CXX object foo.o") == Pair{12, 345});
    CHECK(ParseNinjaProgress("[1/1]") == Pair{1, 1});
    CHECK(ParseNinjaProgress("[0/10] Linking") == Pair{0, 10});
    CHECK_FALSE(ParseNinjaProgress("").has_value());
    CHECK_FALSE(ParseNinjaProgress("12/345").has_value());
    CHECK_FALSE(ParseNinjaProgress("[12/] x").has_value());
    CHECK_FALSE(ParseNinjaProgress("[/12] x").has_value());
    CHECK_FALSE(ParseNinjaProgress("[a/b] x").has_value());
    CHECK_FALSE(ParseNinjaProgress("[12/345 x").has_value());
    CHECK_FALSE(ParseNinjaProgress("[99999999999/1] x").has_value());
    CHECK_FALSE(ParseNinjaProgress("FAILED: foo.o").has_value());
}

namespace {

void MakeBuildOutput(const Layout& layout, const std::string& marker) {
    Write(InstalledExe(layout.build / "bin"), marker);
    Write(layout.build / "bin" / "sub" / "data.txt", marker);
}

} // namespace

TEST_CASE("Promote copies build/bin and tracks current and previous", "[tester]") {
    TempDir dir;
    const auto layout = MakeLayout(dir.path);
    State state;
    std::error_code ec;

    MakeBuildOutput(layout, "one");
    REQUIRE(Promote(layout, Sha1, state, ec));
    CHECK_FALSE(ec);
    const auto first = layout.installs / "0123456789";
    CHECK(Read(InstalledExe(first)) == "one");
    CHECK(Read(first / "sub" / "data.txt") == "one");
    CHECK_FALSE(fs::exists(layout.installs / "0123456789.tmp"));
    CHECK(PathFromUtf8(state.current_install) == first);
    CHECK(state.previous_install.empty());
    CHECK(state.built_sha == Sha1);
    CHECK(state.built_at > 1'600'000'000);

    MakeBuildOutput(layout, "two");
    REQUIRE(Promote(layout, Sha2, state, ec));
    const auto second = layout.installs / "fedcba9876";
    CHECK(Read(InstalledExe(second)) == "two");
    CHECK(PathFromUtf8(state.current_install) == second);
    CHECK(PathFromUtf8(state.previous_install) == first);
    CHECK(state.built_sha == Sha2);
    CHECK(fs::exists(first));

    MakeBuildOutput(layout, "three");
    REQUIRE(Promote(layout, Sha3, state, ec));
    const auto third = layout.installs / "aaaaaaaaaa";
    CHECK(PathFromUtf8(state.current_install) == third);
    CHECK(PathFromUtf8(state.previous_install) == second);

    PruneInstalls(layout, state);
    CHECK_FALSE(fs::exists(first));
    CHECK(fs::exists(second));
    CHECK(fs::exists(third));
}

TEST_CASE("Promote refuses without an exe", "[tester]") {
    TempDir dir;
    const auto layout = MakeLayout(dir.path);
    Write(layout.build / "bin" / "other.txt", "x");
    State state;
    std::error_code ec;
    CHECK_FALSE(Promote(layout, Sha1, state, ec));
    CHECK(ec);
    CHECK(state.current_install.empty());
    CHECK_FALSE(fs::exists(layout.InstallDir(Sha1)));
}

TEST_CASE("Promote replaces a stale install but not the current one", "[tester]") {
    TempDir dir;
    const auto layout = MakeLayout(dir.path);
    State state;
    std::error_code ec;

    Write(layout.InstallDir(Sha1) / "stale.txt", "stale");
    Write(fs::path{layout.InstallDir(Sha1)}.concat(".tmp") / "leftover.txt", "x");
    MakeBuildOutput(layout, "fresh");
    REQUIRE(Promote(layout, Sha1, state, ec));
    CHECK_FALSE(fs::exists(layout.InstallDir(Sha1) / "stale.txt"));
    CHECK(Read(InstalledExe(layout.InstallDir(Sha1))) == "fresh");

    const auto before = state;
    MakeBuildOutput(layout, "again");
    CHECK_FALSE(Promote(layout, Sha1, state, ec));
    CHECK(ec == std::errc::device_or_resource_busy);
    CHECK(Read(InstalledExe(layout.InstallDir(Sha1))) == "fresh");
    CHECK(state.current_install == before.current_install);
    CHECK(state.previous_install == before.previous_install);
}

TEST_CASE("PruneInstalls keeps current and previous only", "[tester]") {
    TempDir dir;
    const auto layout = MakeLayout(dir.path);
    for (const char* name : {"aaaaaaaaaa", "bbbbbbbbbb", "cccccccccc", "dddddddddd.tmp"}) {
        Write(layout.installs / name / "x", "x");
    }
    Write(layout.installs / "note.txt", "x");

    State state;
    state.current_install = PathToUtf8(layout.installs / "aaaaaaaaaa");
    state.previous_install = PathToUtf8(layout.installs / "bbbbbbbbbb");
    PruneInstalls(layout, state);
    CHECK(fs::exists(layout.installs / "aaaaaaaaaa"));
    CHECK(fs::exists(layout.installs / "bbbbbbbbbb"));
    CHECK_FALSE(fs::exists(layout.installs / "cccccccccc"));
    CHECK_FALSE(fs::exists(layout.installs / "dddddddddd.tmp"));

    PruneInstalls(MakeLayout(dir.path / "nowhere"), state); // no folder is not an error
    PruneInstalls(layout, State{});
    CHECK_FALSE(fs::exists(layout.installs / "aaaaaaaaaa"));
}

TEST_CASE("InstalledExe", "[tester]") {
    const auto exe = InstalledExe(fs::path{"/x"} / "installs" / "abc");
#ifdef _WIN32
    CHECK(exe.filename() == "eglantine.exe");
#else
    CHECK(exe.filename() == "eglantine");
#endif
    CHECK(exe.parent_path() == fs::path{"/x"} / "installs" / "abc");
}

TEST_CASE("DiskUsage and RemoveAll", "[tester]") {
    TempDir dir;
    const auto layout = MakeLayout(dir.path / "root");
    Write(layout.src / "a.txt", std::string(100, 'a'));
    Write(layout.build / "sub" / "b.txt", std::string(50, 'b'));
    Write(layout.installs / "x" / "c.txt", "c");
    Write(layout.tools / "d.txt", "d");
    Write(layout.logs / "e.txt", "e");
    Write(layout.root / "keep.txt", "k");

    CHECK(DiskUsage(layout.src) == 100);
    CHECK(DiskUsage(layout.root) == 154);
    CHECK(DiskUsage(dir.path / "missing") == 0);

    std::error_code ec;
    REQUIRE(RemoveAll(layout, ec));
    CHECK_FALSE(ec);
    for (const auto* folder : {&layout.src, &layout.build, &layout.installs, &layout.tools, &layout.logs}) {
        CHECK_FALSE(fs::exists(*folder));
    }
    CHECK(fs::exists(layout.root / "keep.txt"));
    CHECK(RemoveAll(layout, ec)); // already gone

    CHECK_FALSE(RemoveAll(Layout{}, ec));
    CHECK(ec);
}

TEST_CASE("DecideLaunch follows the policy", "[tester]") {
    State state;
    state.source = Source{Source::Kind::Branch, "dev", 0};
    state.built_sha = Sha1;
    const auto decide = [&](bool self_is_tester, bool release_flag, bool install_exists,
                            const std::optional<RemoteInfo>& remote) {
        return DecideLaunch(state, self_is_tester, release_flag, install_exists, remote);
    };
    const RemoteInfo same{Sha1};
    const RemoteInfo newer{Sha2};

    CHECK(decide(false, false, true, same) == LaunchAction::StartTester);
    CHECK(decide(false, false, true, std::nullopt) == LaunchAction::StartTester);
    CHECK(decide(false, false, true, newer) == LaunchAction::OfferUpdate);
    CHECK(decide(false, false, false, same) == LaunchAction::OfferFirstBuild);
    CHECK(decide(false, false, false, std::nullopt) == LaunchAction::OfferFirstBuild);

    // The release, a tester install itself and --release never hand off.
    CHECK(decide(true, false, true, newer) == LaunchAction::RunRelease);
    CHECK(decide(false, true, true, newer) == LaunchAction::RunRelease);
    CHECK(decide(false, true, false, std::nullopt) == LaunchAction::RunRelease);
    state.source.reset();
    CHECK(decide(false, false, true, newer) == LaunchAction::RunRelease);
    CHECK(decide(false, false, false, std::nullopt) == LaunchAction::RunRelease);
}

TEST_CASE("DecideLaunch on a closed pull request", "[tester]") {
    State state;
    state.source = Source{Source::Kind::PullRequest, {}, 838};
    state.built_sha = Sha1;

    RemoteInfo remote{Sha1};
    CHECK(DecideLaunch(state, false, false, true, remote) == LaunchAction::StartTester);
    remote.pr_open = false;
    CHECK(DecideLaunch(state, false, false, true, remote) == LaunchAction::OfferClosedPullRequest);
    remote.pr_open = true;
    remote.pr_merged = true;
    CHECK(DecideLaunch(state, false, false, true, remote) == LaunchAction::OfferClosedPullRequest);
    remote.pr_merged = false;
    remote.sha = Sha2;
    CHECK(DecideLaunch(state, false, false, true, remote) == LaunchAction::OfferUpdate);
    // A branch's open/merged flags mean nothing.
    state.source = Source{Source::Kind::Branch, "dev", 0};
    remote.pr_open = false;
    remote.sha = Sha1;
    CHECK(DecideLaunch(state, false, false, true, remote) == LaunchAction::StartTester);
}

TEST_CASE("HasInstall needs the exe on disk", "[tester]") {
    TempDir dir;
    State state;
    CHECK_FALSE(HasInstall(state));
    state.current_install = PathToUtf8(dir.path / "installs" / "0123456789");
    CHECK_FALSE(HasInstall(state));
    Write(InstalledExe(dir.path / "installs" / "0123456789"), "x");
    CHECK(HasInstall(state));
    CHECK(ShortSha(Sha1) == "0123456789");
    CHECK(ShortSha("abc") == "abc");
}

// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "frontend_common/update_installer.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    TempDir() {
        path = fs::temp_directory_path() /
               ("eglantine-update-installer-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
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
    std::ofstream{file, std::ios::binary} << content;
}

std::string Read(const fs::path& file) {
    std::ifstream stream{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("Staged and previous files sit beside the target", "[updater]") {
    const fs::path target = "/opt/eglantine/eglantine-linux-x86_64.AppImage";
    CHECK(UpdateInstaller::StagedPath(target) == "/opt/eglantine/eglantine-linux-x86_64.AppImage.new");
    CHECK(UpdateInstaller::PreviousPath(target) == "/opt/eglantine/eglantine-linux-x86_64.AppImage.prev");
}

TEST_CASE("Swap replaces the target and keeps one previous copy", "[updater]") {
    TempDir dir;
    const auto target = dir.path / "eglantine.AppImage";
    const auto staged = UpdateInstaller::StagedPath(target);
    const auto previous = UpdateInstaller::PreviousPath(target);
    Write(target, "old");
    fs::permissions(target, fs::perms::owner_all | fs::perms::group_read);
    Write(staged, "new");
    Write(previous, "stale backup");

    std::error_code ec;
    REQUIRE(UpdateInstaller::CanReplace(target));
    REQUIRE(UpdateInstaller::Swap(staged, target, ec));
    CHECK_FALSE(ec);
    CHECK(Read(target) == "new");
    CHECK(Read(previous) == "old");
    CHECK_FALSE(fs::exists(staged));
    const auto permissions = fs::status(target).permissions();
    CHECK((permissions & fs::perms::owner_exec) != fs::perms::none);
    CHECK((permissions & fs::perms::group_read) != fs::perms::none);
}

TEST_CASE("Swap leaves the target untouched when the staged file is missing", "[updater]") {
    TempDir dir;
    const auto target = dir.path / "eglantine.AppImage";
    Write(target, "old");

    std::error_code ec;
    CHECK_FALSE(UpdateInstaller::Swap(UpdateInstaller::StagedPath(target), target, ec));
    CHECK(ec);
    CHECK(Read(target) == "old");
    CHECK_FALSE(fs::exists(UpdateInstaller::PreviousPath(target)));
}

TEST_CASE("Rollback exchanges the target with its previous copy", "[updater]") {
    TempDir dir;
    const auto target = dir.path / "eglantine.AppImage";
    const auto previous = UpdateInstaller::PreviousPath(target);
    Write(target, "new");
    Write(previous, "old");

    std::error_code ec;
    REQUIRE(UpdateInstaller::Rollback(target, ec));
    CHECK(Read(target) == "old");
    CHECK(Read(previous) == "new");
    REQUIRE(UpdateInstaller::Rollback(target, ec));
    CHECK(Read(target) == "new");
    CHECK(Read(previous) == "old");
    CHECK_FALSE(fs::exists(UpdateInstaller::StagedPath(target)));
}

TEST_CASE("Rollback without a previous copy fails and changes nothing", "[updater]") {
    TempDir dir;
    const auto target = dir.path / "eglantine.AppImage";
    Write(target, "current");

    std::error_code ec;
    CHECK_FALSE(UpdateInstaller::Rollback(target, ec));
    CHECK(ec);
    CHECK(Read(target) == "current");
}

TEST_CASE("Only an existing file in a writable folder can be replaced", "[updater]") {
    TempDir dir;
    const auto target = dir.path / "eglantine.AppImage";
    CHECK_FALSE(UpdateInstaller::CanReplace(target));
    CHECK_FALSE(UpdateInstaller::CanReplace(dir.path));
    Write(target, "x");
    Write(dir.path / ".eglantine-update-write-test", "kept");
    CHECK(UpdateInstaller::CanReplace(target));
    CHECK(Read(dir.path / ".eglantine-update-write-test") == "kept");
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator{dir.path}) {
        ++count;
    }
    CHECK(count == 2);
}

TEST_CASE("Only one holder at a time gets the lock", "[updater]") {
    TempDir dir;
    const auto path = UpdateInstaller::LockPath(dir.path / "eglantine.AppImage");
    CHECK(path == dir.path / ".eglantine.AppImage.lock");
    {
        const UpdateInstaller::Lock first{path};
        CHECK(first.Held());
        const UpdateInstaller::Lock second{path};
        CHECK_FALSE(second.Held());
    }
    const UpdateInstaller::Lock third{path};
    CHECK(third.Held());
}

namespace {

void Put(const fs::path& root, const std::string& relative, const std::string& content) {
    fs::create_directories((root / relative).parent_path());
    Write(root / relative, content);
}

std::set<std::string> Files(const fs::path& root) {
    std::set<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator{root}) {
        if (entry.is_regular_file()) {
            files.insert(fs::relative(entry.path(), root).generic_string());
        }
    }
    return files;
}

} // namespace

TEST_CASE("Install replaces files, adds new ones and keeps the old ones as a backup", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    const auto staging = install / ".update-staging";
    Put(install, "eglantine.exe", "old exe");
    Put(install, "Qt6Core.dll", "old qt");
    Put(install, "plugins/a.dll", "old plugin");
    Put(install, "user/config/qt-config.ini", "my settings");
    Put(staging, "eglantine.exe", "new exe");
    Put(staging, "Qt6Core.dll", "new qt");
    Put(staging, "plugins/b.dll", "new plugin");
    Put(staging, "user/config/qt-config.ini", "must not be installed");

    std::error_code ec;
    REQUIRE(UpdateInstaller::Install(staging, install, ec));

    CHECK(Read(install / "eglantine.exe") == "new exe");
    CHECK(Read(install / "Qt6Core.dll") == "new qt");
    CHECK(Read(install / "plugins/b.dll") == "new plugin");
    CHECK(Read(install / "plugins/a.dll") == "old plugin");
    CHECK(Read(install / "user/config/qt-config.ini") == "my settings");
    CHECK_FALSE(fs::exists(staging));
    const auto backup = install / ".update-backup";
    CHECK(Read(backup / "eglantine.exe") == "old exe");
    CHECK(Read(backup / "Qt6Core.dll") == "old qt");
    CHECK_FALSE(fs::exists(backup / "user"));
    CHECK(Files(install / "user") == std::set<std::string>{"config/qt-config.ini"});
    CHECK(Read(install / "install-manifest.txt") ==
          "Qt6Core.dll\neglantine.exe\nplugins/b.dll\n");
    CHECK_FALSE(fs::exists(install / ".update-backup.tmp"));
}

TEST_CASE("A later update removes the files the new version dropped", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    Put(install, "eglantine.exe", "v1");
    Put(install, "old.dll", "v1 only");
    Put(install, "keep.dll", "v1");
    Put(install, "user/data.bin", "mine");
    std::error_code ec;

    Put(install, ".update-staging/eglantine.exe", "v2");
    Put(install, ".update-staging/old.dll", "v2");
    Put(install, ".update-staging/keep.dll", "v2");
    REQUIRE(UpdateInstaller::Install(install / ".update-staging", install, ec));

    Put(install, ".update-staging/eglantine.exe", "v3");
    Put(install, ".update-staging/keep.dll", "v3");
    REQUIRE(UpdateInstaller::Install(install / ".update-staging", install, ec));

    CHECK(Files(install) ==
          std::set<std::string>{"eglantine.exe", "keep.dll", "install-manifest.txt",
                                ".update-backup/eglantine.exe", ".update-backup/keep.dll",
                                ".update-backup/old.dll", ".update-backup/install-manifest.txt",
                                "user/data.bin"});
    CHECK(Read(install / "eglantine.exe") == "v3");
    CHECK(Read(install / ".update-backup/old.dll") == "v2");
    CHECK(Read(install / ".update-backup/eglantine.exe") == "v2");
}

TEST_CASE("Restoring the backup brings back the previous version and can be undone", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    Put(install, "eglantine.exe", "v1");
    Put(install, "gone.dll", "v1 only");
    Put(install, "user/data.bin", "mine");
    std::error_code ec;

    Put(install, ".update-staging/eglantine.exe", "v2");
    Put(install, ".update-staging/new.dll", "v2 only");
    REQUIRE(UpdateInstaller::Install(install / ".update-staging", install, ec));
    REQUIRE(fs::exists(install / "new.dll"));

    REQUIRE(UpdateInstaller::Install(install / ".update-backup", install, ec));
    CHECK(Read(install / "eglantine.exe") == "v1");
    CHECK(Read(install / "gone.dll") == "v1 only");
    CHECK_FALSE(fs::exists(install / "new.dll"));
    CHECK(Read(install / "user/data.bin") == "mine");
    CHECK(Read(install / ".update-backup/eglantine.exe") == "v2");
    CHECK(Read(install / ".update-backup/new.dll") == "v2 only");

    REQUIRE(UpdateInstaller::Install(install / ".update-backup", install, ec));
    CHECK(Read(install / "eglantine.exe") == "v2");
    CHECK(Read(install / "new.dll") == "v2 only");
    CHECK_FALSE(fs::exists(install / "gone.dll"));
    CHECK(Read(install / "user/data.bin") == "mine");
}

TEST_CASE("Updates after a restore leave the user's own folders and the lock alone", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    Put(install, "eglantine.exe", "v1");
    Put(install, "games/game.nsp", "mine");
    const UpdateInstaller::Lock lock{UpdateInstaller::LockPath(install / "eglantine.exe")};
    REQUIRE(lock.Held());
    std::error_code ec;

    Put(install, ".update-staging/eglantine.exe", "v2");
    REQUIRE(UpdateInstaller::Install(install / ".update-staging", install, ec));
    REQUIRE(UpdateInstaller::Install(install / ".update-backup", install, ec));
    Put(install, ".update-staging/eglantine.exe", "v3");
    REQUIRE(UpdateInstaller::Install(install / ".update-staging", install, ec));
    Put(install, ".update-staging/eglantine.exe", "v4");
    REQUIRE(UpdateInstaller::Install(install / ".update-staging", install, ec));

    CHECK(Read(install / "eglantine.exe") == "v4");
    CHECK(Read(install / "games/game.nsp") == "mine");
    CHECK(fs::exists(UpdateInstaller::LockPath(install / "eglantine.exe")));
}

#ifndef _WIN32
TEST_CASE("A failed install puts every file back", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    Put(install, "a.dll", "old a");
    Put(install, "locked/b.dll", "old b");
    Put(install, ".update-staging/a.dll", "new a");
    Put(install, ".update-staging/locked/b.dll", "new b");
    if (geteuid() == 0) {
        SKIP("root ignores directory permissions");
    }
    fs::permissions(install / "locked", fs::perms::owner_read | fs::perms::owner_exec);

    std::error_code ec;
    CHECK_FALSE(UpdateInstaller::Install(install / ".update-staging", install, ec));
    CHECK(ec);
    fs::permissions(install / "locked", fs::perms::owner_all);
    CHECK(Read(install / "a.dll") == "old a");
    CHECK(Read(install / "locked/b.dll") == "old b");
    CHECK_FALSE(fs::exists(install / ".update-backup"));
    CHECK_FALSE(fs::exists(install / ".update-backup.tmp"));
    CHECK_FALSE(fs::exists(install / "install-manifest.txt"));
}

TEST_CASE("A failed restore keeps the backup it restores from", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    const auto backup = install / ".update-backup";
    Put(install, "a.dll", "v2 a");
    Put(install, "locked/b.dll", "v2 b");
    Put(backup, "a.dll", "v1 a");
    Put(backup, "locked/b.dll", "v1 b");
    if (geteuid() == 0) {
        SKIP("root ignores directory permissions");
    }
    fs::permissions(install / "locked", fs::perms::owner_read | fs::perms::owner_exec);

    std::error_code ec;
    CHECK_FALSE(UpdateInstaller::Install(backup, install, ec));
    fs::permissions(install / "locked", fs::perms::owner_all);
    CHECK(Read(install / "a.dll") == "v2 a");
    CHECK(Read(install / "locked/b.dll") == "v2 b");
    CHECK(Read(backup / "a.dll") == "v1 a");
    CHECK(Read(backup / "locked/b.dll") == "v1 b");
}
#endif

TEST_CASE("Files a failed install could not put back block the next install", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    Put(install, "a.dll", "old a");
    Put(install, ".update-backup.tmp/b.dll", "only copy");
    Put(install, ".update-staging/a.dll", "new a");

    CHECK(UpdateInstaller::HasPendingBackup(install));
    std::error_code ec;
    CHECK_FALSE(UpdateInstaller::Install(install / ".update-staging", install, ec));
    CHECK(ec == std::errc::file_exists);
    CHECK(Read(install / "a.dll") == "old a");
    CHECK(Read(install / ".update-backup.tmp/b.dll") == "only copy");
    CHECK(Read(install / ".update-staging/a.dll") == "new a");
}

TEST_CASE("An empty leftover pending backup does not block an install", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    Put(install, "a.dll", "old a");
    Put(install, ".update-staging/a.dll", "new a");
    fs::create_directories(install / ".update-backup.tmp");

    CHECK_FALSE(UpdateInstaller::HasPendingBackup(install));
    std::error_code ec;
    REQUIRE(UpdateInstaller::Install(install / ".update-staging", install, ec));
    CHECK(Read(install / "a.dll") == "new a");
    CHECK_FALSE(fs::exists(install / ".update-backup.tmp"));
}

TEST_CASE("The user folder is protected whatever its letter case", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    const auto staging = install / ".update-staging";
    Put(install, "eglantine.exe", "old exe");
    Put(install, "USER/data.bin", "mine");
    Put(staging, "eglantine.exe", "new exe");
    Put(staging, "User/config.ini", "must not be installed");

    std::error_code ec;
    REQUIRE(UpdateInstaller::Install(staging, install, ec));
    CHECK(Read(install / "USER/data.bin") == "mine");
    CHECK_FALSE(fs::exists(install / "User/config.ini"));
    CHECK(Read(install / "install-manifest.txt") == "eglantine.exe\n");
    CHECK(Files(install / ".update-backup") ==
          std::set<std::string>{"eglantine.exe", "install-manifest.txt"});
}

TEST_CASE("Path list entries inside a folder are removed", "[updater]") {
    using UpdateInstaller::StripPathListEntries;
    CHECK(StripPathListEntries("/tmp/.mount_x/usr/lib:/usr/lib:/tmp/.mount_x:/opt/lib",
                               "/tmp/.mount_x") == "/usr/lib:/opt/lib");
    CHECK(StripPathListEntries("/tmp/.mount_xy/lib:/usr/lib", "/tmp/.mount_x") ==
          "/tmp/.mount_xy/lib:/usr/lib");
    CHECK(StripPathListEntries("", "/tmp/.mount_x").empty());
    CHECK(StripPathListEntries("/a::/b", "/tmp/.mount_x") == "/a:/b");
    CHECK(StripPathListEntries("/a:/b", "") == "/a:/b");
}

TEST_CASE("Installing and restoring never change the user folder", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    const std::string saves{"\x00\x01\xff binary save\r\n", 17};
    Put(install, "eglantine.exe", "v1");
    Put(install, "user/config/qt-config.ini", "my settings");
    Put(install, "user/nand/save.bin", saves);
    Put(install, "user/empty/.keep", "");
    const auto before = Files(install / "user");
    const auto stamp = fs::last_write_time(install / "user/nand/save.bin");

    Put(install, ".update-staging/eglantine.exe", "v2");
    Put(install, ".update-staging/user/config/qt-config.ini", "from the release");
    Put(install, ".update-staging/user/new.bin", "from the release");
    std::error_code ec;
    REQUIRE(UpdateInstaller::Install(install / ".update-staging", install, ec));
    REQUIRE(UpdateInstaller::Install(install / ".update-backup", install, ec));
    CHECK(Read(install / "eglantine.exe") == "v1");

    CHECK(Files(install / "user") == before);
    CHECK(Read(install / "user/config/qt-config.ini") == "my settings");
    CHECK(Read(install / "user/nand/save.bin") == saves);
    CHECK((fs::last_write_time(install / "user/nand/save.bin") == stamp));
    CHECK_FALSE(fs::exists(install / ".update-backup/user"));
}

TEST_CASE("A file the new version dropped is deleted and comes back on restore", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    std::error_code ec;
    Put(install, ".update-staging/eglantine.exe", "v1");
    Put(install, ".update-staging/dropped.dll", "v1 only");
    REQUIRE(UpdateInstaller::Install(install / ".update-staging", install, ec));

    Put(install, ".update-staging/eglantine.exe", "v2");
    REQUIRE(UpdateInstaller::Install(install / ".update-staging", install, ec));
    CHECK_FALSE(fs::exists(install / "dropped.dll"));
    CHECK(Read(install / ".update-backup/dropped.dll") == "v1 only");

    REQUIRE(UpdateInstaller::Install(install / ".update-backup", install, ec));
    CHECK(Read(install / "dropped.dll") == "v1 only");
    CHECK(Read(install / "eglantine.exe") == "v1");
}

TEST_CASE("A failed install leaves the install folder exactly as it was", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    Put(install, "a.dll", "old a");
    Put(install, "m.dll", "old m");
    Put(install, "user/data.bin", "mine");
    // A file where the new version needs a folder, so placing sub/x fails after a.dll and m.dll
    // were already swapped.
    Put(install, "sub", "a file in the way");
    Put(install, "install-manifest.txt", "a.dll\nm.dll\ngone.dll\n");
    Put(install, "gone.dll", "obsolete");
    Put(install, ".update-staging/a.dll", "new a");
    Put(install, ".update-staging/m.dll", "new m");
    Put(install, ".update-staging/sub/x", "new x");
    const auto before = Files(install);
    const auto manifest = Read(install / "install-manifest.txt");

    std::error_code ec;
    CHECK_FALSE(UpdateInstaller::Install(install / ".update-staging", install, ec));
    CHECK(ec);
    // Install writes the manifest it is about to place into the staging folder.
    CHECK(fs::remove(install / ".update-staging/install-manifest.txt"));
    CHECK(Files(install) == before);
    CHECK(Read(install / "a.dll") == "old a");
    CHECK(Read(install / "m.dll") == "old m");
    CHECK(Read(install / "gone.dll") == "obsolete");
    CHECK(Read(install / "sub") == "a file in the way");
    CHECK(Read(install / "user/data.bin") == "mine");
    CHECK(Read(install / "install-manifest.txt") == manifest);
    CHECK_FALSE(fs::exists(install / ".update-backup"));
    CHECK_FALSE(fs::exists(install / ".update-backup.tmp"));
    CHECK(Read(install / ".update-staging/a.dll") == "new a");
}

TEST_CASE("An empty staging folder is refused", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    Put(install, "a.dll", "old a");
    Put(install, "install-manifest.txt", "a.dll\n");
    fs::create_directories(install / ".update-staging");

    std::error_code ec;
    CHECK_FALSE(UpdateInstaller::Install(install / ".update-staging", install, ec));
    CHECK(Read(install / "a.dll") == "old a");
    CHECK_FALSE(UpdateInstaller::Install(install / ".update-missing", install, ec));
    CHECK(Read(install / "a.dll") == "old a");
}

TEST_CASE("Manifest lines that are absolute or climb out of the folder are ignored", "[updater]") {
    TempDir dir;
    const auto install = dir.path / "app";
    const auto outside = dir.path / "outside.txt";
    Write(outside, "outside");
    Put(install, "eglantine.exe", "v1");
    Put(install, "dropped.dll", "v1 only");
    Put(install, "user/data.bin", "mine");
    Put(install, "../sibling.txt", "sibling");
    const auto absolute = outside.generic_string();
    Put(install, "install-manifest.txt",
        "eglantine.exe\ndropped.dll\n../outside.txt\nuser/../../outside.txt\n./user/data.bin\n"
        "user/data.bin\n" +
            absolute + "\n/outside.txt\n\\outside.txt\n");
    Put(install, ".update-staging/eglantine.exe", "v2");

    std::error_code ec;
    REQUIRE(UpdateInstaller::Install(install / ".update-staging", install, ec));
    CHECK(Read(outside) == "outside");
    CHECK(Read(dir.path / "sibling.txt") == "sibling");
    CHECK(Read(install / "user/data.bin") == "mine");
    CHECK_FALSE(fs::exists(install / "dropped.dll"));
    CHECK(Read(install / "eglantine.exe") == "v2");
    CHECK(Files(install / ".update-backup") ==
          std::set<std::string>{"eglantine.exe", "dropped.dll", "install-manifest.txt"});
}

TEST_CASE("Two holders of the same lock exclude each other across targets", "[updater]") {
    TempDir dir;
    const auto a = UpdateInstaller::LockPath(dir.path / "one");
    const auto b = UpdateInstaller::LockPath(dir.path / "two");
    const UpdateInstaller::Lock first{a};
    const UpdateInstaller::Lock other{b};
    const UpdateInstaller::Lock again{a};
    CHECK(first.Held());
    CHECK(other.Held());
    CHECK_FALSE(again.Held());
}

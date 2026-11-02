// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <openssl/evp.h>

#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/vfs/vfs_real.h"

namespace {

namespace fs = std::filesystem;
using FileSys::NupContainer;

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    ("nup-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void Put(const fs::path& p, std::string_view value) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << value;
}

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string Read(const FileSys::VirtualFile& f) {
    std::string out(f->GetSize(), '\0');
    f->Read(reinterpret_cast<u8*>(out.data()), out.size(), 0);
    return out;
}

// Enough of an NPDM for the packer: the program ID at ACI0 + 0x10.
std::string Npdm(u64 program_id) {
    std::string npdm(0x100, '\0');
    npdm.replace(0x80, 4, "ACI0");
    for (int i = 0; i < 8; ++i) {
        npdm[0x90 + i] = static_cast<char>(program_id >> (8 * i));
    }
    return npdm;
}

std::array<u8, 32> Sha256(std::string_view data) {
    std::array<u8, 32> out{};
    unsigned int length{};
    EVP_Digest(data.data(), data.size(), out.data(), &length, EVP_sha256(), nullptr);
    return out;
}

u64 Le(const std::string& buf, size_t at, size_t bytes) {
    u64 value = 0;
    for (size_t i = 0; i < bytes; ++i) {
        value |= static_cast<u64>(static_cast<u8>(buf[at + i])) << (8 * i);
    }
    return value;
}

struct RawFile {
    std::string name;
    u64 size;
    std::array<u8, 32> sha256;
    u64 extent_offset;
    u64 extent_size;
    u8 codec;
};

// Reads the file records straight from the TOC, which NupContainer does not expose.
std::vector<RawFile> RawFiles(const std::string& nup) {
    const u64 toc = Le(nup, 0x08, 8);
    const u64 packages = Le(nup, toc + 0x04, 4);
    const u64 dirs = Le(nup, toc + 0x08, 8);
    const u64 files = Le(nup, toc + 0x10, 8);
    const u64 pool = toc + Le(nup, toc + 0x18, 8);
    std::vector<RawFile> out;
    u64 pos = toc + 0x30 + packages * 0x40 + dirs * 0x18;
    for (u64 i = 0; i < files; ++i, pos += 0x50) {
        RawFile file{nup.substr(pool + Le(nup, pos, 4), Le(nup, pos + 4, 4)),
                     Le(nup, pos + 0x08, 8),
                     {},
                     Le(nup, pos + 0x10, 8),
                     Le(nup, pos + 0x18, 8),
                     static_cast<u8>(nup[pos + 0x40])};
        std::copy_n(nup.begin() + static_cast<std::ptrdiff_t>(pos + 0x20), 32,
                    file.sha256.begin());
        out.push_back(file);
    }
    return out;
}

// Every file under `dir` with its contents, keyed by its path relative to `dir`.
std::map<std::string, std::string> SourceTree(const fs::path& dir) {
    std::map<std::string, std::string> out;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().filename() != ".listing") {
            out[fs::relative(entry.path(), dir).generic_string()] = ReadAll(entry.path());
        }
    }
    return out;
}

void PackedTree(const FileSys::VirtualDir& dir, const std::string& prefix,
                std::map<std::string, std::string>& out) {
    if (dir == nullptr) {
        return;
    }
    for (const auto& file : dir->GetFiles()) {
        out[prefix + file->GetName()] = Read(file);
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        PackedTree(sub, prefix + sub->GetName() + "/", out);
    }
}

std::map<std::string, std::string> PackedTree(const FileSys::VirtualDir& dir) {
    std::map<std::string, std::string> out;
    PackedTree(dir, "", out);
    return out;
}

constexpr u64 TitleId = 0x0400ABCD00010000ULL;

void MakeDump(const fs::path& root) {
    Put(root / "Base/ExeFS/main", "base main");
    Put(root / "Base/ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "Base/RomFS/shared.bin", std::string(5000, 's'));
    Put(root / "Base/RomFS/copy/shared.bin", std::string(5000, 's'));
    Put(root / "Base/RomFS/empty", "");
    Put(root / "Base/RomFS/.listing", "listing");
    Put(root / "Base/RomFS/System/Resource/ResourceSizeTable.Product.100.rsizetable", "rst");
    Put(root / "Base/RomFS/big.bin", std::string(64 * 1024, 'b'));
    Put(root / "Base/RomFS/random.bin", [] {
        std::string data(3000, '\0');
        u32 x = 12345;
        for (auto& c : data) {
            x = x * 1664525 + 1013904223;
            c = static_cast<char>(x >> 24);
        }
        return data;
    }());
    Put(root / "updates/65536/exefs/main", "update main");
    Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/65536/romfs/shared.bin", std::string(5000, 's'));
    Put(root / "updates/65536/romfs/System/Resource/ResourceSizeTable.Product.110.rsizetable",
        "rst");
    Put(root / "updates/65536/romfs/Zeta/a", "a");
    Put(root / "updates/65536/romfs/alpha", "alpha");
    Put(root / "updates/131072/exefs/main", "update 2 main");
    Put(root / "updates/131072/exefs/main.npdm", Npdm(TitleId));
    Put(root / "dlc/0400ABCD00011001/0/romfs/addon.bin", "addon data");
}

} // namespace

TEST_CASE("NUP packs base, updates and add-ons and reads back through NupContainer", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options{.raw_threshold = 16 * 1024};
    std::string error;
    REQUIRE(FileSys::Nup::PackDump(root, archive, options, {}, nullptr, &error) ==
            FileSys::Nup::PackResult::Success);
    INFO(error);
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);

    const auto& packages = nup.GetPackages();
    REQUIRE(packages.size() == 4);
    REQUIRE(packages[0].kind == NupContainer::PackageKind::Base);
    REQUIRE(packages[0].title_id == TitleId);
    REQUIRE(packages[0].version == 0);
    REQUIRE(packages[0].npdm.size() == 0x100);
    REQUIRE(packages[1].kind == NupContainer::PackageKind::Update);
    REQUIRE(packages[1].version == 65536);
    REQUIRE(packages[1].title_id == TitleId);
    REQUIRE_FALSE(packages[1].standalone_update);
    REQUIRE(packages[2].kind == NupContainer::PackageKind::Update);
    REQUIRE(packages[2].version == 131072);
    REQUIRE(packages[2].romfs_dir == 0xFFFFFFFF);
    REQUIRE(packages[3].kind == NupContainer::PackageKind::Addon);
    REQUIRE(packages[3].title_id == 0x0400ABCD00011001ULL);
    REQUIRE(packages[3].exefs_dir == 0xFFFFFFFF);
    REQUIRE(nup.FindBestUpdatePackage() == 2);

    REQUIRE(PackedTree(nup.GetExeFSDir(0)) == SourceTree(root / "Base/ExeFS"));
    REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(root / "Base/RomFS"));
    REQUIRE(PackedTree(nup.GetExeFSDir(1)) == SourceTree(root / "updates/65536/exefs"));
    REQUIRE(PackedTree(nup.GetRomFSDir(1)) == SourceTree(root / "updates/65536/romfs"));
    REQUIRE(PackedTree(nup.GetExeFSDir(2)) == SourceTree(root / "updates/131072/exefs"));
    REQUIRE(PackedTree(nup.GetRomFSDir(3)) == SourceTree(root / "dlc/0400ABCD00011001/0/romfs"));
    REQUIRE_FALSE(nup.GetRomFSDir(0)->GetFile(".listing"));

    const auto bytes = ReadAll(archive);
    const auto files = RawFiles(bytes);
    std::map<std::string, std::vector<RawFile>> by_name;
    for (const auto& file : files) {
        by_name[file.name].push_back(file);
        if (file.size != 0) {
            REQUIRE(file.extent_offset % 16 == 0);
        }
    }
    const auto shared = by_name.at("shared.bin");
    REQUIRE(shared.size() == 3);
    for (const auto& file : shared) {
        REQUIRE(file.sha256 == Sha256(std::string(5000, 's')));
        REQUIRE(file.codec == 1);
        REQUIRE(file.extent_offset == shared[0].extent_offset);
        REQUIRE(file.extent_size == shared[0].extent_size);
    }
    const auto& empty = by_name.at("empty").at(0);
    REQUIRE(empty.size == 0);
    REQUIRE(empty.extent_offset == 0);
    REQUIRE(empty.extent_size == 0);
    REQUIRE(empty.codec == 0);
    REQUIRE(empty.sha256 == Sha256(""));
    // Highly compressible, but above the raw threshold.
    const auto& big = by_name.at("big.bin").at(0);
    REQUIRE(big.codec == 0);
    REQUIRE(big.extent_size == 64 * 1024);
    REQUIRE(big.sha256 == Sha256(std::string(64 * 1024, 'b')));
    // Incompressible, so stored raw.
    const auto& random = by_name.at("random.bin").at(0);
    REQUIRE(random.codec == 0);
    REQUIRE(random.extent_size == 3000);
    REQUIRE_FALSE(by_name.contains(".listing"));
}

TEST_CASE("NUP marks updates standalone when the dump has no base", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "updates/65536/exefs/main", "main");
    Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/65536/romfs/file", "rom");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 1);
    REQUIRE(nup.GetPackages()[0].kind == NupContainer::PackageKind::Update);
    REQUIRE(nup.GetPackages()[0].standalone_update);
    REQUIRE(nup.GetPackages()[0].title_id == TitleId);
    REQUIRE(Read(nup.GetRomFSDir(0)->GetFile("file")) == "rom");
}

TEST_CASE("NUP packs a bare exefs/romfs dump as one base", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "ExeFS/main", "main");
    Put(root / "ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "RomFS/file", "rom");
    Put(root / "notes.txt", "extra");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 1);
    REQUIRE(nup.GetPackages()[0].kind == NupContainer::PackageKind::Base);
    REQUIRE_FALSE(nup.GetPackages()[0].standalone_update);
    REQUIRE(Read(nup.GetExeFSDir(0)->GetFile("main")) == "main");
    REQUIRE(Read(nup.GetRomFSDir(0)->GetFile("file")) == "rom");
    const auto files = RawFiles(ReadAll(archive));
    REQUIRE(std::ranges::count(files, std::string{"notes.txt"}, &RawFile::name) == 1);
}

TEST_CASE("NUP low storage resumes and deletes the packed sources", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto copy = t.path / "copy" / "Game";
    fs::create_directories(copy);
    fs::copy(root, copy, fs::copy_options::recursive);
    const auto archive = t.path / "Game.nup";

    std::atomic_bool cancel = false;
    bool cancelled = false;
    FileSys::Nup::PackOptions options{.delete_sources = true, .chunk_bytes = 1};
    auto result = FileSys::Nup::PackDump(
        root, archive, options,
        [&](const auto& p) {
            if (p.files_done >= 4 && !cancelled) {
                cancel = true;
                cancelled = true;
            }
        },
        &cancel);
    REQUIRE(result == FileSys::Nup::PackResult::Cancelled);
    REQUIRE(FileSys::Nup::HasResumableJournal(archive));
    REQUIRE_FALSE(fs::exists(root / "Base/ExeFS/main"));
    REQUIRE_FALSE(fs::exists(root / "Base/ExeFS/main.npdm"));

    cancel = false;
    std::string error;
    REQUIRE(FileSys::Nup::PackDump(root, archive, options, {}, &cancel, &error) ==
            FileSys::Nup::PackResult::Success);
    INFO(error);
    REQUIRE_FALSE(fs::exists(archive.string() + ".journal"));
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));
    REQUIRE_FALSE(fs::exists(root / "Base/ExeFS"));
    REQUIRE_FALSE(fs::exists(root / "updates"));
    REQUIRE_FALSE(fs::exists(root / "dlc"));
    // The listing is not packed, so it is left behind with the folders holding it.
    REQUIRE(ReadAll(root / "Base/RomFS/.listing") == "listing");
    REQUIRE_FALSE(fs::exists(root / "Base/RomFS/shared.bin"));

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 4);
    const auto npdm = Npdm(TitleId);
    REQUIRE(nup.GetPackages()[0].npdm == std::vector<u8>(npdm.begin(), npdm.end()));
    REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(copy / "Base/RomFS"));
    REQUIRE(PackedTree(nup.GetRomFSDir(1)) == SourceTree(copy / "updates/65536/romfs"));

    // The resumed archive matches one packed in a single pass.
    const auto again = t.path / "again.nup";
    REQUIRE(FileSys::Nup::PackDump(copy, again) == FileSys::Nup::PackResult::Success);
    REQUIRE(ReadAll(archive) == ReadAll(again));
}

TEST_CASE("NUP rejects overwrite and a dump without NPDM", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "exefs/main", "main");
    Put(root / "romfs/a", "rom");
    const auto archive = t.path / "out.nup";
    Put(archive, "existing");
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::IoError);
    REQUIRE(ReadAll(archive) == "existing");
    fs::remove(archive);
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::InvalidSource);
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));
}

// Archives are meant to be byte-identical on every platform. A change to this hash means
// packing a dump no longer reproduces archives made by earlier builds (for example after a zstd
// update), so make it deliberately.
TEST_CASE("NUP output is reproducible", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    // Large and compressible, so it covers what used to be zstd's multithreaded path.
    Put(root / "Base/RomFS/large.bin", [] {
        std::string data(20 * 1024 * 1024, '\0');
        u32 x = 1;
        for (auto& c : data) {
            x = x * 1664525 + 1013904223;
            c = static_cast<char>('a' + (x >> 28));
        }
        return data;
    }());
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    std::string hex;
    for (const u8 b : Sha256(ReadAll(archive))) {
        hex += fmt::format("{:02x}", b);
    }
    REQUIRE(hex == "df8446903ab2c39a51af0304935eb4aa5d8ead105440976ceae7ab97133f6143");
}

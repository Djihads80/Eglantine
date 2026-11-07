// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include <cstdlib>
#include "core/file_sys/nup2_container.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {

using FileSys::Nup2Container;

std::array<u8, 32> Sha256(const u8* data, std::size_t size) {
    std::array<u8, 32> out{};
    unsigned int length = 0;
    EVP_Digest(data, size, out.data(), &length, EVP_sha256(), nullptr);
    return out;
}

void AppendU16(std::vector<u8>& out, u16 value) {
    out.push_back(static_cast<u8>(value & 0xFF));
    out.push_back(static_cast<u8>((value >> 8) & 0xFF));
}

void AppendU32(std::vector<u8>& out, u32 value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<u8>((value >> shift) & 0xFF));
    }
}

void AppendU64(std::vector<u8>& out, u64 value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<u8>((value >> shift) & 0xFF));
    }
}

struct BuiltContainer {
    std::vector<u8> bytes;
    std::size_t data_start{};
    std::size_t meta_offset{};
    std::size_t meta_entry_size{};
};

// Builds a minimal but fully valid S5.2-style container: one base package
// with an ExeFS file, a RomFS file and a bigger RomFS file. File-table rows
// use PACKAGE-RELATIVE paths (SPEC Section 3); scope prefixes live only in
// the entry names.
BuiltContainer BuildContainer() {
    struct SourceFile {
        std::string scope_path;    // full container path ("base/romfs/a.bin")
        std::string relative_path; // package-relative path ("romfs/a.bin")
        std::vector<u8> data;
    };
    const std::vector<SourceFile> sources{
        {"base/exefs/main.npdm", "exefs/main.npdm", {1, 2, 3, 4, 5}},
        {"base/romfs/a.bin", "romfs/a.bin", {0xAA, 0xBB}},
        {"base/romfs/big.bin", "romfs/big.bin", std::vector<u8>(600, 0x5A)},
    };

    std::vector<std::string> names{"meta.nup"};
    for (const auto& source : sources) {
        names.push_back(source.scope_path);
    }
    std::vector<u32> name_offsets;
    std::vector<u8> string_table;
    for (const auto& name : names) {
        name_offsets.push_back(static_cast<u32>(string_table.size()));
        string_table.insert(string_table.end(), name.begin(), name.end());
        string_table.push_back('\0');
    }
    const u64 raw_prefix = 0x10 + 0x40 * names.size() + string_table.size();
    const u64 data_start = (raw_prefix + 0x1FF) & ~static_cast<u64>(0x1FF);
    const u32 string_table_size =
        static_cast<u32>(data_start - raw_prefix + string_table.size());

    std::vector<std::array<u8, 32>> file_hashes;
    std::vector<u8> content_hash_input;
    std::vector<u8> row_bytes;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        file_hashes.push_back(Sha256(sources[i].data.data(), sources[i].data.size()));
        content_hash_input.insert(content_hash_input.end(),
                                  sources[i].relative_path.begin(),
                                  sources[i].relative_path.end());
        content_hash_input.push_back('\0');
        AppendU64(content_hash_input, sources[i].data.size());
        content_hash_input.insert(content_hash_input.end(), file_hashes[i].begin(),
                                  file_hashes[i].end());
        row_bytes.insert(row_bytes.end(), sources[i].relative_path.begin(),
                         sources[i].relative_path.end());
        row_bytes.push_back('\0');
        AppendU64(row_bytes, sources[i].data.size());
        row_bytes.insert(row_bytes.end(), file_hashes[i].begin(), file_hashes[i].end());
    }
    const auto content_hash = Sha256(content_hash_input.data(), content_hash_input.size());

    std::vector<u8> tlvs;
    AppendU16(tlvs, 0x0004);
    AppendU16(tlvs, 5);
    const std::string display_version = "1.0.0";
    tlvs.insert(tlvs.end(), display_version.begin(), display_version.end());
    AppendU16(tlvs, 0x0005);
    AppendU16(tlvs, 10);
    const std::string title_name = "Test Title";
    tlvs.insert(tlvs.end(), title_name.begin(), title_name.end());

    std::vector<u8> meta;
    meta.insert(meta.end(), {'N', 'U', 'P', 0x02});
    AppendU16(meta, 0x40);            // header size
    AppendU16(meta, 0);               // format patch version
    AppendU32(meta, 1);               // package count
    AppendU32(meta, 0);               // reserved
    AppendU64(meta, sources.size());  // file count
    const std::vector<u8> header_hash_placeholder(32, 0);
    meta.insert(meta.end(), header_hash_placeholder.begin(), header_hash_placeholder.end());
    AppendU32(meta, 0);               // reserved
    AppendU32(meta, 0);               // feature flags

    meta.push_back(0);                // kind: base
    meta.push_back(0);                // flags
    AppendU16(meta, static_cast<u16>(tlvs.size()));
    AppendU64(meta, 0x010074A01BF12000ULL); // title id
    AppendU64(meta, 0);                     // application id
    AppendU32(meta, 0);                     // version
    AppendU32(meta, static_cast<u32>(sources.size()));
    const std::vector<u8> zero_base_hash(32, 0);
    meta.insert(meta.end(), zero_base_hash.begin(), zero_base_hash.end());
    meta.insert(meta.end(), content_hash.begin(), content_hash.end());
    AppendU64(meta, 1);               // first entry index
    AppendU32(meta, 0);               // reserved
    meta.insert(meta.end(), tlvs.begin(), tlvs.end());
    meta.insert(meta.end(), row_bytes.begin(), row_bytes.end());

    std::vector<u8> entry_table;
    std::vector<u8> data_region;
    const u64 meta_entry_size = meta.size() + 32;
    u64 cursor = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const bool is_meta = i == 0;
        const u64 size = is_meta ? meta_entry_size : sources[i - 1].data.size();
        AppendU64(entry_table, cursor);
        AppendU64(entry_table, size);
        AppendU32(entry_table, name_offsets[i]);
        AppendU32(entry_table, is_meta ? 0u : static_cast<u32>(std::min<u64>(size, 0x200)));
        AppendU64(entry_table, 0);
        std::array<u8, 32> prefix_hash{};
        if (is_meta) {
            prefix_hash = Sha256(reinterpret_cast<const u8*>(""), 0);
        } else {
            const auto head = std::min<u64>(size, 0x200);
            prefix_hash = Sha256(sources[i - 1].data.data(), static_cast<std::size_t>(head));
        }
        entry_table.insert(entry_table.end(), prefix_hash.begin(), prefix_hash.end());
        if (!is_meta) {
            data_region.insert(data_region.end(), sources[i - 1].data.begin(),
                               sources[i - 1].data.end());
        }
        const u64 aligned = (size + 0x1FF) & ~static_cast<u64>(0x1FF);
        data_region.insert(data_region.end(), static_cast<std::size_t>(aligned - size), 0);
        cursor += aligned;
    }

    std::vector<u8> prefix_bytes;
    AppendU32(prefix_bytes, 0x30534648); // HFS0
    AppendU32(prefix_bytes, static_cast<u32>(names.size()));
    AppendU32(prefix_bytes, string_table_size);
    AppendU32(prefix_bytes, 0);
    prefix_bytes.insert(prefix_bytes.end(), entry_table.begin(), entry_table.end());
    prefix_bytes.insert(prefix_bytes.end(), string_table.begin(), string_table.end());
    prefix_bytes.insert(prefix_bytes.end(),
                        static_cast<std::size_t>(data_start - prefix_bytes.size()), 0);
    const auto header_hash = Sha256(prefix_bytes.data(), prefix_bytes.size());
    std::memcpy(meta.data() + 0x18, header_hash.data(), 32);
    const auto meta_digest = Sha256(meta.data(), meta.size());

    BuiltContainer built;
    built.data_start = static_cast<std::size_t>(data_start);
    built.bytes = std::move(prefix_bytes);
    built.meta_offset = built.bytes.size();
    built.meta_entry_size = meta_entry_size;
    built.bytes.insert(built.bytes.end(), meta.begin(), meta.end());
    built.bytes.insert(built.bytes.end(), meta_digest.begin(), meta_digest.end());
    built.bytes.insert(built.bytes.end(), data_region.begin(), data_region.end());
    return built;
}

} // namespace

TEST_CASE("NUP2 container parses and verifies a hand-built container", "[core:nup2]") {
    const auto built = BuildContainer();
    auto backing = std::make_shared<FileSys::VectorVfsFile>(built.bytes, "test.nup");
    Nup2Container container(backing);

    REQUIRE(container.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(container.GetPackages().size() == 1);

    const auto& package = container.GetPackages()[0];
    CHECK(package.kind == Nup2Container::PackageKind::Base);
    CHECK(package.title_id == 0x010074A01BF12000ULL);
    CHECK(package.display_version == "1.0.0");
    CHECK(package.title_name == "Test Title");
    CHECK(package.files.size() == 3);

    REQUIRE(container.FindBasePackage().has_value());
    REQUIRE(container.FindBestUpdatePackage() == std::nullopt);

    const auto exefs = container.GetExeFSDir(0);
    REQUIRE(exefs != nullptr);
    CHECK(exefs->GetFile("main.npdm") != nullptr);
    const auto romfs = container.GetRomFSDir(0);
    REQUIRE(romfs != nullptr);
    CHECK(romfs->GetFile("a.bin") != nullptr);
    CHECK(romfs->GetFile("big.bin") != nullptr);

    CHECK(container.VerifyIntegrity({}) == Loader::ResultStatus::Success);
}

TEST_CASE("NUP2 container detects data corruption", "[core:nup2]") {
    auto built = BuildContainer();
    built.bytes[built.data_start + 0x200] ^= 0xFF; // first data byte of main.npdm
    auto backing = std::make_shared<FileSys::VectorVfsFile>(built.bytes, "corrupt.nup");
    Nup2Container container(backing);
    REQUIRE(container.GetStatus() == Loader::ResultStatus::Success);
    CHECK(container.VerifyIntegrity({}) != Loader::ResultStatus::Success);
}

TEST_CASE("NUP2 container rejects header corruption at parse", "[core:nup2]") {
    auto built = BuildContainer();
    built.bytes[0x10 + 0x10] ^= 0xFF; // inside the entry table -> header hash breaks
    auto backing = std::make_shared<FileSys::VectorVfsFile>(built.bytes, "corrupt.nup");
    Nup2Container container(backing);
    CHECK(container.GetStatus() != Loader::ResultStatus::Success);
}

TEST_CASE("NUP2 container rejects a huge declared entry count", "[core:nup2]") {
    auto built = BuildContainer();
    built.bytes[4] = 0xFF;
    built.bytes[5] = 0xFF;
    built.bytes[6] = 0xFF;
    built.bytes[7] = 0x03; // ~67 million entries
    auto backing = std::make_shared<FileSys::VectorVfsFile>(built.bytes, "hostile.nup");
    Nup2Container container(backing);
    CHECK(container.GetStatus() != Loader::ResultStatus::Success);
}

TEST_CASE("NUP2 rejects file-table paths that escape the scope", "[core:nup2]") {
    auto built = BuildContainer();
    // Tamper a file-table row path into an absolute path and re-sign the meta
    // record the way a malicious packer would.
    // the ROW stores the package-relative path; the entry name has the prefix
    const std::array<u8, 12> needle{'r', 'o', 'm', 'f', 's', '/', 'a', '.', 'b', 'i', 'n', '\0'};
    const auto pos = static_cast<std::size_t>(std::distance(
        built.bytes.begin() + static_cast<std::ptrdiff_t>(built.meta_offset),
        std::search(built.bytes.begin() + static_cast<std::ptrdiff_t>(built.meta_offset),
                    built.bytes.end(), needle.begin(), needle.end())) +
        built.meta_offset);
    REQUIRE(pos + needle.size() <= built.bytes.size());
    const std::array<u8, 12> evil{'/', 't', 'm', 'p', '/', 'e', 'v', 'i', 'l', '\0', 'x', 0};
    for (std::size_t i = 0; i < needle.size(); ++i) {
        built.bytes[pos + i] = i < evil.size() ? evil[i] : built.bytes[pos + i];
    }
    const std::size_t meta_end = built.meta_offset + built.meta_entry_size;
    const std::vector<u8> body(built.bytes.begin() + static_cast<std::ptrdiff_t>(built.meta_offset),
                               built.bytes.begin() + static_cast<std::ptrdiff_t>(meta_end - 32));
    const auto digest = Sha256(body.data(), body.size());
    std::memcpy(built.bytes.data() + meta_end - 32, digest.data(), 32);

    auto backing = std::make_shared<FileSys::VectorVfsFile>(built.bytes, "evil.nup");
    Nup2Container container(backing);
    CHECK(container.GetStatus() != Loader::ResultStatus::Success);
}

TEST_CASE("IsNup2File distinguishes v2 containers from other files", "[core:nup2]") {
    const auto built = BuildContainer();
    auto nup = std::make_shared<FileSys::VectorVfsFile>(built.bytes, "test.nup");
    CHECK(FileSys::Nup2Container::IsNup2File(nup));

    auto empty = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{}, "empty.nup");
    CHECK(!FileSys::Nup2Container::IsNup2File(empty));

    auto junk = std::make_shared<FileSys::VectorVfsFile>(
        std::vector<u8>{'N', 'U', 'P', 0x01, 0, 0, 0, 0}, "v1.nup");
    CHECK(!FileSys::Nup2Container::IsNup2File(junk));
}

// Smoke test against a real packed container (SPEC-conformant output of the
// python nup tool). Set NUP2_REAL_CONTAINER to enable.
TEST_CASE("NUP2 real container binds and verifies", "[core:nup2][real]") {
    const char* path = std::getenv("NUP2_REAL_CONTAINER");
    if (path == nullptr) {
        SKIP("NUP2_REAL_CONTAINER not set");
    }
    FileSys::RealVfsFilesystem real;
    auto backing = real.OpenFile(path, FileSys::OpenMode::Read);
    REQUIRE(backing != nullptr);
    FileSys::Nup2Container container(backing);
    REQUIRE(container.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(!container.GetPackages().empty());
    CHECK(container.VerifyIntegrity({}) == Loader::ResultStatus::Success);
}

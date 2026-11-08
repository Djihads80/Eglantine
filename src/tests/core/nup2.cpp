// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include "common/lz4_compression.h"
#include "core/file_sys/nup2_container.h"
#include "core/file_sys/nup2_writer.h"
#include "core/file_sys/vfs/vfs_mapped.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/nso.h"
#include "core/loader/nso_verify.h"

namespace {

namespace fs = std::filesystem;
using FileSys::Nup2Container;
namespace Nup2 = FileSys::Nup2;

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    ("nup2-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::vector<u8> ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void WriteFile(const fs::path& p, const std::vector<u8>& data) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::vector<u8> Pattern(std::size_t size, u32 seed) {
    std::vector<u8> out(size);
    u32 state = seed * 2654435761u + 1;
    for (auto& byte : out) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<u8>(state >> 24);
    }
    return out;
}


std::array<u8, 32> Sha256(const std::vector<u8>& data) {
    std::array<u8, 32> out{};
    unsigned int length{};
    EVP_Digest(data.data(), data.size(), out.data(), &length, EVP_sha256(), nullptr);
    return out;
}

FileSys::VirtualFile Vec(std::vector<u8> data, std::string name = "f") {
    return std::make_shared<FileSys::VectorVfsFile>(std::move(data), std::move(name));
}

template <typename T>
void PutAt(std::vector<u8>& buf, std::size_t at, T value) {
    std::memcpy(buf.data() + at, &value, sizeof(T));
}

template <typename T>
T GetAt(const std::vector<u8>& buf, std::size_t at) {
    T value{};
    std::memcpy(&value, buf.data() + at, sizeof(T));
    return value;
}

// A parseable NPDM: META header, an ACID block with an access-control record, and an ACI0 block carrying the
// program ID.
std::vector<u8> Npdm(u64 program_id) {
    constexpr u32 acid_offset = 0x80;
    constexpr u32 aci_offset = 0x400;
    std::vector<u8> npdm(0x480);
    std::memcpy(npdm.data(), "META", 4);
    PutAt<u32>(npdm, 0x70, aci_offset);
    PutAt<u32>(npdm, 0x74, 0x80);
    PutAt<u32>(npdm, 0x78, acid_offset);
    PutAt<u32>(npdm, 0x7C, 0x2C + 0x240);
    std::memcpy(npdm.data() + acid_offset + 0x200, "ACID", 4);
    PutAt<u32>(npdm, acid_offset + 0x220, 0x240); // fac_offset
    PutAt<u32>(npdm, acid_offset + 0x224, 0x2C);  // fac_size
    std::memcpy(npdm.data() + aci_offset, "ACI0", 4);
    std::memcpy(npdm.data() + aci_offset + 0x10, &program_id, sizeof(program_id));
    PutAt<u32>(npdm, aci_offset + 0x20, 0x40); // fah_offset
    PutAt<u32>(npdm, aci_offset + 0x24, 0x1C); // fah_size
    PutAt<u32>(npdm, aci_offset + 0x30, 0x60); // kac_offset
    PutAt<u32>(npdm, aci_offset + 0x34, 4);    // kac_size
    return npdm;
}
constexpr u64 TitleId = 0x0100ABCD00001000;

struct Built {
    std::vector<u8> bytes;
    std::vector<u8> main;
    std::vector<u8> romfs;
};

// Writes a small v2 file through PackFromFiles and returns its bytes.
Built Build(std::size_t romfs_size = 300 * 1024, bool with_icon = false, const std::filesystem::path& keep = {}) {
    TempDir dir;
    Built built;
    built.main = Pattern(5000, 1);
    built.romfs = Pattern(romfs_size, 2);
    std::vector<Nup2::ExeFsSource> exefs{{"main", Vec(built.main)}, {"main.npdm", Vec(Npdm(TitleId))}};
    if (with_icon) {
        exefs.push_back({"icon.dat", Vec(Pattern(777, 3))});
    }
    Nup2::PackOptions options;
    options.title_id = TitleId;
    options.program_version = 0x010203;
    options.display_version = "1.2.3";
    options.title_name = "Test Title";
    std::string error;
    const auto out = keep.empty() ? dir.path / "out.nup" : keep;
    REQUIRE(Nup2::PackFromFiles(exefs, Vec(built.romfs), out, options, {}, &error) == Nup2::PackResult::Success);
    built.bytes = ReadFile(out);
    return built;
}

bool Parses(const std::vector<u8>& bytes) {
    return Nup2Container(Vec(bytes)).IsValid();
}

} // namespace

TEST_CASE("NUP2 container: valid file", "[core][nup2]") {
    const auto built = Build(300 * 1024, true);
    const Nup2Container container(Vec(built.bytes));
    REQUIRE(container.IsValid());
    REQUIRE(container.GetTitleId() == TitleId);
    REQUIRE(container.GetProgramVersion() == 0x010203);
    REQUIRE(container.GetDisplayVersion() == "1.2.3");
    REQUIRE(container.GetTitleName() == "Test Title");
    REQUIRE(container.GetIcon() == Pattern(777, 3));
    // The icon is a table entry but not part of the ExeFS the game sees.
    REQUIRE(container.GetExeFsDir()->GetFiles().size() == 2);
    REQUIRE(container.GetExeFsEntries().size() == 3);
    for (const auto& entry : container.GetExeFsEntries()) {
        REQUIRE(entry.offset % Nup2Container::ExeFsAlignment == 0);
    }
}

TEST_CASE("NUP2 container: malformed headers are rejected", "[core][nup2]") {
    const auto built = Build();
    REQUIRE(Parses(built.bytes));

    SECTION("not a NUP v2 file") {
        auto bytes = built.bytes;
        bytes[3] = 0x01;
        REQUIRE_FALSE(Nup2Container::IsNup2File(Vec(bytes)));
        REQUIRE_FALSE(Parses(bytes));
        REQUIRE_FALSE(Nup2Container::IsNup2File(nullptr));
        REQUIRE_FALSE(Nup2Container::IsNup2File(Vec({})));
    }
    SECTION("truncated superblock") {
        auto bytes = built.bytes;
        bytes.resize(100);
        REQUIRE(Nup2Container::IsNup2File(Vec(bytes)));
        REQUIRE_FALSE(Parses(bytes));
    }
    SECTION("unsupported revision") {
        auto bytes = built.bytes;
        PutAt<u32>(bytes, 0x04, 2);
        REQUIRE_FALSE(Parses(bytes));
    }
    SECTION("file size differs from the header") {
        auto bytes = built.bytes;
        bytes.pop_back();
        REQUIRE_FALSE(Parses(bytes));
        bytes = built.bytes;
        bytes.push_back(0);
        REQUIRE_FALSE(Parses(bytes));
    }
    SECTION("RomFS offset not aligned") {
        auto bytes = built.bytes;
        PutAt<u64>(bytes, 0x30, GetAt<u64>(bytes, 0x30) + 0x1000);
        REQUIRE_FALSE(Parses(bytes));
    }
    SECTION("RomFS runs past the end") {
        auto bytes = built.bytes;
        PutAt<u64>(bytes, 0x38, GetAt<u64>(bytes, 0x38) + 1);
        REQUIRE_FALSE(Parses(bytes));
        PutAt<u64>(bytes, 0x38, ~u64{0});
        REQUIRE_FALSE(Parses(bytes));
    }
    SECTION("RomFS offset past the end") {
        auto bytes = built.bytes;
        PutAt<u64>(bytes, 0x30, u64{1} << 40);
        REQUIRE_FALSE(Parses(bytes));
    }
    SECTION("meta table out of bounds") {
        auto bytes = built.bytes;
        PutAt<u64>(bytes, 0x20, u64{1} << 40);
        REQUIRE_FALSE(Parses(bytes));
        bytes = built.bytes;
        PutAt<u64>(bytes, 0x28, ~u64{0});
        REQUIRE_FALSE(Parses(bytes));
    }
    SECTION("entry count larger than the table") {
        auto bytes = built.bytes;
        PutAt<u32>(bytes, 0x40, 1000);
        REQUIRE_FALSE(Parses(bytes));
        PutAt<u32>(bytes, 0x40, 0xFFFFFFFFu);
        REQUIRE_FALSE(Parses(bytes));
    }
    SECTION("ExeFS entry out of bounds") {
        auto bytes = built.bytes;
        const u64 meta = GetAt<u64>(bytes, 0x20);
        PutAt<u64>(bytes, meta, u64{1} << 40);
        REQUIRE_FALSE(Parses(bytes));
        bytes = built.bytes;
        PutAt<u64>(bytes, meta + 8, ~u64{0});
        REQUIRE_FALSE(Parses(bytes));
    }
    SECTION("ExeFS entry without a name") {
        auto bytes = built.bytes;
        const u64 meta = GetAt<u64>(bytes, 0x20);
        bytes[meta + 16] = 0;
        REQUIRE_FALSE(Parses(bytes));
    }
    SECTION("name fields without a terminator stay inside their fields") {
        auto bytes = built.bytes;
        std::fill_n(bytes.begin() + 0x48, 32, 'v');
        std::fill_n(bytes.begin() + 0x68, 128, 't');
        const Nup2Container container(Vec(bytes));
        REQUIRE(container.IsValid());
        REQUIRE(container.GetDisplayVersion() == std::string(32, 'v'));
        REQUIRE(container.GetTitleName() == std::string(128, 't'));
    }
}

TEST_CASE("NUP2 writer: layout and RomFS identity", "[core][nup2]") {
    const auto built = Build();
    const Nup2Container container(Vec(built.bytes));
    REQUIRE(container.IsValid());

    const auto romfs = container.GetRomFs();
    REQUIRE(romfs->GetSize() == built.romfs.size());
    REQUIRE(romfs->ReadAllBytes() == built.romfs);
    REQUIRE(GetAt<u64>(built.bytes, 0x30) % Nup2Container::RomFsAlignment == 0);
    REQUIRE(GetAt<u64>(built.bytes, 0x18) == built.bytes.size());

    const auto main = container.GetExeFsDir()->GetFile("main");
    REQUIRE(main != nullptr);
    REQUIRE(main->ReadAllBytes() == built.main);
    REQUIRE(container.GetIcon().empty());
}

TEST_CASE("NUP2 writer: rejects bad input", "[core][nup2]") {
    TempDir dir;
    Nup2::PackOptions options;
    options.title_id = TitleId;
    std::string error;
    const auto out = dir.path / "out.nup";

    SECTION("no RomFS") {
        REQUIRE(Nup2::PackFromFiles({{"main", Vec({1})}, {"main.npdm", Vec(Npdm(TitleId))}}, nullptr, out, options,
                                    {}, &error) == Nup2::PackResult::RomFsMissing);
        REQUIRE_FALSE(fs::exists(out));
    }
    SECTION("ExeFS missing main or main.npdm") {
        REQUIRE(Nup2::PackFromFiles({{"main", Vec({1})}}, Vec({1}), out, options, {}, &error) ==
                Nup2::PackResult::ExeFsMissing);
        REQUIRE(Nup2::PackFromFiles({{"main.npdm", Vec(Npdm(TitleId))}}, Vec({1}), out, options, {}, &error) ==
                Nup2::PackResult::ExeFsMissing);
        REQUIRE_FALSE(fs::exists(out));
    }
    SECTION("hidden and over-long names are dropped") {
        const std::string long_name(48, 'x');
        REQUIRE(Nup2::PackFromFiles({{"main", Vec({1})},
                                     {"main.npdm", Vec(Npdm(TitleId))},
                                     {".hidden", Vec({2})},
                                     {long_name, Vec({3})}},
                                    Vec({1}), out, options, {}, &error) == Nup2::PackResult::Success);
        const Nup2Container container(Vec(ReadFile(out)));
        REQUIRE(container.IsValid());
        REQUIRE(container.GetExeFsEntries().size() == 2);
    }
    SECTION("title id is read from main.npdm when not given") {
        options.title_id = 0;
        REQUIRE(Nup2::PackFromFiles({{"main", Vec({1})}, {"main.npdm", Vec(Npdm(0x0100FEED00002000))}}, Vec({1}),
                                    out, options, {}, &error) == Nup2::PackResult::Success);
        REQUIRE(Nup2Container(Vec(ReadFile(out))).GetTitleId() == 0x0100FEED00002000);
    }
    SECTION("unparsable main.npdm") {
        options.title_id = 0;
        REQUIRE(Nup2::PackFromFiles({{"main", Vec({1})}, {"main.npdm", Vec({1, 2, 3})}}, Vec({1}), out, options, {},
                                    &error) == Nup2::PackResult::BadMetadata);
    }
    SECTION("cancelled write removes the output") {
        const std::atomic_bool cancel{true};
        REQUIRE(Nup2::PackFromFiles({{"main", Vec({1})}, {"main.npdm", Vec(Npdm(TitleId))}}, Vec(Pattern(1 << 20, 4)),
                                    out, options, {}, &error, &cancel) == Nup2::PackResult::Cancelled);
        REQUIRE_FALSE(fs::exists(out));
    }
}

namespace {
// A source that reports its full size but cannot be read.
class UnreadableFile final : public FileSys::VectorVfsFile {
public:
    using VectorVfsFile::VectorVfsFile;
    std::size_t Read(u8*, std::size_t, std::size_t) const override {
        return 0;
    }
};
} // namespace

TEST_CASE("NUP2 writer: a failed source read names the copy and removes the output", "[core][nup2]") {
    TempDir dir;
    Nup2::PackOptions options;
    options.title_id = TitleId;
    std::string error;
    const auto out = dir.path / "out.nup";
    const auto bad = std::make_shared<UnreadableFile>(Pattern(4096, 5), "romfs");
    REQUIRE(Nup2::PackFromFiles({{"main", Vec({1})}, {"main.npdm", Vec(Npdm(TitleId))}}, bad, out, options, {},
                                &error) == Nup2::PackResult::IoError);
    REQUIRE(error.find("cannot copy") != std::string::npos);
    REQUIRE(error.find("source offset") != std::string::npos);
    REQUIRE_FALSE(fs::exists(out));
}

TEST_CASE("NUP2 RomFS SHA-256", "[core][nup2]") {
    // Several 32 MiB slices, so the in-order hashing across workers is exercised.
    const auto built = Build(70 * 1024 * 1024);
    const Nup2Container container(Vec(built.bytes));
    REQUIRE(container.IsValid());
    REQUIRE(container.GetRomFsSha256().has_value());
    REQUIRE(*container.GetRomFsSha256() == Sha256(built.romfs));

    std::string error;
    u64 last_done = 0;
    REQUIRE(Nup2::VerifyRomFsHash(Vec(built.bytes), [&](u64 done, u64) { last_done = done; }, &error));
    REQUIRE(last_done == built.romfs.size());

    SECTION("a changed RomFS byte is reported") {
        auto bytes = built.bytes;
        bytes[GetAt<u64>(bytes, 0x30) + 40 * 1024 * 1024] ^= 0xFF;
        REQUIRE_FALSE(Nup2::VerifyRomFsHash(Vec(bytes), {}, &error));
        REQUIRE(error.find("does not match") != std::string::npos);
    }
    SECTION("a changed hash is reported") {
        auto bytes = built.bytes;
        bytes[Nup2Container::RomFsSha256Offset] ^= 0x01;
        REQUIRE_FALSE(Nup2::VerifyRomFsHash(Vec(bytes), {}, &error));
    }
    SECTION("a file without a recorded hash") {
        auto bytes = built.bytes;
        PutAt<u32>(bytes, 0x14, 0);
        REQUIRE_FALSE(Nup2Container(Vec(bytes)).GetRomFsSha256().has_value());
        REQUIRE_FALSE(Nup2::VerifyRomFsHash(Vec(bytes), {}, &error));
        REQUIRE(error.find("no RomFS SHA-256") != std::string::npos);
    }
    SECTION("not a NUP v2 file") {
        REQUIRE_FALSE(Nup2::VerifyRomFsHash(Vec(Pattern(8192, 6)), {}, &error));
    }
}

TEST_CASE("NUP2 ConvertFromDump", "[core][nup2]") {
    TempDir dir;
    const auto root = dir.path / "dump";
    WriteFile(root / "exefs" / "main", Pattern(3000, 7));
    WriteFile(root / "exefs" / "main.npdm", Npdm(TitleId));
    WriteFile(root / "romfs" / "a.bin", Pattern(5000, 8));
    WriteFile(root / "romfs" / "sub" / "b.bin", Pattern(70000, 9));

    std::string error;
    const auto out = dir.path / "out.nup";
    REQUIRE(Nup2::ConvertFromDump(root, out, {}, &error) == Nup2::PackResult::Success);
    const Nup2Container container(Vec(ReadFile(out)));
    REQUIRE(container.IsValid());
    REQUIRE(container.GetTitleId() == TitleId);
    REQUIRE(container.GetExeFsDir()->GetFile("main")->ReadAllBytes() == Pattern(3000, 7));
    REQUIRE(container.GetRomFs()->GetSize() > 75000);
    REQUIRE(container.GetRomFsSha256().has_value());
    REQUIRE(Nup2::VerifyRomFsHash(Vec(ReadFile(out)), {}, &error));

    SECTION("a folder without an ExeFS") {
        fs::remove_all(root / "exefs");
        REQUIRE(Nup2::ConvertFromDump(root, dir.path / "none.nup", {}, &error) == Nup2::PackResult::ExeFsMissing);
        REQUIRE_FALSE(fs::exists(dir.path / "none.nup"));
    }
}

TEST_CASE("MappedVfsFile", "[core][nup2]") {
    TempDir dir;
    const auto data = Pattern(600 * 1024, 10);
    const auto path = dir.path / "mapped.bin";
    WriteFile(path, data);

    const auto file = FileSys::MappedVfsFile::Open(path.string());
    REQUIRE(file != nullptr);
    REQUIRE(file->GetSize() == data.size());
    REQUIRE(file->IsReadable());
    REQUIRE_FALSE(file->IsWritable());

    SECTION("reads match the file") {
        std::vector<u8> out(1000);
        REQUIRE(file->Read(out.data(), out.size(), 12345) == out.size());
        REQUIRE(std::equal(out.begin(), out.end(), data.begin() + 12345));

        std::vector<u8> big(400 * 1024);
        REQUIRE(file->Read(big.data(), big.size(), 100) == big.size());
        REQUIRE(std::equal(big.begin(), big.end(), data.begin() + 100));
    }
    SECTION("reads past the end are short") {
        std::vector<u8> out(100);
        REQUIRE(file->Read(out.data(), out.size(), data.size() - 40) == 40);
        REQUIRE(file->Read(out.data(), out.size(), data.size()) == 0);
        REQUIRE(file->Read(out.data(), out.size(), data.size() + 4096) == 0);
    }
    SECTION("missing file") {
        REQUIRE(FileSys::MappedVfsFile::Open((dir.path / "missing.bin").string()) == nullptr);
    }
    SECTION("writes are refused") {
        const u8 byte = 1;
        REQUIRE(file->Write(&byte, 1, 0) == 0);
    }
}

namespace {

constexpr std::size_t NsoHeaderSize = sizeof(Loader::NSOHeader);

// An NSO whose three segments hold `contents`, optionally LZ4-compressed, with correct hashes in the header.
std::vector<u8> BuildNso(const std::array<std::vector<u8>, 3>& contents, bool compressed, u32 check_flags) {
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.flags = check_flags << 3;
    std::vector<u8> body;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto stored = compressed ? Common::Compression::CompressDataLZ4HCMax(contents[i].data(), contents[i].size())
                                       : contents[i];
        header.segments[i].offset = static_cast<u32>(NsoHeaderSize + body.size());
        header.segments[i].size = static_cast<u32>(contents[i].size());
        header.segments_compressed_size[i] = static_cast<u32>(stored.size());
        header.segment_hashes[i] = Sha256(contents[i]);
        if (compressed) {
            header.flags |= 1u << i;
        }
        body.insert(body.end(), stored.begin(), stored.end());
    }
    std::vector<u8> out(NsoHeaderSize);
    std::memcpy(out.data(), &header, NsoHeaderSize);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

const std::array<std::vector<u8>, 3> NsoContents{Pattern(0x3000, 11), Pattern(0x1000, 12), Pattern(0x800, 13)};

} // namespace

TEST_CASE("NSO segment hashes", "[core][nup2][nso]") {
    using Loader::NSOHashStatus;

    for (const bool compressed : {false, true}) {
        DYNAMIC_SECTION((compressed ? "compressed" : "uncompressed") << " segments") {
            auto nso = BuildNso(NsoContents, compressed, 0b111);
            auto report = Loader::VerifyNSOSegmentHashes(*Vec(nso));
            REQUIRE(report.header_valid);
            REQUIRE(report.AllMatch());
            REQUIRE(report.FlaggedMatch());
            for (const auto& segment : report.segments) {
                REQUIRE(segment.flagged);
            }

            if (!compressed) {
                // Flip a byte in the RoData segment.
                nso[NsoHeaderSize + 0x3000 + 5] ^= 0xFF;
                report = Loader::VerifyNSOSegmentHashes(*Vec(nso));
                REQUIRE(report.header_valid);
                REQUIRE(report.segments[0].status == NSOHashStatus::Match);
                REQUIRE(report.segments[1].status == NSOHashStatus::Mismatch);
                REQUIRE(report.segments[2].status == NSOHashStatus::Match);
                REQUIRE_FALSE(report.AllMatch());
                REQUIRE_FALSE(report.FlaggedMatch());
            }
        }
    }
}

TEST_CASE("NSO segment hashes: flags decide what counts", "[core][nup2][nso]") {
    using Loader::NSOHashStatus;
    // Only the Data segment is flagged for checking, and only the RoData segment is damaged.
    auto nso = BuildNso(NsoContents, false, 0b100);
    nso[NsoHeaderSize + 0x3000 + 5] ^= 0xFF;
    const auto report = Loader::VerifyNSOSegmentHashes(*Vec(nso));
    REQUIRE(report.segments[1].status == NSOHashStatus::Mismatch);
    REQUIRE_FALSE(report.segments[1].flagged);
    REQUIRE(report.segments[2].flagged);
    REQUIRE(report.FlaggedMatch());
    REQUIRE_FALSE(report.AllMatch());
}

TEST_CASE("NSO segment hashes: unreadable input", "[core][nup2][nso]") {
    using Loader::NSOHashStatus;

    SECTION("not an NSO") {
        const auto report = Loader::VerifyNSOSegmentHashes(*Vec(Pattern(0x400, 14)));
        REQUIRE_FALSE(report.header_valid);
        REQUIRE_FALSE(report.AllMatch());
        REQUIRE_FALSE(report.FlaggedMatch());
    }
    SECTION("shorter than a header") {
        REQUIRE_FALSE(Loader::VerifyNSOSegmentHashes(*Vec(Pattern(0x80, 15))).header_valid);
    }
    SECTION("segment past the end of the file") {
        auto nso = BuildNso(NsoContents, false, 0b111);
        nso.resize(nso.size() - 0x400);
        const auto report = Loader::VerifyNSOSegmentHashes(*Vec(nso));
        REQUIRE(report.header_valid);
        REQUIRE(report.segments[0].status == NSOHashStatus::Match);
        REQUIRE(report.segments[2].status == NSOHashStatus::Unreadable);
    }
    SECTION("absurd segment size") {
        auto nso = BuildNso(NsoContents, false, 0b111);
        Loader::NSOHeader header{};
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[0].size = 0xFFFFFFF0u;
        header.segments_compressed_size[0] = 0xFFFFFFF0u;
        std::memcpy(nso.data(), &header, sizeof(header));
        const auto report = Loader::VerifyNSOSegmentHashes(*Vec(nso));
        REQUIRE(report.segments[0].status == NSOHashStatus::Unreadable);
    }
    SECTION("corrupt compressed data") {
        auto nso = BuildNso(NsoContents, true, 0b111);
        std::fill(nso.begin() + NsoHeaderSize, nso.begin() + NsoHeaderSize + 64, 0xFF);
        const auto report = Loader::VerifyNSOSegmentHashes(*Vec(nso));
        REQUIRE(report.segments[0].status != NSOHashStatus::Match);
    }
}

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/loader/loader.h"
#include "core/loader/nx.h"
#include "tests/core/system_test_context.h"

namespace {

constexpr u64 ApplicationId = 0x0100123456780000;
constexpr std::size_t BigSize = (std::size_t{17} << 20) + 12345;
constexpr std::size_t RandomSize = 160 * 1024 + 77;

FileSys::VirtualFile OpenFixture(std::string_view name) {
    static const auto filesystem = std::make_shared<FileSys::RealVfsFilesystem>();
    return filesystem->OpenFile(std::string(EGLANTINE_NX_FIXTURE_DIR) + "/" + std::string(name),
                                FileSys::OpenMode::Read);
}

/// The fixture generator's xorshift64 byte stream (see nx_fixtures/README.md).
std::vector<u8> Xorshift(u64 x, std::size_t count) {
    std::vector<u8> out(count);
    for (auto& byte : out) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        byte = static_cast<u8>(x >> 32);
    }
    return out;
}

std::vector<u8> Expected(std::string_view file, std::size_t offset, std::size_t length) {
    static const auto period = Xorshift(0x9e3779b97f4a7c15, 1021);
    static const auto random = Xorshift(0x2545f4914f6cdd1d, RandomSize);
    std::vector<u8> out(length);
    for (std::size_t i = 0; i < length; ++i) {
        out[i] = file == "big.bin" ? period[(offset + i) % period.size()] : random[offset + i];
    }
    return out;
}

std::string ReadText(const FileSys::VirtualFile& file) {
    REQUIRE(file != nullptr);
    const auto bytes = file->ReadAllBytes();
    return std::string(bytes.begin(), bytes.end());
}

constexpr std::string_view ValidFixtures[] = {
    "minimal_valid.nx",
    "nested_vfs.nx",
    "zstd_single.nx",
    "shared_snapshots.nx",
    "deleted_between_versions.nx",
    "multiple_programs.nx",
    "jpeg_icon.nx",
    "localized_metadata_icons.nx",
    "dlc_with_program.nx",
    "empty_files.nx",
    "lost_header_page_0.nx",
    "appended_generation.nx",
    "interrupted_commit.nx",
};

constexpr std::string_view InvalidFixtures[] = {
    "corrupted_manifest.nx",
    "corrupted_hot_index.nx",
    "cyclic_directory.nx",
    "repeated_directory.nx",
    "repeated_file.nx",
    "invalid_path.nx",
    "truncated_archive.nx",
    "truncated_header_area.nx",
    "damaged_both_slots.nx",
    "unknown_mandatory_feature.nx",
    "slot_generation_conflict.nx",
};

} // Anonymous namespace

TEST_CASE("NX: archives are identified by either header slot", "[core][nx]") {
    CHECK(Loader::IdentifyFile(OpenFixture("lichii_synthetic.nx")) == Loader::FileType::NX);
    CHECK(Loader::AppLoader_NX::IdentifyType(OpenFixture("lost_header_page_0.nx")) ==
          Loader::FileType::NX);
    CHECK(Loader::GuessFromFilename("Game.NX") == Loader::FileType::NX);
    CHECK(Loader::IsContainerType(Loader::FileType::NX));
    const auto zeros = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(8192), "x.nx");
    CHECK(Loader::AppLoader_NX::IdentifyType(zeros) == Loader::FileType::Error);
}

TEST_CASE("NX: catalog-only queries", "[core][nx]") {
    const auto file = OpenFixture("lichii_synthetic.nx");
    CHECK(Loader::AppLoader_NX::IsBootable(file));
    CHECK(Loader::IsBootableGameContainer(file, Loader::FileType::NX));
    CHECK(Loader::AppLoader_NX::ReadPlatform(file, 0) == 2);
    CHECK(Loader::AppLoader_NX::ReadPlatform(file, ApplicationId) == 2);
    // Header and Catalog only: Manifest damage is found when the loader mounts the archive.
    for (const auto name :
         {"damaged_both_slots.nx", "truncated_header_area.nx", "invalid_offsets.nx"}) {
        INFO(name);
        CHECK_FALSE(Loader::AppLoader_NX::IsBootable(OpenFixture(name)));
    }
}

TEST_CASE("NX: tree snapshot loads the newest update", "[core][nx]") {
    ScopedSystemTestContext environment;
    Core::System system;
    Loader::AppLoader_NX loader{system, OpenFixture("lichii_synthetic.nx")};
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    CHECK(program_id == ApplicationId);
    std::string title;
    REQUIRE(loader.ReadTitle(title) == Loader::ResultStatus::Success);
    CHECK(title == "Synthetic Game");
    FileSys::NACP nacp;
    REQUIRE(loader.ReadControlData(nacp) == Loader::ResultStatus::Success);
    CHECK(nacp.GetApplicationName() == "Synthetic Game");
    CHECK_FALSE(loader.IsRomFSUpdatable());

    FileSys::VirtualFile romfs;
    REQUIRE(loader.ReadRomFS(romfs) == Loader::ResultStatus::Success);
    const auto tree = FileSys::ExtractRomFS(romfs);
    REQUIRE(tree != nullptr);
    CHECK(ReadText(tree->GetFile("shadowed.bin")) == "update version");
    CHECK(ReadText(tree->GetFile("added.bin")) == "new in the update");
    CHECK(ReadText(tree->GetFile("common.bin")) == "base common data");
    const auto deep = tree->GetFileRelative("sub/deep.bin");
    REQUIRE(deep != nullptr);
    CHECK(deep->ReadAllBytes() == std::vector<u8>(5000, 7));
}

TEST_CASE("NX: reads span blocks, codecs and the reader's read limit", "[core][nx]") {
    ScopedSystemTestContext environment;
    Core::System system;
    // Through the VFS (no host path) and through the pinned host file.
    const auto real = OpenFixture("lichii_synthetic.nx");
    for (const auto& file :
         {real,
          FileSys::VirtualFile{std::make_shared<FileSys::VectorVfsFile>(real->ReadAllBytes())}}) {
        Loader::AppLoader_NX loader{system, file};
        FileSys::VirtualFile romfs;
        REQUIRE(loader.ReadRomFS(romfs) == Loader::ResultStatus::Success);
        const auto tree = FileSys::ExtractRomFS(romfs);
        REQUIRE(tree != nullptr);
        const auto big = tree->GetFile("big.bin");
        const auto random = tree->GetFile("random.bin");
        REQUIRE(big != nullptr);
        REQUIRE(random != nullptr);
        REQUIRE(big->GetSize() == BigSize);
        REQUIRE(random->GetSize() == RandomSize);
        CHECK(big->ReadAllBytes() == Expected("big.bin", 0, BigSize));
        CHECK(random->ReadAllBytes() == Expected("random.bin", 0, RandomSize));

        std::mt19937_64 rng{1234};
        for (int i = 0; i < 100; ++i) {
            const auto& target = (i % 2) ? big : random;
            const std::size_t offset = rng() % target->GetSize();
            const std::size_t length =
                std::min<std::size_t>(rng() % (3 << 20), target->GetSize() - offset);
            std::vector<u8> out(length);
            REQUIRE(target->Read(out.data(), length, offset) == length);
            REQUIRE(out == Expected(target->GetName(), offset, length));
        }

        std::atomic<int> mismatches{0};
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                std::mt19937_64 thread_rng{t};
                for (int i = 0; i < 25; ++i) {
                    const auto& target = (t + i) % 2 ? big : random;
                    const std::size_t offset = thread_rng() % target->GetSize();
                    const std::size_t length =
                        std::min<std::size_t>(thread_rng() % (1 << 21), target->GetSize() - offset);
                    std::vector<u8> out(length);
                    if (target->Read(out.data(), length, offset) != length ||
                        out != Expected(target->GetName(), offset, length)) {
                        ++mismatches;
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(mismatches == 0);
    }
}

TEST_CASE("NX: upstream conformance fixtures", "[core][nx]") {
    ScopedSystemTestContext environment;
    Core::System system;
    for (const auto name : InvalidFixtures) {
        INFO(name);
        Loader::AppLoader_NX loader{system, OpenFixture(name)};
        u64 program_id{};
        CHECK(loader.ReadProgramId(program_id) != Loader::ResultStatus::Success);
    }
    for (const auto name : ValidFixtures) {
        INFO(name);
        Loader::AppLoader_NX loader{system, OpenFixture(name)};
        std::vector<u8> icon;
        std::string title;
        loader.ReadIcon(icon);
        loader.ReadTitle(title);
        FileSys::VirtualFile romfs;
        loader.ReadRomFS(romfs);
        Loader::AppLoader::Modules modules;
        loader.ReadNSOModules(modules);
        CHECK(loader.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    }
    Loader::AppLoader_NX corrupted{system, OpenFixture("corrupted_block_payload.nx")};
    CHECK(corrupted.VerifyIntegrity({}) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
}

TEST_CASE("NX: the program index selects a secondary program", "[core][nx]") {
    ScopedSystemTestContext environment;
    Core::System system;
    constexpr u64 Main = 0x0100000000000001;
    for (const auto& [index, expected] :
         {std::pair<std::size_t, u64>{0, Main}, std::pair<std::size_t, u64>{1, Main + 1}}) {
        INFO(index);
        Loader::AppLoader_NX loader{system, OpenFixture("multiple_programs.nx"), Main, index};
        u64 program_id{};
        REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
        CHECK(program_id == expected);
    }
}

TEST_CASE("NX: a standalone update shows its version and can be disabled", "[core][nx]") {
    ScopedSystemTestContext environment;
    Core::System system;
    const auto display_version = [&] {
        Loader::AppLoader_NX loader{system, OpenFixture("standalone_update.nx")};
        std::string version;
        REQUIRE(loader.ReadDisplayVersion(version) == Loader::ResultStatus::Success);
        return version;
    };
    // Added from a bare update folder: no display version, no base dependency.
    CHECK(display_version() == "v131072");
    Settings::values.disabled_addons[ApplicationId] = {"Update@131072"};
    const auto without = display_version();
    Settings::values.disabled_addons.erase(ApplicationId);
    CHECK(without != "v131072");
}

TEST_CASE("NX: an extracted add-on is listed as add-on content after Load", "[core][nx]") {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    kernel.Initialize();
    SCOPE_EXIT {
        system.CoreTiming().ClearPendingEvents();
        kernel.Shutdown();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&bootstrap);
    };
    FileSys::ManualContentProvider provider;
    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, &provider);
    Kernel::KProcess process{kernel};
    Kernel::KAutoObject::Create(&process);
    SCOPE_EXIT {
        kernel.UnregisterKernelObject(&process);
    };
    Loader::AppLoader_NX loader{system, OpenFixture("lichii_synthetic.nx")};
    REQUIRE(loader.Load(process, system).first == Loader::ResultStatus::Success);

    // As the AoC service lists add-ons and the FS service opens their data.
    constexpr u64 AddOnId = ApplicationId + 0x1001;
    const auto& content = system.GetContentProvider();
    const auto entries =
        content.ListEntriesFilter(FileSys::TitleType::AOC, FileSys::ContentRecordType::Data);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].title_id == AddOnId);
    const auto nca = content.GetEntry(AddOnId, FileSys::ContentRecordType::Data);
    REQUIRE(nca != nullptr);
    REQUIRE(nca->GetStatus() == Loader::ResultStatus::Success);
    CHECK(nca->GetTitleId() == AddOnId);
    const auto romfs = FileSys::ExtractRomFS(nca->GetRomFS());
    REQUIRE(romfs != nullptr);
    CHECK(ReadText(romfs->GetFile("dlc.bin")) == "dlc");
}

TEST_CASE("NX: a native base supports external updates and version disabling",
          "[core][nx][external]") {
    for (const auto archive : {"native_external_base.nx", "native_embedded_update.nx"}) {
        DYNAMIC_SECTION(archive) {
            ScopedSystemTestContext environment;
            Core::System system;
            system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
            FileSys::ManualContentProvider provider;
            system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                           &provider);
            Loader::AppLoader_NX loader{system, OpenFixture(archive)};
            u64 program_id{};
            REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
            REQUIRE(program_id == ApplicationId);
            loader.RegisterContent(provider);
            const auto base = provider.GetEntry(ApplicationId, FileSys::ContentRecordType::Program);
            REQUIRE(base != nullptr);
            REQUIRE(base->GetStatus() == Loader::ResultStatus::Success);
            FileSys::RomFSFactory factory{loader, system.GetContentProvider(),
                                          system.GetFileSystemController()};
            const auto data = [&] {
                const auto romfs = factory.OpenCurrentProcess(ApplicationId);
                REQUIRE(romfs != nullptr);
                const auto tree = FileSys::ExtractRomFS(romfs);
                REQUIRE(tree != nullptr);
                const auto file = tree->GetFile("data.bin");
                REQUIRE(file != nullptr);
                return file->ReadAllBytes();
            };
            CHECK(data() ==
                  std::vector<u8>(
                      4096, std::string_view(archive) == "native_external_base.nx" ? 0x11 : 0x22));
            REQUIRE(provider.AddEntriesFromContainer(OpenFixture("external_update.nsp")));
            const FileSys::PatchManager patches{ApplicationId, system.GetFileSystemController(),
                                                system.GetContentProvider()};
            const auto build_byte = [&] {
                const auto exefs = patches.PatchExeFS(base->GetExeFS());
                REQUIRE(exefs != nullptr);
                const auto main = exefs->GetFile("main");
                REQUIRE(main != nullptr);
                return main->ReadByte(64);
            };
            CHECK(data() == std::vector<u8>(4096, 0x33));
            CHECK(build_byte() == 0x33);
            const auto previous = Settings::values.disabled_addons[ApplicationId];
            SCOPE_EXIT {
                Settings::values.disabled_addons[ApplicationId] = previous;
            };
            Settings::values.disabled_addons[ApplicationId] = {"Update@2"};
            CHECK(data() ==
                  std::vector<u8>(
                      4096, std::string_view(archive) == "native_external_base.nx" ? 0x11 : 0x22));
            CHECK(build_byte() ==
                  (std::string_view(archive) == "native_external_base.nx" ? 0x11 : 0x22));
        }
    }
}

TEST_CASE("NX: registering an archive preserves an external NSP add-on", "[core][nx][external]") {
    for (const auto archive : {"lichii_synthetic.nx", "native_external_base.nx"}) {
        DYNAMIC_SECTION(archive) {
            ScopedSystemTestContext environment;
            Core::System system;
            system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
            FileSys::ManualContentProvider provider;
            system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                           &provider);
            REQUIRE(provider.AddEntriesFromContainer(OpenFixture("external_dlc.nsp")));
            Loader::AppLoader_NX loader{system, OpenFixture(archive)};
            loader.RegisterContent(provider);
            const auto entries = provider.ListEntriesFilter(
                FileSys::TitleType::AOC, FileSys::ContentRecordType::Data, std::nullopt);
            REQUIRE(entries.size() == (std::string_view(archive) == "lichii_synthetic.nx" ? 2 : 1));
            const auto dlc =
                provider.GetEntry(ApplicationId + 0x1002, FileSys::ContentRecordType::Data);
            REQUIRE(dlc != nullptr);
            REQUIRE(dlc->GetStatus() == Loader::ResultStatus::Success);
            const auto tree = FileSys::ExtractRomFS(dlc->GetRomFS());
            REQUIRE(tree != nullptr);
            const auto file = tree->GetFile("dlc.bin");
            REQUIRE(file != nullptr);
            CHECK(file->ReadAllBytes() == std::vector<u8>(8192, 0x44));
        }
    }
}

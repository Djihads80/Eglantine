// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/loader/nup2.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nup2_container.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"

namespace Loader {

namespace {

using FileSys::Nup2Container;

// NACP stores 16 uncompressed language entries; map BCP-47 tags onto them.
struct LanguageSlot {
    std::string_view tag;
    std::size_t index;
};

constexpr std::array<LanguageSlot, 18> LanguageSlots{{
    {"en-us", 0},    {"en-gb", 1},  {"ja", 2},      {"fr", 3},      {"de", 4},
    {"es-419", 5},   {"es", 6},     {"it", 7},      {"nl", 8},      {"fr-ca", 9},
    {"pt", 10},      {"ru", 11},    {"ko", 12},     {"zh-hant", 13}, {"zh-hans", 14},
    {"pt-br", 15},   {"pl", 16},    {"th", 17},
}};

void CopyNameToEntry(FileSys::LanguageEntry& entry, std::string_view name) {
    const std::size_t length = std::min(name.size(), entry.application_name.size() - 1);
    std::memcpy(entry.application_name.data(), name.data(), length);
    entry.application_name[length] = '\0';
}

void CopyDeveloperToEntry(FileSys::LanguageEntry& entry, std::string_view developer) {
    const std::size_t length = std::min(developer.size(), entry.developer_name.size() - 1);
    std::memcpy(entry.developer_name.data(), developer.data(), length);
    entry.developer_name[length] = '\0';
}

/**
 * Builds a NACP from the base package's TLVs. Per SPEC §3's display
 * precedence, an exact-language localized name wins over the default one
 * (issue #660): the default only fills languages nothing localized covered.
 */
std::unique_ptr<FileSys::NACP> BuildNacp(const Nup2Container::PackageInfo& base,
                                         const std::string& version) {
    const std::string& effective_title =
        !base.title_name.empty() ? base.title_name : base.source_dump;
    if (effective_title.empty() && base.localized_titles.empty() && base.publisher.empty() &&
        version.empty()) {
        return nullptr;
    }
    auto nacp = std::make_unique<FileSys::NACP>();
    nacp->raw.supported_languages = static_cast<FileSys::SupportedLanguage>(0x3FFFF);

    for (const auto& [lang, name] : base.localized_titles) {
        for (const auto& slot : LanguageSlots) {
            if (slot.index < 16 && slot.tag == lang) {
                CopyNameToEntry(nacp->raw.language_entries.language_entries[slot.index], name);
            }
        }
    }
    auto& default_entry = nacp->raw.language_entries.language_entries[0];
    if (!effective_title.empty() &&
        std::string_view(default_entry.application_name.data()).empty()) {
        CopyNameToEntry(default_entry, effective_title);
    }
    if (!base.publisher.empty() &&
        std::string_view(default_entry.developer_name.data()).empty()) {
        CopyDeveloperToEntry(default_entry, base.publisher);
    }
    if (!version.empty()) {
        const std::size_t length = std::min(version.size(), nacp->raw.version_string.size() - 1);
        std::memcpy(nacp->raw.version_string.data(), version.data(), length);
        nacp->raw.version_string[length] = '\0';
    }
    nacp->raw.save_data_owner_id = base.title_id;

    nacp->language_entries.resize(16);
    std::memcpy(nacp->language_entries.data(), nacp->raw.language_entries.language_entries.data(),
                sizeof(nacp->raw.language_entries.language_entries));
    return nacp;
}

std::string DecodeVersion(u32 version) {
    if (version == 0) {
        return {};
    }
    return fmt::format("{}.{}.{}", version >> 16, (version >> 8) & 0xFF, version & 0xFF);
}

} // namespace

AppLoader_NUP2::AppLoader_NUP2(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)), nup(std::make_unique<Nup2Container>(file)) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return;
    }

    const auto base_index = nup->FindBasePackage();
    const auto update_index = nup->FindBestUpdatePackage();

    // The program comes from the base package; a lone update stored as a base
    // (SPEC §5.2) carries a complete patched tree and boots on its own.
    std::optional<std::size_t> program_index;
    std::optional<std::size_t> exefs_index;
    if (base_index && nup->GetExeFSDir(*base_index) != nullptr) {
        program_index = base_index;
        exefs_index = update_index && nup->GetExeFSDir(*update_index) != nullptr
                          ? update_index
                          : base_index;
    } else if (update_index && nup->GetExeFSDir(*update_index) != nullptr) {
        LOG_INFO(Loader,
                 "NUP2 container is a standalone update; booting the update program directly");
        program_index = update_index;
        exefs_index = update_index;
    }
    if (!program_index || !exefs_index) {
        LOG_ERROR(Loader, "NUP2 container has no package with a usable ExeFS");
        return;
    }

    const auto& program = nup->GetPackages()[*program_index];
    base_title_id = program.title_id;
    has_program = true;

    const auto exefs = nup->GetExeFSDir(*exefs_index);
    if (exefs == nullptr || exefs->GetFile("main") == nullptr ||
        exefs->GetFile("main.npdm") == nullptr) {
        LOG_ERROR(Loader, "NUP2 package has no usable ExeFS");
        return;
    }
    secondary_loader = std::make_unique<AppLoader_DeconstructedRomDirectory>(exefs, true);

    std::optional<std::size_t> display_index;
    if (update_index) {
        display_index = update_index;
    } else if (base_index) {
        display_index = base_index;
    } else {
        display_index = program_index;
    }
    const auto& display = nup->GetPackages()[*display_index];
    display_version = display.display_version.empty() ? DecodeVersion(display.version)
                                                      : display.display_version;

    // Identity TLVs live on the base record; in an update-only container they
    // sit on the update record instead (SPEC §3).
    const auto& identity_package =
        base_index ? nup->GetPackages()[*base_index] : nup->GetPackages()[*program_index];
    nacp_file = BuildNacp(identity_package, display_version);
    if (!identity_package.icon.empty()) {
        icon_file = std::make_shared<FileSys::VectorVfsFile>(identity_package.icon, "icon.dat");
    } else if (!identity_package.localized_icons.empty()) {
        for (const auto& [lang, icon] : identity_package.localized_icons) {
            if (lang == "en-us") {
                icon_file = std::make_shared<FileSys::VectorVfsFile>(icon, "icon_" + lang + ".dat");
            }
        }
        if (icon_file == nullptr) {
            icon_file = std::make_shared<FileSys::VectorVfsFile>(
                identity_package.localized_icons.front().second, "icon.dat");
        }
    }

    const auto addon_count = std::count_if(nup->GetPackages().begin(), nup->GetPackages().end(),
                                           [](const auto& package) {
                                               return package.kind ==
                                                      Nup2Container::PackageKind::Addon;
                                           });
    if (addon_count > 0) {
        LOG_WARNING(Loader,
                    "NUP2 container has {} add-on scope(s); AoC mounting is not supported yet",
                    addon_count);
    }
}

AppLoader_NUP2::~AppLoader_NUP2() = default;

FileType AppLoader_NUP2::IdentifyType(const FileSys::VirtualFile& nup_file) {
    return FileSys::Nup2Container::IsNup2File(nup_file) ? FileType::NUP : FileType::Error;
}

AppLoader::LoadResult AppLoader_NUP2::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }
    if (nup->GetStatus() != ResultStatus::Success) {
        return {nup->GetStatus(), {}};
    }
    if (!has_program || secondary_loader == nullptr) {
        return {ResultStatus::ErrorNUPNoBasePackage, {}};
    }

    const auto result = secondary_loader->Load(process, system);
    if (result.first != ResultStatus::Success) {
        return result;
    }

    system.GetFileSystemController().RegisterProcess(
        process.GetProcessId(), base_title_id,
        std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                system.GetFileSystemController()));

    is_loaded = true;
    return result;
}

ResultStatus AppLoader_NUP2::VerifyIntegrity(
    std::function<bool(size_t, size_t)> progress_callback) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return nup->GetStatus();
    }
    return nup->VerifyIntegrity(std::move(progress_callback));
}

ResultStatus AppLoader_NUP2::ReadRomFS(FileSys::VirtualFile& out_file) {
    const auto base_index = nup->FindBasePackage();
    const auto base_romfs = base_index ? nup->GetRomFSDir(*base_index) : FileSys::VirtualDir{};
    const auto update_index = nup->FindBestUpdatePackage();
    const auto update_romfs =
        update_index ? nup->GetRomFSDir(*update_index) : FileSys::VirtualDir{};
    // A §5.2 container carries the complete merged tree as its base scope; a
    // §5.1 pair overlays the update onto the base.
    const auto merged = FileSys::NupContainer::MergeShadowDirs(base_romfs, update_romfs);
    if (merged == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    out_file = FileSys::CreateRomFS(merged);
    if (out_file == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    return ResultStatus::Success;
}

bool AppLoader_NUP2::IsRomFSUpdatable() const {
    // The update is already merged into the exposed RomFS.
    return false;
}

ResultStatus AppLoader_NUP2::ReadProgramId(u64& out_program_id) {
    if (!has_program) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_id = base_title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP2::ReadProgramIds(std::vector<u64>& out_program_ids) {
    if (!has_program) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_ids.push_back(base_title_id);
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP2::ReadIcon(std::vector<u8>& buffer) {
    if (icon_file == nullptr) {
        return ResultStatus::ErrorNoIcon;
    }
    buffer = icon_file->ReadAllBytes();
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP2::ReadTitle(std::string& title) {
    if (nacp_file == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    title = nacp_file->GetApplicationName();
    if (title.empty()) {
        return ResultStatus::ErrorNoControl;
    }
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP2::ReadControlData(FileSys::NACP& control) {
    if (nacp_file == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    control = *nacp_file;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP2::ReadNSOModules(Modules& modules) {
    if (secondary_loader == nullptr) {
        return ResultStatus::ErrorNotInitialized;
    }
    return secondary_loader->ReadNSOModules(modules);
}

} // namespace Loader

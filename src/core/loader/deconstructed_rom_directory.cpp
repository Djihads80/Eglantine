// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <string_view>
#include "common/logging.h"
#include "common/random.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nso.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

#ifndef HAS_NCE
namespace Core::NCE {
class Patcher {};
} // namespace Core::NCE
#endif

namespace Loader {

struct PatchCollection {
    explicit PatchCollection(bool is_application_) : is_application{is_application_} {
        module_patcher_indices.fill(-1);
        patchers.emplace_back();
    }

    std::vector<Core::NCE::Patcher>* GetPatchers() {
        if (is_application && Settings::IsNceEnabled()) {
            return &patchers;
        }
        return nullptr;
    }

    size_t GetTotalPatchSize() const {
        size_t total_size{};
#ifdef HAS_NCE
        for (auto& patcher : patchers) {
            total_size += patcher.GetSectionSize();
        }
#endif
        return total_size;
    }

    void SaveIndex(size_t module) {
        module_patcher_indices[module] = static_cast<s32>(patchers.size() - 1);
    }

    s32 GetIndex(size_t module) const {
        return module_patcher_indices[module];
    }

    s32 GetLastIndex() const {
        return static_cast<s32>(patchers.size()) - 1;
    }

    bool is_application;
    std::vector<Core::NCE::Patcher> patchers;
    std::array<s32, 13> module_patcher_indices{};
};

namespace {

// RomFS for a loose ExeFS: the first .romfs file next to main, else a romfs/ directory next to
// the exefs/ directory (the usual layout of an extracted title).
FileSys::VirtualFile FindLooseRomFS(const FileSys::VirtualDir& exefs_dir) {
    if (exefs_dir == nullptr) {
        return nullptr;
    }
    for (const auto& f : exefs_dir->GetFiles()) {
        if (f->GetExtension() == "romfs") {
            return f;
        }
    }
    if (const auto parent = exefs_dir->GetParentDirectory()) {
        if (const auto romfs_dir = parent->GetSubdirectory("romfs")) {
            LOG_INFO(Loader, "Using RomFS directory next to the ExeFS");
            return FileSys::CreateRomFS(romfs_dir);
        }
    }
    return nullptr;
}

// The display version of a dump without control data, from its RomFS: Nintendo EPD titles name
// their resource size tables after it (System/Resource/ResourceSizeTable.Product.180.* in Mario
// Kart World 1.8.0). A hotfix can keep its minor version's tables (Product.110 in a 1.1.1).
std::string RomFSProductVersion(const FileSys::VirtualDir& romfs_dir) {
    if (romfs_dir == nullptr) {
        return {};
    }
    const auto system_dir = romfs_dir->GetSubdirectory("System");
    const auto resource_dir = system_dir != nullptr ? system_dir->GetSubdirectory("Resource") : nullptr;
    if (resource_dir == nullptr) {
        return {};
    }
    constexpr std::string_view Prefix = "ResourceSizeTable.Product.";
    std::string best;
    for (const auto& file : resource_dir->GetFiles()) {
        const std::string file_name = file->GetName();
        if (!file_name.starts_with(Prefix) || file_name.size() < Prefix.size() + 4) {
            continue;
        }
        const std::string_view digits = std::string_view{file_name}.substr(Prefix.size(), 3);
        if (file_name[Prefix.size() + 3] != '.' ||
            !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        if (std::string version{digits}; version > best) {
            best = version;
        }
    }
    if (best.empty()) {
        return {};
    }
    return fmt::format("{}.{}.{}", best[0], best[1], best[2]);
}

// Loose ExeFS dumps carry no control data, but icon/nacp sidecars can be dropped next to the
// ExeFS or in the game folder above it (<game>/exefs/main and
// <game>/updates/<title id>/exefs/main layouts).
std::vector<FileSys::VirtualDir> FindSidecarDirs(const FileSys::VirtualDir& exefs_dir) {
    std::vector<FileSys::VirtualDir> dirs;
    for (auto dir = exefs_dir; dir != nullptr;) {
        dirs.push_back(dir);
        if (const auto control_dir = dir->GetSubdirectory("control")) {
            dirs.push_back(control_dir);
        }
        const auto parent = dir->GetParentDirectory();
        if (parent == nullptr) {
            break;
        }
        const auto& name = dir->GetName();
        const bool layout_directory = name == "exefs" || name == "base" || name == "updates";
        const bool update_version = parent->GetName() == "updates";
        if (!layout_directory && !update_version) {
            break;
        }
        dir = parent;
    }
    return dirs;
}

} // namespace

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(FileSys::VirtualFile file_, bool override_update_)
    : AppLoader(std::move(file_))
    , override_update(override_update_)
{
    const auto file_dir = file->GetContainingDirectory();
    const auto sidecar_dirs = FindSidecarDirs(file_dir);

    // Title ID
    const auto npdm = file_dir->GetFile("main.npdm");
    if (npdm != nullptr) {
        const auto res = metadata.Load(npdm);
        if (res == ResultStatus::Success)
            title_id = metadata.GetTitleID();
    }

    // Icon
    FileSys::VirtualFile icon_file = nullptr;
    for (const auto& sidecar_dir : sidecar_dirs) {
        for (const auto& language : FileSys::LANGUAGE_NAMES) {
            icon_file = sidecar_dir->GetFile("icon_" + std::string(language) + ".dat");
            if (icon_file != nullptr) {
                break;
            }
        }
        if (icon_file != nullptr) {
            icon_data = icon_file->ReadAllBytes();
            break;
        }
    }

    if (icon_data.empty()) {
        // Any png, jpeg, or bmp file
        for (const auto& sidecar_dir : sidecar_dirs) {
            const auto& files = sidecar_dir->GetFiles();
            const auto icon_iter =
                std::find_if(files.begin(), files.end(), [](const FileSys::VirtualFile& f) {
                    return f->GetExtension() == "png" || f->GetExtension() == "jpg" ||
                           f->GetExtension() == "bmp" || f->GetExtension() == "jpeg";
                });
            if (icon_iter != files.end()) {
                icon_data = (*icon_iter)->ReadAllBytes();
                break;
            }
        }
    }

    // Metadata
    FileSys::VirtualFile nacp_file = nullptr;
    for (const auto& sidecar_dir : sidecar_dirs) {
        nacp_file = sidecar_dir->GetFile("control.nacp");
        if (nacp_file != nullptr) {
            break;
        }
        const auto& files = sidecar_dir->GetFiles();
        const auto nacp_iter =
            std::find_if(files.begin(), files.end(),
                         [](const FileSys::VirtualFile& f) { return f->GetExtension() == "nacp"; });
        if (nacp_iter != files.end()) {
            nacp_file = *nacp_iter;
            break;
        }
    }

    if (nacp_file != nullptr) {
        control.emplace(nacp_file);
        name = control->GetApplicationName();
    }

    if (name.empty()) {
        for (const auto& sidecar_dir : sidecar_dirs) {
            const auto title = sidecar_dir->GetFile("title.txt");
            if (title == nullptr || title->GetSize() > 4096) {
                continue;
            }
            const auto bytes = title->ReadAllBytes();
            name.assign(bytes.begin(), bytes.end());
            const auto end = name.find_first_of("\r\n\0", 0, 3);
            if (end != std::string::npos) {
                name.resize(end);
            }
            if (!name.empty()) {
                break;
            }
        }
    }

    romfs = FindLooseRomFS(file_dir);
    if (const auto title_dir = file_dir->GetParentDirectory()) {
        romfs_version = RomFSProductVersion(title_dir->GetSubdirectory("romfs"));
    }
}

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(
    FileSys::VirtualDir directory, bool override_update_)
    : AppLoader(directory->GetFile("main"))
    , dir(std::move(directory))
    , override_update(override_update_)
{
    romfs = FindLooseRomFS(dir);
}

FileType AppLoader_DeconstructedRomDirectory::IdentifyType(const FileSys::VirtualFile& dir_file) {
    if (FileSys::IsDirectoryExeFS(dir_file->GetContainingDirectory())) {
        return FileType::DeconstructedRomDirectory;
    }

    return FileType::Error;
}

AppLoader_DeconstructedRomDirectory::LoadResult AppLoader_DeconstructedRomDirectory::Load(
    Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    // Loaded directly from a loose ExeFS (not wrapped by the NCA/NSP loaders, which register
    // the process with the filesystem controller themselves).
    const bool standalone = dir == nullptr;
    if (dir == nullptr) {
        if (file == nullptr) {
            return {ResultStatus::ErrorNullFile, {}};
        }

        dir = file->GetContainingDirectory();
    }

    // Read meta to determine title ID
    FileSys::VirtualFile npdm = dir->GetFile("main.npdm");
    if (npdm == nullptr) {
        return {ResultStatus::ErrorMissingNPDM, {}};
    }

    const ResultStatus result = metadata.Load(npdm);
    if (result != ResultStatus::Success) {
        return {result, {}};
    }

    if (override_update) {
        const FileSys::PatchManager patch_manager(
            metadata.GetTitleID(), system.GetFileSystemController(), system.GetContentProvider());
        dir = patch_manager.PatchExeFS(dir);
    }

    // Reread in case PatchExeFS affected the main.npdm
    npdm = dir->GetFile("main.npdm");
    if (npdm == nullptr) {
        return {ResultStatus::ErrorMissingNPDM, {}};
    }

    const ResultStatus result2 = metadata.Reload(npdm);
    if (result2 != ResultStatus::Success) {
        return {result2, {}};
    }
    metadata.Print();

    // Loose-ExeFS loads bypass the NCA/NSP loaders, which are the ones that normally apply
    // LayeredFS to the RomFS; apply it here so mods in user/load/<titleid>/ work too.
    if (romfs != nullptr) {
        const FileSys::PatchManager patch_manager(metadata.GetTitleID(),
                                                  system.GetFileSystemController(),
                                                  system.GetContentProvider());
        auto patched = patch_manager.PatchRomFS(nullptr, romfs);
        if (patched != nullptr) {
            romfs = std::move(patched);
            LOG_INFO(Loader, "Applied RomFS patches (LayeredFS) for title {:016X}",
                     metadata.GetTitleID());
        }
    }

    // Enable NCE only for applications with 39-bit address space.
    const bool is_39bit =
        metadata.GetAddressSpaceType() == FileSys::ProgramAddressSpaceType::Is39Bit;
    const bool is_application = metadata.GetPoolPartition() == FileSys::PoolPartition::Application;
    Settings::SetNceEnabled(is_39bit);

    const std::array static_modules = {"rtld",    "main",    "subsdk0", "subsdk1", "subsdk2",
                                       "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7",
                                       "subsdk8", "subsdk9", "sdk"};

    std::size_t code_size{};

    // Define an nce patch context for each potential module.
    PatchCollection patch_ctx{is_application};

    // Use the NSO module loader to figure out the code layout
    for (size_t i = 0; i < static_modules.size(); i++) {
        const auto& module = static_modules[i];
        const FileSys::VirtualFile module_file{dir->GetFile(module)};
        if (!module_file) {
            continue;
        }

        const bool should_pass_arguments = std::strcmp(module, "rtld") == 0;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(
            process, system, *module_file, code_size, should_pass_arguments, false, {},
            patch_ctx.GetPatchers(), patch_ctx.GetLastIndex());
        if (!tentative_next_load_addr) {
            return {ResultStatus::ErrorLoadingNSO, {}};
        }

        patch_ctx.SaveIndex(i);
        code_size = *tentative_next_load_addr;
    }

    // Enable direct memory mapping in case of NCE.
    const u64 fastmem_base = [&]() -> size_t {
        if (is_application && Settings::IsNceEnabled()) {
            auto& buffer = system.DeviceMemory().buffer;
            buffer.EnableDirectMappedAddress();
            return reinterpret_cast<u64>(buffer.VirtualBasePointer());
        }
        return 0;
    }();

    // Add patch size to the total module size
    code_size += patch_ctx.GetTotalPatchSize();

    // TODO: this is bad form of ASLR, it sucks
    std::uintptr_t aslr_offset = ((::Settings::values.rng_seed_enabled.GetValue()
        ? ::Settings::values.rng_seed.GetValue() : Common::Random::Random64(0)) << 12) & 0xfff000;

    // Setup the process code layout
    if (process.LoadFromMetadata(system.Kernel(), metadata, code_size, fastmem_base, aslr_offset).IsError()) {
        return {ResultStatus::ErrorUnableToParseKernelMetadata, {}};
    }

    // Load NSO modules
    modules.clear();
    const VAddr base_address{GetInteger(process.GetEntryPoint())};
    VAddr next_load_addr{base_address};
    const FileSys::PatchManager pm{metadata.GetTitleID(), system.GetFileSystemController(),
                                   system.GetContentProvider()};
    for (size_t i = 0; i < static_modules.size(); i++) {
        const auto& module = static_modules[i];
        const FileSys::VirtualFile module_file{dir->GetFile(module)};
        if (!module_file) {
            continue;
        }

        const VAddr load_addr{next_load_addr};
        const bool should_pass_arguments = std::strcmp(module, "rtld") == 0;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(
            process, system, *module_file, load_addr, should_pass_arguments, true, pm,
            patch_ctx.GetPatchers(), patch_ctx.GetIndex(i));
        if (!tentative_next_load_addr) {
            return {ResultStatus::ErrorLoadingNSO, {}};
        }

        next_load_addr = *tentative_next_load_addr;
        modules.insert_or_assign(load_addr, module);
        LOG_DEBUG(Loader, "loaded module {} @ {:#x}", module, load_addr);
    }

    if (standalone) {
        // Without this, fsp-srv SetCurrentProcess fails with ResultTargetNotFound.
        system.GetFileSystemController().RegisterProcess(
            process.GetProcessId(), metadata.GetTitleID(),
            std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                    system.GetFileSystemController()));
    }

    is_loaded = true;
    return {ResultStatus::Success,
            LoadParameters{metadata.GetMainThreadPriority(), metadata.GetMainThreadStackSize()}};
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadRomFS(FileSys::VirtualFile& out_dir) {
    if (romfs == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }

    out_dir = romfs;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadIcon(std::vector<u8>& out_buffer) {
    if (icon_data.empty()) {
        return ResultStatus::ErrorNoIcon;
    }

    out_buffer = icon_data;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadProgramId(u64& out_program_id) {
    out_program_id = title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadTitle(std::string& out_title) {
    if (name.empty()) {
        return ResultStatus::ErrorNoControl;
    }

    out_title = name;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadControlData(FileSys::NACP& out_control) {
    if (!control) {
        return ResultStatus::ErrorNoControl;
    }
    out_control = *control;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadDisplayVersion(std::string& version) {
    if (control && !control->GetVersionString().empty()) {
        version = control->GetVersionString();
        return ResultStatus::Success;
    }
    if (!romfs_version.empty()) {
        version = romfs_version;
        return ResultStatus::Success;
    }
    return ResultStatus::ErrorNoControl;
}

bool AppLoader_DeconstructedRomDirectory::IsRomFSUpdatable() const {
    return false;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadNSOModules(Modules& out_modules) {
    if (!is_loaded) {
        return ResultStatus::ErrorNotInitialized;
    }

    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader

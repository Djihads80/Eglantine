// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/loader/nup2.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_mapped.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"

namespace Loader {

namespace {

// The container is memory-mapped when it can be (EDEN_NUP2_MMAP=0 turns that off): reads are then a copy out of the
// mapping, with no lock, seek or stdio buffer. Falls back to the file the frontend opened.
FileSys::VirtualFile MapIfPossible(const FileSys::VirtualFile& file) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NUP2_MMAP");
        return value == nullptr || value[0] != '0';
    }();
    if (!enabled || file == nullptr) {
        return file;
    }
    const std::string path = file->GetFullPath();
    if (path.empty()) {
        return file;
    }
    if (auto mapped = FileSys::MappedVfsFile::Open(path); mapped != nullptr && mapped->GetSize() == file->GetSize()) {
        return mapped;
    }
    return file;
}

void CopyText(std::span<char> destination, std::string_view text) {
    const std::size_t length = std::min(text.size(), destination.size() - 1);
    std::memcpy(destination.data(), text.data(), length);
    destination[length] = '\0';
}

} // namespace

AppLoader_NUP2::AppLoader_NUP2(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)), container(MapIfPossible(file)) {
    if (!container.IsValid()) {
        return;
    }
    const auto exefs = container.GetExeFsDir();
    if (exefs->GetFile("main") == nullptr || exefs->GetFile("main.npdm") == nullptr) {
        LOG_ERROR(Loader, "NUP v2 has no usable ExeFS");
        return;
    }
    secondary_loader = std::make_unique<AppLoader_DeconstructedRomDirectory>(exefs, true);

    if (!container.GetTitleName().empty() || !container.GetDisplayVersion().empty()) {
        nacp_file = std::make_unique<FileSys::NACP>();
        nacp_file->raw.supported_languages = static_cast<FileSys::SupportedLanguage>(0x3FFFF);
        auto& entry = nacp_file->raw.language_entries.language_entries[0];
        CopyText(entry.application_name, container.GetTitleName());
        CopyText(nacp_file->raw.version_string, container.GetDisplayVersion());
        nacp_file->raw.save_data_owner_id = container.GetTitleId();
        nacp_file->language_entries.resize(16);
        std::memcpy(nacp_file->language_entries.data(), nacp_file->raw.language_entries.language_entries.data(),
                    sizeof(nacp_file->raw.language_entries.language_entries));
    }
}

AppLoader_NUP2::~AppLoader_NUP2() = default;

FileType AppLoader_NUP2::IdentifyType(const FileSys::VirtualFile& nup_file) {
    return FileSys::Nup2Container::IsNup2File(nup_file) ? FileType::NUP2 : FileType::Error;
}

AppLoader::LoadResult AppLoader_NUP2::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }
    if (!container.IsValid() || secondary_loader == nullptr) {
        return {ResultStatus::ErrorNUPBadFormat, {}};
    }
    const auto result = secondary_loader->Load(process, system);
    if (result.first != ResultStatus::Success) {
        return result;
    }
    system.GetFileSystemController().RegisterProcess(
        process.GetProcessId(), container.GetTitleId(),
        std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                system.GetFileSystemController()));
    is_loaded = true;
    return result;
}

ResultStatus AppLoader_NUP2::ReadRomFS(FileSys::VirtualFile& out_file) {
    if (!container.IsValid()) {
        return ResultStatus::ErrorNoRomFS;
    }
    out_file = container.GetRomFs();
    return out_file != nullptr ? ResultStatus::Success : ResultStatus::ErrorNoRomFS;
}

bool AppLoader_NUP2::IsRomFSUpdatable() const {
    return false;
}

ResultStatus AppLoader_NUP2::ReadProgramId(u64& out_program_id) {
    if (!container.IsValid()) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_id = container.GetTitleId();
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP2::ReadProgramIds(std::vector<u64>& out_program_ids) {
    if (!container.IsValid()) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_ids.push_back(container.GetTitleId());
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP2::ReadIcon(std::vector<u8>& buffer) {
    buffer = container.GetIcon();
    return buffer.empty() ? ResultStatus::ErrorNoIcon : ResultStatus::Success;
}

ResultStatus AppLoader_NUP2::ReadTitle(std::string& title) {
    if (container.GetTitleName().empty()) {
        return ResultStatus::ErrorNoControl;
    }
    title = container.GetTitleName();
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

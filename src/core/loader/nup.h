// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "core/file_sys/nup_container.h"
#include "core/loader/loader.h"

namespace FileSys {
class NACP;
class NupContainer;
} // namespace FileSys

namespace Loader {

/// Loads a NUP (NX Package) container: loose ExeFS/RomFS trees for a base
/// program plus its updates in one file. The newest update package is layered
/// over the base; add-on content packages are recognized but not yet mounted.
class AppLoader_NUP final : public AppLoader {
public:
    explicit AppLoader_NUP(FileSys::VirtualFile file_);
    ~AppLoader_NUP() override;

    /**
     * Identifies whether or not the given file is a NUP container.
     *
     * @param nup_file The file to identify.
     *
     * @return FileType::NUP, or FileType::Error if the file is not a NUP.
     */
    static FileType IdentifyType(const FileSys::VirtualFile& nup_file);

    FileType GetFileType() const override {
        return FileType::NUP;
    }

    LoadResult Load(Kernel::KProcess& process, Core::System& system) override;

    ResultStatus VerifyIntegrity(std::function<bool(size_t, size_t)> progress_callback) override;

    ResultStatus ReadRomFS(FileSys::VirtualFile& out_file) override;
    bool IsRomFSUpdatable() const override;
    ResultStatus ReadProgramId(u64& out_program_id) override;
    ResultStatus ReadProgramIds(std::vector<u64>& out_program_ids) override;
    ResultStatus ReadIcon(std::vector<u8>& buffer) override;
    ResultStatus ReadTitle(std::string& title) override;
    ResultStatus ReadControlData(FileSys::NACP& control) override;
    ResultStatus ReadNSOModules(Modules& modules) override;

private:
    std::unique_ptr<FileSys::NupContainer> nup;
    std::unique_ptr<AppLoader> secondary_loader;

    std::unique_ptr<FileSys::NACP> nacp_file;
    FileSys::VirtualFile icon_file;
    u64 base_title_id{};
    std::string display_version;
    bool has_program{};
};

} // namespace Loader

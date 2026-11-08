// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>

#include "core/file_sys/nup2_container.h"
#include "core/loader/loader.h"

namespace FileSys {
class NACP;
}

namespace Loader {

/// Loads a NUP v2 file (see file_sys/nup2_container.h): the ExeFS members and a RomFS that is read straight out of
/// the file at the same offsets the game uses.
class AppLoader_NUP2 final : public AppLoader {
public:
    explicit AppLoader_NUP2(FileSys::VirtualFile file_);
    ~AppLoader_NUP2() override;

    /// FileType::NUP2 when the file starts with the v2 magic, otherwise FileType::Error.
    static FileType IdentifyType(const FileSys::VirtualFile& nup_file);

    FileType GetFileType() const override {
        return FileType::NUP2;
    }

    LoadResult Load(Kernel::KProcess& process, Core::System& system) override;

    ResultStatus ReadRomFS(FileSys::VirtualFile& out_file) override;
    bool IsRomFSUpdatable() const override;
    ResultStatus ReadProgramId(u64& out_program_id) override;
    ResultStatus ReadProgramIds(std::vector<u64>& out_program_ids) override;
    ResultStatus ReadIcon(std::vector<u8>& buffer) override;
    ResultStatus ReadTitle(std::string& title) override;
    ResultStatus ReadControlData(FileSys::NACP& control) override;
    ResultStatus ReadNSOModules(Modules& modules) override;

private:
    FileSys::Nup2Container container;
    std::unique_ptr<AppLoader> secondary_loader;
    std::unique_ptr<FileSys::NACP> nacp_file;
};

} // namespace Loader

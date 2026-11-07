// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "core/file_sys/nup2_container.h"
#include "core/loader/loader.h"

namespace FileSys {
class NACP;
class Nup2Container;
} // namespace FileSys

namespace Loader {

/// Loads a NUP v2 container (SPEC.md v2): an HFS0 partition whose meta.nup
/// record carries versioning, grouping and integrity. The newest update
/// scope is layered over the base; add-on scopes are recognized but not yet
/// mounted.
class AppLoader_NUP2 final : public AppLoader {
public:
    explicit AppLoader_NUP2(FileSys::VirtualFile file_);
    ~AppLoader_NUP2() override;

    /**
     * Identifies whether the file is a NUP v2 container: HFS0 magic with a
     * meta.nup first entry whose data starts with "NUP\x02".
     *
     * @param nup_file The file to identify.
     *
     * @return FileType::NUP, or FileType::Error if the file is not a NUP v2.
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
    std::unique_ptr<FileSys::Nup2Container> nup;
    std::unique_ptr<AppLoader> secondary_loader;

    std::unique_ptr<FileSys::NACP> nacp_file;
    FileSys::VirtualFile icon_file;
    u64 base_title_id{};
    bool has_program{};
    std::string display_version;
};

} // namespace Loader

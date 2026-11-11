// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/loader/loader.h"

namespace FileSys {
class ManualContentProvider;
class NACP;
} // namespace FileSys

namespace Loader {

/// Loads an NX archive (.nx). The newest enabled update of the requested program is started:
/// an extracted-tree snapshot (exefs/ and romfs/, as converted from NUP or a dump folder) or
/// native decrypted NCA content (as converted from an NSP or XCI).
class AppLoader_NX final : public AppLoader {
public:
    AppLoader_NX(Core::System& system, FileSys::VirtualFile file_, u64 program_id = 0,
                 std::size_t program_index = 0);
    ~AppLoader_NX() override;

    /// FileType::NX for an archive that starts with the NX magic, FileType::Error otherwise.
    static FileType IdentifyType(const FileSys::VirtualFile& nx_file);
    /// True when the archive holds a program with a base or a complete update to start.
    static bool IsBootable(const FileSys::VirtualFile& nx_file);
    /// Platform of the application holding `program_id` (0: the first): 1 Switch 1, 2 Switch 2.
    static u8 ReadPlatform(const FileSys::VirtualFile& nx_file, u64 program_id);

    FileType GetFileType() const override {
        return FileType::NX;
    }

    LoadResult Load(Kernel::KProcess& process, Core::System& system) override;
    ResultStatus VerifyIntegrity(std::function<bool(size_t, size_t)> progress_callback) override;

    ResultStatus ReadRomFS(FileSys::VirtualFile& out_file) override;
    ResultStatus ReadUpdateRaw(FileSys::VirtualFile& out_file) override;
    bool IsRomFSUpdatable() const override;
    ResultStatus ReadProgramId(u64& out_program_id) override;
    ResultStatus ReadProgramIds(std::vector<u64>& out_program_ids) override;
    ResultStatus ReadIcon(std::vector<u8>& buffer) override;
    ResultStatus ReadOriginalIcon(u64 application_id, std::vector<u8>& buffer) const;
    ResultStatus ReadTitle(std::string& title) override;
    ResultStatus ReadPublisher(std::string& publisher) const;
    ResultStatus ReadControlData(FileSys::NACP& control) override;
    ResultStatus ReadDisplayVersion(std::string& version) override;
    ResultStatus ReadNSOModules(Modules& modules) override;

    /// The RomFS the program sees, without mods: a tree snapshot as stored, or the native
    /// base RomFS with its newest enabled update applied. Used to dump the RomFS.
    ResultStatus ReadPatchedRomFS(FileSys::VirtualFile& out_file);

    /// Adds the base, update and add-on NCAs of a native archive to a content provider, so the
    /// patch manager finds them as it does for a base NSP with separate update files.
    void RegisterContent(FileSys::ManualContentProvider& provider) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Loader

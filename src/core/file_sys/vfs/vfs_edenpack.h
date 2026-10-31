// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

// Read-only, indexed view of an EdenPack archive. The archive VirtualFile is retained by every
// entry, so SAF-backed files keep using the one descriptor opened by the Android VFS.
class EdenPackVfsFilesystem final : public VfsFilesystem {
public:
    explicit EdenPackVfsFilesystem(VirtualFile archive);

    static VirtualDir Open(VirtualFile archive);
    std::string GetName() const override;
    bool IsWritable() const override;
    bool IsReadable() const override;

private:
    VirtualFile archive;
};

} // namespace FileSys

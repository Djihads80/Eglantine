// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {

class HostFactory {
public:
    explicit HostFactory(VirtualDir host_dir_);
    ~HostFactory();

    VirtualDir Open() const;

private:
    VirtualDir host_dir;
};

} // namespace FileSys

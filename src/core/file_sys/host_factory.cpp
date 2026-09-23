// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/host_factory.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

HostFactory::HostFactory(VirtualDir host_dir_)
    : host_dir(std::move(host_dir_)) {}

HostFactory::~HostFactory() = default;

VirtualDir HostFactory::Open() const {
    return host_dir;
}

} // namespace FileSys

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys::SystemArchive {

VirtualDir MiiModel();

/// Switch 2's Mii resource system data, built from the Switch 1 Mii model archive's RomFS (the
/// stand-in above when that is null).
VirtualFile MiiResource(VirtualFile mii_model_romfs);

} // namespace FileSys::SystemArchive

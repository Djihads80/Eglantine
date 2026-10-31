// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {

// .nx2: a loose Switch 2 dump (ExeFS + RomFS) packed into one file. It is an extracted-type NSP,
// a PFS0 holding the ExeFS files directly, with the RomFS as a single image and any
// control.nacp/icon sidecars next to them, so the NSP loader boots it like the loose dump.
constexpr std::string_view Nx2Extension = "nx2";
constexpr std::string_view Nx2RomFSName = "game.romfs";

// Called with the bytes written so far and the total; returning false cancels.
using Nx2PackProgress = std::function<bool(u64 done, u64 total)>;

// Packs the dump in dump_dir (the folder holding exefs/ and romfs/, or the ExeFS folder itself)
// into out_path. Returns an error message, empty on success. A failed or cancelled pack leaves
// no output file.
std::string PackNx2(const VirtualDir& dump_dir, const std::string& out_path,
                    const Nx2PackProgress& progress);

} // namespace FileSys

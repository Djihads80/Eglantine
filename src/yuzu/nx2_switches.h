// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

#include "common/fs/path_util.h"

namespace Nx2Switches {

/// The user's nx2_switches.txt, read at start and written by the Performance tab. Follows
/// portable mode: <exe>\user on Windows, ./user/config elsewhere; otherwise %APPDATA%\eglantine or
/// $XDG_CONFIG_HOME/eglantine.
inline std::filesystem::path UserFilePath() {
#ifdef _WIN32
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "nx2_switches.txt";
#else
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "nx2_switches.txt";
#endif
}

} // namespace Nx2Switches

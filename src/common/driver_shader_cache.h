// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace Common {

/// Linux, NVIDIA's proprietary driver: keeps the driver's shader cache for this emulator in a
/// folder of its own, with its own size limit. The default cache is one folder shared by every
/// application, with one size limit (1 GB on many systems) that a game's pipelines alone can
/// exceed, and an application that sets another size wipes it for the others. Leaves the
/// environment alone when the user set the cache path or turned the cache off, and keeps a size
/// the user set. Must run before anything loads a GPU driver; other platforms are unchanged.
/// EDEN_NX2_PRIVATE_DRIVER_CACHE=0 turns it off.
void ConfigureDriverShaderCache();

/// What ConfigureDriverShaderCache did, for the log (which isn't running yet when it is called).
[[nodiscard]] std::string DriverShaderCacheDescription();

} // namespace Common

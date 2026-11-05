// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include "video_core/textures/workers.h"

namespace Tegra::Texture {

Common::ThreadWorker& GetThreadWorkers() {
    // A quarter of the hardware threads (at least 2). Half took ~3.5 cores from the emulated
    // CPU cores and the GPU thread on an 8-core laptop when textures were thrashing; with an
    // eighth (2) both threads stayed saturated and streamed textures arrived seconds late (shown
    // as placeholders), while 4 cost no frame rate. EDEN_NX2_TRANSCODE_THREADS=<n> overrides.
    static Common::ThreadWorker workers{[] {
        if (const char* text = std::getenv("EDEN_NX2_TRANSCODE_THREADS")) {
            return (std::max)(static_cast<u32>(std::strtoul(text, nullptr, 10)), 1U);
        }
        return (std::max)(std::thread::hardware_concurrency() / 4, 2U);
    }(), "ImageTranscode", {}, Common::ThreadPlacement::Efficiency};
    return workers;
}

} // namespace Tegra::Texture

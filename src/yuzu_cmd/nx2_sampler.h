// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Nx2Sampler {

// EDEN_NX2_SAMPLE=<thread names>: a sampling profiler for named host threads (Windows only).
// Names are comma separated; a name ending in '*' matches by prefix (CPUCore_*). From
// EDEN_NX2_SAMPLE_START seconds (default 60) for EDEN_NX2_SAMPLE_SECONDS (default 30), each
// thread is paused about 1000 times a second and only its current instruction pointer is
// recorded. Self counts (no stack walking) go to EDEN_NX2_SAMPLE_OUT (default nx2_samples.txt).
// Loaded-image addresses are module+RVA; use the matching linker map to resolve them.
// The sampler never follows stack memory or dynamic JIT unwind metadata.
void StartFromEnvironment();

} // namespace Nx2Sampler

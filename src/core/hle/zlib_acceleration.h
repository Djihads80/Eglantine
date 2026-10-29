// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Kernel {
class KProcess;
}

namespace Core::HLE::Zlib {

// Private SVC immediate patched over a recognised guest inflate_fast entry point.
constexpr u32 InflateFastSvc = 0x7A1B;

// Scans [base, base + size) for a known guest zlib inflate_fast and redirects it to the host.
void Install(Kernel::KProcess& process, u64 base, u64 size);

// Runs the host inflate_fast for a patched call. Returns false if imm is not ours.
bool HandleSvc(Core::System& system, u32 imm, std::span<u64, 8> args);

} // namespace Core::HLE::Zlib

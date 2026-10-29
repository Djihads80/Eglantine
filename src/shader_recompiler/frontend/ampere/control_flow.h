// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/common_types.h"

namespace Shader::Ampere {
// Internal BRA flag: retire the current convergence token on the taken edge.
// This reserved bit is outside Maxwell's relative branch displacement.
inline constexpr u64 RetireBranchFlag = u64{1} << 45;
inline constexpr u64 WarpSync = 0xef98000100000000ULL;
inline constexpr u64 PredicateMask = u64{15} << 16;
}

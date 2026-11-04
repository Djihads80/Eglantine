// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace Service::NS {

/// Returns a copy of the TrueType font `base` with the glyphs for `codepoints` that `donor` has and
/// `base` lacks added to it (outline, advance, and character map entry).
///
/// Both fonts must be TrueType (`glyf`) with the same units per em, so the glyphs need no scaling,
/// and the donor's glyphs must be simple (not composite). The result keeps every original glyph
/// and table; the glyph names of the `post` table are dropped (it becomes version 3.0).
///
/// Returns an empty vector when a font cannot be read, the units per em differ, a table is
/// missing, or nothing needed adding, so callers can fall back to `base` unchanged.
std::vector<std::uint8_t> MergeGlyphs(std::span<const std::uint8_t> base,
                                      std::span<const std::uint8_t> donor,
                                      std::span<const std::uint32_t> codepoints);

} // namespace Service::NS

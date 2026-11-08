// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "core/file_sys/vfs/vfs_types.h"

namespace Loader {

enum class NSOHashStatus {
    /// The segment's hash matches its decompressed contents.
    Match,
    /// The segment's contents do not match the header's hash.
    Mismatch,
    /// The segment could not be read or decompressed, so it could not be checked.
    Unreadable,
};

struct NSOSegmentHashResult {
    NSOHashStatus status{NSOHashStatus::Unreadable};
    /// The header asks loaders to check this segment (flag bits 3-5). A hash that exists but is not flagged is still
    /// reported in `status`, so a tool can use it.
    bool flagged{};
};

struct NSOHashReport {
    /// False when the file is not an NSO or its header is inconsistent; the segments are then not checked.
    bool header_valid{};
    std::array<NSOSegmentHashResult, 3> segments{}; // Text, RoData, Data

    /// True when every segment matches its hash.
    bool AllMatch() const;
    /// True when no segment the header flagged for checking fails to match.
    bool FlaggedMatch() const;
};

/// Decompresses each segment of an NSO and compares its SHA-256 with the hash in the header. This catches corrupt
/// or edited dumps; it is not an authenticity check, because whoever edits a file can recompute the hashes.
/// It needs no running system and does not modify anything.
NSOHashReport VerifyNSOSegmentHashes(const FileSys::VfsFile& nso_file);

} // namespace Loader

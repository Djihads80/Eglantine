// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "common/common_types.h"

// Packs a loose dump into a NUP container. Every package is kept: base/, each
// updates/<version>/ and each dlc/<titleid>/<n>/.
namespace FileSys::Nup {

struct PackProgress {
    u64 bytes_done{};
    u64 bytes_total{};
    u64 files_done{};
    u64 files_total{};
};

enum class PackResult { Success, Cancelled, InvalidSource, IoError };

// NupContainer keeps a zstd file in memory only while it fits its 512 MiB decompression cache;
// a larger one is decompressed again in full on every read, which makes big compressed RomFS
// files unusable. Files above this size are therefore stored raw, with headroom below the cap so
// that one file does not evict everything else from the cache.
constexpr u64 DefaultRawThreshold = 256ULL * 1024 * 1024;

struct PackOptions {
    // Low-storage mode: delete each source file once its extent is synced and journaled.
    bool delete_sources = false;
    u64 chunk_bytes = 256ULL * 1024 * 1024;
    int zstd_level = 3;
    // Files larger than this are always stored raw.
    u64 raw_threshold = DefaultRawThreshold;
};

// A loose dump holds exefs/main and main.npdm, directly or under base/ or updates/<version>/,
// and a romfs folder. Folder names are matched case-insensitively.
bool IsLooseDump(const std::filesystem::path& dir);
// The dump folder a game list entry's exefs/main (or exefs folder) belongs to, if it is a loose
// dump.
std::optional<std::filesystem::path> DumpRootFromMain(const std::filesystem::path& main);
std::filesystem::path DefaultArchivePath(const std::filesystem::path& dump_root);

// Writes <archive>.part and renames it when done; an existing archive is never overwritten. A
// cancelled or failed low-storage run keeps its .part and .journal, and calling this again with
// the same arguments resumes it.
PackResult PackDump(const std::filesystem::path& dump_root, const std::filesystem::path& archive,
                    const PackOptions& options = {},
                    const std::function<void(const PackProgress&)>& progress = {},
                    const std::atomic_bool* cancel = nullptr, std::string* error_out = nullptr);

bool HasResumableJournal(const std::filesystem::path& archive);

} // namespace FileSys::Nup

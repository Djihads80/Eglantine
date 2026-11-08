// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys::Nup2 {

struct PackOptions {
    std::string display_version;
    std::string title_name;
    /// Zero reads the title id from main.npdm.
    u64 title_id{};
    u32 program_version{};
};

enum class PackResult {
    Success,
    ExeFsMissing,
    RomFsMissing,
    BadMetadata,
    IoError,
    Cancelled,
};

struct ExeFsSource {
    std::string name;
    VirtualFile file;
};

/// Writes a NUP v2 file from open files: the ExeFS members and a RomFS (anything readable at the game's offsets, for
/// example a RomFS built from a NUP v1 tree). Nothing is staged on disk. A cancelled or failed write removes the
/// partial output.
PackResult PackFromFiles(const std::vector<ExeFsSource>& exefs, const VirtualFile& romfs,
                         const std::filesystem::path& output, const PackOptions& options,
                         const std::function<void(u64, u64)>& progress, std::string* error,
                         const std::atomic_bool* cancel = nullptr);

/// Converts a NUP v1 file straight to v2: reads the v1 container (decompressing as it goes) and writes the v2
/// file, with no loose folder or intermediate image. Title, version and the ExeFS come from the same package the v1
/// loader would boot. The package's icon is carried over when it has one; otherwise the NUP v2 has none.
PackResult ConvertFromNup1(const VirtualFile& source, const std::filesystem::path& output,
                           const std::function<void(u64, u64)>& progress, std::string* error,
                           const std::atomic_bool* cancel = nullptr);

/// Converts a loose dump folder (the layout NUP v1 packs: exefs/ and romfs/, directly or under base/ and
/// updates/<version>/) to NUP v2. The newest update's ExeFS wins and its RomFS is layered over the base RomFS,
/// as the v1 loader does. Name, version and icon come from the dump; a dump without an icon gets none.
PackResult ConvertFromDump(const std::filesystem::path& dump_root, const std::filesystem::path& output,
                           const std::function<void(u64, u64)>& progress, std::string* error,
                           const std::atomic_bool* cancel = nullptr);

/// Hashes the RomFS inside a NUP v2 file and compares it with the SHA-256 recorded in its header. Returns false with
/// a message in `error` for a mismatch, a read failure, or a file that has no hash recorded.
bool VerifyRomFsHash(const VirtualFile& v2_file, const std::function<void(u64, u64)>& progress, std::string* error);

/// Checks that the RomFS inside a NUP v2 file is byte-identical to the one the v1 loader would build from the given
/// NUP v1 file. Returns false with a message in `error` on the first difference.
bool VerifyAgainstNup1(const VirtualFile& v2_file, const VirtualFile& v1_file,
                       const std::function<void(u64, u64)>& progress, std::string* error);

/// Writes a NUP v2 file (see nup2_container.h): the ExeFS files of `exefs_dir` and the RomFS image `romfs_image`
/// (for example from `eden-cli --dump-romfs`), stored raw. `progress` receives (bytes written, bytes total).
PackResult Pack(const std::filesystem::path& exefs_dir, const std::filesystem::path& romfs_image,
                const std::filesystem::path& output, const PackOptions& options,
                const std::function<void(u64, u64)>& progress, std::string* error);

} // namespace FileSys::Nup2

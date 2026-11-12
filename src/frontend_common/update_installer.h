// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace UpdateInstaller {

// Exclusive lock on a file, held until destruction and released by the system if the process dies.
// It keeps two running copies from staging, swapping or rolling back the same install at once.
class Lock {
public:
    explicit Lock(const std::filesystem::path& path);
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    // Whether this object holds the lock, which is false while another holds it.
    bool Held() const;

private:
    std::intptr_t handle;
};

// Where the lock for `target` lives, beside it.
std::filesystem::path LockPath(const std::filesystem::path& target);

// Where a download is staged before it replaces `target`; always beside it, so the final step is
// a rename on one filesystem.
std::filesystem::path StagedPath(const std::filesystem::path& target);

// Where the replaced file is kept for one rollback.
std::filesystem::path PreviousPath(const std::filesystem::path& target);

// Whether a file can be replaced in place: it exists and its folder accepts new files.
bool CanReplace(const std::filesystem::path& target);

// Replace `target` with `staged`, keeping the old file at PreviousPath(target). The new file takes
// the old file's permissions and is executable. On failure `target` is left as it was.
bool Swap(const std::filesystem::path& staged, const std::filesystem::path& target,
          std::error_code& ec);

// Exchange `target` and PreviousPath(target), so rolling back can itself be undone.
bool Rollback(const std::filesystem::path& target, std::error_code& ec);

// File in an install folder that lists every file the last update put there, relative to it.
inline constexpr std::string_view ManifestName = "install-manifest.txt";

// Folder in an install folder that keeps what the last update replaced.
inline constexpr std::string_view BackupName = ".update-backup";

// Install the files under `staging` over `install_dir`. Files the previous update installed that
// `staging` no longer has are removed. Everything replaced or removed is kept in
// `install_dir`/BackupName, replacing the older backup, and `staging` is deleted. `staging` may
// itself be that backup, which restores the previous version. The top-level folder `user`, which
// holds portable settings and data, is never read, written or removed. When a step fails every
// file is put back as it was.
bool Install(const std::filesystem::path& staging, const std::filesystem::path& install_dir,
             std::error_code& ec);

// Remove the entries of a ':' separated list that are `dir` or lie below it.
std::string StripPathListEntries(std::string_view list, std::string_view dir);

} // namespace UpdateInstaller

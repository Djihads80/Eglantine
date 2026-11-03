// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/common_types.h"

#include <QImage>
#include <QPixmap>
#include <QString>

namespace Core {
class System;
}

namespace NupMetadata {

struct DetectResult {
    bool success{false};
    std::vector<u8> icon_bytes;
    std::string resolved_title;
    QString error_message;
};

/// Returns true if the NUP file exists, is regular, and has write permission.
bool IsArchiveWritable(const std::filesystem::path& archive_path, QString* error_out = nullptr);

/// Returns true if the given NUP archive is currently loaded or running in emulation.
bool IsArchiveRunning(Core::System& system, const std::filesystem::path& archive_path);

/// Safely decodes, crops to square, and normalizes an image to a 256x256 PNG icon buffer.
std::vector<u8> NormalizeAndEncodeIcon(const QImage& source);

/// Resolves common game abbreviations (e.g. "mkw" -> "Mario Kart World") or title IDs.
std::string ResolveTitleAlias(const std::string& raw_name, u64 title_id);

/// Asynchronously searches for an icon using local folder images, boxart repository,
/// and Wikipedia API. Runs on a thread pool with bounded timeouts and cancellation.
void DetectIconAsync(const std::string& query_title, u64 title_id,
                     const std::filesystem::path& archive_path,
                     std::shared_ptr<std::atomic_bool> cancel_token,
                     std::function<void(DetectResult)> on_complete);

/// Safely commits title name and/or icon changes to the NUP archive with crash recovery.
bool CommitMetadataChanges(const std::filesystem::path& archive_path,
                           const std::optional<std::string>& new_title,
                           const std::optional<std::vector<u8>>& new_icon,
                           QString* error_out = nullptr);

/// Checks if a crash recovery backup (.meta.bak) exists for the given archive.
bool HasRecoveryBackup(const std::filesystem::path& archive_path);

/// Restores the NUP archive from its crash recovery backup.
bool RestoreRecoveryBackup(const std::filesystem::path& archive_path, QString* error_out = nullptr);

} // namespace NupMetadata

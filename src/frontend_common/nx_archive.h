// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace FrontendCommon::NxArchive {

/// The archive path to write: `path` with ".nx" appended unless it already ends with it.
std::string WithNxExtension(std::string path);

/// The regular files in `directory`, empty when it cannot be read. Never throws.
std::vector<std::filesystem::path> RegularFiles(const std::filesystem::path& directory);

/// Whether the metadata editor should write `edited` as the title. `shown` is what the editor
/// started from: the archive's title, or its file name when it has none.
bool IsTitleEdit(std::string_view edited, std::string_view shown);

/// The update folder meant: `chosen` when it holds exefs/, else the folder below it that does
/// when each level holds a single folder (a parent picked by mistake, as "updates" around
/// "65536"), at most three levels down. Never throws.
std::filesystem::path UpdateFolder(const std::filesystem::path& chosen);

} // namespace FrontendCommon::NxArchive

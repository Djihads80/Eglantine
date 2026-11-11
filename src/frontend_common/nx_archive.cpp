// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>

#include "frontend_common/nx_archive.h"

namespace FrontendCommon::NxArchive {

std::string WithNxExtension(std::string path) {
    constexpr std::string_view extension = ".nx";
    const bool has_extension =
        path.size() >= extension.size() &&
        std::equal(extension.begin(), extension.end(), path.end() - extension.size(),
                   [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
    if (!has_extension) {
        path += extension;
    }
    return path;
}

std::vector<std::filesystem::path> RegularFiles(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (std::error_code type_ec; it->is_regular_file(type_ec)) {
            files.push_back(it->path());
        }
    }
    return files;
}

bool IsTitleEdit(std::string_view edited, std::string_view shown) {
    return !edited.empty() && edited != shown;
}

std::filesystem::path UpdateFolder(const std::filesystem::path& chosen) {
    std::error_code ec;
    auto folder = chosen;
    for (int depth = 0; depth < 3 && !std::filesystem::is_directory(folder / "exefs", ec);
         ++depth) {
        std::optional<std::filesystem::path> only;
        for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (std::error_code type_ec; !it->is_directory(type_ec)) {
                continue;
            }
            if (only) {
                return chosen;
            }
            only = it->path();
        }
        if (ec || !only) {
            return chosen;
        }
        folder = *only;
    }
    return std::filesystem::is_directory(folder / "exefs", ec) ? folder : chosen;
}

} // namespace FrontendCommon::NxArchive

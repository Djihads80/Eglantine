// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/fs/symlink.h"
#include "migration_worker.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <QMap>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"

namespace {

// Configs store data paths as absolute strings, so a copied config would keep using the old
// emulator's folders. Point any value under old_root at new_root instead.
void RewriteConfigPaths(const std::filesystem::path& config_dir,
                        const std::filesystem::path& old_root,
                        const std::filesystem::path& new_root) {
    namespace fs = std::filesystem;

    const std::array<std::pair<std::string, std::string>, 2> roots{{
        {Common::FS::PathToUTF8String(old_root), Common::FS::PathToUTF8String(new_root)},
        {Common::FS::ToUTF8String(old_root.generic_u8string()),
         Common::FS::ToUTF8String(new_root.generic_u8string())},
    }};

    const auto rewrite_value = [&roots](std::string& value) {
        const size_t start = value.starts_with('"') ? 1 : 0;
        for (const auto& [from, to] : roots) {
            if (value.compare(start, from.size(), from) != 0) {
                continue;
            }
            const size_t end = start + from.size();
            if (end == value.size() || value[end] == '/' || value[end] == '\\' ||
                value[end] == '"' || value[end] == '\r') {
                value.replace(start, from.size(), to);
                return true;
            }
        }
        return false;
    };

    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(config_dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".ini") {
            continue;
        }

        std::ifstream in(entry.path(), std::ios::binary);
        std::stringstream out;
        bool changed = false;
        std::string line;
        while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string value = line.substr(eq + 1);
                if (rewrite_value(value)) {
                    line.replace(eq + 1, std::string::npos, value);
                    changed = true;
                }
            }
            out << line << '\n';
        }
        in.close();

        if (changed) {
            std::ofstream(entry.path(), std::ios::binary | std::ios::trunc) << out.str();
        }
    }
}

} // namespace

MigrationWorker::MigrationWorker(const Emulator selected_emu_, const bool clear_shader_cache_,
                                 const MigrationStrategy strategy_)
    : QObject(), selected_emu(selected_emu_), clear_shader_cache(clear_shader_cache_),
      strategy(strategy_) {}

void MigrationWorker::process() {
    namespace fs = std::filesystem;
    constexpr auto copy_options = fs::copy_options::update_existing | fs::copy_options::recursive;

    const fs::path legacy_user_dir = selected_emu.get_user_dir();
    const fs::path legacy_config_dir = selected_emu.get_config_dir();
    const fs::path legacy_cache_dir = selected_emu.get_cache_dir();

    // TODO(crueter): Make these constexpr since they're defaulted
    fs::path eden_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir);
    fs::path config_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
    fs::path cache_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir);
    fs::path shader_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);

    eden_dir.make_preferred();
    config_dir.make_preferred();
    cache_dir.make_preferred();
    shader_dir.make_preferred();

    try {
        fs::remove_all(eden_dir);
    } catch (fs::filesystem_error& _) {
        // directory may not exist at this point, that's fine
    }

    switch (strategy) {
    case MigrationStrategy::Link:
        // Create symlinks/directory junctions if requested

        // Windows 11 has random permission nonsense to deal with.
        try {
            Common::FS::CreateSymlink(legacy_user_dir, eden_dir);
        } catch (const fs::filesystem_error& e) {
            emit error(tr("Linking the old directory failed. You may need to re-run with "
                          "administrative privileges on Windows.\nOS gave error: %1")
                           .arg(tr(e.what())));
            std::exit(-1);
        }

        // Windows doesn't need any more links, because cache and config
        // are already children of the root directory
#ifndef WIN32
        if (fs::is_directory(legacy_config_dir)) {
            Common::FS::CreateSymlink(legacy_config_dir, config_dir);
        }

        if (fs::is_directory(legacy_cache_dir)) {
            Common::FS::CreateSymlink(legacy_cache_dir, cache_dir);
        }
#endif

        success_text.append(tr("\n\nNote that your configuration and data will be shared with %1.\n"
                               "If this is not desirable, delete the following files:\n%2\n%3\n%4")
                                .arg(selected_emu.name(), QString::fromStdString(eden_dir.string()),
                                     QString::fromStdString(config_dir.string()),
                                     QString::fromStdString(cache_dir.string())));

        break;
    case MigrationStrategy::Move:
        // Rename directories if deletion is requested (achieves the same result)
        fs::rename(legacy_user_dir, eden_dir);

        // Windows doesn't need any more renames, because cache and config
        // are already children of the root directory
#ifndef WIN32
        if (fs::is_directory(legacy_config_dir)) {
            fs::rename(legacy_config_dir, config_dir);
        }

        if (fs::is_directory(legacy_cache_dir)) {
            fs::rename(legacy_cache_dir, cache_dir);
        }
#endif
        RewriteConfigPaths(config_dir, legacy_user_dir, eden_dir);
        break;
    case MigrationStrategy::Copy:
    default:
        // Default behavior: copy
        fs::copy(legacy_user_dir, eden_dir, copy_options);

        // Windows doesn't need any more copies, because cache and config
        // are already children of the root directory
#ifndef WIN32
        if (fs::is_directory(legacy_config_dir)) {
            fs::copy(legacy_config_dir, config_dir, copy_options);
        }

        if (fs::is_directory(legacy_cache_dir)) {
            fs::copy(legacy_cache_dir, cache_dir, copy_options);
        }
#endif
        RewriteConfigPaths(config_dir, legacy_user_dir, eden_dir);

        success_text.append(tr("\n\nIf you wish to clean up the files which were left in the old "
                               "data location, you can do so by deleting the following directory:\n"
                               "%1")
                                .arg(QString::fromStdString(legacy_user_dir.string())));
        break;
    }

    // Delete and re-create shader dir
    if (clear_shader_cache) {
        fs::remove_all(shader_dir);
        fs::create_directories(shader_dir);
    }

    emit finished(success_text, legacy_user_dir.string());
}

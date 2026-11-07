// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include "common/fs/path_util.h"
#include "common/settings.h"

class ScopedSystemTestContext {
public:
    ScopedSystemTestContext() {
        if (!std::filesystem::create_directory(directory)) {
            throw std::runtime_error("Cannot create isolated System test directory");
        }
        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, directory);
        Settings::values.memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_4Gb);
        Settings::values.use_multi_core.SetValue(false);
    }

    ~ScopedSystemTestContext() {
        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, previous_nand);
        Settings::values.current_user = previous_user;
        Settings::values.memory_layout_mode.SetValue(previous_memory);
        Settings::values.use_multi_core.SetValue(previous_multicore);
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    ScopedSystemTestContext(const ScopedSystemTestContext&) = delete;
    ScopedSystemTestContext& operator=(const ScopedSystemTestContext&) = delete;

private:
    const std::filesystem::path previous_nand =
        Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    const s32 previous_user = Settings::values.current_user.GetValue();
    const Settings::MemoryLayout previous_memory = Settings::values.memory_layout_mode.GetValue();
    const bool previous_multicore = Settings::values.use_multi_core.GetValue();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("hwacod-system-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
};

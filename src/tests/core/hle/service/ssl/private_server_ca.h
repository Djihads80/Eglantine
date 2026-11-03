// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <system_error>

#include "common/common_types.h"
#include "common/fs/fs_util.h"
#include "common/settings.h"

/// A file of the temporary directory holding contents, removed with this
class TemporaryFile {
public:
    explicit TemporaryFile(std::span<const u8> contents)
        : path{std::filesystem::temp_directory_path() /
               ("eden_private_server_ca_" + std::to_string(std::random_device{}()) + ".der")} {
        std::ofstream{path, std::ios::binary}.write(reinterpret_cast<const char*>(contents.data()),
                                                     static_cast<std::streamsize>(contents.size()));
    }

    ~TemporaryFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    /// The path, as the private_server_ca setting holds it
    std::string Path() const {
        return Common::FS::PathToUTF8String(path);
    }

private:
    std::filesystem::path path;
};

/// Sets private_server_enabled and private_server_ca, which are restored with this
class PrivateServerCaSettings {
public:
    PrivateServerCaSettings(bool enabled, const std::string& ca)
        : enabled_before{Settings::values.private_server_enabled.GetValue()},
          ca_before{Settings::values.private_server_ca.GetValue()} {
        Settings::values.private_server_enabled.SetValue(enabled);
        Settings::values.private_server_ca.SetValue(ca);
    }

    ~PrivateServerCaSettings() {
        Settings::values.private_server_enabled.SetValue(enabled_before);
        Settings::values.private_server_ca.SetValue(ca_before);
    }

    PrivateServerCaSettings(const PrivateServerCaSettings&) = delete;
    PrivateServerCaSettings& operator=(const PrivateServerCaSettings&) = delete;

private:
    bool enabled_before;
    std::string ca_before;
};

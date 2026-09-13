// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/launch_timestamp_cache.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include "common/container/unordered_map.h"

#include <glaze/glaze.hpp>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"

namespace Core::LaunchTimestampCache {
namespace {

struct CacheEntry {
    std::optional<s64> timestamp{};
    std::optional<u64> launch_count{};
};

using CacheMap = ::Common::unordered_map<u64, s64>;
using CountMap = ::Common::unordered_map<u64, u64>;

std::mutex g_mutex;
CacheMap g_cache;
CountMap g_counts;
bool g_loaded = false;

std::filesystem::path GetCachePath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "launched.json";
}

std::optional<std::string> ReadFileToString(const std::filesystem::path& path) {
    const std::ifstream file{path, std::ios::in | std::ios::binary};
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool WriteStringToFile(const std::filesystem::path& path, const std::string& data) {
    if (!Common::FS::CreateParentDirs(path)) {
        return false;
    }
    std::ofstream file{path, std::ios::out | std::ios::binary | std::ios::trunc};
    if (!file) {
        return false;
    }
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(file);
}

void Load() {
    if (g_loaded) {
        return;
    }

    g_loaded = true;

    const auto path = GetCachePath();
    if (!std::filesystem::exists(path)) {
        return;
    }

    const auto data = ReadFileToString(path);
    if (!data) {
        LOG_WARNING(Core, "Failed to read launch timestamp cache: {}",
                    Common::FS::PathToUTF8String(path));
        return;
    }

    // legacy format is a raw timestamp only
    using MixedCache = std::variant<CacheEntry, s64>;
    std::unordered_map<std::string, MixedCache> raw_cache{};
    const auto ec = glz::read<glz::opts{.null_terminated = false}>(raw_cache, data.value());

    if (ec) {
        LOG_WARNING(Core, "Failed to parse launch cache:\n{}", glz::format_error(ec, data.value()));
        return;
    }

    for (const auto& [key_str, val] : raw_cache) {
        u64 key{};
        try {
            key = std::stoull(key_str, nullptr, 16);
        } catch (...) {
            continue;
        }

        if (std::holds_alternative<CacheEntry>(val)) {
            const auto& entry = std::get<CacheEntry>(val);
            if (entry.timestamp)
                g_cache[key] = entry.timestamp.value();
            if (entry.launch_count)
                g_counts[key] = entry.launch_count.value();
        } else if (std::holds_alternative<s64>(val)) {
            g_cache[key] = std::get<s64>(val);
        }
    }
}

void Save() {
    std::unordered_map<std::string, CacheEntry> json_map{};
    for (const auto& [key, value] : g_cache) {
        CacheEntry entry{
            .timestamp = value,
            .launch_count = 0
        };

        if (const auto count_it = g_counts.find(key); count_it != g_counts.end()) {
            entry.launch_count = count_it->second;
        }

        json_map[std::format("{:016X}", key)] = entry;
    }

    std::string buffer{};
    auto ec = glz::write<glz::opts{.prettify = true}>(json_map, buffer);
    if (ec) {
        LOG_WARNING(Core, "Failed to serialize launch cache:\n{}", glz::format_error(ec, buffer));
    }

    const auto path = GetCachePath();
    if (!WriteStringToFile(path, buffer)) {
        LOG_WARNING(Core, "Failed to write launch timestamp cache: {}",
                    Common::FS::PathToUTF8String(path));
    }}

s64 NowSeconds() {
    return std::time(nullptr);
}

} // namespace

void SaveLaunchTimestamp(u64 title_id) {
    std::scoped_lock lk{g_mutex};
    Load();
    g_cache[title_id] = NowSeconds();
    g_counts[title_id] = g_counts[title_id] + 1;
    Save();
}

s64 GetLaunchTimestamp(u64 title_id) {
    std::scoped_lock lk{g_mutex};
    Load();
    const auto it = g_cache.find(title_id);
    if (it != g_cache.end()) {
        return it->second;
    }
    // we need a timestamp, i decided on 01/01/2026 00:00
    return 1767225600;
}

u64 GetLaunchCount(u64 title_id) {
    std::scoped_lock lk{g_mutex};
    Load();
    const auto it = g_counts.find(title_id);
    if (it != g_counts.end()) {
        return it->second;
    }
    return 0;
}

} // namespace Core::LaunchTimestampCache

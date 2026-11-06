// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/common_types.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace FileSys {

namespace {

constexpr size_t MaxSaveDataBackups = 2;

bool FilesMatch(const std::filesystem::path& source, const std::filesystem::path& backup) {
    std::ifstream source_file{source, std::ios::binary};
    std::ifstream backup_file{backup, std::ios::binary};
    if (!source_file.is_open() || !backup_file.is_open()) {
        return false;
    }
    std::array<char, 64 * 1024> source_buffer{};
    std::array<char, 64 * 1024> backup_buffer{};
    while (true) {
        source_file.read(source_buffer.data(), static_cast<std::streamsize>(source_buffer.size()));
        backup_file.read(backup_buffer.data(), static_cast<std::streamsize>(backup_buffer.size()));
        const auto source_count = source_file.gcount();
        const auto backup_count = backup_file.gcount();
        if (source_count != backup_count ||
            !std::equal(source_buffer.begin(), source_buffer.begin() + source_count,
                        backup_buffer.begin())) {
            return false;
        }
        if (source_count == 0) {
            return source_file.eof() && backup_file.eof();
        }
    }
}

bool VerifyDirectoryCopy(const std::filesystem::path& source,
                         const std::filesystem::path& backup) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it{source, ec};
    const std::filesystem::recursive_directory_iterator end;
    if (ec) {
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            return false;
        }
        const auto source_status = it->symlink_status(ec);
        if (ec) {
            return false;
        }
        const auto relative_path = it->path().lexically_relative(source);
        if (relative_path.empty()) {
            return false;
        }
        const auto backup_path = backup / relative_path;
        const auto backup_status = std::filesystem::symlink_status(backup_path, ec);
        if (ec || source_status.type() != backup_status.type()) {
            return false;
        }
        if (std::filesystem::is_symlink(source_status)) {
            const auto source_target = std::filesystem::read_symlink(it->path(), ec);
            if (ec) {
                return false;
            }
            const auto backup_target = std::filesystem::read_symlink(backup_path, ec);
            if (ec || source_target != backup_target) {
                return false;
            }
        } else if (std::filesystem::is_regular_file(source_status)) {
            const auto source_size = std::filesystem::file_size(it->path(), ec);
            if (ec) {
                return false;
            }
            const auto backup_size = std::filesystem::file_size(backup_path, ec);
            if (ec || source_size != backup_size || !FilesMatch(it->path(), backup_path)) {
                return false;
            }
        } else if (!std::filesystem::is_directory(source_status)) {
            return false;
        }
    }
    return !ec;
}

bool CreateSaveDataBackup(const std::filesystem::path& source, u64 title_id, u128 user_id,
                          SaveDataType type) {
    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec) || ec) {
        return false;
    }

    const std::string profile = fmt::format("{:016X}{:016X}", user_id[1], user_id[0]);
    const auto backup_root = Common::FS::GetEdenPath(Common::FS::EdenPath::SaveBackupDir) /
                             fmt::format("{:016X}", title_id) /
                             fmt::format("{}-{}", type == SaveDataType::Account ? "account" : "device",
                                         profile);
    std::filesystem::create_directories(backup_root, ec);
    if (ec) {
        LOG_WARNING(Service_FS, "Could not create save backup directory: {}", ec.message());
        return false;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    std::filesystem::path final_path;
    std::filesystem::path staging_path;
    bool staging_created = false;
    for (u32 attempt = 0; attempt < 1000; ++attempt) {
        const auto suffix = attempt == 0 ? fmt::format("{}", timestamp)
                                         : fmt::format("{}-{}", timestamp, attempt);
        final_path = backup_root / suffix;
        staging_path = backup_root / fmt::format("{}.tmp", suffix);
        if (std::filesystem::exists(final_path, ec)) {
            if (ec) {
                break;
            }
            continue;
        }
        if (ec) {
            break;
        }
        staging_created = std::filesystem::create_directory(staging_path, ec);
        if (staging_created) {
            break;
        }
        if (ec == std::errc::file_exists) {
            ec.clear();
            continue;
        }
        break;
    }
    if (!staging_created) {
        LOG_WARNING(Service_FS, "Could not reserve a unique save backup directory: {}",
                    ec ? ec.message() : "too many name collisions");
        return false;
    }

    std::filesystem::copy(source, staging_path,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::copy_symlinks,
                          ec);
    if (ec) {
        const auto copy_error = ec;
        std::error_code cleanup_error;
        std::filesystem::remove_all(staging_path, cleanup_error);
        LOG_WARNING(Service_FS, "Save backup copy failed; original save data was left untouched");
        LOG_DEBUG(Service_FS, "Save backup copy error: {}", copy_error.message());
        return false;
    }

    if (!VerifyDirectoryCopy(source, staging_path)) {
        std::filesystem::remove_all(staging_path, ec);
        LOG_WARNING(Service_FS, "Save backup verification failed; original save data was left untouched");
        return false;
    }

    std::filesystem::rename(staging_path, final_path, ec);
    if (ec) {
        const auto publish_error = ec;
        std::error_code cleanup_error;
        std::filesystem::remove_all(staging_path, cleanup_error);
        LOG_WARNING(Service_FS, "Could not publish verified save backup: {}",
                    publish_error.message());
        return false;
    }

    std::vector<std::filesystem::directory_entry> backups;
    for (std::filesystem::directory_iterator it{backup_root, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_directory(ec) && !ec && it->path().extension() != ".tmp") {
            backups.emplace_back(*it);
        }
    }
    if (ec) {
        LOG_WARNING(Service_FS, "Could not enumerate old save backups: {}", ec.message());
        return true;
    }
    std::sort(backups.begin(), backups.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.path().filename() < rhs.path().filename();
    });
    while (backups.size() > MaxSaveDataBackups) {
        std::filesystem::remove_all(backups.front().path(), ec);
        if (ec) {
            LOG_WARNING(Service_FS, "Could not prune an old save backup: {}", ec.message());
            break;
        }
        backups.erase(backups.begin());
    }
    return true;
}

bool ShouldSaveDataBeAutomaticallyCreated(SaveDataSpaceId space, const SaveDataAttribute& attr) {
    return attr.type == SaveDataType::Cache || attr.type == SaveDataType::Temporary ||
           (space == SaveDataSpaceId::User && ///< Normal Save Data -- Current Title & User
            (attr.type == SaveDataType::Account || attr.type == SaveDataType::Device) &&
            attr.program_id == 0 && attr.system_save_data_id == 0);
}

std::string GetFutureSaveDataPath(SaveDataSpaceId space_id, SaveDataType type, u64 title_id,
                                  u128 user_id) {
    // Only detect nand user saves.
    const auto space_id_path = [space_id]() -> std::string_view {
        switch (space_id) {
        case SaveDataSpaceId::User:
            return "/user/save";
        default:
            return "";
        }
    }();

    if (space_id_path.empty()) {
        return "";
    }

    Common::UUID uuid;
    std::memcpy(uuid.uuid.data(), user_id.data(), sizeof(Common::UUID));

    // Only detect account/device saves from the future location.
    switch (type) {
    case SaveDataType::Account:
        return fmt::format("{}/account/{}/{:016X}/0", space_id_path, uuid.RawString(), title_id);
    case SaveDataType::Device:
        return fmt::format("{}/device/{:016X}/0", space_id_path, title_id);
    default:
        return "";
    }
}

} // Anonymous namespace

SaveDataFactory::SaveDataFactory(Core::System& system_, ProgramId program_id_,
                                 VirtualDir save_directory_)
    : system{system_}, program_id{program_id_}, dir{std::move(save_directory_)} {}

SaveDataFactory::~SaveDataFactory() = default;

VirtualDir SaveDataFactory::Create(SaveDataSpaceId space, const SaveDataAttribute& meta) const {
    const auto save_directory = GetFullPath(program_id, dir, space, meta.type, meta.program_id,
                                            meta.user_id, meta.system_save_data_id);

    return dir->CreateDirectoryRelative(save_directory);
}

VirtualDir SaveDataFactory::Open(SaveDataSpaceId space, const SaveDataAttribute& meta) const {

    const auto save_directory = GetFullPath(program_id, dir, space, meta.type, meta.program_id,
                                            meta.user_id, meta.system_save_data_id);

    auto out = dir->GetDirectoryRelative(save_directory);

    if (Settings::values.automatic_save_data_backups.GetValue() && out != nullptr &&
        space == SaveDataSpaceId::User &&
        (meta.type == SaveDataType::Account || meta.type == SaveDataType::Device)) {
        const auto title_id = meta.program_id == 0 ? program_id : meta.program_id;
        bool should_backup = false;
        {
            std::scoped_lock lock{backup_mutex};
            should_backup = backed_up_saves.emplace(save_directory).second;
        }
        if (should_backup) {
            const auto source_path = std::filesystem::path{
                Common::FS::ToU8String(out->GetFullPath())};
            CreateSaveDataBackup(source_path, title_id, meta.user_id, meta.type);
        }
    }

    if (out == nullptr && (ShouldSaveDataBeAutomaticallyCreated(space, meta) && auto_create)) {
        return Create(space, meta);
    }

    return out;
}

VirtualDir SaveDataFactory::GetSaveDataSpaceDirectory(SaveDataSpaceId space) const {
    return dir->GetDirectoryRelative(GetSaveDataSpaceIdPath(space));
}

std::string SaveDataFactory::GetSaveDataSpaceIdPath(SaveDataSpaceId space) {
    switch (space) {
    case SaveDataSpaceId::System:
        return "/system/";
    case SaveDataSpaceId::User:
    case SaveDataSpaceId::SdUser:
        return "/user/";
    case SaveDataSpaceId::Temporary:
        return "/temp/";
    default:
        ASSERT_MSG(false, "Unrecognized SaveDataSpaceId: {:02X}", static_cast<u8>(space));
        return "/unrecognized/"; ///< To prevent corruption when ignoring asserts.
    }
}

std::string SaveDataFactory::GetFullPath(ProgramId program_id, VirtualDir dir,
                                         SaveDataSpaceId space, SaveDataType type, u64 title_id,
                                         u128 user_id, u64 save_id) {
    // According to switchbrew, if a save is of type SaveData and the title id field is 0, it should
    // be interpreted as the title id of the current process.
    if (type == SaveDataType::Account || type == SaveDataType::Device) {
        if (title_id == 0) {
            title_id = program_id;
        }
    }

    // For compat with a future impl.
    if (std::string future_path =
            GetFutureSaveDataPath(space, type, title_id & ~(0xFFULL), user_id);
        !future_path.empty()) {
        // Check if this location exists, and prefer it over the old.
        if (const auto future_dir = dir->GetDirectoryRelative(future_path); future_dir != nullptr) {
            LOG_INFO(Service_FS, "Using save at new location: {}", future_path);
            return future_path;
        }
    }

    std::string out = GetSaveDataSpaceIdPath(space);

    switch (type) {
    case SaveDataType::System:
        return fmt::format("{}save/{:016X}/{:016X}{:016X}", out, save_id, user_id[1], user_id[0]);
    case SaveDataType::Account:
    case SaveDataType::Device:
        return fmt::format("{}save/{:016X}/{:016X}{:016X}/{:016X}", out, 0, user_id[1], user_id[0],
                           title_id);
    case SaveDataType::Temporary:
        return fmt::format("{}{:016X}/{:016X}{:016X}/{:016X}", out, 0, user_id[1], user_id[0],
                           title_id);
    case SaveDataType::Cache:
        return fmt::format("{}save/cache/{:016X}", out, title_id);
    default:
        ASSERT_MSG(false, "Unrecognized SaveDataType: {:02X}", static_cast<u8>(type));
        return fmt::format("{}save/unknown_{:X}/{:016X}", out, static_cast<u8>(type), title_id);
    }
}

std::string SaveDataFactory::GetUserGameSaveDataRoot(u128 user_id, bool future) {
    if (future) {
        Common::UUID uuid;
        std::memcpy(uuid.uuid.data(), user_id.data(), sizeof(Common::UUID));
        return fmt::format("/user/save/account/{}", uuid.RawString());
    }
    return fmt::format("/user/save/{:016X}/{:016X}{:016X}", 0, user_id[1], user_id[0]);
}

SaveDataSize SaveDataFactory::ReadSaveDataSize(SaveDataType type, u64 title_id,
                                               u128 user_id) const {
    const auto path =
        GetFullPath(program_id, dir, SaveDataSpaceId::User, type, title_id, user_id, 0);
    const auto relative_dir = GetOrCreateDirectoryRelative(dir, path);

    const auto size_file = relative_dir->GetFile(GetSaveDataSizeFileName());
    if (size_file == nullptr || size_file->GetSize() < sizeof(SaveDataSize)) {
        return {0, 0};
    }

    SaveDataSize out;
    if (size_file->ReadObject(&out) != sizeof(SaveDataSize)) {
        return {0, 0};
    }

    return out;
}

void SaveDataFactory::WriteSaveDataSize(SaveDataType type, u64 title_id, u128 user_id,
                                        SaveDataSize new_value) const {
    const auto path =
        GetFullPath(program_id, dir, SaveDataSpaceId::User, type, title_id, user_id, 0);
    const auto relative_dir = GetOrCreateDirectoryRelative(dir, path);

    const auto size_file = relative_dir->CreateFile(GetSaveDataSizeFileName());
    if (size_file == nullptr) {
        return;
    }

    size_file->Resize(sizeof(SaveDataSize));
    size_file->WriteObject(new_value);
}

void SaveDataFactory::SetAutoCreate(bool state) {
    auto_create = state;
}

} // namespace FileSys

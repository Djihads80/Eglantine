// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/submission_package.h"
#include "core/loader/loader.h"

#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"

#include "qt_common/game_list/game_list_p.h"

#include "qt_common/game_list/model.h"
#include "qt_common/game_list/worker.h"

namespace {

QString GetGameListCachedObject(const std::string& filename, const std::string& ext,
                                const std::function<QString()>& generator) {
    if (!UISettings::values.cache_game_list || filename == "0000000000000000") {
        return generator();
    }

    const auto path =
        Common::FS::PathToUTF8String(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                                     "game_list" / fmt::format("{}.{}", filename, ext));

    void(Common::FS::CreateParentDirs(path));

    if (!Common::FS::Exists(path)) {
        const auto str = generator();

        QFile file{QString::fromStdString(path)};
        if (file.open(QFile::WriteOnly)) {
            file.write(str.toUtf8());
        }

        return str;
    }

    QFile file{QString::fromStdString(path)};
    if (file.open(QFile::ReadOnly)) {
        return QString::fromUtf8(file.readAll());
    }

    return generator();
}

std::pair<std::vector<u8>, std::string> GetGameListCachedObject(
    const std::string& filename, const std::string& ext,
    const std::function<std::pair<std::vector<u8>, std::string>()>& generator) {
    if (!UISettings::values.cache_game_list || filename == "0000000000000000") {
        return generator();
    }

    const auto game_list_dir =
        Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "game_list";
    const auto jpeg_name = fmt::format("{}.jpeg", filename);
    const auto app_name = fmt::format("{}.appname.txt", filename);

    const auto path1 = Common::FS::PathToUTF8String(game_list_dir / jpeg_name);
    const auto path2 = Common::FS::PathToUTF8String(game_list_dir / app_name);

    void(Common::FS::CreateParentDirs(path1));

    if (!Common::FS::Exists(path1) || !Common::FS::Exists(path2)) {
        const auto [icon, nacp] = generator();

        QFile file1{QString::fromStdString(path1)};
        if (!file1.open(QFile::WriteOnly)) {
            LOG_ERROR(Frontend, "Failed to open cache file.");
            return generator();
        }

        if (!file1.resize(icon.size())) {
            LOG_ERROR(Frontend, "Failed to resize cache file to necessary size.");
            return generator();
        }

        if (file1.write(reinterpret_cast<const char*>(icon.data()), icon.size()) !=
            s64(icon.size())) {
            LOG_ERROR(Frontend, "Failed to write data to cache file.");
            return generator();
        }

        QFile file2{QString::fromStdString(path2)};
        if (file2.open(QFile::WriteOnly)) {
            file2.write(nacp.data(), nacp.size());
        }

        return std::make_pair(icon, nacp);
    }

    QFile file1(QString::fromStdString(path1));
    QFile file2(QString::fromStdString(path2));

    if (!file1.open(QFile::ReadOnly)) {
        LOG_ERROR(Frontend, "Failed to open cache file for reading.");
        return generator();
    }

    if (!file2.open(QFile::ReadOnly)) {
        LOG_ERROR(Frontend, "Failed to open cache file for reading.");
        return generator();
    }

    std::vector<u8> vec(file1.size());
    if (file1.read(reinterpret_cast<char*>(vec.data()), vec.size()) !=
        static_cast<s64>(vec.size())) {
        return generator();
    }

    const auto data = file2.readAll();
    return std::make_pair(vec, data.toStdString());
}

void GetMetadataFromControlNCA(const FileSys::PatchManager& patch_manager, const FileSys::NCA& nca,
                               std::vector<u8>& icon, std::string& name) {
    std::tie(icon, name) = GetGameListCachedObject(
        fmt::format("{:016X}", patch_manager.GetTitleID()), {}, [&patch_manager, &nca] {
            const auto [nacp, icon_f] = patch_manager.ParseControlNCA(nca);
            return std::make_pair(icon_f->ReadAllBytes(), nacp->GetApplicationName());
        });
}

bool HasSupportedFileExtension(const std::string& file_name) {
    const QFileInfo file = QFileInfo(QString::fromStdString(file_name));
    return QtCommon::supported_file_extensions.contains(file.suffix(), Qt::CaseInsensitive);
}

bool IsExtractedNCAMain(const std::string& file_name) {
    return QFileInfo(QString::fromStdString(file_name)).fileName() == QStringLiteral("main");
}

QString FormatGameName(const std::string& physical_name) {
    const QString physical_name_as_qstring = QString::fromStdString(physical_name);
    const QFileInfo file_info(physical_name_as_qstring);

    if (IsExtractedNCAMain(physical_name)) {
        return file_info.dir().path();
    }

    return physical_name_as_qstring;
}

QDir ExtractedGameDirectory(const std::string& physical_name) {
    QDir directory = QFileInfo(QString::fromStdString(physical_name)).dir();
    if (directory.dirName().compare(QStringLiteral("exefs"), Qt::CaseInsensitive) != 0 ||
        !directory.cdUp()) {
        return QFileInfo(QString::fromStdString(physical_name)).dir();
    }
    if (directory.dirName().compare(QStringLiteral("updates"), Qt::CaseInsensitive) == 0 ||
        directory.dirName().compare(QStringLiteral("base"), Qt::CaseInsensitive) == 0) {
        directory.cdUp();
    } else {
        QDir parent = directory;
        if (parent.cdUp() &&
            parent.dirName().compare(QStringLiteral("updates"), Qt::CaseInsensitive) == 0 &&
            parent.cdUp()) {
            directory = parent;
        }
    }
    return directory;
}

QString LooseExeFSName(const std::string& physical_name) {
    return ExtractedGameDirectory(physical_name).dirName();
}

std::size_t ExtractedGameSize(const std::string& physical_name,
                              const std::atomic_bool& stop_requested) {
    QDirIterator files{ExtractedGameDirectory(physical_name).absolutePath(),
                       QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks,
                       QDirIterator::Subdirectories};
    std::size_t size = 0;
    while (!stop_requested && files.hasNext()) {
        files.next();
        const auto file_size = files.fileInfo().size();
        if (file_size > 0) {
            size += static_cast<std::size_t>(file_size);
        }
    }
    return size;
}

QString FormatPatchNameVersions(const FileSys::PatchManager& patch_manager,
                                Loader::AppLoader& loader, bool updatable = true) {
    QString out;
    FileSys::VirtualFile update_raw;
    loader.ReadUpdateRaw(update_raw);
    for (const auto& patch : patch_manager.GetPatches(update_raw)) {
        const bool is_update = patch.name == "Update";
        if (!updatable && is_update) {
            continue;
        }

        const QString type =
            QString::fromStdString(patch.enabled ? patch.name : "[D] " + patch.name);

        if (patch.version.empty()) {
            out.append(QStringLiteral("%1\n").arg(type));
        } else {
            auto ver = patch.version;

            // Display container name for packed updates
            if (is_update && ver == "PACKED") {
                ver = Loader::GetFileTypeString(loader.GetFileType());
            }

            out.append(QStringLiteral("%1 (%2)\n").arg(type, QString::fromStdString(ver)));
        }
    }

    out.chop(1);
    return out;
}

QList<QStandardItem*> MakeGameListEntry(const std::string& path, const std::string& name,
                                        const std::size_t size, const std::vector<u8>& icon,
                                        Loader::AppLoader& loader, u64 program_id,
                                        const PlayTime::PlayTimeManager& play_time_manager,
                                        const FileSys::PatchManager& patch) {
    auto const file_type = loader.GetFileType();
    auto const file_type_string = QString::fromStdString(Loader::GetFileTypeString(file_type));

    QString patch_versions = GetGameListCachedObject(
        fmt::format("{:016X}", patch.GetTitleID()), "pv.txt", [&patch, &loader] {
            return FormatPatchNameVersions(patch, loader, loader.IsRomFSUpdatable());
        });

    u64 play_time = play_time_manager.GetPlayTime(program_id);
    return QList<QStandardItem*>{
        new GameListItemPath(FormatGameName(path), icon, QString::fromStdString(name),
                             file_type_string, program_id, play_time, patch_versions),
        new GameListItem(file_type_string),
        new GameListItemSize(size),
        new GameListItemPlayTime(play_time),
        new GameListItem(patch_versions),
    };
}
} // Anonymous namespace

GameListWorker::GameListWorker(FileSys::VirtualFilesystem vfs_,
                               FileSys::ManualContentProvider* provider_,
                               QVector<UISettings::GameDir>& game_dirs_,
                               const PlayTime::PlayTimeManager& play_time_manager_,
                               Core::System& system_)
    : vfs{std::move(vfs_)}, provider{provider_}, game_dirs{game_dirs_},
      play_time_manager{play_time_manager_},
      system{system_} {
    // We want the game list to manage our lifetime.
    setAutoDelete(false);
}

GameListWorker::~GameListWorker() {
    this->disconnect();
    stop_requested.store(true);
    processing_completed.Wait();
}

void GameListWorker::ProcessEvents(GameListModel* model) {
    while (true) {
        std::function<void(GameListModel*)> func;
        {
            // Lock queue to protect concurrent modification.
            std::scoped_lock lk(lock);

            // If we can't pop a function, return.
            if (queued_events.empty()) {
                return;
            }

            // Pop a function.
            func = std::move(queued_events.back());
            queued_events.pop_back();
        }

        // Run the function.
        func(model);
    }
}

template <typename F>
void GameListWorker::RecordEvent(F&& func) {
    {
        // Lock queue to protect concurrent modification.
        std::scoped_lock lk(lock);

        // Add the function into the front of the queue.
        queued_events.emplace_front(std::move(func));
    }

    // Data now available.
    emit DataAvailable();
}

void GameListWorker::AddTitlesToGameList(GameListDir* parent_dir) {
    using namespace FileSys;

    const auto& cache = system.GetContentProviderUnion();

    auto installed_games = cache.ListEntriesFilterOrigin(std::nullopt, TitleType::Application,
                                                         ContentRecordType::Program);

    if (parent_dir->type() == static_cast<int>(GameListItemType::SdmcDir)) {
        installed_games = cache.ListEntriesFilterOrigin(
            ContentProviderUnionSlot::SDMC, TitleType::Application, ContentRecordType::Program);
    } else if (parent_dir->type() == static_cast<int>(GameListItemType::UserNandDir)) {
        installed_games = cache.ListEntriesFilterOrigin(
            ContentProviderUnionSlot::UserNAND, TitleType::Application, ContentRecordType::Program);
    } else if (parent_dir->type() == static_cast<int>(GameListItemType::SysNandDir)) {
        installed_games = cache.ListEntriesFilterOrigin(
            ContentProviderUnionSlot::SysNAND, TitleType::Application, ContentRecordType::Program);
    }

    for (const auto& [slot, game] : installed_games) {
        if (slot == ContentProviderUnionSlot::FrontendManual) {
            continue;
        }

        const auto file = cache.GetEntryUnparsed(game.title_id, game.type);
        std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(system, file);
        if (!loader) {
            continue;
        }

        std::vector<u8> icon;
        std::string name;
        u64 program_id = 0;
        const auto result = loader->ReadProgramId(program_id);

        if (result != Loader::ResultStatus::Success) {
            continue;
        }

        const PatchManager patch{program_id, system.GetFileSystemController(),
                                 system.GetContentProvider()};
        LOG_INFO(Frontend, "PatchManager initiated for id {:X}", program_id);
        const auto control = cache.GetEntry(game.title_id, ContentRecordType::Control);
        if (control != nullptr) {
            GetMetadataFromControlNCA(patch, *control, icon, name);
        }

        auto entry = MakeGameListEntry(file->GetFullPath(), name, file->GetSize(), icon, *loader,
                                       program_id, play_time_manager, patch);
        RecordEvent([=](GameListModel* model) { model->AddEntry(entry, parent_dir); });
    }
}

void GameListWorker::ScanFileSystem(ScanTarget target, const std::string& dir_path, bool deep_scan,
                                    GameListDir* parent_dir) {
    const auto process_candidate = [this, target,
                                    parent_dir](const std::filesystem::path& path) {
        const auto physical_name = Common::FS::PathToUTF8String(path);
        const auto file = vfs->OpenFile(physical_name, FileSys::OpenMode::Read);
        if (!file) {
            return;
        }

        auto loader = Loader::GetLoader(system, file);
        if (!loader) {
            return;
        }

        const auto file_type = loader->GetFileType();
        if (file_type == Loader::FileType::Unknown || file_type == Loader::FileType::Error) {
            return;
        }

        if (target == ScanTarget::PopulateGameList &&
            (file_type == Loader::FileType::XCI || file_type == Loader::FileType::XCZ ||
             file_type == Loader::FileType::NSP || file_type == Loader::FileType::NSZ) &&
            !Loader::IsBootableGameContainer(file, file_type)) {
            return;
        }

        u64 program_id = 0;
        const auto res2 = loader->ReadProgramId(program_id);

        if (target == ScanTarget::FillManualContentProvider) {
            if (res2 == Loader::ResultStatus::Success && file_type == Loader::FileType::NCA) {
                provider->AddEntry(FileSys::TitleType::Application,
                                   FileSys::GetCRTypeFromNCAType(FileSys::NCA{file}.GetType()),
                                   program_id, file);
            } else if (Settings::values.ext_content_from_game_dirs.GetValue() &&
                       (file_type == Loader::FileType::XCI ||
                        file_type == Loader::FileType::XCZ ||
                        file_type == Loader::FileType::NSP ||
                        file_type == Loader::FileType::NSZ)) {
                void(provider->AddEntriesFromContainer(file));
            }
        } else {
            std::vector<u64> program_ids;
            loader->ReadProgramIds(program_ids);

            const auto addEntry = [this, physical_name,
                                   parent_dir](std::unique_ptr<Loader::AppLoader>& app_loader,
                                               const u64 id) {
                std::vector<u8> icon;
                [[maybe_unused]] const auto res1 = app_loader->ReadIcon(icon);

                std::string name = " ";
                const auto res3 = app_loader->ReadTitle(name);
                if ((res3 != Loader::ResultStatus::Success || name.empty() || name == " ") &&
                    IsExtractedNCAMain(physical_name)) {
                    if (const QString loose = LooseExeFSName(physical_name); !loose.isEmpty()) {
                        name = loose.toStdString();
                    }
                }

                const FileSys::PatchManager patch{id, system.GetFileSystemController(),
                                                  system.GetContentProvider()};

                const auto size = app_loader->GetFileType() == Loader::FileType::DeconstructedRomDirectory
                    ? ExtractedGameSize(physical_name, stop_requested)
                    : Common::FS::GetSize(physical_name);
                if (stop_requested) return;
                auto entry = MakeGameListEntry(physical_name, name, size, icon, *app_loader,
                                               id, play_time_manager, patch);

                RecordEvent([=](GameListModel* model) { model->AddEntry(entry, parent_dir); });
            };

            if (res2 == Loader::ResultStatus::Success && program_ids.size() > 1 &&
                (file_type == Loader::FileType::XCI || file_type == Loader::FileType::XCZ ||
                 file_type == Loader::FileType::NSP || file_type == Loader::FileType::NSZ)) {
                for (const auto id : program_ids) {
                    // dravee suggested this, only viable way to
                    // not show sub-games in qlaunch for now.
                    if ((id & 0xFFF) != 0) {
                        continue;
                    }
                    loader = Loader::GetLoader(system, file, id);
                    if (!loader) {
                        continue;
                    }

                    addEntry(loader, id);
                }
            } else {
                addEntry(loader, program_id);
            }
        }
    };

    const auto callback = [this, &process_candidate](const std::filesystem::path& path) -> bool {
        if (stop_requested) return false;
        if (Common::FS::IsDir(path)) {
            watch_list.append(QString::fromStdString(Common::FS::PathToUTF8String(path)));
        } else {
            const auto name = Common::FS::PathToUTF8String(path);
            if (HasSupportedFileExtension(name) || IsExtractedNCAMain(name)) process_candidate(path);
        }
        return true;
    };
    if (deep_scan) {
        Common::FS::IterateDirEntriesRecursively(dir_path, callback, Common::FS::DirEntryFilter::All);
    } else {
        Common::FS::IterateDirEntries(dir_path, callback, Common::FS::DirEntryFilter::File);
        const auto scan_exefs = [&](const QDir& directory) {
            const auto main = directory.filePath(QStringLiteral("main"));
            if (QFileInfo::exists(main)) {
                process_candidate(std::filesystem::path{Common::FS::ToU8String(main.toStdString())});
            }
            watch_list.append(directory.absolutePath());
        };
        const auto scan_title = [&](const QDir& title) {
            const QDir updates{title.filePath(QStringLiteral("updates"))};
            auto versions = updates.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            std::sort(versions.begin(), versions.end(), [](const QString& a, const QString& b) {
                return a.toULongLong() > b.toULongLong();
            });
            for (const auto& version : versions) {
                const QDir exefs{updates.filePath(version + QStringLiteral("/exefs"))};
                if (QFileInfo::exists(exefs.filePath(QStringLiteral("main")))) {
                    scan_exefs(exefs);
                    return;
                }
            }
            for (const auto* relative : {"updates/exefs", "exefs", "base/exefs"}) {
                const QDir exefs{title.filePath(QString::fromLatin1(relative))};
                if (QFileInfo::exists(exefs.filePath(QStringLiteral("main")))) {
                    scan_exefs(exefs);
                    return;
                }
            }
        };
        const QDir root{QString::fromStdString(dir_path)};
        scan_title(root);
        for (const auto& child : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (stop_requested) {
                break;
            }
            if (child != QStringLiteral("exefs") && child != QStringLiteral("romfs") &&
                child != QStringLiteral("updates")) {
                scan_title(QDir{root.filePath(child)});
            }
        }
    }
}

void GameListWorker::run() {
    watch_list.clear();
    provider->ClearAllEntries();

    const auto DirEntryReady = [&](GameListDir* game_list_dir) {
        RecordEvent([=](GameListModel* model) { model->AddDirEntry(game_list_dir); });
    };

    for (UISettings::GameDir& game_dir : game_dirs) {
        if (stop_requested) {
            break;
        }

        GameListDir* game_list_dir;
        bool scan = false;

        if (game_dir.path == std::string("SDMC")) {
            game_list_dir = new GameListDir(game_dir, GameListItemType::SdmcDir);
        } else if (game_dir.path == std::string("UserNAND")) {
            game_list_dir = new GameListDir(game_dir, GameListItemType::UserNandDir);
        } else if (game_dir.path == std::string("SysNAND")) {
            game_list_dir = new GameListDir(game_dir, GameListItemType::SysNandDir);
        } else {
            const QString qpath = QString::fromStdString(game_dir.path);
            if (QDir(qpath).exists()) {
                watch_list.append(qpath);
            }

            game_list_dir = new GameListDir(game_dir);
            scan = true;
        }

        DirEntryReady(game_list_dir);
        if (scan) {
            ScanFileSystem(ScanTarget::FillManualContentProvider, game_dir.path, game_dir.deep_scan,
                           game_list_dir);
            ScanFileSystem(ScanTarget::PopulateGameList, game_dir.path, game_dir.deep_scan,
                           game_list_dir);
        } else {
            AddTitlesToGameList(game_list_dir);
        }
    }

    RecordEvent([this](GameListModel* model) { model->DonePopulating(watch_list); });
    processing_completed.Set();
}

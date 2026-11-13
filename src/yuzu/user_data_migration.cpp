// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QTranslator>
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_string_lookup.h"
#include "user_data_migration.h"
#include "yuzu/migration_dialog.h"

// Needs to be included at the end due to https://bugreports.qt.io/browse/QTBUG-73263
#include <algorithm>
#include <filesystem>
#include <QButtonGroup>
#include <QCheckBox>
#include <QGuiApplication>
#include <QProgressDialog>
#include <QRadioButton>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

UserDataMigrator::UserDataMigrator(QMainWindow* main_window) {
    // NOTE: Logging is not initialized yet, do not produce logs here.

    // Check migration if config directory does not exist
    // TODO: ProfileManager messes with us a bit here, and force-creates the
    // /nand/system/save/8000000000000010/su/avators/profiles.dat file. Find a way to reorder
    // operations and have it create after this guy runs.
    if (!std::filesystem::is_directory(Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir))) {
        ShowMigrationPrompt(main_window);
    }
}

void UserDataMigrator::ShowMigrationPrompt(QMainWindow* main_window) {
    namespace fs = std::filesystem;
    using namespace QtCommon::StringLookup;

    MigrationDialog migration_prompt;
    migration_prompt.setWindowTitle(QObject::tr("Migration"));

    // mutually exclusive
    QButtonGroup* group = new QButtonGroup(&migration_prompt);

    // MACRO MADNESS, indeed

#define BUTTON(clazz, name, text, tooltip, checkState)                                             \
    clazz* name = new clazz(&migration_prompt);                                                    \
    name->setText(text);                                                                           \
    name->setToolTip(Lookup(tooltip));                                                             \
    name->setChecked(checkState);                                                                  \
    migration_prompt.addBox(name);

    BUTTON(QCheckBox, clear_shaders, QObject::tr("Clear Shader Cache"), MigrationTooltipClearShader,
           true)

#define RADIO(name, text, tooltip, checkState, strategy)                                           \
    BUTTON(QRadioButton, name, text, tooltip, checkState)                                          \
    group->addButton(name, static_cast<int>(MigrationWorker::MigrationStrategy::strategy));

    RADIO(keep_old, QObject::tr("Keep Old Data"), MigrationTooltipKeepOld, true, Copy)
    RADIO(clear_old, QObject::tr("Clear Old Data"), MigrationTooltipClearOld, false, Move)
    RADIO(link_old, QObject::tr("Link Old Directory"), MigrationTooltipLinkOld, false, Link)

#undef RADIO
#undef BUTTON

    std::vector<Emulator> found{};

    for (const Emulator& emu : legacy_emus)
        if (fs::is_directory(emu.get_user_dir()))
            found.emplace_back(emu);

    if (found.empty()) {
        return;
    }

    // makes my life easier
    qRegisterMetaType<Emulator>();

    QString prompt_text = Lookup(MigrationPromptPrefix);

    // natural language processing is a nightmare
    for (const Emulator& emu : found) {
        prompt_text = prompt_text % QStringLiteral("\n ") % emu.name();

        QAbstractButton* button = migration_prompt.addButton(emu.name());

        // This is cursed, but it's actually the most efficient way by a mile
        button->setProperty("emulator", QVariant::fromValue(emu));
    }

    prompt_text.append(QObject::tr("\n\n"));
    prompt_text = prompt_text % QStringLiteral("\n\n") % Lookup(MigrationPrompt);

    migration_prompt.setText(prompt_text);
    migration_prompt.addButton(QObject::tr("No"), true);

    migration_prompt.exec();

    QAbstractButton* button = migration_prompt.clickedButton();

    if (button->text() == QObject::tr("No")) {
        return ShowMigrationCancelledMessage(main_window);
    }

    MigrationWorker::MigrationStrategy strategy =
        static_cast<MigrationWorker::MigrationStrategy>(group->checkedId());

    selected_emu = button->property("emulator").value<Emulator>();

    MigrateUserData(main_window, clear_shaders->isChecked(), strategy);
}

void UserDataMigrator::ShowMigrationCancelledMessage(QMainWindow* main_window) {
    QMessageBox::information(main_window, QObject::tr("Migration"),
                             QObject::tr("You can manually re-trigger this prompt by deleting the "
                                         "new config directory:\n%1")
                                 .arg(QString::fromStdString(Common::FS::GetEdenPathString(
                                     Common::FS::EdenPath::ConfigDir))),
                             QMessageBox::Ok);
}

void UserDataMigrator::MigrateUserData(QMainWindow* main_window, const bool clear_shader_cache,
                                       const MigrationWorker::MigrationStrategy strategy) {
    // Create a dialog to let the user know it's migrating
    QProgressDialog* progress = new QProgressDialog(main_window);
    progress->setWindowTitle(QObject::tr("Migrating"));
    progress->setLabelText(QObject::tr("Migrating, this may take a while..."));
    progress->setRange(0, 0);
    progress->setCancelButton(nullptr);
    progress->setWindowModality(Qt::WindowModality::ApplicationModal);

    QThread* thread = new QThread(main_window);
    MigrationWorker* worker = new MigrationWorker(selected_emu, clear_shader_cache, strategy);
    worker->moveToThread(thread);

    thread->connect(thread, &QThread::started, worker, &MigrationWorker::process);

    thread->connect(worker, &MigrationWorker::finished, progress,
                    [=, this](const QString& success_text, const std::string& path) {
                        progress->close();
                        QMessageBox::information(main_window, QObject::tr("Migration"),
                                                 success_text, QMessageBox::Ok);

                        migrated = true;
                        thread->quit();
                    });

    thread->connect(worker, &MigrationWorker::finished, worker, &QObject::deleteLater);
    thread->connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    thread->start();
    progress->exec();
}

void UserDataMigrator::ReconcileLegacyPaths(QMainWindow* main_window, QtConfig& config) {
    namespace fs = std::filesystem;
    using Common::FS::EdenPath;

    if (!UISettings::values.check_legacy_paths.GetValue()) {
        return;
    }

    const fs::path eden_dir = Common::FS::GetEdenPath(EdenPath::EdenDir);
    static constexpr std::array paths{EdenPath::NANDDir, EdenPath::SDMCDir, EdenPath::SaveDir,
                                      EdenPath::LoadDir, EdenPath::DumpDir, EdenPath::TASDir};

    struct Move {
        EdenPath path;
        fs::path from;
        fs::path to;
        fs::path legacy_dir;
        fs::path relative;
    };
    std::vector<Move> moves;

    for (const Emulator& emu : legacy_emus) {
        const fs::path legacy_dir = emu.get_user_dir();
        std::error_code ec;
        // Linked migrations share the folder on purpose.
        if (!fs::is_directory(legacy_dir, ec) || fs::equivalent(legacy_dir, eden_dir, ec)) {
            continue;
        }

        for (const EdenPath path : paths) {
            const fs::path current = Common::FS::GetEdenPath(path).lexically_normal();
            const fs::path relative = current.lexically_relative(legacy_dir.lexically_normal());
            if (relative.empty() || relative == "." || *relative.begin() == "..") {
                continue;
            }
            moves.push_back({path, current, (eden_dir / relative).lexically_normal(), legacy_dir,
                             relative});
        }
    }

    if (moves.empty()) {
        return;
    }

    // Group by old root so each root is shown once, followed by the folder names under it.
    QMap<QString, QStringList> folders_by_root;
    for (const Move& move : moves) {
        QString name = QString::fromStdString(Common::FS::ToUTF8String(
            move.relative.generic_u8string()));
        while (name.endsWith(QLatin1Char('/'))) {
            name.chop(1);
        }
        QStringList& names = folders_by_root[QString::fromStdString(
            Common::FS::PathToUTF8String(move.legacy_dir))];
        if (!names.contains(name)) {
            names.append(name);
        }
    }

    QString listing;
    for (auto it = folders_by_root.cbegin(); it != folders_by_root.cend(); ++it) {
        listing += QStringLiteral("%1\n    %2\n")
                       .arg(it.key(), it.value().join(QStringLiteral(", ")));
    }

    const auto answer = QMessageBox::question(
        main_window, QObject::tr("Old data folders"),
        QObject::tr("Eglantine is still using data folders left over from migrating:\n\n%1\n"
                    "Copy them into %2 (newer files win) and use them from now on? The old "
                    "folders are left in place.\n\n"
                    "Choose No to keep the current folders; you won't be asked again.")
            .arg(listing, QString::fromStdString(Common::FS::PathToUTF8String(eden_dir))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (answer != QMessageBox::Yes) {
        UISettings::values.check_legacy_paths.SetValue(false);
        config.SaveAllValues();
        return;
    }

    QProgressDialog progress(QObject::tr("Copying data, this may take a while..."), QString(), 0,
                             0, main_window);
    progress.setWindowTitle(QObject::tr("Old data folders"));
    progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::WindowModality::ApplicationModal);

    QFutureWatcher<QString> watcher;
    QObject::connect(&watcher, &QFutureWatcher<QString>::finished, &progress,
                     &QProgressDialog::close);
    watcher.setFuture(QtConcurrent::run([moves]() -> QString {
        constexpr auto options = fs::copy_options::update_existing | fs::copy_options::recursive;
        try {
            std::vector<fs::path> copied;
            for (const Move& move : moves) {
                if (std::find(copied.begin(), copied.end(), move.from) != copied.end()) {
                    continue;
                }
                fs::create_directories(move.to);
                fs::copy(move.from, move.to, options);
                copied.push_back(move.from);
            }
        } catch (const fs::filesystem_error& e) {
            return QString::fromStdString(e.what());
        }
        return QString();
    }));
    progress.exec();
    watcher.waitForFinished();

    if (const QString error = watcher.result(); !error.isEmpty()) {
        QMessageBox::warning(main_window, QObject::tr("Old data folders"),
                             QObject::tr("Copying failed, the current folders are still in use.\n"
                                         "%1")
                                 .arg(error));
        return;
    }

    for (const Move& move : moves) {
        Common::FS::SetEdenPath(move.path, move.to);
    }
    config.SaveAllValues();
}

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <qdesktopservices.h>
#include "common/logging.h"
#include "common/scm_rev.h"
#include "qt_common/abstract/frontend.h"
#include "qt_common/abstract/progress.h"
#include "qt_common/config/uisettings.h"
#ifdef ENABLE_UPDATE_CHECKER
#include "frontend_common/update_installer.h"
#ifdef _WIN32
#include "qt_common/util/compress.h"
#endif
#endif
#include "download.h"
#include "ui_update_dialog.h"
#include "update_dialog.h"

#include <QDesktopServices>

#undef GetSaveFileName

QString UpdateDialog::ChannelName(UpdateChecker::Channel channel) {
    return channel == UpdateChecker::Channel::Stable ? tr("Stable") : tr("Release candidate");
}

UpdateDialog::UpdateDialog(const Common::Net::Release& release, UpdateChecker::Channel channel,
                           QWidget* parent)
    : QDialog(parent), ui(new Ui::UpdateDialog) {
    ui->setupUi(this);

    ui->version->setText(tr("%1 is available. You are running %2.\nUpdate channel: %3")
                             .arg(QString::fromStdString(release.title),
                                  QString::fromUtf8(Common::g_build_version),
                                  ChannelName(channel)));
    ui->url->setText(
        tr("<a href=\"%1\">View on GitHub</a>").arg(QString::fromStdString(release.html_url)));

    std::string text{release.body};
    if (auto pos = text.find("# Packages"); pos != std::string::npos) {
        text = text.substr(0, pos);
    }

    ui->body->setMarkdown(QString::fromStdString(text));

    ui->buttonBox->button(QDialogButtonBox::No)->setText(tr("Remind Me Later"));
    auto* skip = ui->buttonBox->addButton(tr("Skip This Version"), QDialogButtonBox::ActionRole);
    connect(skip, &QPushButton::clicked, this, [this] { done(SkipVersion); });

    // TODO(crueter): Find a way to set default
    const auto assets = release.GetPlatformAssets(
        UpdateChecker::DownloadPlatform(UISettings::values.update_variant.GetValue()),
        Common::Net::CurrentLinuxPackage());

    ui->buttonBox->button(QDialogButtonBox::Yes)
        ->setText(assets.empty() ? tr("Open Release Page") : tr("Download"));

    if (assets.empty()) {
        ui->groupBox->setHidden(true);
        connect(this, &QDialog::accepted, this, [release]() {
            QDesktopServices::openUrl(QUrl{QString::fromStdString(release.html_url)});
        });
    } else if (assets.size() == 1) {
        ui->groupBox->setHidden(true);
        m_asset = assets[0];

        connect(this, &QDialog::accepted, this, &UpdateDialog::Download);
    } else {
        u32 i = 0;
        for (const Common::Net::Asset& a : assets) {
            QRadioButton* r = new QRadioButton(tr(a.name.c_str()), this);
            connect(r, &QRadioButton::toggled, this, [a, this](bool checked) {
                if (checked)
                    m_asset = a;
            });

            if (i == 0)
                r->setChecked(true);
            ++i;

            ui->radioButtons->addWidget(r);
        }

        connect(this, &QDialog::accepted, this, &UpdateDialog::Download);
    }
}

UpdateDialog::~UpdateDialog() {
    delete ui;
}

#if defined(ENABLE_UPDATE_CHECKER) && (defined(__linux__) || defined(_WIN32))
void UpdateDialog::Restart(QWidget* window, const QString& target) {
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto app_dir = environment.value(QStringLiteral("APPDIR"));
    if (!app_dir.isEmpty()) {
        for (const auto& key : environment.keys()) {
            const auto value = environment.value(key);
            if (!value.contains(app_dir)) {
                continue;
            }
            const auto stripped = UpdateInstaller::StripPathListEntries(value.toStdString(),
                                                                        app_dir.toStdString());
            if (stripped.empty()) {
                environment.remove(key);
            } else {
                environment.insert(key, QString::fromStdString(stripped));
            }
        }
    }
    for (const auto& key : {"APPDIR", "APPIMAGE", "ARGV0", "OWD"}) {
        environment.remove(QString::fromLatin1(key));
    }

    // Wait for this process to exit, so the new one reads the settings this one writes on close.
    QProcess process;
    process.setProgram(QStringLiteral("/bin/sh"));
    process.setArguments(
        {QStringLiteral("-c"),
         QStringLiteral("i=0; while kill -0 \"$1\" 2>/dev/null && [ \"$i\" -lt 1500 ]; do "
                        "sleep 0.2; i=$((i+1)); done; kill -0 \"$1\" 2>/dev/null && exit 0; "
                        "exec \"$2\""),
         QStringLiteral("sh"), QString::number(QCoreApplication::applicationPid()), target});
    process.setProcessEnvironment(environment);
    if (!process.startDetached()) {
        LOG_ERROR(Frontend, "Could not start the restart helper for {}", target.toStdString());
        QtCommon::Frontend::Critical(tr("Failed to restart"),
                                     tr("Please start %1 again yourself.").arg(target));
        return;
    }
    if (window) {
        QMetaObject::invokeMethod(window, &QWidget::close, Qt::QueuedConnection);
    }
}
#endif

#if defined(ENABLE_UPDATE_CHECKER) && defined(_WIN32)
bool UpdateDialog::LaunchInstaller(QWidget* window, const QString& staging) {
    // The helper runs from a copy, because the installed one is replaced along with everything else.
    const QString helper =
        QCoreApplication::applicationDirPath() + QStringLiteral("/eglantine-updater.exe");
    const QString copy = QDir::temp().filePath(
        QStringLiteral("eglantine-updater-%1.exe").arg(QCoreApplication::applicationPid()));
    QFile::remove(copy);
    if (!QFile::copy(helper, copy)) {
        LOG_ERROR(Frontend, "Could not copy the installer helper {}", helper.toStdString());
        QtCommon::Frontend::Critical(tr("Failed to install the update"),
                                     tr("Could not start the update installer."));
        return false;
    }

    const auto install_dir = QCoreApplication::applicationDirPath();
    const auto native = [](const QString& path) { return QDir::toNativeSeparators(path); };
    if (!QProcess::startDetached(
            copy, {QString::number(QCoreApplication::applicationPid()), native(staging),
                   native(install_dir), native(QCoreApplication::applicationFilePath())})) {
        LOG_ERROR(Frontend, "Could not start the installer helper {}", copy.toStdString());
        QtCommon::Frontend::Critical(tr("Failed to install the update"),
                                     tr("Could not start the update installer."));
        return false;
    }
    if (window) {
        QMetaObject::invokeMethod(window, &QWidget::close, Qt::QueuedConnection);
    }
    return true;
}
#endif

void UpdateDialog::Download() {
#if defined(ENABLE_UPDATE_CHECKER) && defined(_WIN32)
    if (m_asset.filename.ends_with(".zip")) {
        const std::filesystem::path exe{QCoreApplication::applicationFilePath().toStdWString()};
        if (UpdateInstaller::CanReplace(exe) &&
            QFile::exists(QCoreApplication::applicationDirPath() +
                          QStringLiteral("/eglantine-updater.exe"))) {
            InstallOnRestart(exe);
            return;
        }
    }
#endif
#if defined(ENABLE_UPDATE_CHECKER) && defined(__linux__)
    if (Common::Net::CurrentLinuxPackage() == Common::Net::LinuxPackage::AppImage &&
        m_asset.filename.ends_with(".AppImage")) {
        const std::filesystem::path target{qEnvironmentVariable("APPIMAGE").toStdString()};
        if (UpdateInstaller::CanReplace(target)) {
            InstallInPlace(target);
            return;
        }
    }
#endif

    // The application folder is read-only inside an AppImage and is the app bundle on macOS.
    auto default_dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (default_dir.isEmpty()) {
        default_dir = QDir::homePath();
    }

    const auto filename = QtCommon::Frontend::GetSaveFileName(
        tr("New Version Location"),
        default_dir % QStringLiteral("/") % QString::fromStdString(m_asset.filename),
        tr("All Files (*.*)"));

    if (filename.isEmpty() || !DownloadTo(filename))
        return;

#ifdef __linux__
    if (filename.endsWith(QStringLiteral(".AppImage"), Qt::CaseInsensitive)) {
        QFile::setPermissions(filename, QFile::permissions(filename) | QFile::ExeOwner |
                                            QFile::ExeGroup | QFile::ExeOther);
    }
#endif

    // Download is complete. User may choose to open the folder it is in.
    auto button = QtCommon::Frontend::Question(
        tr("Download Complete"),
        tr("Successfully downloaded %1. Would you like to open the folder it is in?")
            .arg(QString::fromStdString(m_asset.filename)),
        QtCommon::Frontend::Yes | QtCommon::Frontend::No);

    if (button == QtCommon::Frontend::Yes) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(filename).absolutePath()));
    }
}

#if defined(ENABLE_UPDATE_CHECKER) && defined(__linux__)
void UpdateDialog::InstallInPlace(const std::filesystem::path& target) {
    const UpdateInstaller::Lock lock{UpdateInstaller::LockPath(target)};
    if (!lock.Held()) {
        QtCommon::Frontend::Critical(tr("Failed to install the update"),
                                     tr("Another Eglantine window is already updating this "
                                        "installation."));
        return;
    }

    const auto staged = UpdateInstaller::StagedPath(target);
    if (!DownloadTo(QString::fromStdString(staged.string()))) {
        return;
    }

    std::error_code ec;
    if (!UpdateInstaller::Swap(staged, target, ec)) {
        LOG_ERROR(Frontend, "Could not replace {}: {}", target.string(), ec.message());
        std::filesystem::remove(staged, ec);
        QtCommon::Frontend::Critical(
            tr("Failed to install the update"),
            tr("Could not replace %1. Your installation was not changed.")
                .arg(QString::fromStdString(target.string())));
        return;
    }

    LOG_INFO(Frontend, "Installed {} over {}", m_asset.filename, target.string());
    const auto button = QtCommon::Frontend::Question(
        tr("Update Installed"),
        tr("The new version is installed. The previous version is kept as %1.\n\nRestart now?")
            .arg(QString::fromStdString(UpdateInstaller::PreviousPath(target).string())),
        QtCommon::Frontend::Yes | QtCommon::Frontend::No);
    if (button == QtCommon::Frontend::Yes) {
        Restart(parentWidget(), QString::fromStdString(target.string()));
    }
}
#endif

#if defined(ENABLE_UPDATE_CHECKER) && defined(_WIN32)
void UpdateDialog::InstallOnRestart(const std::filesystem::path& exe) {
    const auto install_dir = exe.parent_path();
    const UpdateInstaller::Lock lock{UpdateInstaller::LockPath(exe)};
    if (!lock.Held()) {
        QtCommon::Frontend::Critical(tr("Failed to install the update"),
                                     tr("Another Eglantine window is already updating this "
                                        "installation."));
        return;
    }

    const auto staging = install_dir / ".update-staging";
    const auto archive = install_dir / ".update-download.zip";
    const auto cleanup = [&] {
        std::error_code ec;
        std::filesystem::remove_all(staging, ec);
        std::filesystem::remove(archive, ec);
    };
    const auto fail = [&](const QString& text) {
        cleanup();
        QtCommon::Frontend::Critical(tr("Failed to install the update"), text);
    };

    cleanup();
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    if (ec) {
        fail(tr("Could not create a folder for the update."));
        return;
    }
    if (!DownloadTo(QString::fromStdWString(archive.wstring()))) {
        cleanup();
        return;
    }

    auto progress = QtCommon::Frontend::newProgressDialog(tr("Unpacking..."), tr("Cancel"), 0, 100);
    progress->show();
    const auto unpacked = QtCommon::Compress::extractDir(
        QString::fromStdWString(archive.wstring()), QString::fromStdWString(staging.wstring()),
        [&](std::size_t total, std::size_t done) {
            QGuiApplication::processEvents();
            if (total != 0) {
                progress->setValue(static_cast<int>((done * 100) / total));
            }
            return !progress->wasCanceled();
        });
    const bool canceled = progress->wasCanceled();
    progress->close();
    if (canceled) {
        cleanup();
        QtCommon::Frontend::Information(tr("Update Canceled"),
                                        tr("The update was canceled and nothing was changed."));
        return;
    }
    if (unpacked.isEmpty() || !std::filesystem::exists(staging / exe.filename(), ec)) {
        fail(tr("The downloaded update could not be unpacked."));
        return;
    }
    std::filesystem::remove(archive, ec);

    const auto button = QtCommon::Frontend::Question(
        tr("Update Ready"),
        tr("Eglantine will close, install the update and start again. The previous version is "
           "kept, and your settings and data are not changed.\n\nRestart now?"),
        QtCommon::Frontend::Yes | QtCommon::Frontend::No);
    if (button != QtCommon::Frontend::Yes ||
        !LaunchInstaller(parentWidget(), QString::fromStdWString(staging.wstring()))) {
        cleanup();
    }
}
#endif

bool UpdateDialog::DownloadTo(const QString& filename) {
    QSaveFile file(filename);
    if (!file.open(QIODevice::Truncate | QIODevice::WriteOnly)) {
        LOG_WARNING(Frontend, "Could not open file {}", filename.toStdString());
        QtCommon::Frontend::Critical(tr("Failed to save file"),
                                     tr("Could not open file %1 for writing.").arg(filename));
        return false;
    }

    auto progress =
        QtCommon::Frontend::newProgressDialog(tr("Downloading..."), tr("Cancel"), 0, 100);
    progress->show();

    QGuiApplication::processEvents();

    UpdateDownload::Callbacks callbacks;
    callbacks.progress = [&](u64 processed_size, u64 total_size) {
        QGuiApplication::processEvents();
        if (total_size != 0) {
            progress->setValue(static_cast<int>((processed_size * 100) / total_size));
        }
        return !progress->wasCanceled();
    };
    callbacks.retry = [&](int attempt, int attempts) {
        progress->setValue(0);
        progress->setLabelText(tr("Downloading... (try %1 of %2)").arg(attempt).arg(attempts));
        QEventLoop wait;
        QTimer::singleShot(2000 * (attempt - 1), &wait, &QEventLoop::quit);
        wait.exec();
        return !progress->wasCanceled();
    };

    const auto outcome = UpdateDownload::Download(
        {m_asset.url, m_asset.path, m_asset.sha256, m_asset.size}, file, callbacks);
    progress->close();

    const auto fail = [&file](const QString& title, const QString& text) {
        file.cancelWriting();
        QtCommon::Frontend::Critical(title, text);
        return false;
    };

    switch (outcome.status) {
    case UpdateDownload::Status::Ok:
        break;
    case UpdateDownload::Status::Canceled:
        file.cancelWriting();
        QtCommon::Frontend::Information(tr("Download Canceled"),
                                        tr("The download was canceled and no file was saved."));
        return false;
    case UpdateDownload::Status::WriteFailed:
        return fail(tr("Failed to save file"), tr("Could not write to file %1.").arg(filename));
    case UpdateDownload::Status::Network:
        return fail(tr("Failed to download file"),
                    tr("Could not download from %1%2\nCheck your internet connection and try "
                       "again.")
                        .arg(QString::fromStdString(m_asset.url),
                             QString::fromStdString(m_asset.path)));
    case UpdateDownload::Status::HttpError:
        return fail(tr("Failed to download file"),
                    tr("Could not download from %1%2\nError code: %3")
                        .arg(QString::fromStdString(m_asset.url),
                             QString::fromStdString(m_asset.path),
                             QString::number(outcome.http_status)));
    case UpdateDownload::Status::BadChecksum:
        return fail(tr("Download failed verification"),
                    tr("The downloaded file does not match the checksum GitHub published for it, "
                       "so it was discarded. Try again later."));
    }

    if (!file.commit()) {
        LOG_WARNING(Frontend, "Could not commit to file {}", filename.toStdString());
        return fail(tr("Failed to save file"), tr("Could not commit to file %1.").arg(filename));
    }

    return true;
}

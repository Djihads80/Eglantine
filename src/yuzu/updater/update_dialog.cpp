// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
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
#include "ui_update_dialog.h"
#include "update_dialog.h"

#include "common/httplib.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

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
    const auto assets = release.GetPlatformAssets();

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

void UpdateDialog::Download() {
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

// TODO: migrate to a net.cpp wrapper
bool UpdateDialog::DownloadTo(const QString& filename) {
    QSaveFile file(filename);
    if (!file.open(QIODevice::Truncate | QIODevice::WriteOnly)) {
        LOG_WARNING(Frontend, "Could not open file {}", filename.toStdString());
        QtCommon::Frontend::Critical(tr("Failed to save file"),
                                     tr("Could not open file %1 for writing.").arg(filename));
        return false;
    }

    // TODO(crueter): Move to net.cpp
    constexpr std::size_t timeout_seconds = 15;

    httplib::Client client(m_asset.url);
    client.set_connection_timeout(timeout_seconds);
    client.set_read_timeout(timeout_seconds);
    client.set_write_timeout(timeout_seconds);
    // GitHub redirects release downloads to another host.
    client.set_follow_location(true);

#ifdef YUZU_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif

    auto progress =
        QtCommon::Frontend::newProgressDialog(tr("Downloading..."), tr("Cancel"), 0, 100);
    progress->show();

    QGuiApplication::processEvents();

    auto progress_callback = [&](size_t processed_size, size_t total_size) {
        QGuiApplication::processEvents();
        if (total_size != 0) {
            progress->setValue(static_cast<int>((processed_size * 100) / total_size));
        }
        return !progress->wasCanceled();
    };

    QCryptographicHash hash(QCryptographicHash::Sha256);
    u64 received = 0;
    bool body_wanted = false;
    bool write_failed = false;

    // A redirect response carries no file, only the final answer is saved.
    auto response_handler = [&body_wanted](const httplib::Response& response) {
        body_wanted = response.status >= 200 && response.status < 300;
        return true;
    };

    auto content_receiver = [&](const char* t_data, size_t data_length) -> bool {
        if (!body_wanted) {
            return true;
        }
        hash.addData(QByteArrayView(t_data, static_cast<qsizetype>(data_length)));
        received += data_length;
        if (file.write(t_data, data_length) == -1) {
            LOG_WARNING(Frontend, "Could not write {} bytes to file {}", data_length,
                        filename.toStdString());
            write_failed = true;
            return false;
        }

        return true;
    };

    // GitHub's file server sometimes answers 502-504 or drops the connection for a moment: try up
    // to three times, a few seconds apart, starting the file over each time.
    constexpr int attempts = 3;
    httplib::Result result;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (attempt > 1) {
            file.seek(0);
            file.resize(0);
            hash.reset();
            received = 0;
            progress->setValue(0);
            progress->setLabelText(tr("Downloading... (try %1 of %2)").arg(attempt).arg(attempts));
            QEventLoop wait;
            QTimer::singleShot(2000 * (attempt - 1), &wait, &QEventLoop::quit);
            wait.exec();
            if (progress->wasCanceled()) {
                break;
            }
        }
        body_wanted = false;
        result = client.Get(m_asset.path, response_handler, content_receiver, progress_callback);
        const bool transient = !write_failed && !progress->wasCanceled() &&
                               ((!result && result.error() != httplib::Error::Canceled) ||
                                (result && (result->status >= 500 || result->status == 429)));
        if (!transient || attempt == attempts) {
            break;
        }
        LOG_WARNING(Frontend, "GET to {}{} failed ({}), trying again", m_asset.url, m_asset.path,
                    result ? std::to_string(result->status) : httplib::to_string(result.error()));
    }
    const bool canceled = progress->wasCanceled();
    progress->close();

    const auto fail = [&file](const QString& title, const QString& text) {
        file.cancelWriting();
        QtCommon::Frontend::Critical(title, text);
        return false;
    };

    if (canceled) {
        file.cancelWriting();
        QtCommon::Frontend::Information(tr("Download Canceled"),
                                        tr("The download was canceled and no file was saved."));
        return false;
    }
    if (write_failed) {
        return fail(tr("Failed to save file"), tr("Could not write to file %1.").arg(filename));
    }
    if (!result) {
        LOG_ERROR(Frontend, "GET to {}{} failed: {}", m_asset.url, m_asset.path,
                  httplib::to_string(result.error()));
        return fail(tr("Failed to download file"),
                    tr("Could not download from %1%2\nCheck your internet connection and try "
                       "again.")
                        .arg(QString::fromStdString(m_asset.url),
                             QString::fromStdString(m_asset.path)));
    }

    const auto& response = result.value();
    if (response.status < 200 || response.status >= 300) {
        LOG_ERROR(Frontend, "GET to {}{} returned error status code: {}", m_asset.url, m_asset.path,
                  response.status);
        return fail(tr("Failed to download file"),
                    tr("Could not download from %1%2\nError code: %3")
                        .arg(QString::fromStdString(m_asset.url),
                             QString::fromStdString(m_asset.path),
                             QString::number(response.status)));
    }

    const bool wrong_size = m_asset.size != 0 && received != m_asset.size;
    const bool wrong_hash = !m_asset.sha256.empty() &&
                            hash.result().toHex() != QByteArray::fromStdString(m_asset.sha256);
    if (wrong_size || wrong_hash) {
        LOG_ERROR(Frontend, "Downloaded {} does not match the published size or SHA-256",
                  m_asset.filename);
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

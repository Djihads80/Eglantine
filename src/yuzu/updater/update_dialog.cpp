// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <QRadioButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/split.hpp>
#include <qdesktopservices.h>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "qt_common/abstract/frontend.h"
#include "qt_common/abstract/progress.h"
#include "qt_common/util/compress.h"
#include "ui_update_dialog.h"
#include "update_dialog.h"

#include "common/httplib.h"
#include "common/scm_rev.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

#include <openssl/evp.h>

#include <QDesktopServices>

#undef GetSaveFileName

static std::string sha256_hash(EVP_MD_CTX* ctx, const std::string& input) {
    char output[65];
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, input.c_str(), input.size());
    EVP_DigestFinal_ex(ctx, hash, &hash_len);

    for (unsigned int i = 0; i < hash_len; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[hash_len * 2] = '\0';

    return std::string(output);
}

UpdateDialog::UpdateDialog(const Common::Net::Release& release, QWidget* parent)
    : QDialog(parent), ui(new Ui::UpdateDialog) {
    ui->setupUi(this);

    ui->version->setText(
        tr("%1 is available for download.").arg(QString::fromStdString(release.name)));
    ui->url->setText(
        tr("<a href=\"%1\">View on Forgejo</a>").arg(QString::fromStdString(release.html_url)));

    std::string text{release.body};
    if (auto pos = text.find("# Packages"); pos != std::string::npos) {
        text = text.substr(0, pos);
    }

    ui->body->setMarkdown(QString::fromStdString(text));

    // TODO(crueter): Find a way to set default
    const auto assets = Common::Net::GetPlatformAssets(release);

    if (assets.empty()) {
        ui->groupBox->setHidden(true);
        connect(this, &QDialog::accepted, this, [release]() {
            qDebug() << release.html_url;
            QDesktopServices::openUrl(QUrl{QString::fromStdString(release.html_url)});
        });
    } else if (assets.size() == 1) {
        ui->groupBox->setHidden(true);
        m_asset = assets[0];

        connect(this, &QDialog::accepted, this, &UpdateDialog::Download);
    } else {
        u32 i = 0;
        for (const Common::Net::NamedAsset& a : assets) {
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

// TODO: migrate progress callback download to Common::Net
// TODO: migrate to QtCommon
void UpdateDialog::Download() {
    namespace fs = std::filesystem;
    fs::path appDir = QCoreApplication::applicationDirPath().toStdString();
    fs::path parentDir = appDir.parent_path();
    std::string appDirName = appDir.filename();
    fs::path oldDir = parentDir / fmt::format("{}_{}", appDirName, Common::g_build_version);

    std::error_code ec;
    fs::path tmpDir = fs::temp_directory_path(ec);

    if (ec) {
        // tmp dir creation failed, just use cache
        LOG_WARNING(Frontend, "Failed to create temporary directory: {}", ec.message());
        tmpDir = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "update/download";
    } else {
        tmpDir /= "eden/update/download";
    }

    fs::remove_all(tmpDir);
    fs::create_directories(tmpDir);

    const auto filename = m_asset.asset.name;
    const auto absFilename = tmpDir / m_asset.asset.name;
    std::ofstream file(absFilename, std::ios::out | std::ios::binary);

    // first 3 will be [protocol, <blank>, host]
    // everything thereafter is the url
    // TODO: just use glaze
    const auto url = m_asset.asset.browser_download_url;
    std::vector<std::string> split;
    boost::algorithm::split(split, url, boost::is_any_of("/"));

    const std::span<std::string> host_slice(split.begin(), split.begin() + 3);
    const std::span<std::string> path_slice(split.begin() + 3, split.end());

    const auto host = boost::algorithm::join(host_slice, "/");
    const auto path = boost::algorithm::join(path_slice, "/");

    std::unique_ptr<httplib::Client> client = std::make_unique<httplib::Client>(host);
    constexpr std::size_t timeout_seconds = 15;
    client->set_connection_timeout(timeout_seconds);
    client->set_read_timeout(timeout_seconds);
    client->set_write_timeout(timeout_seconds);

#ifdef YUZU_BUNDLED_OPENSSL
    client->load_ca_cert_store(kCert, sizeof(kCert));
#endif

    if (client == nullptr) {
        LOG_ERROR(Frontend, "Invalid URL {}", m_asset.asset.browser_download_url);
        return;
    }

    auto progress =
        QtCommon::Frontend::newProgressDialog(tr("Downloading..."), tr("Cancel"), 0, 100);
    progress->show();

    QGuiApplication::processEvents();

    // Progress dialog.
    auto progress_callback = [&](size_t processed_size, size_t total_size) {
        QGuiApplication::processEvents();
        progress->setValue(static_cast<int>((processed_size * 100) / total_size));
        return !progress->wasCanceled();
    };

    // Write file in chunks.
    std::string tmp_data;
    auto content_receiver = [&file, filename, &tmp_data](const char* t_data, size_t data_length) -> bool {
        tmp_data += t_data;
        try {
            file.write(t_data, data_length);
        } catch (std::exception &e) {
            LOG_WARNING(Frontend, "Could not write {} bytes to file {}, error=", data_length,
                        filename, e.what());
            QtCommon::Frontend::Critical(tr("Failed to save file"),
                                         tr("Could not write to file %1.").arg(filename));
            return false;
        }

        return true;
    };

    // Now send off request
    auto result = client->Get(path, content_receiver, progress_callback);

    // commit to file
    try {
        file.flush();
    } catch (std::exception &e) {
        LOG_WARNING(Frontend, "Could not commit to file {}, error={}", filename, e.what());
        QtCommon::Frontend::Critical(tr("Failed to save file"),
                                     tr("Could not commit to file %1.").arg(filename));
        progress->close();
        return;
    }

    if (!result) {
        LOG_ERROR(Frontend, "GET to {} returned null", url);
        progress->close();
        return;
    }

    const auto& response = result.value();
    if (response.status >= 400) {
        LOG_ERROR(Frontend, "GET to {} returned error status code: {}", url,
                  response.status);
        QtCommon::Frontend::Critical(tr("Failed to download file"),
                                     tr("Could not download from %1\nError code: %3")
                                         .arg(QString::fromStdString(url),
                                              QString::number(response.status)));
        progress->close();
        return;
    }

    if (!response.has_header("content-type")) {
        LOG_ERROR(Frontend, "GET to {} returned no content", url);
        progress->close();
        return;
    }

    // TODO(crueter): Test
    if (m_asset.asset.digest) {
        progress->setLabelText(tr("Verifying..."));
        progress->setValue(0);
        progress->setMaximum(0);

        auto ctx = EVP_MD_CTX_new();
        auto actual = std::format("sha256:{}", sha256_hash(ctx, tmp_data));
        auto expected = m_asset.asset.digest.value();

        // TODO: auto-retry
        if (actual != expected) {
            LOG_ERROR(Frontend, "Hash mismatch, expected {}, got {}", expected, actual);
            QtCommon::Frontend::Critical(
                tr("Failed to download file"),
                tr("File has invalid hash.\nGot:%1\nExpected: %2\nPlease re-launch Eden and try "
                   "again.")
                    .arg(QString::fromStdString(expected), QString::fromStdString(actual)));
        }
    }

    progress->close();

    // Download is complete.
    if (filename.ends_with(".dmg") || filename.ends_with(".exe")) {
        // dmg, exe will just open it and let the OS handle the rest.
        auto localUrl = QUrl::fromLocalFile(QString::fromStdString(filename));
        QDesktopServices::openUrl(localUrl);
    } else if (filename.ends_with(".zip")) {
        // zip will request the user to keep the old version or not,
        // then unzip to the current app dir and replace everything.
        auto button = QtCommon::Frontend::Question(
            tr("Download Complete"),
            tr("Would you like to keep the old version of Eden? This will be stored in %1 in the "
               "same directory as your current file.")
                .arg(QString::fromStdString(oldDir.filename())));

        if (button == QtCommon::Frontend::Yes) {
            fs::rename(appDir, oldDir);
        } else {
            fs::remove_all(appDir);
        }

        fs::create_directories(appDir);

        progress->setLabelText(tr("Extracting..."));
        progress->setValue(0);
        progress->setMaximum(100);
        progress->show();
        QtCommon::Compress::extractDir(QString::fromStdString(absFilename.string()),
                                       QString::fromStdString(appDir.string()), progress_callback);
    }
}

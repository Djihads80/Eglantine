// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <QDialog>
#include "common/net/net.h"
#include "frontend_common/update_checker.h"

class QRadioButton;
namespace Ui {
class UpdateDialog;
}

class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    // exec() returns QDialog::Accepted to download, QDialog::Rejected to be reminded later.
    static constexpr int SkipVersion = 2;

    explicit UpdateDialog(const Common::Net::Release& release, UpdateChecker::Channel channel,
                          QWidget* parent = nullptr);
    ~UpdateDialog();

    static QString ChannelName(UpdateChecker::Channel channel);

#if defined(ENABLE_UPDATE_CHECKER) && (defined(__linux__) || defined(_WIN32))
    // Start `target` again once this process has exited, then close `window`.
    static void Restart(QWidget* window, const QString& target);
#endif

#if defined(ENABLE_UPDATE_CHECKER) && defined(_WIN32)
    // Start the installer helper on the files under `staging`, then close `window`.
    static bool LaunchInstaller(QWidget* window, const QString& staging);
#endif

private slots:
    void Download();

private:
    // Download the asset to `filename`, verified against the published size and SHA-256. The file
    // is only written when everything succeeds, and the user is told when it does not.
    bool DownloadTo(const QString& filename);

#if defined(ENABLE_UPDATE_CHECKER) && defined(__linux__)
    // Replace the running AppImage with the new one, keeping the old one for a rollback.
    void InstallInPlace(const std::filesystem::path& target);
#endif
#if defined(ENABLE_UPDATE_CHECKER) && defined(_WIN32)
    // Download and unpack the new version beside the running one; a helper installs it on exit.
    void InstallOnRestart(const std::filesystem::path& exe);
#endif

    Ui::UpdateDialog* ui;
    QList<QRadioButton*> m_buttons;
    Common::Net::Asset m_asset;
};

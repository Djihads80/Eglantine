// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

private slots:
    void Download();

private:
    // Download the asset to `filename`, verified against the published size and SHA-256. The file
    // is only written when everything succeeds, and the user is told when it does not.
    bool DownloadTo(const QString& filename);

    Ui::UpdateDialog* ui;
    QList<QRadioButton*> m_buttons;
    Common::Net::Asset m_asset;
};

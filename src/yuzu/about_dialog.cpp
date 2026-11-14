// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QIcon>
#include <fmt/ranges.h>
#include "common/scm_rev.h"
#include "ui_aboutdialog.h"
#include "yuzu/about_dialog.h"

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent), ui{std::make_unique<Ui::AboutDialog>()} {
    static const std::string build_id = std::string{Common::g_build_id};
    static const std::string yuzu_build = Common::BuildDisplayName();

    const auto override_build =
        fmt::format(fmt::runtime(std::string(Common::g_title_bar_format_idle)), build_id);
    const auto yuzu_build_version = override_build.empty() ? yuzu_build : override_build;

    ui->setupUi(this);
    // Try and request the icon from Qt theme (Linux?)
    const QIcon yuzu_logo = QIcon::fromTheme(QStringLiteral("org.yuzu_emu.yuzu"));
    if (!yuzu_logo.isNull()) {
        ui->labelLogo->setPixmap(yuzu_logo.pixmap(200));
    }
    // Everything the identity does not already carry: the label this run was
    // given, the compiler, and when it was built. Ordered by what gets asked for.
    static const std::string details = fmt::format(
        "{} | {} | {}", std::string{Common::g_build_version}, std::string{Common::g_compiler_id},
        std::string{Common::g_build_date});
    ui->labelBuildInfo->setText(ui->labelBuildInfo->text().arg(
        QString::fromStdString(yuzu_build_version), QString::fromStdString(details)));
}

AboutDialog::~AboutDialog() = default;

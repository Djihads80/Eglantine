// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include "common/scm_rev.h"
#include "frontend_common/update_installer.h"
#include "tester_build_dialog.h"
#include "tester_build_runner.h"

namespace {
constexpr int MaxLogLines = 5000;
}

TesterBuildDialog::TesterBuildDialog(TesterBuildRunner& runner, QWidget* parent)
    : QDialog(parent), m_runner(runner) {
    setWindowTitle(tr("Tester build"));
    setMinimumSize(560, 360);
    resize(680, 440);

    m_source = new QLabel(tr("Repository: %1").arg(QString::fromUtf8(Common::g_build_auto_update_repo)));
    m_step = new QLabel(tr("Starting..."));
    m_step->setWordWrap(true);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 0);
    m_remaining = new QLabel;
    m_step_timer.start();

    m_toggle_log = new QPushButton(tr("Show log"));
    m_toggle_log->setCheckable(true);
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(MaxLogLines);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->hide();

    m_consent = new QWidget;
    m_consent_text = new QLabel;
    m_consent_text->setWordWrap(true);
    m_consent_items = new QPlainTextEdit;
    m_consent_items->setReadOnly(true);
    m_consent_items->setMaximumHeight(90);
    m_consent_command = new QPlainTextEdit;
    m_consent_command->setReadOnly(true);
    m_consent_command->setMaximumHeight(60);
    m_consent_command->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto* consent_layout = new QVBoxLayout(m_consent);
    consent_layout->setContentsMargins(0, 0, 0, 0);
    consent_layout->addWidget(m_consent_text);
    consent_layout->addWidget(m_consent_items);
    consent_layout->addWidget(m_consent_command);
    m_consent->hide();

    m_copy = new QPushButton(tr("Copy"));
    m_accept = new QPushButton;
    m_open_logs = new QPushButton(tr("Open log folder"));
    m_start = new QPushButton(tr("Start tester build"));
    m_cancel = new QPushButton(tr("Cancel"));
    for (auto* button : {m_copy, m_accept, m_open_logs, m_start}) {
        button->hide();
    }

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_open_logs);
    buttons->addStretch();
    buttons->addWidget(m_copy);
    buttons->addWidget(m_accept);
    buttons->addWidget(m_start);
    buttons->addWidget(m_cancel);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_source);
    layout->addWidget(m_step);
    layout->addWidget(m_progress);
    layout->addWidget(m_remaining, 0, Qt::AlignRight);
    layout->addWidget(m_consent);
    layout->addWidget(m_toggle_log, 0, Qt::AlignLeft);
    layout->addWidget(m_log, 1);
    layout->addStretch(0);
    layout->addLayout(buttons);

    connect(m_toggle_log, &QPushButton::toggled, this, [this](bool shown) {
        m_log->setVisible(shown);
        m_toggle_log->setText(shown ? tr("Hide log") : tr("Show log"));
    });
    connect(m_cancel, &QPushButton::clicked, this, &TesterBuildDialog::reject);
    connect(m_copy, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(m_consent_command->toPlainText()); });
    connect(m_accept, &QPushButton::clicked, this, [this] {
        m_consent->hide();
        m_copy->hide();
        m_accept->hide();
        m_runner.Respond(true);
    });
    connect(m_open_logs, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_runner.LogDirectory()));
    });
    connect(m_start, &QPushButton::clicked, this, &TesterBuildDialog::StartBuiltEmulator);

    connect(&m_runner, &TesterBuildRunner::SourceResolved, this, [this](const QString& name) {
        m_source->setText(tr("%1\nRepository: %2")
                              .arg(name, QString::fromUtf8(Common::g_build_auto_update_repo)));
    });
    connect(&m_runner, &TesterBuildRunner::StepStarted, this, [this](const QString& label) {
        m_step->setText(label);
        m_remaining->clear();
        m_step_timer.restart();
    });
    connect(&m_runner, &TesterBuildRunner::LogLine, this,
            [this](const QString& line) { m_pending << line; });
    connect(&m_runner, &TesterBuildRunner::Progress, this, [this](int done, int total) {
        m_progress->setRange(0, total);
        m_progress->setValue(done);
        m_remaining->setText(
            TesterBuildRunner::RemainingText(done, total, m_step_timer.elapsed() / 1000));
    });
    connect(&m_runner, &TesterBuildRunner::ConsentNeeded, this, &TesterBuildDialog::OnConsent);
    connect(&m_runner, &TesterBuildRunner::Finished, this, &TesterBuildDialog::OnFinished);

    m_flush.setInterval(150);
    connect(&m_flush, &QTimer::timeout, this, &TesterBuildDialog::FlushLog);
    m_flush.start();
}

void TesterBuildDialog::reject() {
    if (m_running) {
        m_cancel->setEnabled(false);
        m_step->setText(tr("Canceling..."));
        m_runner.Cancel();
        return;
    }
    QDialog::reject();
}

void TesterBuildDialog::FlushLog() {
    if (m_pending.isEmpty()) {
        return;
    }
    m_log->appendPlainText(m_pending.join(QLatin1Char('\n')));
    m_pending.clear();
}

void TesterBuildDialog::OnConsent(const QStringList& items, const QString& text,
                                  const QString& command, const QString& accept_label) {
    m_consent_text->setText(text);
    m_consent_items->setPlainText(items.join(QLatin1Char('\n')));
    m_consent_command->setPlainText(command);
    m_consent_command->setVisible(!command.isEmpty());
    m_copy->setVisible(!command.isEmpty());
    m_accept->setText(accept_label);
    m_accept->setVisible(!accept_label.isEmpty());
    m_consent->show();
    m_remaining->clear();
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    if (accept_label.isEmpty()) {
        // Nothing to wait for: the runner stops and says why.
        m_runner.Respond(false);
    }
}

void TesterBuildDialog::OnFinished(int result, const QString& message) {
    m_running = false;
    m_flush.stop();
    FlushLog();
    const bool failed = result == TesterBuildRunner::Failed;
    m_step->setText(message);
    m_remaining->clear();
    m_progress->setRange(0, 100);
    m_progress->setValue(failed || result == TesterBuildRunner::Canceled ? 0 : 100);
    m_exit_code = TesterBuildRunner::ExitCode(static_cast<TesterBuildRunner::Result>(result));
    m_cancel->setText(tr("Close"));
    m_cancel->setEnabled(true);
    if (failed) {
        m_toggle_log->setChecked(true);
        m_open_logs->show();
    }
    m_start->setVisible(!m_runner.InstalledExePath().isEmpty() &&
                        (result == TesterBuildRunner::Success ||
                         result == TesterBuildRunner::UpToDate));
}

void TesterBuildDialog::StartBuiltEmulator() {
    const QString exe = m_runner.InstalledExePath();
    QProcess process;
    process.setProgram(exe);
    // A portable user folder is found from the working directory on Linux.
#ifdef _WIN32
    process.setWorkingDirectory(QFileInfo{exe}.absolutePath());
#else
    process.setWorkingDirectory(QDir::currentPath());
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
    process.setProcessEnvironment(environment);
#endif
    if (!process.startDetached()) {
        m_step->setText(tr("Could not start %1.").arg(exe));
        return;
    }
    accept();
}

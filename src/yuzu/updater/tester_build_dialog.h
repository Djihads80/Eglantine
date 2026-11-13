// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <QDialog>
#include <QElapsedTimer>
#include <QStringList>
#include <QTimer>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class TesterBuildRunner;

namespace TesterLauncher {
class CallerProcess;
}

// The window of the tester builder: a thin view over TesterBuildRunner's signals.
class TesterBuildDialog : public QDialog {
    Q_OBJECT

public:
    // `caller_pid` is the Eglantine that started the builder, closed before the new build starts.
    explicit TesterBuildDialog(TesterBuildRunner& runner, qint64 caller_pid = 0,
                               QWidget* parent = nullptr);
    ~TesterBuildDialog() override;

    int ExitCode() const {
        return m_exit_code;
    }

    void reject() override;

private:
    void OnFinished(int result, const QString& message);
    void OnConsent(const QStringList& items, const QString& text, const QString& command,
                   const QString& accept_label);
    void FlushLog();
    void StartBuiltEmulator();
    void LaunchBuiltEmulator();

    TesterBuildRunner& m_runner;
    QLabel* m_source;
    QLabel* m_step;
    QProgressBar* m_progress;
    QLabel* m_remaining;
    QElapsedTimer m_step_timer;
    QPushButton* m_toggle_log;
    QPlainTextEdit* m_log;

    QWidget* m_consent;
    QLabel* m_consent_text;
    QPlainTextEdit* m_consent_items;
    QPlainTextEdit* m_consent_command;
    QPushButton* m_copy;
    QPushButton* m_accept;

    QPushButton* m_open_logs;
    QPushButton* m_start;
    QPushButton* m_cancel;

    std::unique_ptr<TesterLauncher::CallerProcess> m_caller;
    QTimer m_caller_wait;
    int m_caller_polls{};

    QStringList m_pending;
    QTimer m_flush;
    bool m_running{true};
    int m_exit_code{1};
};

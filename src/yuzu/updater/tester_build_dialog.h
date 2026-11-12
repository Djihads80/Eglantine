// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QStringList>
#include <QTimer>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class TesterBuildRunner;

// The window of the tester builder: a thin view over TesterBuildRunner's signals.
class TesterBuildDialog : public QDialog {
    Q_OBJECT

public:
    explicit TesterBuildDialog(TesterBuildRunner& runner, QWidget* parent = nullptr);

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

    TesterBuildRunner& m_runner;
    QLabel* m_source;
    QLabel* m_step;
    QProgressBar* m_progress;
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

    QStringList m_pending;
    QTimer m_flush;
    bool m_running{true};
    int m_exit_code{1};
};

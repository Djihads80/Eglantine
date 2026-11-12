// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <QFutureWatcher>
#include <QString>
#include <QStringList>
#include <QWidget>
#include "frontend_common/tester_build.h"
#include "yuzu/updater/tester_build_launcher.h"

class QComboBox;
class QLabel;
class QPushButton;

// The "Build source" part of the Updates settings. It is bound to tester.json rather than to a
// setting: an older tester build would not know a new setting value and would overwrite it.
class TesterBuildSettings : public QWidget {
    Q_OBJECT

public:
    explicit TesterBuildSettings(QWidget* parent = nullptr);
    ~TesterBuildSettings() override;

    // Called before the configuration dialog closes or applies. Returns false, after telling the
    // user why, when the entered source is not valid. A pull request not seen before first gets a
    // warning; declining it puts the saved source back.
    bool ConfirmChanges();

    // Saves the source, then offers a build or the way back to the release build.
    void Apply();

private:
    std::optional<TesterBuild::Source> EnteredSource() const;
    bool IsTesterChosen() const;
    std::optional<TesterBuild::Source> SavedSource() const;
    void ShowSource(const std::optional<TesterBuild::Source>& source);
    void OnKindChanged();
    void UpdateValidation();
    void UpdateStatus();
    void FillBranches();
    void CheckNow();
    void BuildNow();
    void DeleteBuilds();
    bool WarnAboutPullRequest();
    bool SaveSource(const std::optional<TesterBuild::Source>& source);

    QComboBox* m_kind;
    QWidget* m_tester;
    QComboBox* m_source;
    QLabel* m_message;
    QLabel* m_status;
    QPushButton* m_check;
    QPushButton* m_build;
    QPushButton* m_delete;

    QFutureWatcher<QStringList> m_branches_watcher;
    QFutureWatcher<TesterLauncher::RemoteResult> m_check_watcher;
    QFutureWatcher<quint64> m_size_watcher;
    bool m_branches_requested{};
    // Result of "Check now", for the source it was made for.
    QString m_check_text;
    bool m_up_to_date{};
    bool m_warning_accepted{};
};

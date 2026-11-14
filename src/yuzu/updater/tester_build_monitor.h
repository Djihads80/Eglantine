// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <QFuture>
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include "tester_build_launcher.h"

class QWidget;

// Inside a tester build: looks for a newer commit of the followed branch or pull request and offers
// to build it. The running build is not touched, the new one installs into another folder.
class TesterBuildMonitor : public QObject {
    Q_OBJECT

public:
    // `game_running` is asked before a background check, which stays quiet while a game runs.
    TesterBuildMonitor(std::function<bool()> game_running, QWidget* window);

    // Checks now, then every 30 minutes.
    void Start();

    // A check the user asked for: the result is always reported.
    void CheckNow();

private:
    void Check(bool manual);
    void OnResult();
    void OfferBuild(const QString& name);

    std::function<bool()> m_game_running;
    QWidget* m_window;
    QTimer m_timer;
    QFuture<TesterLauncher::RemoteResult> m_future;
    QFutureWatcher<TesterLauncher::RemoteResult> m_watcher;
    bool m_running{};
    bool m_manual{};
    std::optional<TesterBuild::Source> m_source;
    std::string m_offered_sha;
};

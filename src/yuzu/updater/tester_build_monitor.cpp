// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QMessageBox>
#include <QPushButton>
#include <QtConcurrent>
#include "qt_common/config/uisettings.h"
#include "tester_build_monitor.h"

namespace {
constexpr int CheckIntervalMilliseconds = 30 * 60 * 1000;
}

TesterBuildMonitor::TesterBuildMonitor(std::function<bool()> game_running, QWidget* window)
    : QObject(window), m_game_running(std::move(game_running)), m_window(window) {
    connect(&m_watcher, &QFutureWatcher<TesterLauncher::RemoteResult>::finished, this,
            &TesterBuildMonitor::OnResult);
    m_timer.setInterval(CheckIntervalMilliseconds);
    connect(&m_timer, &QTimer::timeout, this, [this] { Check(false); });
}

void TesterBuildMonitor::Start() {
    Check(false);
    m_timer.start();
}

void TesterBuildMonitor::CheckNow() {
    Check(true);
}

void TesterBuildMonitor::Check(bool manual) {
    if (m_running) {
        m_manual |= manual;
        return;
    }
    if (!manual && (!UISettings::values.check_for_updates.GetValue() || m_game_running())) {
        return;
    }
    const auto state = TesterBuild::LoadState(TesterLauncher::StateFile());
    if (!state.source) {
        if (manual) {
            QMessageBox::information(m_window, tr("Check for Updates"),
                                     tr("No branch or pull request is being followed."));
        }
        return;
    }
    m_running = true;
    m_manual = manual;
    m_source = state.source;
    m_future = QtConcurrent::run(
        [source = *state.source] { return TesterLauncher::FetchRemote(source); });
    m_watcher.setFuture(m_future);
}

void TesterBuildMonitor::OnResult() {
    const bool manual = std::exchange(m_manual, false);
    m_running = false;
    const auto result = m_future.result();
    const auto title = tr("Check for Updates");

    if (!result.info) {
        if (manual) {
            QMessageBox::warning(
                m_window, title,
                result.status != 0
                    ? tr("GitHub answered with HTTP error %1. Please try again later.")
                          .arg(result.status)
                    : tr("Could not reach GitHub. Check your internet connection and try again."));
        }
        return;
    }
    // The builder may have saved a newer build since the check started.
    const auto state = TesterBuild::LoadState(TesterLauncher::StateFile());
    if (state.source != m_source) {
        // The followed source changed in settings while the check ran: the result describes the
        // old one, so ask again for the new one.
        Check(manual);
        return;
    }
    const auto name = QString::fromStdString(state.source->DisplayName());
    if (result.info->sha == state.built_sha) {
        if (manual) {
            QMessageBox::information(m_window, title,
                                     tr("%1 is up to date (%2).")
                                         .arg(name, QString::fromStdString(TesterBuild::ShortSha(
                                                        state.built_sha))));
        }
        return;
    }
    if (manual || result.info->sha != m_offered_sha) {
        m_offered_sha = result.info->sha;
        OfferBuild(name);
    }
}

void TesterBuildMonitor::OfferBuild(const QString& name) {
    auto* box = new QMessageBox(QMessageBox::Question, tr("Tester build"),
                                tr("%1 has a new commit. Build it now?").arg(name), {}, m_window);
    box->setAttribute(Qt::WA_DeleteOnClose);
    auto* build = box->addButton(tr("Build"), QMessageBox::AcceptRole);
    box->addButton(tr("Later"), QMessageBox::RejectRole);
    connect(build, &QPushButton::clicked, this, [] {
        const auto state = TesterBuild::LoadState(TesterLauncher::StateFile());
        TesterLauncher::StartBuilder(TesterLauncher::BuilderExe(state), false);
    });
    box->setModal(false);
    box->show();
}

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <QDialog>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace VideoCommon::ShaderWarm {
struct Result;
} // namespace VideoCommon::ShaderWarm

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

/// True when the title has pre-compiled shaders (a shader precache file).
bool ShaderWarmFilesExist(u64 title_id);

/// Removes the title's pre-compiled shaders. Returns false and fills `error` on failure.
bool DeleteShaderWarmFiles(u64 title_id, std::string& error);

/// Pre-compiles a game's shaders from its files on a worker thread (the shader precache builder)
/// and shows the game's icon, a progress bar animated between the builder's reports and the time
/// left. Closing it while it works stops the build first.
class ShaderWarmDialog : public QDialog {
    Q_OBJECT

public:
    explicit ShaderWarmDialog(Core::System& system, std::filesystem::path game_path, u64 title_id,
                              QWidget* parent = nullptr);
    ~ShaderWarmDialog() override;

    void Start();
    bool IsRunning() const {
        return running;
    }
    /// The title whose shaders this dialog pre-compiles.
    u64 TitleId() const {
        return title_id;
    }

signals:
    void BuildFinished(bool ok);

protected:
    void reject() override;

private:
    struct Reported {
        QString stage;
        size_t done{};
        size_t total{};
        size_t programs{};
        double fraction{};
    };

    void Tick();
    void UpdateTimeLeft(double fraction, std::chrono::steady_clock::time_point now);
    void OnFinished(const VideoCommon::ShaderWarm::Result& result);

    Core::System& system;
    std::filesystem::path game_path;
    u64 title_id;

    QLabel* stage_label{};
    QProgressBar* progress_bar{};
    QLabel* time_label{};
    QLabel* result_label{};
    QPushButton* button{};
    QTimer* timer{};

    std::mutex reported_mutex;
    std::optional<Reported> reported; ///< Latest report of the worker, under reported_mutex

    double shown_fraction{};
    std::chrono::steady_clock::time_point start_time{};
    std::chrono::steady_clock::time_point last_tick{};
    std::chrono::steady_clock::time_point last_time_left{};
    /// (seconds since start, fraction) of the last reports, for the rate behind the time left.
    std::deque<std::pair<double, double>> samples;
    std::optional<double> seconds_left;

    bool running{};
    bool stopping{};
    std::jthread worker;
};

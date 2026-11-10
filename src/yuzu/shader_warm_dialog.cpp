// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <exception>

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "common/fs/path_util.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "yuzu/shader_warm_dialog.h"

namespace ShaderWarm = VideoCommon::ShaderWarm;

namespace {

constexpr int ProgressSteps = 10000;
constexpr int IconSize = 96;
/// Time constant of the bar's approach to the latest report.
constexpr double SmoothingSeconds = 0.35;
/// Reports the rate behind the time left is measured over.
constexpr double RateWindowSeconds = 15.0;

/// The game's title and icon as its loader reports them; the file name when it has no title.
std::pair<QString, QPixmap> ReadGameInfo(Core::System& system,
                                         const std::filesystem::path& game_path) {
    const std::string path = Common::FS::PathToUTF8String(game_path);
    QString name = QFileInfo(QString::fromStdString(path)).completeBaseName();
    QPixmap icon;
    FileSys::RealVfsFilesystem filesystem;
    if (const auto loader =
            Loader::GetLoader(system, filesystem.OpenFile(path, FileSys::OpenMode::Read))) {
        std::string title;
        if (loader->ReadTitle(title) == Loader::ResultStatus::Success && !title.empty()) {
            name = QString::fromStdString(title);
        }
        std::vector<u8> bytes;
        if (loader->ReadIcon(bytes) == Loader::ResultStatus::Success) {
            icon.loadFromData(bytes.data(), static_cast<u32>(bytes.size()));
        }
    }
    return {name, icon};
}

QString StageText(const QString& stage, size_t done, size_t total, size_t programs) {
    const QLocale locale;
    if (stage == QStringLiteral("Extracting shaders")) {
        return ShaderWarmDialog::tr("Reading shader archive %1 of %2 · %3 shaders found")
            .arg(locale.toString(qulonglong{std::min(done + 1, total)}))
            .arg(locale.toString(qulonglong{total}))
            .arg(locale.toString(qulonglong{programs}));
    }
    if (stage == QStringLiteral("Converting")) {
        return ShaderWarmDialog::tr("Converting shaders: %1 of %2")
            .arg(locale.toString(qulonglong{done}))
            .arg(locale.toString(qulonglong{total}));
    }
    if (stage == QStringLiteral("Writing")) {
        return ShaderWarmDialog::tr("Saving…");
    }
    return ShaderWarmDialog::tr("Opening the game files…");
}

QString DurationText(double seconds) {
    const int total = static_cast<int>(std::lround(seconds));
    if (total >= 3600) {
        return ShaderWarmDialog::tr("%1 h %2 min").arg(total / 3600).arg((total % 3600) / 60);
    }
    if (total >= 60) {
        return ShaderWarmDialog::tr("%1 min %2 s").arg(total / 60).arg(total % 60, 2, 10,
                                                                         QLatin1Char('0'));
    }
    return ShaderWarmDialog::tr("%1 s").arg(total);
}

} // Anonymous namespace

bool ShaderWarmFilesExist(u64 title_id) {
    return ShaderWarm::HasWarmFile(title_id);
}

bool DeleteShaderWarmFiles(u64 title_id, std::string& error) {
    return ShaderWarm::DeleteWarmFiles(title_id, &error);
}

ShaderWarmDialog::ShaderWarmDialog(Core::System& system_, std::filesystem::path game_path_,
                                   u64 title_id_, QWidget* parent)
    : QDialog(parent), system{system_}, game_path{std::move(game_path_)}, title_id{title_id_} {
    const auto [name, icon] = ReadGameInfo(system, game_path);
    setWindowTitle(tr("Pre-compile Shaders"));
    if (!icon.isNull()) {
        setWindowIcon(QIcon(icon));
    }
    setMinimumWidth(480);

    auto* const icon_label = new QLabel(this);
    icon_label->setFixedSize(IconSize, IconSize);
    if (!icon.isNull()) {
        icon_label->setPixmap(
            icon.scaled(IconSize, IconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    auto* const title_label = new QLabel(name, this);
    QFont title_font = title_label->font();
    title_font.setPointSizeF(title_font.pointSizeF() * 1.3);
    title_font.setBold(true);
    title_label->setFont(title_font);
    title_label->setWordWrap(true);

    stage_label = new QLabel(StageText({}, 0, 0, 0), this);
    stage_label->setWordWrap(true);

    progress_bar = new QProgressBar(this);
    progress_bar->setRange(0, ProgressSteps);
    progress_bar->setValue(0);
    progress_bar->setFormat(QStringLiteral("%p%"));

    time_label = new QLabel(tr("Estimating time left…"), this);
    QPalette dim = time_label->palette();
    dim.setColor(QPalette::WindowText, dim.color(QPalette::PlaceholderText));
    time_label->setPalette(dim);

    result_label = new QLabel(this);
    result_label->setWordWrap(true);
    result_label->hide();

    auto* const text_column = new QVBoxLayout;
    text_column->addWidget(title_label);
    text_column->addWidget(stage_label);
    text_column->addSpacing(6);
    text_column->addWidget(progress_bar);
    text_column->addWidget(time_label);
    text_column->addStretch();

    auto* const top = new QHBoxLayout;
    top->setSpacing(16);
    top->addWidget(icon_label, 0, Qt::AlignTop);
    top->addLayout(text_column, 1);

    button = new QPushButton(tr("Cancel"), this);
    auto* const buttons = new QDialogButtonBox(this);
    buttons->addButton(button, QDialogButtonBox::RejectRole);
    connect(button, &QPushButton::clicked, this, &ShaderWarmDialog::reject);

    auto* const layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(result_label);
    layout->addWidget(buttons);

    timer = new QTimer(this);
    timer->setInterval(16);
    connect(timer, &QTimer::timeout, this, &ShaderWarmDialog::Tick);
}

ShaderWarmDialog::~ShaderWarmDialog() {
    // The jthread asks the build to stop and joins it.
    worker.request_stop();
}

void ShaderWarmDialog::Start() {
    if (running) {
        return;
    }
    running = true;
    stopping = false;
    start_time = std::chrono::steady_clock::now();
    last_tick = start_time;
    timer->start();
    worker = std::jthread([this](std::stop_token stop) {
        const auto on_progress = [this](const ShaderWarm::Progress& progress) {
            std::scoped_lock lock{reported_mutex};
            reported = Reported{QString::fromStdString(progress.stage), progress.done,
                                progress.total, progress.programs, progress.fraction};
        };
        ShaderWarm::Result result;
        try {
            result = ShaderWarm::BuildWarmFiles(system, game_path, title_id, on_progress, stop);
        } catch (const std::exception& e) {
            result.ok = false;
            result.error = e.what();
        }
        QMetaObject::invokeMethod(
            this, [this, result] { OnFinished(result); }, Qt::QueuedConnection);
    });
}

void ShaderWarmDialog::reject() {
    if (!running) {
        QDialog::reject();
        return;
    }
    if (!stopping) {
        stopping = true;
        worker.request_stop();
        button->setEnabled(false);
        button->setText(tr("Cancelling…"));
    }
}

void ShaderWarmDialog::Tick() {
    std::optional<Reported> latest;
    {
        std::scoped_lock lock{reported_mutex};
        latest = reported;
    }
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last_tick).count();
    last_tick = now;
    if (!latest) {
        return;
    }
    if (!stopping) {
        stage_label->setText(StageText(latest->stage, latest->done, latest->total, latest->programs));
    }
    // Ease towards the latest report, so the bar moves continuously between the builder's reports.
    const double target = std::clamp(latest->fraction, 0.0, 1.0);
    shown_fraction += (target - shown_fraction) * (1.0 - std::exp(-dt / SmoothingSeconds));
    progress_bar->setValue(static_cast<int>(std::lround(shown_fraction * ProgressSteps)));
    UpdateTimeLeft(target, now);
}

void ShaderWarmDialog::UpdateTimeLeft(double fraction, std::chrono::steady_clock::time_point now) {
    const double elapsed = std::chrono::duration<double>(now - start_time).count();
    if (samples.empty() || samples.back().second != fraction) {
        samples.emplace_back(elapsed, fraction);
    }
    while (samples.size() > 2 && elapsed - samples.front().first > RateWindowSeconds) {
        samples.pop_front();
    }
    // Twice a second: an estimate that changes every frame can't be read.
    if (now - last_time_left < std::chrono::milliseconds{500}) {
        return;
    }
    const double previous_tick =
        std::chrono::duration<double>(now - last_time_left).count();
    last_time_left = now;
    if (stopping) {
        time_label->setText(tr("Cancelling…"));
        return;
    }
    const double window = samples.size() >= 2 ? samples.back().first - samples.front().first : 0.0;
    const double progress = samples.size() >= 2 ? samples.back().second - samples.front().second : 0.0;
    if (elapsed < 3.0 || fraction < 0.01 || window < 2.0 || progress <= 0.0) {
        if (seconds_left) {
            // No new estimate: keep counting the last one down.
            *seconds_left = std::max(0.0, *seconds_left - previous_tick);
        } else {
            time_label->setText(tr("Estimating time left…  ·  %1 elapsed").arg(DurationText(elapsed)));
            return;
        }
    } else {
        const double estimate = (1.0 - fraction) * window / progress;
        seconds_left = seconds_left ? 0.7 * std::max(0.0, *seconds_left - previous_tick) +
                                          0.3 * estimate
                                    : estimate;
    }
    const QString left = *seconds_left < 10.0
                             ? tr("A few seconds left")
                             : tr("About %1 left").arg(DurationText(*seconds_left > 60.0
                                                                        ? std::round(*seconds_left / 10.0) * 10.0
                                                                        : std::round(*seconds_left / 5.0) * 5.0));
    time_label->setText(tr("%1  ·  %2 elapsed").arg(left, DurationText(elapsed)));
}

void ShaderWarmDialog::OnFinished(const ShaderWarm::Result& result) {
    if (worker.joinable()) {
        worker.join();
    }
    running = false;
    timer->stop();
    const bool cancelled = stopping;
    stopping = false;
    emit BuildFinished(result.ok);
    if (cancelled) {
        QDialog::reject();
        return;
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    button->setEnabled(true);
    button->setText(tr("Close"));
    time_label->setText(tr("Took %1").arg(DurationText(elapsed)));
    result_label->show();
    if (!result.ok) {
        progress_bar->setValue(static_cast<int>(std::lround(shown_fraction * ProgressSteps)));
        stage_label->setText(tr("Pre-compiling failed."));
        result_label->setText(QString::fromStdString(result.error));
        return;
    }
    shown_fraction = 1.0;
    progress_bar->setValue(ProgressSteps);
    stage_label->setText(tr("Done."));
    if (result.compute_programs == 0) {
        result_label->setText(tr("No compute shaders were found in this game's files."));
        return;
    }
    const QLocale locale;
    result_label->setText(
        tr("%1 compute shaders are ready. They are compiled into the GPU driver's cache in the "
           "background the next time you start the game, so the game doesn't wait for them later.")
            .arg(locale.toString(qulonglong{result.compute_programs})));
}

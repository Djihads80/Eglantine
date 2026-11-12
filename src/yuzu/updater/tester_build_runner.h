// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <QByteArray>
#include <QFile>
#include <QObject>
#include <QProcessEnvironment>
#include <QSemaphore>
#include <QString>
#include <QStringList>
#include "frontend_common/tester_build.h"

// Runs a tester build from fetching the source to installing it. Run() blocks and is meant for a
// worker thread; everything it reports comes out as signals, so a window and the console front end
// only have to listen.
class TesterBuildRunner : public QObject {
    Q_OBJECT

public:
    enum Result { Success, UpToDate, Failed, Canceled };

    struct Options {
        QString source;
        // Rebuild even when the fetched commit is the one already built.
        bool force{};
        // The user agreed beforehand to install missing tools (--yes).
        bool yes{};
        // No window: elevate with sudo and use the terminal.
        bool console{};
    };

    explicit TesterBuildRunner(const Options& options, QObject* parent = nullptr);

    void Run();

    // Thread-safe: stops the running process tree and makes Run() return Canceled.
    void Cancel();

    // Thread-safe: the answer to ConsentNeeded; true installs or checks again.
    void Respond(bool accept);

    static int ExitCode(Result result);

    // Valid once Run() has returned.
    QString InstalledExePath() const;
    QString LogDirectory() const;
    QStringList LogTail(int lines) const;

signals:
    void SourceResolved(const QString& name);
    void StepStarted(const QString& label);
    void LogLine(const QString& line);
    // `total` is 0 while there is nothing to measure.
    void Progress(int done, int total);
    // `accept_label` is empty when there is nothing to accept and the build must stop.
    void ConsentNeeded(const QStringList& items, const QString& text, const QString& command,
                       const QString& accept_label);
    void Finished(int result, const QString& message);

private:
    struct Missing {
        // Windows: the recipe's "provides" names. Linux: package names, or tool names when the
        // distribution is unknown.
        std::vector<std::string> provides;
        std::vector<std::string> packages;

        bool Empty() const {
            return provides.empty() && packages.empty();
        }
    };

    Result Execute();
    bool Prepare();
    bool EnsureTools();
    bool EnsureTool(const TesterBuild::ToolDownload& tool);
    bool Download(const TesterBuild::ToolDownload& tool, const std::filesystem::path& archive);
    bool Extract(const std::filesystem::path& archive, const std::filesystem::path& staging);
    bool EnsureToolchain();
    bool Detect(Missing& missing);
    void DetectWindows();
    bool DetectLinux(Missing& missing);
    bool Install(const Missing& missing, bool& manual);
    bool InstallWindows(const Missing& missing);
    bool InstallLinux(const Missing& missing, bool& manual);
    bool Consent(const QStringList& items, const QString& text, const QString& command,
                 const QString& accept_label);
    bool Fetch(std::string& sha);
    bool Build();
    bool Finish(const std::string& sha);
    void LinkUserFolder(const std::filesystem::path& install);

    TesterBuild::BuildRequest MakeRequest() const;
    QProcessEnvironment BuildEnvironment() const;

    // The exit code, or nothing when the process could not start or the build was canceled.
    std::optional<int> RunStep(const TesterBuild::Step& step, const QProcessEnvironment& env,
                               QByteArray* captured = nullptr);
    static void KillTree(qint64 pid);
    // Fails with a message unless the step ran and exited with 0.
    bool CheckStep(const TesterBuild::Step& step, const std::optional<int>& code);

    bool IsCanceled() const {
        return m_canceled;
    }
    bool Fail(const QString& message);
    void Log(const QString& line);
    void HandleLine(const QString& line, bool capturing);
    void PruneLogs() const;
    bool SaveState();

    Options m_options;
    std::atomic<bool> m_canceled{false};
    QSemaphore m_reply;
    std::atomic<bool> m_reply_accept{false};

    QString m_message;
    QFile m_log;
    std::deque<QString> m_tail;

    TesterBuild::Platform m_platform;
    TesterBuild::Layout m_layout;
    std::filesystem::path m_state_path;
    TesterBuild::State m_state;
    TesterBuild::Source m_source;
    TesterBuild::Recipe m_recipe;
    TesterBuild::Toolchain m_tools;
    TesterBuild::PackageManager m_manager{TesterBuild::PackageManager::None};
    bool m_packages_verified{true};

    QProcessEnvironment m_base_env;
    QProcessEnvironment m_vs_env;
    bool m_have_vs_env{};
    // A Visual Studio is installed, but older than the recipe's minimum.
    bool m_old_vs{};
    std::filesystem::path m_vulkan_sdk;
    std::filesystem::path m_installed_exe;
    int m_last_percent{-1};
};

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QThread>
#include <QTimer>
#include "frontend_common/tester_build.h"
#include "frontend_common/update_installer.h"
#include "tester_build_dialog.h"
#include "tester_build_main.h"
#include "tester_build_runner.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

namespace {

std::atomic<bool> g_interrupted{false};

#ifdef _WIN32
// This is a GUI-subsystem program: print to the terminal it was started from, unless its output
// is already redirected.
void AttachParentConsole() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((out == nullptr || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
}

BOOL WINAPI OnConsoleEvent(DWORD) {
    g_interrupted = true;
    return TRUE;
}

void InstallInterruptHandler() {
    SetConsoleCtrlHandler(OnConsoleEvent, TRUE);
}
#else
void InstallInterruptHandler() {
    std::signal(SIGINT, [](int) { g_interrupted = true; });
    std::signal(SIGTERM, [](int) { g_interrupted = true; });
}
#endif

void Print(const QString& text) {
    const auto bytes = text.toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

struct Arguments {
    TesterBuildRunner::Options options;
};

Arguments ParseArguments(int argc, char** argv) {
    Arguments arguments;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--console") == 0) {
            arguments.options.console = true;
        } else if (std::strcmp(arg, "--force") == 0) {
            arguments.options.force = true;
        } else if (std::strcmp(arg, "--yes") == 0) {
            arguments.options.yes = true;
        } else if (std::strcmp(arg, "--source") == 0 && i + 1 < argc) {
            arguments.options.source = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strncmp(arg, "--source=", 9) == 0) {
            arguments.options.source = QString::fromLocal8Bit(arg + 9);
        }
    }
    return arguments;
}

void Report(bool console, const QString& text) {
    if (console) {
        Print(text);
    } else {
        QMessageBox::critical(nullptr, QObject::tr("Tester build"), text);
    }
}

int RunConsole(TesterBuildRunner& runner, const TesterBuildRunner::Options& options) {
    int exit_code = 1;
    int last_percent = -1;
    QElapsedTimer step_timer;
    step_timer.start();
    const auto* context = QCoreApplication::instance();
    QObject::connect(&runner, &TesterBuildRunner::SourceResolved, context,
                     [](const QString& name) { Print(QObject::tr("Source: %1").arg(name)); });
    QObject::connect(&runner, &TesterBuildRunner::StepStarted, context, [&](const QString& label) {
        last_percent = -1;
        step_timer.restart();
        Print(label);
    });
    QObject::connect(&runner, &TesterBuildRunner::Progress, context, [&](int done, int total) {
        if (total == 0) {
            return;
        }
        const int percent = done * 100 / total;
        if (percent != last_percent) {
            last_percent = percent;
            const auto left =
                TesterBuildRunner::RemainingText(done, total, step_timer.elapsed() / 1000);
            Print(QStringLiteral("  %1% (%2/%3)").arg(percent).arg(done).arg(total) +
                  (left.isEmpty() ? QString{} : QStringLiteral(", ") + left));
        }
    });
    QObject::connect(&runner, &TesterBuildRunner::ConsentNeeded, context,
                     [&](const QStringList& items, const QString& text, const QString& command,
                         const QString& accept_label) {
                         Print(text);
                         for (const auto& item : items) {
                             Print(QStringLiteral("  - ") + item);
                         }
                         if (!command.isEmpty()) {
                             Print(command);
                         }
                         runner.Respond(options.yes && !accept_label.isEmpty());
                     });
    QObject::connect(&runner, &TesterBuildRunner::Finished, context,
                     [&](int result, const QString& message) {
                         if (result == TesterBuildRunner::Failed) {
                             Print(QObject::tr("Last lines of the log:"));
                             for (const auto& line : runner.LogTail(30)) {
                                 Print(QStringLiteral("  ") + line);
                             }
                         }
                         Print(message);
                         exit_code =
                             TesterBuildRunner::ExitCode(static_cast<TesterBuildRunner::Result>(result));
                         QCoreApplication::exit(exit_code);
                     });

    InstallInterruptHandler();
    QTimer interrupt;
    QObject::connect(&interrupt, &QTimer::timeout, context, [&] {
        if (g_interrupted) {
            runner.Cancel();
        }
    });
    interrupt.start(200);

    std::unique_ptr<QThread> thread{QThread::create([&runner] { runner.Run(); })};
    thread->start();
    QCoreApplication::exec();
    thread->wait();
    return exit_code;
}

} // namespace

int RunTesterBuild(int argc, char** argv) {
    const auto arguments = ParseArguments(argc, argv);
    const bool console = arguments.options.console;
#ifdef _WIN32
    if (console) {
        AttachParentConsole();
    }
#endif

    std::optional<QCoreApplication> core;
    std::optional<QApplication> gui;
    if (console) {
        core.emplace(argc, argv);
    } else {
        gui.emplace(argc, argv);
    }
    QCoreApplication::setOrganizationName(QStringLiteral("eden"));
    QCoreApplication::setApplicationName(QStringLiteral("Eglantine"));

    const auto root = TesterBuild::DefaultRoot();
    std::error_code ec;
    if (!root.empty()) {
        std::filesystem::create_directories(root, ec);
    }
    if (root.empty() || ec) {
        Report(console, QObject::tr("Could not find a folder to build in. Set EGLANTINE_TESTER_ROOT."));
        return 1;
    }
    const UpdateInstaller::Lock lock{root / "builder.lock"};
    if (!lock.Held()) {
        Report(console, QObject::tr("A tester build is already running."));
        return 1;
    }

    TesterBuildRunner runner{arguments.options};
    if (console) {
        return RunConsole(runner, arguments.options);
    }

    TesterBuildDialog dialog{runner};
    std::unique_ptr<QThread> thread{QThread::create([&runner] { runner.Run(); })};
    thread->start();
    dialog.exec();
    runner.Cancel();
    thread->wait();
    return dialog.ExitCode();
}

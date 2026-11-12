// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <QString>
#include <QStringList>
#include "frontend_common/tester_build.h"

// The release exe's side of tester builds: it is always the entry point, and hands over to the
// installed tester build when the user follows a branch or pull request.
namespace TesterLauncher {

// tester.json, in the config folder.
std::filesystem::path StateFile();

// The exe to start for a plain relaunch of this build: $APPIMAGE inside an AppImage.
QString SelfExe();

// Whether this exe lies in the tester install folder.
bool IsTesterBuild();

// The exe that runs the builder: always the release build, as the followed branch may be older than
// the builder. Inside a tester build that is the one the state remembers.
QString BuilderExe(const TesterBuild::State& state);

struct RemoteResult {
    // Empty when the check failed; `status` is then the HTTP status, or 0 without a response.
    std::optional<TesterBuild::RemoteInfo> info;
    int status{};
};

// Blocking: callers run it on a worker thread.
RemoteResult FetchRemote(const TesterBuild::Source& source);

// Starts a program that outlives this process. On Linux the AppImage environment is removed first,
// since the program is not part of this AppImage.
bool StartDetached(const QString& program, const QStringList& args,
                   const QString& working_dir = {});

// `<exe> --tester-build`, which shows the builder window.
bool StartBuilder(const QString& exe, bool force, const QString& source = {});

// Called at the start of main(), once it is known this is not a helper process. Returns the exit
// code when the launcher handled this start (it never falls through into the emulator), and
// nothing when the emulator should start normally.
std::optional<int> Run(int argc, char** argv);

} // namespace TesterLauncher

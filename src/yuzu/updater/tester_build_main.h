// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// `eglantine --tester-build [--console] [--source <text>] [--force] [--yes]`. Starts nothing of the
// emulator. Returns the exit code: 0 on success or already up to date, 1 on failure, 2 when canceled.
int RunTesterBuild(int argc, char** argv);

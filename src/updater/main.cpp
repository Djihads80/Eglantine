// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Installs a staged update for Windows. Eglantine copies this program to a temporary folder and starts
// it before closing, because the running eglantine.exe and its DLLs cannot be replaced.
//
// eglantine-updater.exe <process id> <staging folder> <install folder> <program to start>

#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <windows.h>

#include <shellapi.h>

#include "frontend_common/update_installer.h"

namespace fs = std::filesystem;

namespace {

void Log(const fs::path& install_dir, const std::string& message) {
    std::ofstream{install_dir / ".update-helper.log", std::ios::app} << message << '\n';
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc != 5) {
        return 2;
    }
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 10));
    const fs::path staging{argv[2]};
    const fs::path install_dir{argv[3]};
    const fs::path program{argv[4]};
    LocalFree(argv);

    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        const DWORD wait = WaitForSingleObject(process, 120000);
        CloseHandle(process);
        if (wait != WAIT_OBJECT_0) {
            Log(install_dir, "Eglantine did not close in time, the update was not installed");
            if (staging.filename() == ".update-staging") {
                std::error_code ignored;
                fs::remove_all(staging, ignored);
            }
            MessageBoxW(nullptr,
                        L"Eglantine did not close in time, so the update was not installed. Your "
                        L"installation was not changed. Try the update again.",
                        L"Eglantine update", MB_OK | MB_ICONINFORMATION);
            return 1;
        }
    }

    // Eglantine released the lock when it handed over, so another window may have taken it since.
    const UpdateInstaller::Lock lock{UpdateInstaller::LockPath(program)};
    if (!lock.Held()) {
        Log(install_dir, "Another update holds the lock, the update was not installed");
        MessageBoxW(nullptr,
                    L"Another Eglantine window is updating this installation, so this update was "
                    L"not installed. Start Eglantine again once it has finished.",
                    L"Eglantine update", MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    // Files an earlier failed install could not put back; Install would refuse, so say why.
    if (UpdateInstaller::HasPendingBackup(install_dir)) {
        Log(install_dir, "Files from an earlier failed update are in .update-backup.tmp, the "
                         "update was not installed");
        if (staging.filename() == ".update-staging") {
            std::error_code ignored;
            fs::remove_all(staging, ignored);
        }
        MessageBoxW(nullptr,
                    L"An earlier update could not put back some files of your previous version. "
                    L"They are kept in .update-backup.tmp in the Eglantine folder. Copy them back "
                    L"from there, or delete that folder if you no longer need them, then try the "
                    L"update again. Your installation was not changed.",
                    L"Eglantine update", MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    std::error_code ec;
    const bool installed = UpdateInstaller::Install(staging, install_dir, ec);
    if (installed) {
        Log(install_dir, "Installed the staged update");
    } else if (std::error_code ignored;
               fs::exists(install_dir / UpdateInstaller::PendingBackupName, ignored)) {
        Log(install_dir,
            "Install failed, some previous files are still in .update-backup.tmp: " + ec.message());
        MessageBoxW(nullptr,
                    L"Eglantine could not install the update, and some files of your previous "
                    L"version could not be put back. They are kept in .update-backup.tmp in the "
                    L"Eglantine folder. Copy them back from there, or download Eglantine again. "
                    L"Your settings and data are not affected.",
                    L"Eglantine update", MB_OK | MB_ICONERROR);
        return 1;
    } else {
        Log(install_dir, "Install failed, the previous files were put back: " + ec.message());
        MessageBoxW(nullptr,
                    L"Eglantine could not install the update. Your previous version was put back and "
                    L"will start now. See .update-helper.log in the Eglantine folder for details.",
                    L"Eglantine update", MB_OK | MB_ICONERROR);
    }

    std::wstring command = L"\"" + program.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        install_dir.c_str(), &startup, &info)) {
        Log(install_dir, "Could not start the program after the update");
        return 1;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return installed ? 0 : 1;
}

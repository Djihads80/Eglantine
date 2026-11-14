// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include "common/fs/path_util.h"
#include "common/net/net.h"
#include "common/scm_rev.h"
#include "frontend_common/config.h"
#include "frontend_common/update_installer.h"
#include "qt_common/config/uisettings.h"
#include "tester_build_launcher.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace TesterLauncher {

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr auto RemoteTimeout = 5s;
constexpr int WatchMilliseconds = 10'000;

QString ToQ(const fs::path& path) {
    return QString::fromUtf8(TesterBuild::PathToUtf8(path));
}

fs::path SelfPath() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(size);
    return fs::path{buffer};
#elif defined(__linux__)
    // Inside an AppImage /proc/self/exe is in the mount, not where the file was started from.
    if (const auto image = qEnvironmentVariable("APPIMAGE"); !image.isEmpty()) {
        return fs::path{image.toStdString()};
    }
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#else
    return fs::path{QCoreApplication::applicationFilePath().toStdString()};
#endif
}

QStringList Arguments(int argc, [[maybe_unused]] char** argv) {
    QStringList args;
#ifdef _WIN32
    // argv is in the ANSI code page, which loses characters of a path.
    int count = 0;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) {
            args << QString::fromWCharArray(wide[i]);
        }
        LocalFree(wide);
        return args;
    }
#endif
    for (int i = 1; i < argc; ++i) {
        args << QString::fromLocal8Bit(argv[i]);
    }
    return args;
}

#ifndef _WIN32
QProcessEnvironment ChildEnvironment() {
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto app_dir = environment.value(QStringLiteral("APPDIR"));
    if (!app_dir.isEmpty()) {
        for (const auto& key : environment.keys()) {
            const auto value = environment.value(key);
            if (!value.contains(app_dir)) {
                continue;
            }
            const auto stripped = UpdateInstaller::StripPathListEntries(value.toStdString(),
                                                                        app_dir.toStdString());
            if (stripped.empty()) {
                environment.remove(key);
            } else {
                environment.insert(key, QString::fromStdString(stripped));
            }
        }
    }
    for (const auto& key : {"APPDIR", "APPIMAGE", "ARGV0", "OWD"}) {
        environment.remove(QString::fromLatin1(key));
    }
    return environment;
}
#endif

bool ShiftHeld() {
#ifdef _WIN32
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
    return false;
#endif
}

// Starts `program` and waits up to `timeout_ms` for it to end, without a QCoreApplication so the
// silent handoff stays cheap. Returns false when it could not be started; `exit_code` is set when
// it ended within the time.
bool StartAndWatch(const QString& program, const QStringList& args, int timeout_ms,
                   std::optional<int>& exit_code) {
#ifdef _WIN32
    qint64 pid = 0;
    if (!QProcess::startDetached(program, args, QString{}, &pid)) {
        return false;
    }
    if (HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     static_cast<DWORD>(pid))) {
        DWORD code = 0;
        if (WaitForSingleObject(process, static_cast<DWORD>(timeout_ms)) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(process, &code)) {
            exit_code = static_cast<int>(code);
        }
        CloseHandle(process);
    }
    return true;
#else
    std::vector<QByteArray> storage{program.toLocal8Bit()};
    for (const auto& arg : args) {
        storage.push_back(arg.toLocal8Bit());
    }
    std::vector<QByteArray> environment_storage;
    for (const auto& entry : ChildEnvironment().toStringList()) {
        environment_storage.push_back(entry.toLocal8Bit());
    }
    std::vector<char*> argv, envp;
    for (auto& text : storage) {
        argv.push_back(text.data());
    }
    for (auto& text : environment_storage) {
        envp.push_back(text.data());
    }
    argv.push_back(nullptr);
    envp.push_back(nullptr);

    // The write end closes on a successful exec, so EOF on the read end means the exec worked and
    // anything read is the errno of the failed one.
    int exec_pipe[2];
    if (pipe(exec_pipe) != 0) {
        return false;
    }
    // Not pipe2: macOS does not have it.
    fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);
    const pid_t pid = fork();
    if (pid < 0) {
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        return false;
    }
    if (pid == 0) {
        setsid();
        execve(argv[0], argv.data(), envp.data());
        const int error = errno;
        [[maybe_unused]] const auto written = write(exec_pipe[1], &error, sizeof(error));
        _exit(127);
    }
    close(exec_pipe[1]);
    int exec_error = 0;
    ssize_t got;
    do {
        got = read(exec_pipe[0], &exec_error, sizeof(exec_error));
    } while (got < 0 && errno == EINTR);
    close(exec_pipe[0]);
    if (got > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return false;
    }
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return true;
#endif
}

struct PendingRemote {
    std::mutex mutex;
    std::condition_variable done_signal;
    bool done{};
    RemoteResult result;
};

// A slow check is abandoned rather than waited for: its thread is detached and ends with the
// process.
std::optional<TesterBuild::RemoteInfo> CheckRemote(const TesterBuild::Source& source) {
    const auto pending = std::make_shared<PendingRemote>();
    std::thread{[pending, source] {
        auto result = FetchRemote(source);
        std::lock_guard lock{pending->mutex};
        pending->result = std::move(result);
        pending->done = true;
        pending->done_signal.notify_all();
    }}.detach();

    std::unique_lock lock{pending->mutex};
    if (!pending->done_signal.wait_for(lock, RemoteTimeout, [&] { return pending->done; })) {
        return std::nullopt;
    }
    return pending->result.info;
}

bool CheckEnabled() {
    Config::ReadGlobalSettingEarly("qt-config", UISettings::values.check_for_updates);
    return UISettings::values.check_for_updates.GetValue();
}

} // namespace

fs::path StateFile() {
    return TesterBuild::StatePath(Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir));
}

QString SelfExe() {
    const auto image = qEnvironmentVariable("APPIMAGE");
    return image.isEmpty() ? ToQ(SelfPath()) : image;
}

bool IsTesterBuild() {
    const auto root = TesterBuild::DefaultRoot();
    return !root.empty() && TesterBuild::IsTesterInstall(SelfPath(), root);
}

QString BuilderExe(const TesterBuild::State& state) {
    if (IsTesterBuild()) {
        const auto release = QString::fromUtf8(state.release_exe);
        if (!release.isEmpty() && QFile::exists(release)) {
            return release;
        }
    }
    return SelfExe();
}

RemoteResult FetchRemote(const TesterBuild::Source& source) {
    const auto fetched = source.kind == TesterBuild::Source::Kind::Branch
                             ? Common::Net::GetBranchHead(source.branch)
                             : Common::Net::GetPullRequestHead(source.pr);
    RemoteResult result;
    result.status = fetched.status;
    if (fetched.head) {
        result.info = TesterBuild::RemoteInfo{fetched.head->sha, fetched.head->title,
                                              fetched.head->author, fetched.head->open,
                                              fetched.head->merged};
    }
    return result;
}

bool StartDetached(const QString& program, const QStringList& args, const QString& working_dir) {
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    if (!working_dir.isEmpty()) {
        process.setWorkingDirectory(working_dir);
    }
#ifndef _WIN32
    process.setProcessEnvironment(ChildEnvironment());
#endif
    return process.startDetached();
}

bool StartBuilder(const QString& exe, bool force, const QString& source, bool close_caller) {
    QStringList args{QStringLiteral("--tester-build")};
    if (force) {
        args << QStringLiteral("--force");
    }
    if (!source.isEmpty()) {
        args << QStringLiteral("--source") << source;
    }
    if (close_caller) {
        args << QStringLiteral("--caller") << QString::number(QCoreApplication::applicationPid());
    }
    return StartDetached(exe, args);
}

#ifdef _WIN32
CallerProcess::CallerProcess(qint64 pid)
    : m_handle(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(pid))) {}

CallerProcess::~CallerProcess() {
    if (m_handle) {
        CloseHandle(m_handle);
    }
}

bool CallerProcess::Running() const {
    return m_handle && WaitForSingleObject(m_handle, 0) == WAIT_TIMEOUT;
}

void CallerProcess::RequestClose() const {
    if (!Running()) {
        return;
    }
    // Only the windows without an owner: the main window, not its dialogs.
    DWORD pid = GetProcessId(m_handle);
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            DWORD owner_pid = 0;
            GetWindowThreadProcessId(window, &owner_pid);
            if (owner_pid == *reinterpret_cast<DWORD*>(param) && IsWindowVisible(window) &&
                !GetWindow(window, GW_OWNER)) {
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&pid));
}
#else
CallerProcess::CallerProcess(qint64 pid) {
#ifdef SYS_pidfd_open
    m_fd = static_cast<int>(syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0));
#else
    (void)pid;
#endif
}

CallerProcess::~CallerProcess() {
    if (m_fd >= 0) {
        ::close(m_fd);
    }
}

bool CallerProcess::Running() const {
    if (m_fd < 0) {
        return false;
    }
    // A pidfd turns readable once the process has exited.
    pollfd fd{m_fd, POLLIN, 0};
    return poll(&fd, 1, 0) == 0;
}

void CallerProcess::RequestClose() const {
#ifdef SYS_pidfd_send_signal
    if (Running()) {
        // The main window closes on SIGTERM.
        syscall(SYS_pidfd_send_signal, m_fd, SIGTERM, nullptr, 0);
    }
#endif
}
#endif

std::optional<int> Run(int argc, char** argv) {
    const auto args = Arguments(argc, argv);
    auto state = TesterBuild::LoadState(StateFile());
    const auto root = TesterBuild::DefaultRoot();
    const bool self_is_tester = !root.empty() && TesterBuild::IsTesterInstall(SelfPath(), root);

    // A CI build of a branch follows that branch, once per branch.
    const std::string follow = Common::g_build_follow_branch;
    if (!follow.empty() && !self_is_tester && state.auto_source != follow) {
        if (const auto source = TesterBuild::ParseSource("branch:" + follow,
                                                         Common::g_build_auto_update_repo)) {
            TesterBuild::SetSource(state, source);
            state.auto_source = follow;
            std::error_code ec;
            TesterBuild::SaveState(StateFile(), state, ec);
        }
    }
    if (!state.source) {
        return std::nullopt;
    }
    // This exe is the build of its own commit when it was made for the followed branch.
    std::optional<std::string> self_commit;
    if (!follow.empty() && state.source->kind == TesterBuild::Source::Kind::Branch &&
        state.source->branch == follow) {
        self_commit = TesterBuild::ParseRevParse(Common::g_scm_rev);
    }
    const bool release_flag = args.contains(QStringLiteral("--release")) || ShiftHeld();
    const bool install_exists = TesterBuild::HasInstall(state);

    using Action = TesterBuild::LaunchAction;
    // With an install the answer without a remote only depends on the flags, which is all that
    // is needed to know whether anything has to be checked.
    if (TesterBuild::DecideLaunch(state, self_is_tester, release_flag, true, std::nullopt) ==
        Action::RunRelease) {
        return std::nullopt;
    }
    std::optional<TesterBuild::RemoteInfo> remote;
    if ((install_exists || self_commit) && CheckEnabled()) {
        remote = CheckRemote(*state.source);
    }
    const auto action = TesterBuild::DecideLaunch(state, self_is_tester, release_flag,
                                                  install_exists, remote, self_commit);
    if (action == Action::RunRelease) {
        return std::nullopt;
    }

    const auto name = QString::fromStdString(state.source->DisplayName());
    const QString self = SelfExe();
    const QString install_exe =
        install_exists
            ? ToQ(TesterBuild::InstalledExe(TesterBuild::PathFromUtf8(state.current_install)))
            : QString{};

    // Only a prompt needs a QApplication.
    std::optional<QApplication> app;
    // Returns the index of the clicked button; closing the window gives `on_close`.
    const auto ask = [&](const QString& text, const QStringList& buttons, int on_close) {
        if (!app) {
#ifdef _WIN32
            QApplication::setStyle(QStringLiteral("windowsvista"));
#endif
            app.emplace(const_cast<int&>(argc), argv);
        }
        QMessageBox box{QMessageBox::Question, QObject::tr("Eglantine"), text};
        std::vector<QAbstractButton*> created;
        for (const auto& label : buttons) {
            created.push_back(box.addButton(label, QMessageBox::ActionRole));
        }
        box.setDefaultButton(static_cast<QPushButton*>(created[0]));
        box.setEscapeButton(created[static_cast<std::size_t>(on_close)]);
        box.exec();
        for (std::size_t i = 0; i < created.size(); ++i) {
            if (box.clickedButton() == created[i]) {
                return static_cast<int>(i);
            }
        }
        return on_close;
    };
    const auto start_release = [&] {
        if (!StartDetached(self, QStringList{QStringLiteral("--release")} + args)) {
            ask(QObject::tr("Could not start %1.").arg(self), {QObject::tr("Close")}, 0);
        }
        return 0;
    };
    const auto start_builder = [&](bool force) {
        if (!StartBuilder(self, force, {}, false)) {
            ask(QObject::tr("Could not start %1.").arg(self), {QObject::tr("Close")}, 0);
        }
        return 0;
    };
    // `watch` is false when the user was already asked something and the build just runs.
    const auto start_tester = [&](bool watch) {
        std::optional<int> exit_code;
        const bool started =
            StartAndWatch(install_exe, args, watch ? WatchMilliseconds : 0, exit_code);
        if (started && (!exit_code || *exit_code == 0)) {
            return 0;
        }
        const auto text = started ? QObject::tr("The tester build closed right after starting.")
                                  : QObject::tr("The tester build could not be started.");
        switch (ask(text,
                    {QObject::tr("Rebuild"), QObject::tr("Use release build"),
                     QObject::tr("Close")},
                    2)) {
        case 0:
            return start_builder(true);
        case 1:
            return start_release();
        default:
            return 0;
        }
    };

    switch (action) {
    case Action::OfferFirstBuild:
        return ask(QObject::tr("No tester build of %1 is installed yet.").arg(name),
                   {QObject::tr("Build now"), QObject::tr("Use release build")}, 1) == 0
                   ? start_builder(false)
                   : start_release();
    case Action::OfferUpdate: {
        const auto old_sha = install_exists ? state.built_sha : self_commit.value_or(std::string{});
        const auto text = QObject::tr("%1 has a new commit (%2 -> %3).")
                              .arg(name, QString::fromStdString(TesterBuild::ShortSha(old_sha)),
                                   QString::fromStdString(TesterBuild::ShortSha(remote->sha)));
        if (!install_exists) {
            // Nothing installed to fall back on: this exe is the build to keep using.
            return ask(text, {QObject::tr("Build now"), QObject::tr("Later")}, 1) == 0
                       ? start_builder(false)
                       : start_release();
        }
        switch (ask(text,
                    {QObject::tr("Build now"), QObject::tr("Later"),
                     QObject::tr("Use release build")},
                    1)) {
        case 0:
            return start_builder(false);
        case 2:
            return start_release();
        default:
            return start_tester(false);
        }
    }
    case Action::OfferClosedPullRequest: {
        const auto text = (remote->pr_merged ? QObject::tr("%1 was merged.")
                                             : QObject::tr("%1 was closed."))
                              .arg(name);
        if (ask(text,
                {QObject::tr("Keep using it"),
                 QObject::tr("Use release build and stop following")},
                0) == 1) {
            state.source.reset();
            std::error_code ec;
            TesterBuild::SaveState(StateFile(), state, ec);
            return start_release();
        }
        return start_tester(false);
    }
    case Action::StartTester:
    case Action::RunRelease:
        break;
    }
    return start_tester(true);
}

} // namespace TesterLauncher

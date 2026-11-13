// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QThread>
#include <QVersionNumber>
#include "common/fs/path_util.h"
#include "common/scm_rev.h"
#include "frontend_common/update_checker.h"
#include "qt_common/config/uisettings.h"
#include "download.h"
#include "tester_build_launcher.h"
#include "qt_common/util/compress.h"
#include "tester_build_runner.h"

#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr int MaxLogs = 5;
constexpr std::size_t MaxTail = 200;

QString Q(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string Utf8(const QString& text) {
    return text.toStdString();
}

QString ToQ(const fs::path& path) {
    return Q(TesterBuild::PathToUtf8(path));
}

fs::path ToPath(const QString& text) {
    return TesterBuild::PathFromUtf8(Utf8(text));
}

// Process output is in the console's code page on Windows.
std::string OutputUtf8(const QByteArray& output) {
    return QString::fromLocal8Bit(output).toStdString();
}

#ifdef _WIN32
constexpr QChar PathSeparator = QLatin1Char(';');
#else
constexpr QChar PathSeparator = QLatin1Char(':');
#endif

std::string PathEnv(const QProcessEnvironment& env) {
    return Utf8(env.value(QStringLiteral("PATH")));
}

void PrependPath(QProcessEnvironment& env, const std::vector<fs::path>& dirs) {
    QStringList entries;
    for (const auto& dir : dirs) {
        if (!dir.empty()) {
            entries << QDir::toNativeSeparators(ToQ(dir));
        }
    }
    const auto old = env.value(QStringLiteral("PATH"));
    if (!old.isEmpty()) {
        entries << old;
    }
    env.insert(QStringLiteral("PATH"), entries.join(PathSeparator));
}

bool SameFile(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec) && !ec;
}

} // namespace

TesterBuildRunner::TesterBuildRunner(const Options& options, QObject* parent)
    : QObject(parent), m_options(options), m_platform(TesterBuild::CurrentPlatform()),
      m_download_platform(
          UpdateChecker::DownloadPlatform(UISettings::values.update_variant.GetValue())),
      m_base_env(QProcessEnvironment::systemEnvironment()) {
    // A missing login must fail the clone, not wait for a prompt nobody can see.
    m_base_env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
}

int TesterBuildRunner::ExitCode(Result result) {
    switch (result) {
    case Success:
    case UpToDate:
        return 0;
    case Canceled:
        return 2;
    case Failed:
        break;
    }
    return 1;
}

QString TesterBuildRunner::RemainingText(int done, int total, qint64 elapsed_seconds) {
    if (done < 0 || total <= 0 || elapsed_seconds < 0) {
        return {};
    }
    const auto seconds = TesterBuild::EstimateRemaining(
        static_cast<u32>(done), static_cast<u32>(total), static_cast<u64>(elapsed_seconds));
    if (!seconds) {
        return {};
    }
    if (*seconds < 60) {
        return tr("less than a minute left");
    }
    const auto minutes = (*seconds + 59) / 60;
    if (minutes < 60) {
        return tr("about %1 min left").arg(minutes);
    }
    return tr("about %1 h %2 min left").arg(minutes / 60).arg(minutes % 60);
}

QString TesterBuildRunner::InstalledExePath() const {
    return ToQ(m_installed_exe);
}

QString TesterBuildRunner::LogDirectory() const {
    return ToQ(m_layout.logs);
}

QStringList TesterBuildRunner::LogTail(int lines) const {
    QStringList tail;
    const auto count = static_cast<std::size_t>(std::max(lines, 0));
    for (auto it = m_tail.size() > count ? m_tail.end() - static_cast<std::ptrdiff_t>(count)
                                         : m_tail.begin();
         it != m_tail.end(); ++it) {
        tail << *it;
    }
    return tail;
}

void TesterBuildRunner::Cancel() {
    m_canceled = true;
    m_reply.release();
}

void TesterBuildRunner::Respond(bool accept) {
    m_reply_accept = accept;
    m_reply.release();
}

void TesterBuildRunner::Run() {
    const auto result = Execute();
    if (m_message.isEmpty()) {
        m_message = result == Canceled ? tr("Canceled.") : tr("Done.");
    }
    Log(m_message);
    m_log.close();
    emit Finished(result, m_message);
}

bool TesterBuildRunner::Fail(const QString& message) {
    if (m_message.isEmpty()) {
        m_message = message;
    }
    return false;
}

void TesterBuildRunner::Log(const QString& line) {
    if (m_log.isOpen()) {
        m_log.write(line.toUtf8() + '\n');
        m_log.flush();
    }
    m_tail.push_back(line);
    if (m_tail.size() > MaxTail) {
        m_tail.pop_front();
    }
    emit LogLine(line);
}

void TesterBuildRunner::HandleLine(const QString& line, bool capturing) {
    if (line.isEmpty()) {
        return;
    }
    if (!capturing) {
        Log(line);
    }
    if (const auto progress = TesterBuild::ParseNinjaProgress(Utf8(line))) {
        emit Progress(static_cast<int>(progress->first), static_cast<int>(progress->second));
    }
}

bool TesterBuildRunner::CheckStep(const TesterBuild::Step& step, const std::optional<int>& code) {
    if (IsCanceled()) {
        return false;
    }
    if (!code) {
        return Fail(tr("Could not start %1.").arg(ToQ(step.program.filename())));
    }
    if (*code != 0) {
        return Fail(tr("%1 failed with exit code %2. The end of the log is below.")
                        .arg(Q(step.label), QString::number(*code)));
    }
    return true;
}

void TesterBuildRunner::KillTree(qint64 pid) {
#ifdef _WIN32
    // cmake's ninja and the compilers survive killing the direct child.
    QProcess killer;
    killer.setProgram(QStringLiteral("taskkill"));
    killer.setArguments({QStringLiteral("/T"), QStringLiteral("/F"), QStringLiteral("/PID"),
                         QString::number(pid)});
    killer.setStandardOutputFile(QProcess::nullDevice());
    killer.setStandardErrorFile(QProcess::nullDevice());
    killer.start();
    killer.waitForFinished(15000);
#else
    ::kill(-static_cast<pid_t>(pid), SIGTERM);
    QThread::msleep(300);
    ::kill(-static_cast<pid_t>(pid), SIGKILL);
#endif
}

std::optional<int> TesterBuildRunner::RunStep(const TesterBuild::Step& step,
                                              const QProcessEnvironment& env,
                                              QByteArray* captured) {
    QString program = ToQ(step.program);
    QStringList args;
    for (const auto& arg : step.args) {
        args << Q(arg);
    }
#ifndef _WIN32
    if (step.elevated) {
        const auto resolved = TesterBuild::FindProgram(Utf8(program), PathEnv(env));
        const auto elevator = TesterBuild::FindProgram(m_options.console ? "sudo" : "pkexec",
                                                       PathEnv(env));
        args.prepend(resolved.empty() ? program : ToQ(resolved));
        program = elevator.empty() ? QString::fromLatin1(m_options.console ? "sudo" : "pkexec")
                                   : ToQ(elevator);
    }
#endif
    Log(QStringLiteral("$ ") + program + QLatin1Char(' ') + args.join(QLatin1Char(' ')));

    QProcess process;
    process.setProcessEnvironment(env);
    if (!step.cwd.empty()) {
        process.setWorkingDirectory(ToQ(step.cwd));
    }
    process.setProgram(program);
#ifdef _WIN32
    // A command line meant for cmd.exe has its own quotes, which Qt would escape.
    if (QFileInfo{program}.fileName().compare(QStringLiteral("cmd.exe"), Qt::CaseInsensitive) ==
        0) {
        process.setNativeArguments(args.join(QLatin1Char(' ')));
    } else {
        process.setArguments(args);
    }
#else
    process.setArguments(args);
    // Its own process group, so that canceling reaches everything it started.
    process.setChildProcessModifier([] { ::setsid(); });
    if (step.elevated && m_options.console) {
        process.setInputChannelMode(QProcess::ForwardedInputChannel);
    }
#endif

    process.start();
    if (!process.waitForStarted(30000)) {
        Log(tr("Could not start: %1").arg(process.errorString()));
        return std::nullopt;
    }
    const qint64 pid = process.processId();

    QByteArray out_pending;
    QByteArray err_pending;
    const auto split = [this](QByteArray& pending, const QByteArray& data, bool quiet) {
        pending.append(data);
        for (;;) {
            const auto end = pending.indexOf('\n');
            const auto cr = pending.indexOf('\r');
            const auto cut = end < 0 ? cr : (cr < 0 ? end : std::min(end, cr));
            if (cut < 0) {
                break;
            }
            HandleLine(QString::fromLocal8Bit(pending.left(cut)), quiet);
            pending.remove(0, cut + 1);
        }
    };
    const auto drain = [&] {
        const auto out = process.readAllStandardOutput();
        if (captured) {
            captured->append(out);
        }
        split(out_pending, out, captured != nullptr);
        split(err_pending, process.readAllStandardError(), false);
    };

    while (process.state() != QProcess::NotRunning) {
        if (!process.waitForReadyRead(100)) {
            process.waitForFinished(20);
        }
        drain();
        if (IsCanceled()) {
            KillTree(pid);
            process.kill();
            process.waitForFinished(10000);
            return std::nullopt;
        }
    }
    drain();
    split(out_pending, "\n", captured != nullptr);
    split(err_pending, "\n", false);

    const int code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : 1;
    if (code != 0) {
        Log(tr("Exited with code %1").arg(code));
    }
    return code;
}

bool TesterBuildRunner::SaveState() {
    std::error_code ec;
    if (!TesterBuild::SaveState(m_state_path, m_state, ec)) {
        return Fail(tr("Could not save %1: %2")
                        .arg(ToQ(m_state_path), QString::fromStdString(ec.message())));
    }
    return true;
}

void TesterBuildRunner::PruneLogs() const {
    const QDir dir{ToQ(m_layout.logs)};
    const auto names = dir.entryList({QStringLiteral("build-*.log")}, QDir::Files,
                                     QDir::Name | QDir::Reversed);
    for (int i = MaxLogs; i < names.size(); ++i) {
        QFile::remove(dir.filePath(names[i]));
    }
}

bool TesterBuildRunner::Prepare() {
    const auto root = TesterBuild::DefaultRoot();
    if (root.empty()) {
        return Fail(tr("Could not find a folder to build in. Set EGLANTINE_TESTER_ROOT."));
    }
    m_layout = TesterBuild::MakeLayout(root);
    std::error_code ec;
    fs::create_directories(m_layout.tools, ec);
    if (!ec) {
        fs::create_directories(m_layout.logs, ec);
    }
    if (ec) {
        return Fail(tr("Could not create %1: %2")
                        .arg(ToQ(root), QString::fromStdString(ec.message())));
    }

    m_log.setFileName(ToQ(m_layout.logs) + QStringLiteral("/build-") +
                      QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
                      QStringLiteral(".log"));
    if (!m_log.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return Fail(tr("Could not create the log file %1.").arg(m_log.fileName()));
    }
    PruneLogs();
    Log(tr("Tester build in %1").arg(ToQ(root)));

    m_state_path = TesterBuild::StatePath(
        Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir));
    m_state = TesterBuild::LoadState(m_state_path);
    if (!m_options.source.isEmpty()) {
        const auto source =
            TesterBuild::ParseSource(Utf8(m_options.source), Common::g_build_auto_update_repo);
        if (!source) {
            return Fail(tr("\"%1\" is not a branch name or pull request.").arg(m_options.source));
        }
        m_state.source = source;
    }
    if (!m_state.source) {
        return Fail(tr("No branch or pull request is set. Pass --source <branch or PR number>."));
    }
    m_source = *m_state.source;
    emit SourceResolved(Q(m_source.DisplayName()));

    // Where "Release" in the settings leads back to, unless this is itself a tester build.
    const auto image = qEnvironmentVariable("APPIMAGE");
    const auto self = ToPath(image.isEmpty() ? QCoreApplication::applicationFilePath() : image);
    if (!TesterBuild::IsTesterInstall(self, root)) {
        m_state.release_exe = Utf8(ToQ(self));
    }
    if (!SaveState()) {
        return false;
    }

    const auto recipe =
        TesterBuild::ParseRecipe(TesterBuild::EmbeddedRecipe(), m_platform);
    if (!recipe) {
        return Fail(tr("The build recipe is not valid."));
    }
    m_recipe = *recipe;
    return true;
}

bool TesterBuildRunner::EnsureTools() {
    for (const auto& tool : m_recipe.tools) {
        if (!EnsureTool(tool)) {
            return false;
        }
    }
    if (m_tools.cmake.empty()) {
        m_tools.cmake = TesterBuild::FindProgram("cmake", PathEnv(m_base_env));
    }
    if (m_tools.ninja.empty()) {
        m_tools.ninja = TesterBuild::FindProgram("ninja", PathEnv(m_base_env));
    }
    if (m_tools.cmake.empty() || m_tools.ninja.empty()) {
        return Fail(tr("The recipe does not provide CMake and Ninja."));
    }
    return true;
}

bool TesterBuildRunner::EnsureTool(const TesterBuild::ToolDownload& tool) {
    const auto dir = m_layout.tools / (tool.name + "-" + tool.version);
    const auto bin = dir / TesterBuild::PathFromUtf8(tool.bin);
    const auto exe_name = m_platform == TesterBuild::Platform::Windows ? tool.name + ".exe"
                                                                         : tool.name;
    const auto remember = [&](const fs::path& bin_dir) {
        if (tool.name == "cmake") {
            m_tools.cmake = bin_dir / exe_name;
        } else if (tool.name == "ninja") {
            m_tools.ninja = bin_dir / exe_name;
        } else if (tool.name == "glslang") {
            // The build looks for glslangValidator, which the glslang releases no longer ship
            // under that name.
            const auto validator = bin_dir / (m_platform == TesterBuild::Platform::Windows
                                                  ? "glslangValidator.exe"
                                                  : "glslangValidator");
            std::error_code copy_ec;
            if (!fs::exists(validator, copy_ec)) {
                fs::copy_file(bin_dir / exe_name, validator, copy_ec);
            }
            if (fs::is_regular_file(validator, copy_ec)) {
                m_portable_glslang = validator;
            }
        }
    };
    std::error_code ec;
    if (fs::is_regular_file(bin / exe_name, ec)) {
        remember(bin);
        return true;
    }

    emit StepStarted(tr("Downloading %1 %2").arg(Q(tool.name), Q(tool.version)));
    emit Progress(0, 0);
    const auto filename = tool.url.substr(tool.url.rfind('/') + 1);
    const auto archive = m_layout.tools / TesterBuild::PathFromUtf8(filename);
    const auto staging = fs::path{dir}.concat(".tmp");
    const auto cleanup = [&] {
        std::error_code ignored;
        fs::remove(archive, ignored);
        fs::remove_all(staging, ignored);
    };
    cleanup();
    if (!Download(tool.url, tool.sha256, 0, archive)) {
        cleanup();
        return false;
    }

    emit StepStarted(tr("Unpacking %1 %2").arg(Q(tool.name), Q(tool.version)));
    emit Progress(0, 0);
    fs::create_directories(staging, ec);
    if (ec || !Extract(archive, staging) || !fs::is_regular_file(staging / TesterBuild::PathFromUtf8(tool.bin) / exe_name, ec)) {
        cleanup();
        return Fail(tr("Could not unpack %1.").arg(Q(filename)));
    }
#ifndef _WIN32
    const auto staged_exe = ToQ(staging / TesterBuild::PathFromUtf8(tool.bin) / exe_name);
    QFile::setPermissions(staged_exe, QFile::permissions(staged_exe) | QFile::ExeOwner |
                                          QFile::ExeGroup | QFile::ExeOther);
#endif
    fs::remove_all(dir, ec);
    fs::rename(staging, dir, ec);
    if (ec) {
        cleanup();
        return Fail(tr("Could not install %1: %2")
                        .arg(Q(tool.name), QString::fromStdString(ec.message())));
    }
    fs::remove(archive, ec);
    remember(bin);
    return true;
}

bool TesterBuildRunner::Download(const std::string& url_text, const std::string& sha256, u64 size,
                                 const fs::path& archive) {
    const auto url = Q(url_text);
    const auto host_end = url.indexOf(QLatin1Char('/'), url.indexOf(QStringLiteral("://")) + 3);
    if (host_end < 0) {
        return Fail(tr("The download address %1 is not valid.").arg(url));
    }
    Log(tr("Downloading %1").arg(url));

    QSaveFile file{ToQ(archive)};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return Fail(tr("Could not write %1.").arg(ToQ(archive)));
    }

    UpdateDownload::Callbacks callbacks;
    callbacks.progress = [this](u64 processed, u64 total) {
        if (total != 0) {
            const auto percent = static_cast<int>(processed * 100 / total);
            if (percent != m_last_percent) {
                m_last_percent = percent;
                emit Progress(percent, 100);
            }
        }
        return !IsCanceled();
    };
    callbacks.retry = [this](int attempt, int attempts) {
        Log(tr("Download interrupted, trying again (%1 of %2)").arg(attempt).arg(attempts));
        for (int i = 0; i < 20 * (attempt - 1) && !IsCanceled(); ++i) {
            QThread::msleep(100);
        }
        return !IsCanceled();
    };

    m_last_percent = -1;
    const auto outcome = UpdateDownload::Download(
        {Utf8(url.left(host_end)), Utf8(url.mid(host_end)), sha256, size}, file, callbacks);
    switch (outcome.status) {
    case UpdateDownload::Status::Ok:
        break;
    case UpdateDownload::Status::Canceled:
        file.cancelWriting();
        return false;
    case UpdateDownload::Status::WriteFailed:
        file.cancelWriting();
        return Fail(tr("Could not write %1.").arg(ToQ(archive)));
    case UpdateDownload::Status::Network:
        file.cancelWriting();
        Log(tr("Download error: %1").arg(Q(outcome.error)));
        return Fail(tr("Could not download %1. Check your internet connection and try again.")
                        .arg(url));
    case UpdateDownload::Status::HttpError:
        file.cancelWriting();
        return Fail(tr("Could not download %1 (error %2).").arg(url).arg(outcome.http_status));
    case UpdateDownload::Status::BadChecksum:
        file.cancelWriting();
        return Fail(tr("The download of %1 does not match the expected SHA-256, so it was "
                       "discarded.")
                        .arg(url));
    }
    if (!file.commit()) {
        return Fail(tr("Could not write %1.").arg(ToQ(archive)));
    }
    Log(tr("Verified SHA-256 %1").arg(Q(sha256)));
    return true;
}

bool TesterBuildRunner::Extract(const fs::path& archive, const fs::path& staging) {
    const auto name = TesterBuild::PathToUtf8(archive.filename());
    if (name.ends_with(".zip")) {
        const auto files = QtCommon::Compress::extractDir(
            ToQ(archive), ToQ(staging), [this](std::size_t total, std::size_t done) {
                if (total != 0) {
                    emit Progress(static_cast<int>(done * 100 / total), 100);
                }
                return !IsCanceled();
            });
        return !files.isEmpty();
    }
    if (name.ends_with(".tar.gz")) {
        const TesterBuild::Step step{"untar",
                                     "Unpacking",
                                     TesterBuild::FindProgram("tar", PathEnv(m_base_env)),
                                     {"-xzf", TesterBuild::PathToUtf8(archive), "-C",
                                      TesterBuild::PathToUtf8(staging)},
                                     {},
                                     false,
                                     false};
        return RunStep(step, m_base_env) == 0;
    }
    return false;
}

bool TesterBuildRunner::EnsureToolchain() {
    bool installed = false;
    for (;;) {
        Missing missing;
        if (!Detect(missing)) {
            return false;
        }
        if (missing.Empty()) {
            return true;
        }
        if (installed) {
            QStringList names;
            for (const auto& name : missing.provides) {
                names << Q(name);
            }
            for (const auto& name : missing.packages) {
                names << Q(name);
            }
            return Fail(tr("Still missing after installing: %1. Install them yourself and run "
                           "the tester build again.")
                            .arg(names.join(QStringLiteral(", "))));
        }
        bool manual = false;
        if (!Install(missing, manual)) {
            return false;
        }
        installed = !manual;
    }
}

bool TesterBuildRunner::Detect(Missing& missing) {
    emit StepStarted(tr("Looking for build tools"));
    emit Progress(0, 0);

    const auto path = PathEnv(m_base_env);
    m_tools.git = TesterBuild::FindProgram("git", path);
    m_tools.glslang = m_portable_glslang.empty()
                          ? TesterBuild::FindProgram("glslangValidator", path)
                          : m_portable_glslang;
    m_vulkan_sdk.clear();
    if (m_platform == TesterBuild::Platform::Windows) {
        // This process's PATH is not refreshed by an install it just ran.
        if (m_tools.git.empty()) {
            for (const auto* variable : {"ProgramFiles", "ProgramW6432"}) {
                const auto base = qEnvironmentVariable(variable);
                if (m_tools.git.empty() && !base.isEmpty()) {
                    m_tools.git = TesterBuild::FindProgram(
                        "git", Utf8(QDir::toNativeSeparators(base + QStringLiteral("/Git/cmd"))));
                }
            }
        }
        if (m_tools.glslang.empty()) {
            std::vector<QString> sdks;
            if (const auto sdk = qEnvironmentVariable("VULKAN_SDK"); !sdk.isEmpty()) {
                sdks.push_back(sdk);
            }
            QDir root{QStringLiteral("C:/VulkanSDK")};
            auto versions = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            std::ranges::sort(versions, [](const QString& a, const QString& b) {
                return QVersionNumber::fromString(a) > QVersionNumber::fromString(b);
            });
            for (const auto& version : versions) {
                sdks.push_back(root.filePath(version));
            }
            for (const auto& sdk : sdks) {
                m_tools.glslang = TesterBuild::FindProgram(
                    "glslangValidator", Utf8(QDir::toNativeSeparators(sdk + QStringLiteral("/Bin"))));
                if (!m_tools.glslang.empty()) {
                    m_vulkan_sdk = ToPath(sdk);
                    break;
                }
            }
        }
        DetectWindows();
        missing.provides = TesterBuild::MissingProvides(m_tools);
        return !IsCanceled();
    }
    return DetectLinux(missing);
}

void TesterBuildRunner::DetectWindows() {
    m_have_vs_env = false;
    m_old_vs = false;
    m_tools.compiler = false;
    if (!fs::exists(TesterBuild::VsWherePath())) {
        return;
    }
    // The prebuilt static Qt only links with a toolset at least as new as the one that built it.
    std::string min_version;
    for (const auto& package : m_recipe.winget) {
        if (package.provides == "msvc") {
            min_version = package.min_version;
        }
    }
    QByteArray output;
    auto code = RunStep(TesterBuild::VsWhereStep(min_version), m_base_env, &output);
    if (!code || *code != 0) {
        return;
    }
    const auto install = TesterBuild::ParseVsWhere(OutputUtf8(output));
    if (install.empty()) {
        if (!min_version.empty()) {
            output.clear();
            const auto any = RunStep(TesterBuild::VsWhereStep(), m_base_env, &output);
            m_old_vs = any == 0 && !TesterBuild::ParseVsWhere(OutputUtf8(output)).empty();
            if (m_old_vs) {
                Log(tr("The installed Visual Studio is older than version %1.")
                        .arg(Q(min_version)));
            }
        }
        return;
    }
    output.clear();
    code = RunStep(TesterBuild::VcVarsStep(install), m_base_env, &output);
    if (!code || *code != 0) {
        return;
    }
    QProcessEnvironment env;
    for (const auto& [name, value] : TesterBuild::ParseEnvironment(OutputUtf8(output))) {
        env.insert(Q(name), Q(value));
    }
    if (!TesterBuild::FindProgram("cl", PathEnv(env)).empty()) {
        m_vs_env = env;
        m_have_vs_env = true;
        m_tools.compiler = true;
    }
}

bool TesterBuildRunner::DetectLinux(Missing& missing) {
    const auto path = PathEnv(m_base_env);
    m_tools.compiler = !TesterBuild::FindProgram("g++", path).empty() ||
                       !TesterBuild::FindProgram("c++", path).empty();

    QFile release{QStringLiteral("/etc/os-release")};
    if (release.open(QIODevice::ReadOnly)) {
        m_manager = TesterBuild::ParseOsRelease(OutputUtf8(release.readAll()));
    }
    const auto list = m_recipe.linux_packages.find(m_manager);
    if (m_manager != TesterBuild::PackageManager::None && list != m_recipe.linux_packages.end() &&
        !list->second.packages.empty()) {
        m_packages_verified = list->second.verified;
        const auto step = TesterBuild::InstalledQueryStep(m_manager, list->second.packages);
        QByteArray output;
        const auto code = RunStep(step, m_base_env, &output);
        if (IsCanceled()) {
            return false;
        }
        if (!code) {
            Log(tr("The installed packages could not be checked."));
            return true;
        }
        missing.packages =
            TesterBuild::ParseMissing(m_manager, OutputUtf8(output), list->second.packages);
        return true;
    }
    if (!m_tools.compiler) {
        missing.provides.emplace_back("g++");
    }
    if (m_tools.git.empty()) {
        missing.provides.emplace_back("git");
    }
    if (m_tools.glslang.empty()) {
        missing.provides.emplace_back("glslangValidator");
    }
    return true;
}

bool TesterBuildRunner::Consent(const QStringList& items, const QString& text,
                                const QString& command, const QString& accept_label) {
    m_reply.tryAcquire(m_reply.available());
    emit ConsentNeeded(items, text, command, accept_label);
    m_reply.acquire();
    return !IsCanceled() && m_reply_accept;
}

bool TesterBuildRunner::Install(const Missing& missing, bool& manual) {
    return m_platform == TesterBuild::Platform::Windows ? InstallWindows(missing)
                                                        : InstallLinux(missing, manual);
}

bool TesterBuildRunner::InstallWindows(const Missing& missing) {
    QStringList items;
    for (const auto& name : missing.provides) {
        const auto package = std::ranges::find(m_recipe.winget, name,
                                               &TesterBuild::WingetPackage::provides);
        QString label = name == "msvc" && m_old_vs
                            ? tr("Visual Studio Build Tools (C++ compiler): the installed Visual "
                                 "Studio is too old, a newer one is required")
                        : name == "msvc"   ? tr("Visual Studio Build Tools (C++ compiler)")
                        : name == "git"  ? tr("Git")
                        : name == "glslang" ? tr("Vulkan SDK (shader compiler)")
                                            : Q(name);
        if (package != m_recipe.winget.end()) {
            label += QStringLiteral(" - ") + Q(package->id);
        }
        items << label;
    }

    const auto winget = TesterBuild::FindProgram("winget", PathEnv(m_base_env));
    if (winget.empty()) {
        Consent(items,
                tr("These tools are needed to build Eglantine, and winget, which would install "
                   "them, was not found. Install them yourself and run the tester build again."),
                {}, {});
        return Fail(tr("Missing tools: %1").arg(items.join(QStringLiteral(", "))));
    }

    const auto accepted =
        Consent(items,
                tr("These tools are needed to build Eglantine and are not installed. They will be "
                   "installed with winget. This can download several GB and needs administrator "
                   "approval: Windows will ask for it. Nothing is installed until you accept."),
                {}, tr("Install"));
    if (!accepted) {
        if (IsCanceled()) {
            return false;
        }
        return Fail(m_options.console && !m_options.yes
                        ? tr("Nothing was installed. Run again with --yes to install the tools "
                             "listed above.")
                        : tr("The tools were not installed, so the build cannot continue."));
    }

    for (auto step : TesterBuild::WingetSteps(m_recipe, missing.provides)) {
        step.program = winget;
        emit StepStarted(Q(step.label));
        emit Progress(0, 0);
        const auto code = RunStep(step, m_base_env);
        if (IsCanceled()) {
            return false;
        }
        if (!code) {
            return Fail(tr("Could not start winget."));
        }
        // Whether it worked is decided by looking for the tools again.
        if (*code != 0) {
            Log(tr("winget exited with code %1; checking what was installed.").arg(*code));
        }
    }
    return true;
}

bool TesterBuildRunner::InstallLinux(const Missing& missing, bool& manual) {
    QStringList items;
    for (const auto& name : missing.packages) {
        items << Q(name);
    }
    if (missing.packages.empty()) {
        for (const auto& name : missing.provides) {
            items << Q(name);
        }
        Consent(items,
                tr("These tools are needed to build Eglantine and are not installed. Your "
                   "distribution is not one the tester build can install packages for: install "
                   "them yourself and run the tester build again."),
                {}, {});
        return Fail(tr("Missing tools: %1").arg(items.join(QStringLiteral(", "))));
    }

    auto step = TesterBuild::InstallPackagesStep(m_manager, missing.packages);
    QString text = tr("These packages are needed to build Eglantine and are not installed. They "
                      "will be installed with your package manager, which needs administrator "
                      "approval. This can download several GB. Nothing is installed until you "
                      "accept.");
    if (!m_packages_verified) {
        text += QStringLiteral("\n\n") +
                tr("This package list has not been tested on your distribution.");
    }

    const auto path = PathEnv(m_base_env);
    const bool graphical = !qEnvironmentVariable("DISPLAY").isEmpty() ||
                           !qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty();
    const bool can_elevate =
        m_options.console ? !TesterBuild::FindProgram("sudo", path).empty()
                          : (graphical && !TesterBuild::FindProgram("pkexec", path).empty());
    if (!can_elevate && !m_options.console) {
        manual = true;
        return Consent(items,
                       text + QStringLiteral("\n\n") +
                           tr("Run this command in a terminal, then press Check again:"),
                       Q(TesterBuild::InstallCommandText(step)), tr("Check again"));
    }
    const auto accepted = Consent(items, text, Q(TesterBuild::InstallCommandText(step)),
                                  can_elevate ? tr("Install") : QString{});
    if (!accepted) {
        if (IsCanceled()) {
            return false;
        }
        return Fail(m_options.console && !m_options.yes
                        ? tr("Nothing was installed. Run again with --yes to install the "
                             "packages listed above.")
                        : tr("The packages were not installed, so the build cannot continue."));
    }
    emit StepStarted(Q(step.label));
    emit Progress(0, 0);
    const auto code = RunStep(step, m_base_env);
    if (IsCanceled()) {
        return false;
    }
    if (!code) {
        return Fail(tr("Could not start the package manager."));
    }
    if (*code != 0) {
        Log(tr("The package manager exited with code %1; checking what was installed.")
                .arg(*code));
    }
    return true;
}

TesterBuild::BuildRequest TesterBuildRunner::MakeRequest() const {
    TesterBuild::BuildRequest request;
    request.layout = m_layout;
    request.source = m_source;
    request.repo = Common::g_build_auto_update_repo;
    request.clone_url = std::string{Common::g_build_auto_update_website} + "/" + request.repo + ".git";
    request.recipe = m_recipe;
    request.tools = m_tools;
    // A build compiled here is never a PGO build.
    request.platform = UpdateChecker::DownloadPlatform(Settings::UpdateVariant::Standard);
    return request;
}

QProcessEnvironment TesterBuildRunner::BuildEnvironment() const {
    auto env = m_have_vs_env ? m_vs_env : m_base_env;
    PrependPath(env, {m_tools.cmake.parent_path(), m_tools.ninja.parent_path(),
                      m_tools.git.parent_path(), m_tools.glslang.parent_path()});
    if (!m_vulkan_sdk.empty() && !env.contains(QStringLiteral("VULKAN_SDK"))) {
        env.insert(QStringLiteral("VULKAN_SDK"), QDir::toNativeSeparators(ToQ(m_vulkan_sdk)));
    }
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    return env;
}

bool TesterBuildRunner::Fetch(std::string& sha) {
    std::error_code ec;
    // A clone that was interrupted leaves a folder that is not a repository.
    if (fs::exists(m_layout.src, ec) && !fs::exists(m_layout.src / ".git", ec)) {
        fs::remove_all(m_layout.src, ec);
    }
    const auto env = BuildEnvironment();
    for (const auto& step : TesterBuild::FetchSteps(MakeRequest())) {
        emit StepStarted(Q(step.label));
        emit Progress(0, 0);
        QByteArray output;
        const auto code = RunStep(step, env, step.capture ? &output : nullptr);
        if (!CheckStep(step, code)) {
            if (step.id == "clone") {
                fs::remove_all(m_layout.src, ec);
            }
            return false;
        }
        if (step.capture) {
            const auto parsed = TesterBuild::ParseRevParse(output.toStdString());
            if (!parsed) {
                return Fail(tr("Could not read the commit that was checked out."));
            }
            sha = *parsed;
        }
    }
    Log(tr("Commit %1").arg(Q(sha)));
    return true;
}

bool TesterBuildRunner::Build() {
    const auto env = BuildEnvironment();
    for (const auto& step : TesterBuild::BuildSteps(MakeRequest())) {
        emit StepStarted(Q(step.label));
        emit Progress(0, 0);
        if (!CheckStep(step, RunStep(step, env))) {
            return false;
        }
    }
    return true;
}

void TesterBuildRunner::LinkUserFolder(const fs::path& install) {
#ifdef _WIN32
    // A portable release keeps settings and saves in a user folder beside it; sharing that folder
    // makes the tester build see the same ones.
    if (m_state.release_exe.empty()) {
        return;
    }
    std::error_code ec;
    const auto user = TesterBuild::PathFromUtf8(m_state.release_exe).parent_path() / "user";
    const auto link = install / "user";
    if (!fs::is_directory(user, ec) || fs::exists(link, ec)) {
        return;
    }
    const TesterBuild::Step step{
        "junction",
        "Sharing the user folder",
        "cmd.exe",
        {"/d", "/s", "/c",
         "\"mklink /J \"" + TesterBuild::PathToUtf8(link) + "\" \"" +
             TesterBuild::PathToUtf8(user) + "\"\""},
        {},
        false,
        false};
    const auto code = RunStep(step, m_base_env);
    if (!code || *code != 0) {
        Log(tr("Could not share the user folder; the tester build will not see your settings."));
    }
#else
    Q_UNUSED(install);
#endif
}

bool TesterBuildRunner::Finish(const std::string& sha) {
    emit StepStarted(tr("Installing"));
    emit Progress(0, 0);
    const auto destination = m_layout.InstallDir(sha);
    if (!m_state.current_install.empty() &&
        SameFile(TesterBuild::PathFromUtf8(m_state.current_install), destination)) {
        // Replacing it could mean replacing a running copy.
        m_installed_exe = TesterBuild::InstalledExe(destination);
        m_message = tr("Built %1, which is already installed. The install was not replaced.")
                        .arg(Q(sha.substr(0, 10)));
        return true;
    }

    std::error_code ec;
    if (!TesterBuild::Promote(m_layout, sha, m_state, ec)) {
        return Fail(tr("Could not install the build: %1").arg(QString::fromStdString(ec.message())));
    }
    return Activate(sha,
                    tr("Installed %1 (%2).").arg(Q(m_source.DisplayName()), Q(sha.substr(0, 10))));
}

bool TesterBuildRunner::Activate(const std::string& sha, const QString& message) {
    const auto destination = m_layout.InstallDir(sha);
    LinkUserFolder(destination);
    if (!SaveState()) {
        return false;
    }
    TesterBuild::PruneInstalls(m_layout, m_state);
    m_installed_exe = TesterBuild::InstalledExe(destination);
    m_message = message;
    emit Progress(100, 100);
    return true;
}

bool TesterBuildRunner::FetchCiBuild(const Common::Net::Release& release, const std::string& sha,
                                     const fs::path& staging) {
    const auto package = m_platform == TesterBuild::Platform::Linux
                             ? Common::Net::LinuxPackage::AppImage
                             : Common::Net::LinuxPackage::Any;
    const auto assets = release.GetPlatformAssets(m_download_platform, package);
    if (assets.empty()) {
        return false;
    }
    const auto& asset = assets.front();

    emit StepStarted(tr("Downloading the CI build of %1").arg(Q(m_source.DisplayName())));
    emit Progress(0, 0);
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) {
        Log(tr("Could not create %1: %2").arg(ToQ(staging), QString::fromStdString(ec.message())));
        return false;
    }
    const auto url = asset.url + asset.path;
    if (m_platform == TesterBuild::Platform::Linux) {
        const auto exe = TesterBuild::InstalledExe(staging);
        if (!Download(url, asset.sha256, asset.size, exe)) {
            return false;
        }
        const auto text = ToQ(exe);
        QFile::setPermissions(text, QFile::permissions(text) | QFile::ExeOwner | QFile::ExeGroup |
                                        QFile::ExeOther);
        return true;
    }

    const auto archive = m_layout.tools / TesterBuild::PathFromUtf8(asset.filename);
    fs::remove(archive, ec);
    const bool downloaded = Download(url, asset.sha256, asset.size, archive);
    if (downloaded) {
        emit StepStarted(tr("Unpacking the CI build"));
        emit Progress(0, 0);
    }
    const bool unpacked = downloaded && Extract(archive, staging);
    fs::remove(archive, ec);
    if (downloaded && !unpacked) {
        Log(tr("Could not unpack the CI build of %1.").arg(Q(sha.substr(0, 10))));
    }
    return unpacked;
}

std::optional<TesterBuildRunner::Result> TesterBuildRunner::TryInstallCiBuild() {
    emit StepStarted(tr("Looking for a CI build"));
    emit Progress(0, 0);
    const auto remote = TesterLauncher::FetchRemote(m_source);
    if (IsCanceled()) {
        return Canceled;
    }
    if (!remote.info || remote.info->sha.empty()) {
        Log(tr("Could not read the newest commit (HTTP status %1); building from source.")
                .arg(remote.status));
        return std::nullopt;
    }
    const std::string sha = remote.info->sha;
    if (sha == m_state.built_sha && TesterBuild::HasInstall(m_state)) {
        m_installed_exe =
            TesterBuild::InstalledExe(TesterBuild::PathFromUtf8(m_state.current_install));
        m_message = tr("Already up to date (%1).").arg(Q(sha.substr(0, 10)));
        return UpToDate;
    }
    if (m_download_platform.empty()) {
        return std::nullopt;
    }

    const auto list = Common::Net::GetReleases();
    if (IsCanceled()) {
        return Canceled;
    }
    if (!list.ok) {
        Log(tr("Could not read the list of CI builds (HTTP status %1); building from source.")
                .arg(list.status));
        return std::nullopt;
    }
    const auto package = m_platform == TesterBuild::Platform::Linux
                             ? Common::Net::LinuxPackage::AppImage
                             : Common::Net::LinuxPackage::Any;
    const auto release = Common::Net::FindReleaseForCommit(
        list.releases, sha, m_download_platform, package);
    if (!release) {
        Log(tr("There is no CI build of %1; building from source.").arg(Q(sha.substr(0, 10))));
        return std::nullopt;
    }
    Log(tr("Found the CI build %1.").arg(Q(release->tag)));

    const auto staging = fs::path{m_layout.InstallDir(sha)}.concat(".tmp");
    std::error_code ec;
    const auto give_up = [&](const QString& reason) -> std::optional<Result> {
        fs::remove_all(staging, ec);
        if (IsCanceled()) {
            return Canceled;
        }
        // The compile path reports its own failures.
        const auto failure = std::exchange(m_message, QString{});
        Log(reason + QStringLiteral(" ") + failure + QStringLiteral(" ") +
            tr("Building from source."));
        return std::nullopt;
    };
    fs::create_directories(m_layout.installs, ec);
    if (!FetchCiBuild(*release, sha, staging)) {
        return give_up(tr("The CI build could not be installed."));
    }
    emit StepStarted(tr("Installing"));
    emit Progress(0, 0);
    if (!TesterBuild::PromoteStaged(m_layout, staging, sha, m_state, ec)) {
        return give_up(tr("The CI build could not be installed: %1")
                           .arg(QString::fromStdString(ec.message())));
    }
    if (!Activate(sha, tr("Installed %1 (%2) from a CI build.")
                           .arg(Q(m_source.DisplayName()), Q(sha.substr(0, 10))))) {
        return Failed;
    }
    return Success;
}

TesterBuildRunner::Result TesterBuildRunner::Execute() {
    const auto stopped = [this] { return IsCanceled() ? Canceled : Failed; };
    if (!Prepare()) {
        return stopped();
    }
    if (!m_options.force) {
        if (const auto installed = TryInstallCiBuild()) {
            return *installed;
        }
    }
    if (!EnsureTools() || !EnsureToolchain()) {
        return stopped();
    }

    std::string sha;
    if (!Fetch(sha)) {
        return stopped();
    }
    if (!m_options.force && sha == m_state.built_sha && !m_state.current_install.empty()) {
        const auto exe = TesterBuild::InstalledExe(TesterBuild::PathFromUtf8(m_state.current_install));
        std::error_code ec;
        if (fs::is_regular_file(exe, ec)) {
            m_installed_exe = exe;
            m_message = tr("Already up to date (%1).").arg(Q(sha.substr(0, 10)));
            return UpToDate;
        }
    }

    std::error_code ec;
    if (!TesterBuild::WriteStamp(m_layout, m_source, sha, ec)) {
        Fail(tr("Could not write the version stamp: %1").arg(QString::fromStdString(ec.message())));
        return stopped();
    }
    m_recipe = TesterBuild::LoadRecipe(m_layout.src, m_platform,
                                       m_source.kind == TesterBuild::Source::Kind::Branch);
    if (!EnsureTools() ||
        (m_platform == TesterBuild::Platform::Linux && !EnsureToolchain())) {
        return stopped();
    }
    if (!Build() || !Finish(sha)) {
        return stopped();
    }
    return Success;
}

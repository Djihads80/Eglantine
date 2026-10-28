// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Static Qt on macOS doesn't use Vulkan
// Other platforms do, and thus have conflicting VulkanMemoryAllocator symbols
#include "core/launch_params.h"
#if defined(QT_STATICPLUGIN) && !defined(__APPLE__)
#undef VMA_IMPLEMENTATION
#endif

#include <boost/algorithm/string/split.hpp>
#include "common/fs/path_util.h"
#include "common/controller_console_state.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "frontend_common/settings_generator.h"
#include "render/performance_overlay.h"
#ifdef HAS_RESHADE
#include "configuration/configure_post_processing.h"
#include "video_core/post_processing/fx_preset.h"
#endif
#include "updater/update_dialog.h"

#include "common/fs/ryujinx_compat.h"
#include "main_window.h"
#include "network/network.h"
#include "qt_common/discord/discord.h"
#include "ui_main.h"

// Other Yuzu stuff //
#include "debugger/console.h"
#include "debugger/controller.h"

#include "about_dialog.h"
#include "data_dialog.h"
#include "deps_dialog.h"
#include "install_dialog.h"

#include "bootmanager.h"
#include "loading_screen.h"
#include "qt_common/util/vk.h"
#include "ryujinx_dialog.h"
#include "set_play_time_dialog.h"
#include "util/util.h"
#include "yuzu/game/game_list.h"

#include "applets/qt_amiibo_settings.h"
#include "applets/qt_controller.h"
#include "applets/qt_error.h"
#include "applets/qt_profile_select.h"
#include "applets/qt_software_keyboard.h"
#include "applets/qt_web_browser.h"

#include "configuration/configure_dialog.h"
#include "configuration/configure_input.h"
#include "configuration/configure_per_game.h"
#include "configuration/configure_tas.h"
#include "configuration/input_profiles.h"

#include "util/clickable_label.h"
#include "util/controller_navigation.h"
#include "util/overlay_dialog.h"

#include "multiplayer/state.h"

// Qt Stuff //
#define QT_NO_OPENGL

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#endif

#include <QActionGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMimeData>
#include <QPalette>
#include <QProgressDialog>
#include <QScreen>
#include <QShortcut>
#include <QStatusBar>
#include <QtConcurrentRun>

// Qt Common //
#include "qt_common/config/shared_translation.h"
#include "qt_common/config/uisettings.h"

#include "qt_common/abstract/frontend.h"

#include "qt_common/qt_common.h"
#include "qt_common/qt_string_lookup.h"

#include "qt_common/util/content.h"
#include "qt_common/util/fs.h"
#include "qt_common/util/meta.h"
#include "qt_common/util/mod.h"
#include "qt_common/util/path.h"

#include "qt_common/render/emu_thread.h"

// These are wrappers to avoid the calls to CreateDirectory and CreateFile because of the Windows
// defines.
static FileSys::VirtualDir VfsFilesystemCreateDirectoryWrapper(const std::string& path,
                                                               FileSys::OpenMode mode) {
    return QtCommon::vfs->CreateDirectory(path, mode);
}

static FileSys::VirtualFile VfsDirectoryCreateFileWrapper(const FileSys::VirtualDir& dir,
                                                          const std::string& path) {
    return dir->CreateFile(path);
}

// Frontend //
#include "frontend_common/play_time_manager.h"

#ifdef ENABLE_UPDATE_CHECKER
#include "frontend_common/update_checker.h"
#endif

// Common //
#include "common/fs/fs.h"
#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "common/string_util.h"

#ifdef ARCHITECTURE_x86_64
#include "common/cpu_features.h"
#endif

// Core //
#include "core/frontend/applets/general.h"
#include "core/frontend/applets/mii_edit.h"
#include "core/frontend/applets/software_keyboard.h"

#include "core/hle/kernel/k_process.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applet_web_browser_types.h"

#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/savedata_factory.h"

#include "core/tools/renderdoc.h"

#include "core/perf_stats.h"

#include "core/crypto/key_manager.h"

// Input //
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/virtual_amiibo.h"

// Video Core //
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_notify.h"

#include <SDL3/SDL.h>
#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

#include <boost/container/flat_set.hpp>

// Platform stuff //
#include <boost/container/flat_set.hpp>

#ifdef __APPLE__
#include <unistd.h> // for chdir
#endif

#ifdef __unix__

#include <csignal>
#include <QSocketNotifier>
#include <sys/socket.h>
#include "qt_common/gui_settings.h"

#endif

#include "qt_common/gamemode.h"
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include "common/controller_overlay_frame.h"
#include "common/param_package.h"

#ifdef _WIN32
#include "common/windows/timer_resolution.h"
#include "core/core_timing.h"

#include <QPlatformSurfaceEvent>
#include <fmt/format.h>
#include <dwmapi.h>
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "Dwmapi.lib")
#endif

static inline void ApplyWindowsTitleBarDarkMode(HWND hwnd, bool enabled) {
    if (!hwnd)
        return;
    BOOL val = enabled ? TRUE : FALSE;
    // 20 = Win11/21H2+
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val))))
        return;
    // 19 = pre-21H2
    DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
}

static inline void ApplyDarkToTopLevel(QWidget* w, bool on) {
    if (!w || !w->isWindow())
        return;
    ApplyWindowsTitleBarDarkMode(reinterpret_cast<HWND>(w->winId()), on);
}

namespace {
struct TitlebarFilter final : QObject {
    bool dark;
    explicit TitlebarFilter(bool is_dark) : QObject(qApp), dark(is_dark) {}

    void setDark(bool is_dark) {
        dark = is_dark;
    }

    void onFocusChanged(QWidget*, QWidget* now) {
        if (now)
            ApplyDarkToTopLevel(now->window(), dark);
    }

    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (auto* w = qobject_cast<QWidget*>(obj)) {
            switch (ev->type()) {
            case QEvent::WinIdChange:
            case QEvent::Show:
            case QEvent::ShowToParent:
            case QEvent::WindowStateChange:
            case QEvent::ZOrderChange:
                ApplyDarkToTopLevel(w, dark);
                break;
            default:
                break;
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};

static TitlebarFilter* g_filter = nullptr;
static QMetaObject::Connection g_focusConn;

} // namespace

static void ApplyGlobalDarkTitlebar(bool is_dark) {
    if (!g_filter) {
        g_filter = new TitlebarFilter(is_dark);
        qApp->installEventFilter(g_filter);
        g_focusConn = QObject::connect(qApp, &QApplication::focusChanged, g_filter,
                                       &TitlebarFilter::onFocusChanged);
    } else {
        g_filter->setDark(is_dark);
    }
    for (QWidget* w : QApplication::topLevelWidgets())
        ApplyDarkToTopLevel(w, is_dark);
}

static void RemoveTitlebarFilter() {
    if (!g_filter)
        return;
    qApp->removeEventFilter(g_filter);
    QObject::disconnect(g_focusConn);
    g_filter->deleteLater();
    g_filter = nullptr;
}

#endif

#ifdef YUZU_CRASH_DUMPS
#include "yuzu/breakpad.h"
#endif

using namespace Common::Literals;

#include "qt_common/discord/discord.h"

#ifdef USE_DISCORD_PRESENCE
#include "qt_common/discord/discord_impl.h"
#endif

#ifdef QT_STATICPLUGIN
#include <QtPlugin>

#if defined(_WIN32)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(__APPLE__)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin)
#endif

#endif

#ifdef _WIN32
#include <windows.h>
extern "C" {
// tells Nvidia and AMD drivers to use the dedicated GPU by default on laptops with switchable
// graphics
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

constexpr int default_mouse_hide_timeout = 2500;
constexpr int default_input_update_timeout = 1;

constexpr size_t CopyBufferSize = 1_MiB;

/**
 * "Callouts" are one-time instructional messages shown to the user. In the config settings, there
 * is a bitfield "callout_flags" options, used to track if a message has already been shown to the
 * user. This is 32-bits - if we have more than 32 callouts, we should retire and recycle old ones.
 */
enum class CalloutFlag : uint32_t {
    DRDDeprecation = 0x2,
};

const int MainWindow::max_recent_files_item;

namespace {

constexpr std::array<std::pair<u32, const char*>, 5> default_game_icon_sizes{
    std::make_pair(0, QT_TRANSLATE_NOOP("MainWindow", "None")),
    std::make_pair(32, QT_TRANSLATE_NOOP("MainWindow", "Small (32x32)")),
    std::make_pair(64, QT_TRANSLATE_NOOP("MainWindow", "Standard (64x64)")),
    std::make_pair(128, QT_TRANSLATE_NOOP("MainWindow", "Large (128x128)")),
    std::make_pair(256, QT_TRANSLATE_NOOP("MainWindow", "Full Size (256x256)")),
};

QString GetTranslatedGameIconSize(size_t index) {
    return QCoreApplication::translate("MainWindow", default_game_icon_sizes[index].second);
}

} // namespace

#ifndef _WIN32
// TODO(crueter): carboxyl does this, is it needed in qml?
inline static bool isDarkMode() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    return scheme == Qt::ColorScheme::Dark;
#else
    const QPalette defaultPalette;
    const auto text = defaultPalette.color(QPalette::WindowText);
    const auto window = defaultPalette.color(QPalette::Window);
    return text.lightness() > window.lightness();
#endif // QT_VERSION
}
#endif // _WIN32

MainWindow::MainWindow(bool has_broken_vulkan)
    : ui{std::make_unique<Ui::MainWindow>()},
      input_subsystem{std::make_shared<InputCommon::InputSubsystem>()}, user_data_migrator{this} {
    QtCommon::Init(this);

    Common::FS::CreateEdenPaths();
    this->config = std::make_unique<QtConfig>();

    Common::Log::Filter filter;
    filter.ParseFilterString(Settings::values.log_filter.GetValue());
    Common::Log::SetGlobalFilter(filter);

    if (user_data_migrator.migrated) {
        // Sort-of hack whereby we only move the old dir if it's a subfolder of the user dir

        using namespace Common::FS;

        static constexpr const std::array<const EdenPath, 4> paths = {
            EdenPath::NANDDir, EdenPath::SDMCDir, EdenPath::DumpDir, EdenPath::LoadDir};

        for (const EdenPath& path : paths) {
            std::string str_path = Common::FS::GetEdenPathString(path);
            if (str_path.starts_with(user_data_migrator.selected_emu.get_user_dir())) {
                boost::replace_all(
                    str_path, user_data_migrator.selected_emu.lower_name().toStdString(), "eden");
                Common::FS::SetEdenPath(path, str_path);
            }
        }
    }

#ifdef __unix__
    SetupSigInterrupts();
#endif

    SetGamemodeEnabled(UISettings::values.enable_gamemode.GetValue());

    UISettings::RestoreWindowState(config);

    LoadTranslation();
    FrontendCommon::GenerateSettings();

    setAcceptDrops(true);
    ui->setupUi(this);
    statusBar()->hide();

    startup_icon_theme = QIcon::themeName();
    // fallback can only be set once, colorful theme icons are okay on both light/dark
    QIcon::setFallbackThemeName(QStringLiteral("colorful"));
    QIcon::setFallbackSearchPaths(QStringList(QStringLiteral(":/icons")));

    default_theme_paths = QIcon::themeSearchPaths();
    UpdateUITheme();

    SetDiscordEnabled(UISettings::values.enable_discord_presence.GetValue());
    discord_rpc->Update();

    play_time_manager = std::make_unique<PlayTime::PlayTimeManager>();

    InitializeWidgets();
    InitializeDebugWidgets();
    InitializeRecentFileMenuActions();
    InitializeHotkeys();

    SetDefaultUIGeometry();
    RestoreUIState();

    ConnectMenuEvents();
    ConnectWidgetEvents();

    QtCommon::SetupHID();
    controller_dialog->refreshConfiguration();

    UpdateWindowTitle();

    show();

#ifdef ENABLE_UPDATE_CHECKER
    if (UISettings::values.check_for_updates) {
        update_future = QtConcurrent::run(
            []() -> std::optional<Common::Net::Release> { return UpdateChecker::GetUpdate(); });
        update_watcher.connect(&update_watcher, &QFutureWatcher<QString>::finished, this,
                               &MainWindow::OnEmulatorUpdateAvailable);
        update_watcher.setFuture(update_future);
    }
#endif

    // Setup content providers.
    QtCommon::SetupContentProviders();

    // Gen keys if necessary
    OnCheckFirmwareDecryption();

#ifdef __unix__
    OnCheckGraphicsBackend();
#endif

    // Check for orphaned profiles and reset profile data if necessary
    QtCommon::Content::FixProfiles();

    if (Settings::values.use_dev_keys.GetValue()) {
        Core::Crypto::KeyManager::Instance().ReloadKeys();
    }
    game_list->PopulateAsync(UISettings::values.game_dirs);

    // Set up game list mode checkboxes.
    SetGameListMode(UISettings::values.game_list_mode.GetValue());

    // make sure menubar has the arrow cursor instead of inheriting from this
    ui->menubar->setCursor(QCursor());
    statusBar()->setCursor(QCursor());

    mouse_hide_timer.setInterval(default_mouse_hide_timeout);
    connect(&mouse_hide_timer, &QTimer::timeout, this, &MainWindow::HideMouseCursor);
    connect(ui->menubar, &QMenuBar::hovered, this, &MainWindow::ShowMouseCursor);

    update_input_timer.setInterval(default_input_update_timeout);
    connect(&update_input_timer, &QTimer::timeout, this, &MainWindow::UpdateInputDrivers);
    update_input_timer.start();

    if (has_broken_vulkan) {
        UISettings::values.has_broken_vulkan = true;

        QMessageBox::warning(this, tr("Broken Vulkan Installation Detected"),
                             tr("Vulkan initialization failed during boot."));
#ifdef HAS_OPENGL
        Settings::values.renderer_backend = Settings::RendererBackend::OpenGL_GLSL;
#else
        Settings::values.renderer_backend = Settings::RendererBackend::Null;
#endif

        UpdateAPIText();
        renderer_status_button->setDisabled(true);
        renderer_status_button->setChecked(false);
    } else {
        VkDeviceInfo::PopulateRecords(vk_device_records, this->window()->windowHandle());
    }

#if !defined(_WIN32)
    SDL_InitSubSystem(SDL_INIT_VIDEO);

    // Set a screensaver inhibition reason string. Currently passed to DBus by SDL and visible to
    // the user through their desktop environment.
    //: TRANSLATORS: This string is shown to the user to explain why yuzu needs to prevent the
    //: computer from sleeping
    QByteArray wakelock_reason = tr("Running a game").toUtf8();
    SDL_SetHint(SDL_HINT_SCREENSAVER_INHIBIT_ACTIVITY_NAME, wakelock_reason.data());

    // SDL disables the screen saver by default, and setting the hint
    // SDL_HINT_VIDEO_ALLOW_SCREENSAVER doesn't seem to work, so we just enable the screen saver
    // for now.
    SDL_EnableScreenSaver();
#endif

    SetupPrepareForSleep();

    // Some moron added a race condition to the status bar
    // so now we have to make this completely unnecessary call
    // to prevent the UI from blowing up.
    UpdateUITheme();

    if (QStringList args = QApplication::arguments(); args.size() >= 2) {
        QList<QByteArray> qba{};
        for (auto const& e : args)
            qba.push_back(e.toUtf8());
        std::vector<char*> args_cstr{};
        for (auto& e : qba)
            args_cstr.push_back(e.data());

        auto const lp = Core::ParseLaunchParams(*QtCommon::system, args_cstr.size(), args_cstr.data(), nullptr);

        // Override fullscreen setting if gamepath or argument is provided
        if (!lp.filepath.empty() || lp.fullscreen) {
            ui->action_Fullscreen->setChecked(lp.fullscreen);
        }

        if (!lp.filepath.empty()) {
            BootGame(QString::fromStdString(lp.filepath), ApplicationAppletParameters());
        } else if (lp.launch_setup) {
            LaunchFirmwareApplet(u64(Service::AM::AppletProgramId::Starter), std::nullopt);
        } else if (lp.launch_qlaunch) {
            LaunchFirmwareApplet(u64(Service::AM::AppletProgramId::QLaunch), std::nullopt);
        } else if (lp.launch_hlaunch) {
            std::filesystem::path const sd_dir =
                Common::FS::GetEdenPathString(Common::FS::EdenPath::SDMCDir);
            auto const hbl_path = (sd_dir / "atmosphere" / "hbl.nsp").string();
            BootGame(
                QString::fromStdString(hbl_path),
                LibraryAppletParameters(0x010000000000100Dull, Service::AM::AppletId::QLaunch));
        }
    }
}

MainWindow::~MainWindow() {
    // The config saves itself on destruction: it must write the user's own players
    RestoreConsoleProfile();

    // will get automatically deleted otherwise
    if (render_window->parent() == nullptr) {
        delete render_window;
    }

#ifdef __unix__
    ::close(sig_interrupt_fds[0]);
    ::close(sig_interrupt_fds[1]);
#endif
}

void MainWindow::AmiiboSettingsShowDialog(const Core::Frontend::CabinetParameters& parameters,
                                          std::shared_ptr<Service::NFC::NfcDevice> nfp_device) {
    cabinet_applet =
        new QtAmiiboSettingsDialog(this, parameters, input_subsystem.get(), nfp_device);
    SCOPE_EXIT {
        cabinet_applet->deleteLater();
        cabinet_applet = nullptr;
    };

    cabinet_applet->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowStaysOnTopHint |
                                   Qt::WindowTitleHint | Qt::WindowSystemMenuHint);
    cabinet_applet->setWindowModality(Qt::WindowModal);

    if (cabinet_applet->exec() == QDialog::Rejected) {
        emit AmiiboSettingsFinished(false, {});
        return;
    }

    emit AmiiboSettingsFinished(true, cabinet_applet->GetName());
}

void MainWindow::AmiiboSettingsRequestExit() {
    if (cabinet_applet) {
        cabinet_applet->reject();
    }
}

void MainWindow::ControllerSelectorReconfigureControllers(
    const Core::Frontend::ControllerParameters& parameters) {
    controller_applet =
        new QtControllerSelectorDialog(this, parameters, input_subsystem.get(), *QtCommon::system);
    SCOPE_EXIT {
        controller_applet->deleteLater();
        controller_applet = nullptr;
    };

    controller_applet->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint |
                                      Qt::WindowStaysOnTopHint | Qt::WindowTitleHint |
                                      Qt::WindowSystemMenuHint);
    controller_applet->setWindowModality(Qt::WindowModal);
    bool is_success = controller_applet->exec() != QDialog::Rejected;

    // Don't forget to apply settings.
    QtCommon::system->HIDCore().DisableAllControllerConfiguration();
    QtCommon::system->ApplySettings();
    SaveAllConfigValues();

    UpdateStatusButtons();

    emit ControllerSelectorReconfigureFinished(is_success);
}

void MainWindow::ControllerSelectorRequestExit() {
    if (controller_applet) {
        controller_applet->reject();
    }
}

void MainWindow::ProfileSelectorSelectProfile(
    const Core::Frontend::ProfileSelectParameters& parameters) {
    profile_select_applet = new QtProfileSelectionDialog(*QtCommon::system, this, parameters);
    SCOPE_EXIT {
        profile_select_applet->deleteLater();
        profile_select_applet = nullptr;
    };

    profile_select_applet->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint |
                                          Qt::WindowStaysOnTopHint | Qt::WindowTitleHint |
                                          Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    profile_select_applet->setWindowModality(Qt::WindowModal);
    if (profile_select_applet->exec() == QDialog::Rejected) {
        emit ProfileSelectorFinishedSelection(std::nullopt);
        return;
    }

    const auto uuid = QtCommon::system->GetProfileManager().GetUser(
        static_cast<std::size_t>(profile_select_applet->GetIndex()));
    if (!uuid.has_value()) {
        emit ProfileSelectorFinishedSelection(std::nullopt);
        return;
    }

    emit ProfileSelectorFinishedSelection(uuid);
}

void MainWindow::ProfileSelectorRequestExit() {
    if (profile_select_applet) {
        profile_select_applet->reject();
    }
}

void MainWindow::SoftwareKeyboardInitialize(
    bool is_inline, Core::Frontend::KeyboardInitializeParameters initialize_parameters) {
    if (software_keyboard) {
        LOG_ERROR(Frontend, "The software keyboard is already initialized!");
        return;
    }

    software_keyboard = new QtSoftwareKeyboardDialog(render_window, *QtCommon::system, is_inline,
                                                     std::move(initialize_parameters));

    if (is_inline) {
        connect(
            software_keyboard, &QtSoftwareKeyboardDialog::SubmitInlineText, this,
            [this](Service::AM::Frontend::SwkbdReplyType reply_type, std::u16string submitted_text,
                   s32 cursor_position) {
                emit SoftwareKeyboardSubmitInlineText(reply_type, submitted_text, cursor_position);
            },
            Qt::QueuedConnection);
    } else {
        connect(
            software_keyboard, &QtSoftwareKeyboardDialog::SubmitNormalText, this,
            [this](Service::AM::Frontend::SwkbdResult result, std::u16string submitted_text,
                   bool confirmed) {
                emit SoftwareKeyboardSubmitNormalText(result, submitted_text, confirmed);
            },
            Qt::QueuedConnection);
    }
}

void MainWindow::SoftwareKeyboardShowNormal() {
    if (!software_keyboard) {
        LOG_ERROR(Frontend, "The software keyboard is not initialized!");
        return;
    }

    const auto& layout = render_window->GetFramebufferLayout();

    const auto x = layout.screen.left;
    const auto y = layout.screen.top;
    const auto w = layout.screen.GetWidth();
    const auto h = layout.screen.GetHeight();
    const auto scale_ratio = devicePixelRatioF();

    software_keyboard->ShowNormalKeyboard(render_window->mapToGlobal(QPoint(x, y) / scale_ratio),
                                          QSize(w, h) / scale_ratio);
}

void MainWindow::SoftwareKeyboardShowTextCheck(
    Service::AM::Frontend::SwkbdTextCheckResult text_check_result,
    std::u16string text_check_message) {
    if (!software_keyboard) {
        LOG_ERROR(Frontend, "The software keyboard is not initialized!");
        return;
    }

    software_keyboard->ShowTextCheckDialog(text_check_result, text_check_message);
}

void MainWindow::SoftwareKeyboardShowInline(
    Core::Frontend::InlineAppearParameters appear_parameters) {
    if (!software_keyboard) {
        LOG_ERROR(Frontend, "The software keyboard is not initialized!");
        return;
    }

    const auto& layout = render_window->GetFramebufferLayout();

    const auto x =
        static_cast<int>(layout.screen.left + (0.5f * layout.screen.GetWidth() *
                                               ((2.0f * appear_parameters.key_top_translate_x) +
                                                (1.0f - appear_parameters.key_top_scale_x))));
    const auto y =
        static_cast<int>(layout.screen.top + (layout.screen.GetHeight() *
                                              ((2.0f * appear_parameters.key_top_translate_y) +
                                               (1.0f - appear_parameters.key_top_scale_y))));
    const auto w = static_cast<int>(layout.screen.GetWidth() * appear_parameters.key_top_scale_x);
    const auto h = static_cast<int>(layout.screen.GetHeight() * appear_parameters.key_top_scale_y);
    const auto scale_ratio = devicePixelRatioF();

    software_keyboard->ShowInlineKeyboard(std::move(appear_parameters),
                                          render_window->mapToGlobal(QPoint(x, y) / scale_ratio),
                                          QSize(w, h) / scale_ratio);
}

void MainWindow::SoftwareKeyboardHideInline() {
    if (!software_keyboard) {
        LOG_ERROR(Frontend, "The software keyboard is not initialized!");
        return;
    }

    software_keyboard->HideInlineKeyboard();
}

void MainWindow::SoftwareKeyboardInlineTextChanged(
    Core::Frontend::InlineTextParameters text_parameters) {
    if (!software_keyboard) {
        LOG_ERROR(Frontend, "The software keyboard is not initialized!");
        return;
    }

    software_keyboard->InlineTextChanged(std::move(text_parameters));
}

void MainWindow::SoftwareKeyboardExit() {
    if (!software_keyboard) {
        return;
    }

    software_keyboard->ExitKeyboard();

    software_keyboard = nullptr;
}

void MainWindow::WebBrowserOpenWebPage(const std::string& main_url,
                                       const std::string& additional_args, bool is_local) {
#ifdef YUZU_USE_QT_WEB_ENGINE

    // Raw input breaks with the web applet, Disable web applets if enabled
    if (Settings::values.disable_web_applet || Settings::values.enable_raw_input) {
        emit WebBrowserClosed(Service::AM::Frontend::WebExitReason::WindowClosed,
                              "http://localhost/");
        return;
    }

    web_applet = new QtNXWebEngineView(this, *QtCommon::system, input_subsystem.get());

    ui->action_Pause->setEnabled(false);
    ui->action_Restart->setEnabled(false);
    ui->action_Stop->setEnabled(false);

    {
        QProgressDialog loading_progress(this);
        loading_progress.setLabelText(tr("Loading Web Applet..."));
        loading_progress.setRange(0, 3);
        loading_progress.setValue(0);

        if (is_local && !Common::FS::Exists(main_url)) {
            loading_progress.show();

            auto future = QtConcurrent::run([this] { emit WebBrowserExtractOfflineRomFS(); });

            while (!future.isFinished()) {
                QCoreApplication::processEvents();

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        loading_progress.setValue(1);

        if (is_local) {
            web_applet->LoadLocalWebPage(main_url, additional_args);
        } else {
            web_applet->LoadExternalWebPage(main_url, additional_args);
        }

        if (render_window->IsLoadingComplete()) {
            render_window->hide();
        }

        const auto& layout = render_window->GetFramebufferLayout();
        const auto scale_ratio = devicePixelRatioF();
        web_applet->resize(layout.screen.GetWidth() / scale_ratio,
                           layout.screen.GetHeight() / scale_ratio);
        web_applet->move(layout.screen.left / scale_ratio,
                         (layout.screen.top / scale_ratio) + menuBar()->height());
        web_applet->setZoomFactor(static_cast<qreal>(layout.screen.GetWidth() / scale_ratio) /
                                  static_cast<qreal>(Layout::ScreenUndocked::Width));

        web_applet->setFocus();
        web_applet->show();

        loading_progress.setValue(2);

        QCoreApplication::processEvents();

        loading_progress.setValue(3);
    }

    bool exit_check = false;

    // TODO (Morph): Remove this
    QAction* exit_action = new QAction(tr("Disable Web Applet"), this);
    connect(exit_action, &QAction::triggered, this, [this] {
        const auto result = QMessageBox::warning(
            this, tr("Disable Web Applet"),
            tr("Disabling the web applet can lead to undefined behavior and should only be used "
               "with Super Mario 3D All-Stars. Are you sure you want to disable the web "
               "applet?\n(This can be re-enabled in the Debug settings.)"),
            QMessageBox::Yes | QMessageBox::No);
        if (result == QMessageBox::Yes) {
            Settings::values.disable_web_applet = true;
            web_applet->SetFinished(true);
        }
    });
    ui->menubar->addAction(exit_action);

    while (!web_applet->IsFinished()) {
        QCoreApplication::processEvents();

        if (!exit_check) {
            web_applet->page()->runJavaScript(
                QStringLiteral("end_applet;"), [&](const QVariant& variant) {
                    exit_check = false;
                    if (variant.toBool()) {
                        web_applet->SetFinished(true);
                        web_applet->SetExitReason(
                            Service::AM::Frontend::WebExitReason::EndButtonPressed);
                    }
                });

            exit_check = true;
        }

        if (web_applet->GetCurrentURL().contains(QStringLiteral("localhost"))) {
            if (!web_applet->IsFinished()) {
                web_applet->SetFinished(true);
                web_applet->SetExitReason(Service::AM::Frontend::WebExitReason::CallbackURL);
            }

            web_applet->SetLastURL(web_applet->GetCurrentURL().toStdString());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto exit_reason = web_applet->GetExitReason();
    const auto last_url = web_applet->GetLastURL();

    web_applet->hide();

    render_window->setFocus();

    if (render_window->IsLoadingComplete()) {
        render_window->show();
    }

    ui->action_Pause->setEnabled(true);
    ui->action_Restart->setEnabled(true);
    ui->action_Stop->setEnabled(true);

    ui->menubar->removeAction(exit_action);

    QCoreApplication::processEvents();

    emit WebBrowserClosed(exit_reason, last_url);

#else

    // Utilize the same fallback as the default web browser applet.
    emit WebBrowserClosed(Service::AM::Frontend::WebExitReason::WindowClosed, "http://localhost/");

#endif
}

void MainWindow::WebBrowserRequestExit() {
#ifdef YUZU_USE_QT_WEB_ENGINE
    if (web_applet) {
        web_applet->SetExitReason(Service::AM::Frontend::WebExitReason::ExitRequested);
        web_applet->SetFinished(true);
    }
#endif
}

void MainWindow::InitializeWidgets() {
    render_window = new GRenderWindow(this, input_subsystem);
    render_window->hide();

    game_list = new GameList(QtCommon::vfs, QtCommon::provider.get(), *play_time_manager,
                             *QtCommon::system, this);
    ui->horizontalLayout->addWidget(game_list);

    game_list_placeholder = new GameListPlaceholder(this);
    ui->horizontalLayout->addWidget(game_list_placeholder);
    game_list_placeholder->setVisible(false);

    loading_screen = new LoadingScreen(this);
    loading_screen->hide();
    ui->horizontalLayout->addWidget(loading_screen);
    connect(loading_screen, &LoadingScreen::Hidden, this, [&] {
        loading_screen->Clear();
        if (emulation_running) {
            render_window->show();
            render_window->setFocus();
        }
    });

    multiplayer_state = new MultiplayerState(this, game_list->GetModel(), ui->action_Leave_Room,
                                             ui->action_Show_Room, *QtCommon::system);
    multiplayer_state->setVisible(false);

    // Create status bar
    message_label = new QLabel();
    // Configured separately for left alignment
    message_label->setFrameStyle(QFrame::NoFrame);
    message_label->setContentsMargins(4, 0, 4, 0);
    message_label->setAlignment(Qt::AlignLeft);
    statusBar()->addPermanentWidget(message_label, 1);

    shader_building_label = new QLabel();
    shader_building_label->setToolTip(tr("The amount of shaders currently being built"));
    res_scale_label = new QLabel();
    res_scale_label->setToolTip(tr("The current selected resolution scaling multiplier."));
    emu_speed_label = new QLabel();
    emu_speed_label->setToolTip(
        tr("Current emulation speed. Values higher or lower than 100% "
           "indicate emulation is running faster or slower than a Switch."));
    game_fps_label = new QLabel();
    game_fps_label->setToolTip(tr("How many frames per second the game is currently displaying. "
                                  "This will vary from game to game and scene to scene."));
    emu_frametime_label = new QLabel();
    emu_frametime_label->setToolTip(
        tr("Time taken to emulate a Switch frame, not counting framelimiting or v-sync. For "
           "full-speed emulation this should be at most 16.67 ms."));

    for (auto& label : {shader_building_label, res_scale_label, emu_speed_label, game_fps_label,
                        emu_frametime_label}) {
        label->setVisible(false);
        label->setFrameStyle(QFrame::NoFrame);
        label->setContentsMargins(4, 0, 4, 0);
        statusBar()->addPermanentWidget(label);
    }

    firmware_label = new QLabel();
    firmware_label->setObjectName(QStringLiteral("FirmwareLabel"));
    firmware_label->setVisible(false);
    firmware_label->setContentsMargins(4, 0, 4, 0);
    firmware_label->setFocusPolicy(Qt::NoFocus);
    statusBar()->addPermanentWidget(firmware_label);

    statusBar()->addPermanentWidget(multiplayer_state->GetStatusText(), 0);
    statusBar()->addPermanentWidget(multiplayer_state->GetStatusIcon(), 0);

    tas_label = new QLabel();
    tas_label->setObjectName(QStringLiteral("TASlabel"));
    tas_label->setFocusPolicy(Qt::NoFocus);
    statusBar()->insertPermanentWidget(0, tas_label);

    volume_popup = new QWidget(this);
    volume_popup->setWindowFlags(Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::Popup);
    volume_popup->setLayout(new QVBoxLayout());
    volume_popup->setMinimumWidth(200);

    volume_slider = new QSlider(Qt::Horizontal);
    volume_slider->setObjectName(QStringLiteral("volume_slider"));
    volume_slider->setMaximum(200);
    volume_slider->setPageStep(5);
    volume_popup->layout()->addWidget(volume_slider);

    volume_button = new VolumeButton();
    volume_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    volume_button->setFocusPolicy(Qt::NoFocus);
    volume_button->setCheckable(true);
    UpdateVolumeUI();
    connect(volume_slider, &QSlider::valueChanged, this, [this](int percentage) {
        Settings::values.audio_muted = false;
        const auto volume = static_cast<u8>(percentage);
        Settings::values.volume.SetValue(volume);
        UpdateVolumeUI();
    });
    connect(volume_button, &QPushButton::clicked, this, [&] {
        UpdateVolumeUI();
        volume_popup->setVisible(!volume_popup->isVisible());
        QRect rect = volume_button->geometry();
        QPoint bottomLeft = statusBar()->mapToGlobal(rect.topLeft());
        bottomLeft.setY(bottomLeft.y() - volume_popup->geometry().height());
        volume_popup->setGeometry(QRect(bottomLeft, QSize(rect.width(), rect.height())));
    });
    volume_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(volume_button, &QPushButton::customContextMenuRequested,
            [this](const QPoint& menu_location) {
                QMenu context_menu;
                context_menu.addAction(
                    Settings::values.audio_muted ? tr("Unmute") : tr("Mute"), [this] {
                        Settings::values.audio_muted = !Settings::values.audio_muted;
                        UpdateVolumeUI();
                    });

                context_menu.addAction(tr("Reset Volume"), [this] {
                    Settings::values.volume.SetValue(100);
                    UpdateVolumeUI();
                });

                context_menu.exec(volume_button->mapToGlobal(menu_location));
                volume_button->repaint();
            });
    connect(volume_button, &VolumeButton::VolumeChanged, this, &MainWindow::UpdateVolumeUI);

    statusBar()->insertPermanentWidget(0, volume_button);

#ifdef HAS_RESHADE
    post_shader_status_button = new QPushButton();
    post_shader_status_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    post_shader_status_button->setFocusPolicy(Qt::NoFocus);
    post_shader_status_button->setCheckable(true);
    connect(post_shader_status_button, &QPushButton::clicked, this, [this] {
        const bool enabled = Settings::values.post_shader_enabled.GetValue();
        Settings::values.post_shader_enabled.SetValue(!enabled);
        UpdatePostShaderText();
    });
    UpdatePostShaderText();
    post_shader_status_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(post_shader_status_button, &QPushButton::customContextMenuRequested,
            [this](const QPoint& menu_location) {
                QMenu context_menu;

                for (auto const& preset : VideoCore::GetFxPresetCatalog()) {
                    context_menu.addAction(QString::fromStdString(preset.name),
                                           [this, name = preset.name] {
                                               VideoCore::ApplyFxPreset(name);
                                               Settings::values.post_shader_enabled.SetValue(true);
                                               UpdatePostShaderText();
                                           });
                }

                context_menu.addSeparator();
                context_menu.addAction(tr("Configure Effects..."), this,
                                       &MainWindow::OnPostProcessingShaders);

                context_menu.exec(post_shader_status_button->mapToGlobal(menu_location));
                post_shader_status_button->repaint();
            });
    statusBar()->insertPermanentWidget(0, post_shader_status_button);
#endif

    // setup AA button
    aa_status_button = new QPushButton();
    aa_status_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    aa_status_button->setFocusPolicy(Qt::NoFocus);
    connect(aa_status_button, &QPushButton::clicked, [&] {
        auto aa_mode = Settings::values.anti_aliasing.GetValue();
        aa_mode = Settings::AntiAliasing(u32(aa_mode) + 1);
        if (u32(aa_mode) > u32(Settings::EnumMetadata<Settings::AntiAliasing>::GetLast()))
            aa_mode = Settings::EnumMetadata<Settings::AntiAliasing>::GetFirst();
        Settings::values.anti_aliasing.SetValue(aa_mode);
        aa_status_button->setChecked(true);
        UpdateAAText();
    });
    UpdateAAText();
    aa_status_button->setCheckable(true);
    aa_status_button->setChecked(true);
    aa_status_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(aa_status_button, &QPushButton::customContextMenuRequested,
            [this](const QPoint& menu_location) {
                QMenu context_menu;
                for (auto const& aa_text_pair : ConfigurationShared::anti_aliasing_texts_map) {
                    context_menu.addAction(aa_text_pair.second, [this, aa_text_pair] {
                        Settings::values.anti_aliasing.SetValue(aa_text_pair.first);
                        UpdateAAText();
                    });
                }
                context_menu.exec(aa_status_button->mapToGlobal(menu_location));
                aa_status_button->repaint();
            });
    statusBar()->insertPermanentWidget(0, aa_status_button);

    // Setup Filter button
    filter_status_button = new QPushButton();
    filter_status_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    filter_status_button->setFocusPolicy(Qt::NoFocus);
    connect(filter_status_button, &QPushButton::clicked, this, &MainWindow::OnToggleAdaptingFilter);
    UpdateFilterText();
    filter_status_button->setCheckable(true);
    filter_status_button->setChecked(true);
    filter_status_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(filter_status_button, &QPushButton::customContextMenuRequested,
            [this](const QPoint& menu_location) {
                QMenu context_menu;
                for (auto const& filter_text_pair : ConfigurationShared::scaling_filter_texts_map) {
                    context_menu.addAction(filter_text_pair.second, [this, filter_text_pair] {
                        Settings::values.scaling_filter.SetValue(filter_text_pair.first);
                        UpdateFilterText();
                    });
                }
                context_menu.exec(filter_status_button->mapToGlobal(menu_location));
                filter_status_button->repaint();
            });
    statusBar()->insertPermanentWidget(0, filter_status_button);

    // Setup Dock button
    dock_status_button = new QPushButton();
    dock_status_button->setObjectName(QStringLiteral("DockingStatusBarButton"));
    dock_status_button->setFocusPolicy(Qt::NoFocus);
    connect(dock_status_button, &QPushButton::clicked, this, &MainWindow::OnToggleDockedMode);
    dock_status_button->setCheckable(true);
    UpdateDockedButton();
    dock_status_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(dock_status_button, &QPushButton::customContextMenuRequested,
            [this](const QPoint& menu_location) {
                QMenu context_menu;

                for (auto const& pair : ConfigurationShared::use_docked_mode_texts_map) {
                    context_menu.addAction(pair.second, [this, &pair] {
                        if (pair.first != Settings::values.use_docked_mode.GetValue()) {
                            OnToggleDockedMode();
                        }
                    });
                }
                context_menu.exec(dock_status_button->mapToGlobal(menu_location));
                dock_status_button->repaint();
            });
    statusBar()->insertPermanentWidget(0, dock_status_button);

    // Setup GPU Accuracy button
    gpu_accuracy_button = new QPushButton();
    gpu_accuracy_button->setObjectName(QStringLiteral("GPUStatusBarButton"));
    gpu_accuracy_button->setCheckable(true);
    gpu_accuracy_button->setFocusPolicy(Qt::NoFocus);
    connect(gpu_accuracy_button, &QPushButton::clicked, this, &MainWindow::OnToggleGpuAccuracy);
    UpdateGPUAccuracyButton();
    gpu_accuracy_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(gpu_accuracy_button, &QPushButton::customContextMenuRequested,
            [this](const QPoint& menu_location) {
                QMenu context_menu;

                for (auto const& gpu_accuracy_pair : ConfigurationShared::gpu_accuracy_texts_map) {
                    context_menu.addAction(gpu_accuracy_pair.second, [this, gpu_accuracy_pair] {
                        Settings::values.gpu_accuracy.SetValue(gpu_accuracy_pair.first);
                        UpdateGPUAccuracyButton();
                    });
                }
                context_menu.exec(gpu_accuracy_button->mapToGlobal(menu_location));
                gpu_accuracy_button->repaint();
            });
    statusBar()->insertPermanentWidget(0, gpu_accuracy_button);

    // Setup Renderer API button
    renderer_status_button = new QPushButton();
    renderer_status_button->setObjectName(QStringLiteral("RendererStatusBarButton"));
    renderer_status_button->setCheckable(true);
    renderer_status_button->setFocusPolicy(Qt::NoFocus);
    connect(renderer_status_button, &QPushButton::clicked, this, &MainWindow::OnToggleGraphicsAPI);
    UpdateAPIText();
    renderer_status_button->setCheckable(true);
    renderer_status_button->setChecked(Settings::values.renderer_backend.GetValue() ==
                                       Settings::RendererBackend::Vulkan);
    renderer_status_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(renderer_status_button, &QPushButton::customContextMenuRequested,
            [this](const QPoint& menu_location) {
                QMenu context_menu;

                for (auto const& renderer_backend_pair :
                     ConfigurationShared::renderer_backend_texts_map) {
                    if (renderer_backend_pair.first == Settings::RendererBackend::Null) {
                        continue;
                    }
                    context_menu.addAction(
                        renderer_backend_pair.second, [this, renderer_backend_pair] {
                            Settings::values.renderer_backend.SetValue(renderer_backend_pair.first);
                            UpdateAPIText();
                        });
                }
                context_menu.exec(renderer_status_button->mapToGlobal(menu_location));
                renderer_status_button->repaint();
            });
    statusBar()->insertPermanentWidget(0, renderer_status_button);

    // Setup Refresh Button
    refresh_button = new QPushButton();
    refresh_button->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    refresh_button->setObjectName(QStringLiteral("RefreshButton"));
    refresh_button->setFocusPolicy(Qt::NoFocus);
    connect(refresh_button, &QPushButton::clicked, this, &MainWindow::OnGameListRefresh);

    statusBar()->insertPermanentWidget(0, refresh_button);

    statusBar()->setVisible(true);
    setStyleSheet(QStringLiteral("QStatusBar::item{border: none;}"));
}

void MainWindow::InitializeDebugWidgets() {
    QMenu* debug_menu = ui->menu_View_Debugging;

    controller_dialog = new ControllerDialog(QtCommon::system->HIDCore(), input_subsystem, this);
    controller_dialog->hide();
    debug_menu->addAction(controller_dialog->toggleViewAction());
}

void MainWindow::InitializeRecentFileMenuActions() {
    for (int i = 0; i < max_recent_files_item; ++i) {
        actions_recent_files[i] = new QAction(this);
        actions_recent_files[i]->setVisible(false);
        connect(actions_recent_files[i], &QAction::triggered, this, &MainWindow::OnMenuRecentFile);

        ui->menu_recent_files->addAction(actions_recent_files[i]);
    }
    ui->menu_recent_files->addSeparator();
    QAction* action_clear_recent_files = new QAction(this);
    action_clear_recent_files->setText(tr("&Clear Recent Files"));
    connect(action_clear_recent_files, &QAction::triggered, this, [this] {
        UISettings::values.recent_files.clear();
        UpdateRecentFiles();
    });
    ui->menu_recent_files->addAction(action_clear_recent_files);

    UpdateRecentFiles();
}

void MainWindow::LinkActionShortcut(QAction* action, const QString& action_name,
                                    const bool tas_allowed) {
    static const auto main_window = std::string("Main Window");
    action->setShortcut(hotkey_registry.GetKeySequence(main_window, action_name.toStdString()));
    action->setShortcutContext(
        hotkey_registry.GetShortcutContext(main_window, action_name.toStdString()));
    action->setAutoRepeat(false);

    this->addAction(action);

    auto* controller =
        QtCommon::system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    const auto* controller_hotkey =
        hotkey_registry.GetControllerHotkey(main_window, action_name.toStdString(), controller);
    connect(
        controller_hotkey, &ControllerShortcut::Activated, this,
        [action, tas_allowed, this] {
            auto [tas_status, current_tas_frame, total_tas_frames] =
                input_subsystem->GetTas()->GetStatus();
            if (tas_allowed || tas_status == InputCommon::TasInput::TasState::Stopped) {
                action->trigger();
            }
        },
        Qt::QueuedConnection);
}

void MainWindow::InitializeHotkeys() {
    hotkey_registry.LoadHotkeys();

    LinkActionShortcut(ui->action_Load_File, QStringLiteral("Load File"));
    LinkActionShortcut(ui->action_Load_Amiibo, QStringLiteral("Load/Remove Amiibo"));
    LinkActionShortcut(ui->action_Exit, QStringLiteral("Exit Eden"));
    LinkActionShortcut(ui->action_Restart, QStringLiteral("Restart Emulation"));
    LinkActionShortcut(ui->action_Pause, QStringLiteral("Continue/Pause Emulation"));
    LinkActionShortcut(ui->action_Stop, QStringLiteral("Stop Emulation"));
    LinkActionShortcut(ui->action_Show_Filter_Bar, QStringLiteral("Toggle Filter Bar"));
    LinkActionShortcut(ui->action_Show_Status_Bar, QStringLiteral("Toggle Status Bar"));
    LinkActionShortcut(ui->action_Show_Performance_Overlay,
                       QStringLiteral("Toggle Performance Overlay"));
    LinkActionShortcut(ui->action_Show_Controller_Overlay,
                       QStringLiteral("Toggle Controller Overlay"));
    LinkActionShortcut(ui->action_Fullscreen, QStringLiteral("Fullscreen"));
    LinkActionShortcut(ui->action_Capture_Screenshot, QStringLiteral("Capture Screenshot"));
    LinkActionShortcut(ui->action_TAS_Start, QStringLiteral("TAS Start/Stop"), true);
    LinkActionShortcut(ui->action_TAS_Record, QStringLiteral("TAS Record"), true);
    LinkActionShortcut(ui->action_TAS_Reset, QStringLiteral("TAS Reset"), true);
    LinkActionShortcut(ui->action_View_Lobby, QStringLiteral("Browse Public Game Lobby"));
    LinkActionShortcut(ui->action_Start_Room, QStringLiteral("Create Room"));
    LinkActionShortcut(ui->action_Connect_To_Room, QStringLiteral("Direct Connect to Room"));
    LinkActionShortcut(ui->action_Show_Room, QStringLiteral("Show Current Room"));
    LinkActionShortcut(ui->action_Leave_Room, QStringLiteral("Leave Room"));
    LinkActionShortcut(ui->action_Configure, QStringLiteral("Configure"));
    LinkActionShortcut(ui->action_Configure_Current_Game, QStringLiteral("Configure Current Game"));

    static const QString main_window = QStringLiteral("Main Window");
    const auto connect_shortcut = [&]<typename Fn>(const QString& action_name, const Fn& function) {
        const auto* hotkey =
            hotkey_registry.GetHotkey(main_window.toStdString(), action_name.toStdString(), this);
        auto* controller =
            QtCommon::system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
        const auto* controller_hotkey = hotkey_registry.GetControllerHotkey(
            main_window.toStdString(), action_name.toStdString(), controller);
        connect(hotkey, &QShortcut::activated, this, function);
        connect(controller_hotkey, &ControllerShortcut::Activated, this, function,
                Qt::QueuedConnection);
    };

    connect_shortcut(QStringLiteral("Exit Fullscreen"), [&] {
        if (emulation_running && ui->action_Fullscreen->isChecked()) {
            ui->action_Fullscreen->setChecked(false);
            ToggleFullscreen();
        }
    });
    connect_shortcut(QStringLiteral("Change Adapting Filter"), &MainWindow::OnToggleAdaptingFilter);
    connect_shortcut(QStringLiteral("Change Docked Mode"), &MainWindow::OnToggleDockedMode);
    connect_shortcut(QStringLiteral("Change GPU Mode"), &MainWindow::OnToggleGpuAccuracy);
    connect_shortcut(QStringLiteral("Audio Mute/Unmute"), &MainWindow::OnMute);
    connect_shortcut(QStringLiteral("Audio Volume Down"), &MainWindow::OnDecreaseVolume);
    connect_shortcut(QStringLiteral("Audio Volume Up"), &MainWindow::OnIncreaseVolume);

    connect_shortcut(QStringLiteral("Toggle Framerate Limit"), [this] {
        Settings::ToggleStandardMode();
        SetFPSSuffix();
    });

    connect_shortcut(QStringLiteral("Toggle Turbo Speed"), [this] {
        Settings::ToggleTurboMode();
        SetFPSSuffix();
    });

    connect_shortcut(QStringLiteral("Toggle Slow Speed"), [this] {
        Settings::ToggleSlowMode();
        SetFPSSuffix();
    });

    connect_shortcut(QStringLiteral("Toggle Renderdoc Capture"), [] {
        if (Settings::values.enable_renderdoc_hotkey) {
            QtCommon::system->GetRenderdocAPI().ToggleCapture();
        }
    });
    connect_shortcut(QStringLiteral("Toggle Mouse Panning"), [&] {
        Settings::values.mouse_panning = !Settings::values.mouse_panning;
        if (Settings::values.mouse_panning) {
            render_window->installEventFilter(render_window);
            render_window->setAttribute(Qt::WA_Hover, true);
        }
    });
}

void MainWindow::SetDefaultUIGeometry() {
    // geometry: 53% of the window contents are in the upper screen half, 47% in the lower half
    const QRect screenRect = QGuiApplication::primaryScreen()->geometry();

    const int w = screenRect.width() * 2 / 3;
    const int h = screenRect.height() * 2 / 3;
    const int x = (screenRect.x() + screenRect.width()) / 2 - w / 2;
    const int y = (screenRect.y() + screenRect.height()) / 2 - h * 53 / 100;

    setGeometry(x, y, w, h);
}

void MainWindow::RestoreUIState() {
    setWindowFlags(windowFlags() & ~Qt::FramelessWindowHint);
    restoreGeometry(UISettings::values.geometry);
    // Work-around because the games list isn't supposed to be full screen
    if (isFullScreen()) {
        showNormal();
    }
    restoreState(UISettings::values.state);
    render_window->setWindowFlags(render_window->windowFlags() & ~Qt::FramelessWindowHint);
    render_window->restoreGeometry(UISettings::values.renderwindow_geometry);

    game_list->LoadInterfaceLayout();

    ui->action_Single_Window_Mode->setChecked(UISettings::values.single_window_mode.GetValue());
    ToggleWindowMode();

    ui->action_Fullscreen->setChecked(UISettings::values.fullscreen.GetValue());

    ui->action_Enable_Overlay_Applet->setChecked(Settings::values.enable_overlay.GetValue());

    ui->action_Show_Filter_Bar->setChecked(UISettings::values.show_filter_bar.GetValue());
    game_list->SetFilterVisible(ui->action_Show_Filter_Bar->isChecked());

    ui->action_Show_Status_Bar->setChecked(UISettings::values.show_status_bar.GetValue());
    statusBar()->setVisible(ui->action_Show_Status_Bar->isChecked());

    ui->action_Show_Performance_Overlay->setChecked(
        UISettings::values.show_perf_overlay.GetValue());
    ui->action_Show_Controller_Overlay->setChecked(
        Settings::values.console_debug_overlay.GetValue());
    if (perf_overlay)
        perf_overlay->setVisible(ui->action_Show_Performance_Overlay->isChecked());
    Debugger::ToggleConsole();
}

void MainWindow::OnAppFocusStateChanged(Qt::ApplicationState state) {
    if (state != Qt::ApplicationHidden && state != Qt::ApplicationInactive &&
        state != Qt::ApplicationActive) {
        LOG_DEBUG(Frontend, "ApplicationState unusual flag: {} ", state);
    }
    if (!emulation_running) {
        return;
    }
    if (UISettings::values.pause_when_in_background) {
        if (QtCommon::emu_thread->IsRunning() &&
            (state & (Qt::ApplicationHidden | Qt::ApplicationInactive))) {
            auto_paused = true;
            OnPauseGame();
        } else if (!QtCommon::emu_thread->IsRunning() && auto_paused &&
                   (state & Qt::ApplicationActive)) {
            auto_paused = false;
            OnStartGame();
        }
    }
    if (UISettings::values.mute_when_in_background) {
        if (!Settings::values.audio_muted &&
            (state & (Qt::ApplicationHidden | Qt::ApplicationInactive))) {
            Settings::values.audio_muted = true;
            auto_muted = true;
        } else if (auto_muted && (state & Qt::ApplicationActive)) {
            Settings::values.audio_muted = false;
            auto_muted = false;
        }
        UpdateVolumeUI();
    }
}

void MainWindow::ConnectWidgetEvents() {
    connect(game_list, &GameList::BootGame, this, &MainWindow::BootGameFromList);
    connect(game_list, &GameList::GameChosen, this, &MainWindow::OnGameListLoadFile);
    connect(game_list, &GameList::OpenDirectory, this, &MainWindow::OnGameListOpenDirectory);
    connect(game_list, &GameList::OpenFolderRequested, this, &MainWindow::OnGameListOpenFolder);
    connect(game_list, &GameList::OpenTransferableShaderCacheRequested, this,
            [this](u64 program_id) { QtCommon::Path::OpenShaderCache(program_id, this); });
    connect(game_list, &GameList::RemoveInstalledEntryRequested, this,
            &MainWindow::OnGameListRemoveInstalledEntry);
    connect(game_list, &GameList::RemoveFileRequested, this, &MainWindow::OnGameListRemoveFile);
    connect(game_list, &GameList::RemovePlayTimeRequested, this,
            &MainWindow::OnGameListRemovePlayTimeData);
    connect(game_list, &GameList::SetPlayTimeRequested, this, &MainWindow::OnGameListSetPlayTime);
    connect(game_list, &GameList::DumpRomFSRequested, this, &MainWindow::OnGameListDumpRomFS);
    connect(game_list, &GameList::VerifyIntegrityRequested, this,
            &MainWindow::OnGameListVerifyIntegrity);
    connect(game_list, &GameList::CopyTIDRequested, this, &MainWindow::OnGameListCopyTID);
    connect(game_list, &GameList::CreateShortcut, this, &MainWindow::OnGameListCreateShortcut);
    connect(game_list, &GameList::AddDirectory, this, &MainWindow::OnGameListAddDirectory);
    connect(game_list_placeholder, &GameListPlaceholder::AddDirectory, this,
            &MainWindow::OnGameListAddDirectory);
    connect(game_list, &GameList::ShowList, this, &MainWindow::OnGameListShowList);
    connect(game_list, &GameList::PopulatingCompleted,
            [this] { multiplayer_state->UpdateGameList(game_list->GetModel()); });
    connect(game_list, &GameList::SaveConfig, this, &MainWindow::OnSaveConfig);

    connect(game_list, &GameList::OpenPerGameGeneralRequested, this,
            &MainWindow::OnGameListOpenPerGameProperties);
    connect(game_list, &GameList::LinkToRyujinxRequested, this, &MainWindow::OnLinkToRyujinx);

    connect(this, &MainWindow::UpdateInstallProgress, this, &MainWindow::IncrementInstallProgress);

    // Software Keyboard Applet
    connect(this, &MainWindow::EmulationStarting, this, &MainWindow::SoftwareKeyboardExit);
    connect(this, &MainWindow::EmulationStopping, this, &MainWindow::SoftwareKeyboardExit);
    connect(&console_mode_timer, &QTimer::timeout, this, &MainWindow::ApplyConsoleControllerMode);
    connect(&console_activity_timer, &QTimer::timeout, this, &MainWindow::PollControllerActivity);
    console_activity_timer.start(std::chrono::milliseconds{40});
    // 250ms: effectively instant to the user, negligible cost (the input reload only
    // runs when the membership changes).
    console_mode_timer.start(std::chrono::milliseconds{250});

    connect(&status_bar_update_timer, &QTimer::timeout, this, &MainWindow::UpdateStatusBar);

    connect(this, &MainWindow::UpdateThemedIcons, multiplayer_state,
            &MultiplayerState::UpdateThemedIcons);
}

namespace {

/// The joystick API is shared with the input driver's threads (the rumble worker);
/// SDL_LockJoysticks is the official guard for calling into it elsewhere.
struct JoystickApiLock {
    JoystickApiLock() {
        SDL_LockJoysticks();
    }
    ~JoystickApiLock() {
        SDL_UnlockJoysticks();
    }
};

/// A pad's stable identity: its hardware serial when it reports one (survives battery
/// swaps and reconnects), otherwise its model GUID. Two pads can legitimately share the
/// fallback identity - that is fine, because slots are keyed by connection instance and
/// this only matches returning pads to their own reservations.
std::string PadIdentity(SDL_JoystickID id) {
    const JoystickApiLock lock;
    SDL_Joystick* js = SDL_OpenJoystick(id);
    if (!js) {
        return {};
    }
    const char* serial = SDL_GetJoystickSerial(js);
    if (serial && serial[0]) {
        std::string out{"serial:"};
        out += serial;
        SDL_CloseJoystick(js);
        return out;
    }
    const SDL_GUID guid = SDL_GetJoystickGUID(js);
    SDL_CloseJoystick(js);
    char buf[33]{};
    for (int i = 0; i < 16; ++i) {
        unsigned byte = guid.data[i];
        // Bytes 2-3 are a CRC of the controller NAME: it rewrites itself when the name
        // rotates ("SteamDeck" <-> "Xbox" on the same pad), which is what made identities
        // appear to migrate between physical pads. InputCommon zeroes these bytes too
        // (SDLDriver::GetGUID): do the same, or identity can never be stable.
        if (i == 2 || i == 3) {
            byte = 0;
        }
        std::snprintf(buf + i * 2, 3, "%02x", byte);
    }
    return std::string{"guid:"} + buf;
}

/// A pad's display name (Xbox Controller, Steam Virtual Gamepad, ...).
QString PadName(SDL_JoystickID id) {
    const JoystickApiLock lock;
    SDL_Joystick* js = SDL_OpenJoystick(id);
    if (!js) {
        return QStringLiteral("?");
    }
    const char* name = SDL_GetJoystickName(js);
    QString out = name ? QString::fromUtf8(name) : QStringLiteral("?");
    SDL_CloseJoystick(js);
    return out;
}

/// True when the emu was launched through Steam: Steam Input is then in the picture and
/// the pads it hands over are virtual ones. A direct launch has neither.
bool LaunchedThroughSteam() {
    static const bool launched = std::getenv("SteamGameId") != nullptr ||
                                 std::getenv("SteamAppId") != nullptr;
    return launched;
}

/// True for Steam Input's virtual pads. Their reported identity (GUID/serial) is not
/// stable when pads cycle - Steam re-binds its virtual devices - so only Steam's own
/// player index is trustworthy for them. Raw pads (real names) keep the identity
/// machinery, and nothing changes when the emu is not launched through Steam.
bool IsSteamVirtualPad(SDL_JoystickID id) {
    if (!LaunchedThroughSteam()) {
        return false;
    }
    // Steam Input rewrites every virtual pad's GUID to carry Valve's vendor (0x28DE,
    // little-endian in bytes 4-5) whatever the real hardware is. The pad NAME is not
    // usable here: the same instance is seen as "SteamDeck" and "Xbox" in one session.
    const JoystickApiLock lock;
    SDL_Joystick* js = SDL_OpenJoystick(id);
    if (!js) {
        return false;
    }
    const SDL_GUID guid = SDL_GetJoystickGUID(js);
    SDL_CloseJoystick(js);
    // Valve makes real controllers too (the deck's own, unhidden for its motion): the virtual
    // gamepad product (0x11FF, bytes 8-9) is what marks Steam's virtual pads
    return guid.data[4] == 0xDE && guid.data[5] == 0x28 && guid.data[8] == 0xFF &&
           guid.data[9] == 0x11;
}

/// The OS device node of a pad (e.g. /dev/input/event5): sometimes the one identity a
/// virtualization layer keeps stable. Empty where unsupported.
QString PadPath(SDL_JoystickID id) {
    const JoystickApiLock lock;
    SDL_Joystick* js = SDL_OpenJoystick(id);
    if (!js) {
        return {};
    }
    const char* path = SDL_GetJoystickPath(js);
    QString out = path ? QString::fromUtf8(path) : QString{};
    SDL_CloseJoystick(js);
    return out;
}

/// True while any button of a pad is held: the Identify handshake reads this.
bool PadAnyButtonPressed(SDL_JoystickID id) {
    const JoystickApiLock lock;
    SDL_Joystick* js = SDL_OpenJoystick(id);
    if (!js) {
        return false;
    }
    bool pressed = false;
    const int count = SDL_GetNumJoystickButtons(js);
    for (int i = 0; i < count; ++i) {
        pressed = pressed || SDL_GetJoystickButton(js, i);
    }
    SDL_CloseJoystick(js);
    return pressed;
}

/// Steam's own player index for a pad (-1 when not assigned by Steam Input).
int PadPlayerIndex(SDL_JoystickID id) {
    const JoystickApiLock lock;
    SDL_Joystick* js = SDL_OpenJoystick(id);
    if (!js) {
        return -1;
    }
    const int index = SDL_GetJoystickPlayerIndex(js);
    SDL_CloseJoystick(js);
    return index;
}

/// A pad's USB vendor id, under the same lock as the rest of the API use.
Uint16 PadVendor(SDL_JoystickID id) {
    const JoystickApiLock lock;
    SDL_Joystick* js = SDL_OpenJoystick(id);
    if (!js) {
        return 0;
    }
    const Uint16 vendor = SDL_GetJoystickVendor(js);
    SDL_CloseJoystick(js);
    return vendor;
}

/// True when SDL reports a gyro on the pad. Under Steam Input this tells whether the virtual
/// pad passes motion through at all (the deck's own motion may only come from a DSU server).
bool PadHasGyro(SDL_JoystickID id) {
    const JoystickApiLock lock;
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        return false;
    }
    const bool gyro = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO);
    SDL_CloseGamepad(pad);
    return gyro;
}

/// Asks the Steam Deck's own controller to report its motion sensors. Steam Input turns them
/// on only while its layout uses the gyro, so this sends the controller's settings report
/// (0x87): IMU mode (0x30) = 0x18, both trackpads (0x07, 0x08) at 7 as Steam keeps them,
/// 0x31 = 2, 0x18 = 0.
bool EnableDeckMotion(const QString& path) {
#ifdef __linux__
    if (!path.startsWith(QStringLiteral("/dev/hidraw"))) {
        return false;
    }
    const int fd = ::open(path.toStdString().c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    std::array<unsigned char, 65> report{0x00, 0x87, 0x0f, 0x30, 0x18, 0x00, 0x07, 0x07, 0x00,
                                         0x08, 0x07, 0x00, 0x31, 0x02, 0x00, 0x18, 0x00};
    const bool sent =
        ::write(fd, report.data(), report.size()) == static_cast<ssize_t>(report.size());
    ::close(fd);
    return sent;
#else
    return false;
#endif
}

/// A pad's USB product id, under the same lock as the rest of the API use.
Uint16 PadProduct(SDL_JoystickID id) {
    const JoystickApiLock lock;
    SDL_Joystick* js = SDL_OpenJoystick(id);
    if (!js) {
        return 0;
    }
    const Uint16 product = SDL_GetJoystickProduct(js);
    SDL_CloseJoystick(js);
    return product;
}

/// Makers of standalone controllers: never a handheld's built-in controls, even when alone
bool IsControllerMaker(Uint16 vendor) {
    switch (vendor) {
    case 0x045E: // Microsoft
    case 0x054C: // Sony
    case 0x057E: // Nintendo
    case 0x2DC8: // 8BitDo
    case 0x046D: // Logitech
    case 0x20D6: // PowerA
    case 0x0E6F: // PDP
    case 0x0F0D: // Hori
    case 0x1532: // Razer
    case 0x0738: // Mad Catz
    case 0x146B: // Nacon
    case 0x3537: // GameSir
        return true;
    default:
        return false;
    }
}

/// Makers of handhelds with built-in controls: Valve, Lenovo, ASUS, MSI. A pad from one of
/// them is taken as the handheld's own on a direct launch.
bool IsHandheldMaker(Uint16 vendor) {
    return vendor == 0x28DE || vendor == 0x17EF || vendor == 0x0B05 || vendor == 0x0DB0;
}

/// True for the controller types Steam reports for standalone controllers
bool IsStandaloneControllerType(SDL_GamepadType type) {
    return type != SDL_GAMEPAD_TYPE_UNKNOWN && type != SDL_GAMEPAD_TYPE_STANDARD;
}

/// Steam Input's handle for the physical controller behind a pad (0 when not under Steam
/// Input, or not known). Steam's virtual pads are fixed slots that Steam refills in its own
/// order, so this handle is the only thing that could tell which physical pad came back.
Uint64 PadSteamHandle(SDL_JoystickID id) {
    const JoystickApiLock lock;
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        return 0;
    }
    const Uint64 handle = SDL_GetGamepadSteamHandle(pad);
    SDL_CloseGamepad(pad);
    return handle;
}

/// True while a pad has an input held: any button, or a stick or trigger past half travel.
bool PadInputHeld(SDL_JoystickID id) {
    const JoystickApiLock lock;
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        return false;
    }
    bool held = false;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !held; ++b) {
        held = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b));
    }
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && !held; ++a) {
        const Sint16 value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a));
        held = value > 16384 || value < -16384;
    }
    SDL_CloseGamepad(pad);
    return held;
}

/// A short rumble pulse on a pad, if it supports rumble.
void PadRumble(SDL_JoystickID id, Uint16 strength, Uint32 duration_ms) {
    const JoystickApiLock lock;
    if (SDL_Gamepad* pad = SDL_OpenGamepad(id)) {
        SDL_RumbleGamepad(pad, strength, strength, duration_ms);
        SDL_CloseGamepad(pad);
    }
}

/// An SDL motion binding for a device, as InputCommon's own auto-mapping builds it.
std::string SdlMotionParam(const std::string& guid, int port) {
    Common::ParamPackage param;
    param.Set("engine", "sdl");
    param.Set("motion", 0);
    param.Set("port", port);
    param.Set("guid", guid);
    return param.Serialize();
}

/// Points every SDL binding of a player at one device (guid + port), leaving keyboard and
/// other engines alone. Bindings address pads by (guid, port), never by slot, so this is
/// what makes a slot's pad the one that actually drives the player. True when it changed.
bool RouteSdlBindings(Settings::PlayerInput& player, const std::string& guid, int port) {
    bool routed = false;
    const auto route = [&](auto& params) {
        for (std::string& raw : params) {
            Common::ParamPackage param{raw};
            if (param.Get("engine", "") != "sdl") {
                continue;
            }
            const std::string old_guid = param.Get("guid", "");
            if (old_guid == guid && param.Get("port", -1) == port) {
                continue;
            }
            param.Set("guid", guid);
            param.Set("port", port);
            if (param.Has("guid2") && param.Get("guid2", "") == old_guid) {
                param.Set("guid2", guid);
            }
            raw = param.Serialize();
            routed = true;
        }
    };
    route(player.buttons);
    route(player.analogs);
    route(player.motions);
    return routed;
}

} // Anonymous namespace

void MainWindow::SaveAllConfigValues() {
    if (!console_profile) {
        config->SaveAllValues();
        return;
    }
    // A console mode drives the players at runtime; the file keeps the user's own. The
    // global copy is the one a global save writes, also while a game runs on its own copy.
    auto& players = Settings::values.players.GetValue(true);
    std::swap(players, *console_profile);
    config->SaveAllValues();
    std::swap(players, *console_profile);
}

namespace {
/// PROBE: the handheld set's motion bindings, to trace what clears the DSU gyro
std::string MotionTrace(const Settings::PlayerInput& player) {
    std::string out;
    for (const auto& motion : player.motions) {
        out += '[' + motion + ']';
    }
    return out;
}
} // Anonymous namespace

void MainWindow::RestoreConsoleProfile() {
    if (!console_profile) {
        return;
    }
    LOG_INFO(Frontend, "console-mode: PROBE restore, profile P1 motions {}",
             MotionTrace((*console_profile)[0]));
    // Booting a game switches the players to its own copy (the per-game config read), and
    // the engine drove that copy since. Both get the user's players back, or the global copy
    // keeps the engine's pre-boot state and that is what the next save writes.
    Settings::values.players.GetValue(true) = *console_profile;
    if (!Settings::values.players.UsingGlobal()) {
        Settings::values.players.GetValue() = ConsoleBindingSets();
    }
    console_profile.reset();
    game_profiles.reset();
}

std::array<Settings::PlayerInput, 10> MainWindow::ConsoleBindingSets() const {
    auto sets = *console_profile;
    if (game_profiles) {
        for (size_t i = 0; i < sets.size(); ++i) {
            if (!(*game_profiles)[i].profile_name.empty()) {
                sets[i] = (*game_profiles)[i];
            }
        }
    }
    return sets;
}

namespace {

/// A binding's mapping without its device (guid, port): what an edit in the Controls tab
/// changes, as opposed to the device routing that a console mode owns. Order- and
/// float-format-insensitive, so the tab rewriting "0.5" as "0.500000" is no edit.
std::string BindingMapping(const std::string& raw) {
    if (raw.empty()) {
        return {};
    }
    Common::ParamPackage param{raw};
    // Only an SDL binding's guid/port is device routing; for other engines (a DSU server's
    // address, say) they are the mapping itself
    if (param.Get("engine", "") == "sdl") {
        param.Erase("guid");
        param.Erase("guid2");
        param.Erase("port");
    }
    std::vector<std::string> fields;
    std::string field;
    for (const char c : param.Serialize() + ",") {
        if (c != ',') {
            field += c;
            continue;
        }
        const auto colon = field.find(':');
        if (colon != std::string::npos) {
            const std::string value = field.substr(colon + 1);
            char* end = nullptr;
            const float number = std::strtof(value.c_str(), &end);
            if (!value.empty() && end && *end == '\0') {
                field = field.substr(0, colon + 1) + fmt::format("{}", number);
            }
        }
        fields.push_back(field);
        field.clear();
    }
    std::sort(fields.begin(), fields.end());
    std::string out;
    for (const auto& f : fields) {
        out += f + ',';
    }
    return out;
}

/// Puts a profile's mappings on a binding set, keeping the set's own controller (guid, port)
/// on SDL bindings: the set stays on whatever controller carries it
void ApplyProfileMappings(Settings::PlayerInput& set, const Settings::PlayerInput& profile) {
    const auto apply = [](auto& to, const auto& from) {
        for (size_t i = 0; i < to.size(); ++i) {
            Common::ParamPackage taken{from[i]};
            const Common::ParamPackage kept{to[i]};
            if (!from[i].empty() && !to[i].empty() && taken.Get("engine", "") == "sdl" &&
                kept.Get("engine", "") == "sdl") {
                taken.Set("guid", kept.Get("guid", ""));
                taken.Set("port", kept.Get("port", 0));
                if (kept.Has("guid2")) {
                    taken.Set("guid2", kept.Get("guid2", ""));
                } else {
                    taken.Erase("guid2");
                }
                to[i] = taken.Serialize();
            } else {
                to[i] = from[i];
            }
        }
    };
    apply(set.buttons, profile.buttons);
    apply(set.analogs, profile.analogs);
    apply(set.motions, profile.motions);
}

/// Brings the edited mappings of one binding array into the profile's, keeping the profile's
/// own device (guid, port) on SDL bindings: that device is what the user's profile drives
/// outside the console modes. True when anything was taken.
template <typename Array>
bool MergeMappings(Array& profile, const Array& before, const Array& after) {
    bool taken = false;
    for (size_t i = 0; i < after.size(); ++i) {
        if (BindingMapping(after[i]) == BindingMapping(before[i])) {
            continue;
        }
        Common::ParamPackage edited{after[i]};
        const Common::ParamPackage original{profile[i]};
        if (!after[i].empty() && !profile[i].empty() && edited.Get("engine", "") == "sdl" &&
            original.Get("engine", "") == "sdl") {
            edited.Set("guid", original.Get("guid", ""));
            edited.Set("port", original.Get("port", 0));
            if (original.Has("guid2")) {
                edited.Set("guid2", original.Get("guid2", ""));
            } else {
                edited.Erase("guid2");
            }
            profile[i] = edited.Serialize();
        } else {
            profile[i] = after[i];
        }
        taken = true;
    }
    return taken;
}

} // Anonymous namespace

void MainWindow::MergeConsoleProfileEdits(const std::array<Settings::PlayerInput, 10>& before) {
    if (!console_profile) {
        return;
    }
    // The Controls tab edits the global copy, whatever copy a running game uses
    const auto& after = Settings::values.players.GetValue(true);
    // The handheld's own non-SDL motion (a DSU server on the deck) is held back from every
    // external pad: whatever the dialog makes of a player the handheld does not hold, it must
    // never clear that binding from the handheld's set. Kept here and put back at the end.
    const auto handheld_motions_kept = (*console_profile)[0].motions;
    const auto own_slot = pad_slots.find(builtin_pad_id);
    // (While the handheld holds a player, edits on that player are its set's own: only a
    // handheld without a player - externals around - cannot have edited it)
    const bool handheld_has_player = builtin_pad_id && own_slot != pad_slots.end();
    LOG_INFO(Frontend,
             "console-mode: PROBE merge start, profile P1 motions {} before {} after {} "
             "handheld_has_player={}",
             MotionTrace((*console_profile)[0]), MotionTrace(before[0]), MotionTrace(after[0]),
             handheld_has_player);
    // The SDL device (guid, port) a player's bindings point at
    const auto sdl_device = [](const Settings::PlayerInput& player) {
        for (const std::string& raw : player.buttons) {
            const Common::ParamPackage param{raw};
            if (!raw.empty() && param.Get("engine", "") == "sdl") {
                return std::optional<std::pair<std::string, int>>{
                    {param.Get("guid", ""), param.Get("port", 0)}};
            }
        }
        return std::optional<std::pair<std::string, int>>{};
    };
    for (size_t i = 0; i < after.size(); ++i) {
        const auto& a = after[i];
        const auto& b = before[i];
        // Picking a device in the tab re-maps the player to that device's defaults: a device
        // change is a controller swap (below), and its side-effect mapping is no edit
        if (const auto device = sdl_device(a); device && device != sdl_device(b)) {
            continue;
        }
        // The connected flags and the device routing stay the engine's: the engine keeps
        // re-pointing bindings while the tab is open (the tab even switches the players to
        // the global copy the engine drives), so only mapping changes count as edits. An edit
        // on the handheld's own pad goes to player one's set, the one it carries everywhere.
        const auto own = pad_slots.find(builtin_pad_id);
        size_t target = static_cast<size_t>(slot_set[i]);
        auto& p = (*console_profile)[target];
        bool edited = MergeMappings(p.buttons, b.buttons, a.buttons);
        edited = MergeMappings(p.analogs, b.analogs, a.analogs) || edited;
        // An external pad never gets the handheld's own non-SDL motion (a DSU server on the
        // deck): on a player the handheld does not hold, that binding being absent is the
        // engine's doing, never an edit, and must not wipe it from the handheld's set (a swap
        // closing the dialog did, through P1 held by an Xbox pad)
        auto edited_motions = a.motions;
        if (target != 0 || !(builtin_pad_id && own != pad_slots.end() &&
                             own->second == static_cast<int>(i))) {
            const auto& handheld_motions = (*console_profile)[0].motions;
            for (size_t k = 0; k < edited_motions.size(); ++k) {
                const std::string& kept = p.motions[k];
                const bool handheld_motion =
                    !kept.empty() && Common::ParamPackage{kept}.Get("engine", "") != "sdl" &&
                    std::find(handheld_motions.begin(), handheld_motions.end(), kept) !=
                        handheld_motions.end();
                if (handheld_motion && edited_motions[k].empty()) {
                    edited_motions[k] = b.motions[k];
                }
            }
        }
        edited = MergeMappings(p.motions, b.motions, edited_motions) || edited;
        // The profile picked in the player's tab (its name, for the tab to show it again)
        if (a.profile_name != b.profile_name) {
            p.profile_name = a.profile_name;
            edited = true;
        }
        if (a.controller_type != b.controller_type || a.vibration_enabled != b.vibration_enabled ||
            a.vibration_strength != b.vibration_strength) {
            p.controller_type = a.controller_type;
            p.vibration_enabled = a.vibration_enabled;
            p.vibration_strength = a.vibration_strength;
            edited = true;
        }
        if (!edited) {
            continue;
        }
        LOG_INFO(Frontend, "console-mode: Controls edit on P{} kept in profile P{}", i + 1,
                 target + 1);
    }

    if (!handheld_has_player) {
        auto& motions = (*console_profile)[0].motions;
        for (size_t k = 0; k < motions.size(); ++k) {
            const std::string& kept = handheld_motions_kept[k];
            if (motions[k].empty() && !kept.empty() &&
                Common::ParamPackage{kept}.Get("engine", "") != "sdl") {
                motions[k] = kept;
                LOG_INFO(Frontend, "console-mode: kept the handheld's motion {} on P1", kept);
            }
        }
    }
    LOG_INFO(Frontend, "console-mode: PROBE merge end, profile P1 motions {}",
             MotionTrace((*console_profile)[0]));

    // A controller swap from the Controls tab: a player whose device was changed to the
    // device another player's pad holds trades slots with it (or takes it, when the player had
    // no pad). Each pad then carries its new player's binding set; the routing pass re-points
    // them next tick. A device no pad holds is ignored, like any other device change here.
    if (!input_subsystem) {
        return;
    }
    // The open pads and the (guid, port) each is bound as; the driver lock must not be held
    // around GetSDLBinding, so the instances are listed first
    std::vector<u32> instances;
    {
        const JoystickApiLock lock;
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int k = 0; k < count; ++k) {
            instances.push_back(ids[k]);
        }
        SDL_free(ids);
    }
    std::map<u32, std::pair<std::string, int>> bound;
    for (const u32 inst : instances) {
        if (const auto binding = input_subsystem->GetSDLBinding(inst)) {
            bound[inst] = *binding;
        }
    }
    for (size_t i = 0; i < after.size(); ++i) {
        const auto chosen = sdl_device(after[i]);
        if (!chosen || chosen == sdl_device(before[i])) {
            continue;
        }
        u32 pad = 0;
        for (const auto& [inst, binding] : bound) {
            if (binding == *chosen) {
                pad = inst;
                break;
            }
        }
        if (!pad) {
            continue;
        }
        if (pad == builtin_pad_id &&
            Settings::values.controller_applet_mode.GetValue() ==
                Settings::AppletMode::HandheldConsoleMode) {
            LOG_INFO(Frontend, "console-mode: Controls swap to P{} ignored: the handheld's own pad",
                     i + 1);
            continue;
        }
        const auto pad_slot = pad_slots.find(pad);
        if (pad_slot != pad_slots.end() && pad_slot->second == static_cast<int>(i)) {
            continue;
        }
        u32 holder = 0;
        for (const auto& [inst, slot] : pad_slots) {
            if (slot == static_cast<int>(i) && bound.contains(inst)) {
                holder = inst;
                break;
            }
        }
        const int from = pad_slot != pad_slots.end() ? pad_slot->second : -1;
        if (holder) {
            if (from >= 0) {
                pad_slots[holder] = from;
            } else {
                pad_slots.erase(holder);
                pad_identity.erase(holder);
            }
        }
        pad_slots[pad] = static_cast<int>(i);
        LOG_INFO(Frontend, "console-mode: Controls swap: inst={} P{} -> P{}{}", pad,
                 from >= 0 ? from + 1 : 0, i + 1,
                 holder ? fmt::format(", inst={} P{} -> P{}", holder, i + 1,
                                      from >= 0 ? from + 1 : 0)
                        : std::string{});
    }
}

namespace {
/// The green/red flashes blink four times a second for their 1.5 seconds
bool BlinkOn(qint64 elapsed_ms) {
    return elapsed_ms < 1500 && (elapsed_ms / 125) % 2 == 0;
}
} // Anonymous namespace

void MainWindow::RenderControllerOverlay() {
    // A row lights blue while its controller has an input held, or for a moment after a tap
    // (so a tap between two polls still shows). Motion never lights it: a resting gyro reports
    // constant noise.
    const auto now = std::chrono::steady_clock::now();
    std::set<u32> active;
    for (const auto& row : overlay_rows) {
        if (row.pad == 0) {
            continue;
        }
        const auto last =
            input_subsystem ? input_subsystem->GetSDLLastActivity(row.pad) : std::nullopt;
        if (PadInputHeld(row.pad) || (last && now - *last < std::chrono::milliseconds{250})) {
            active.insert(row.pad);
        }
    }
    overlay_active_pads = active;
    {
        auto& state = Common::GetControllerConsoleState();
        std::scoped_lock lock{state.mutex};
        for (auto& player : state.players) {
            const auto row = std::find_if(overlay_rows.begin(), overlay_rows.end(),
                                          [&](const auto& r) { return r.player == player.player; });
            player.active = row != overlay_rows.end() && active.contains(row->pad);
        }
    }

    const auto mode = Settings::values.controller_applet_mode.GetValue();
    const bool automatic = mode == Settings::AppletMode::ConsoleMode ||
                           mode == Settings::AppletMode::HandheldConsoleMode;
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const auto publish = [](Common::ControllerOverlayFrame& frame, const QImage* image) {
        std::scoped_lock lock{frame.mutex};
        if (!image) {
            if (frame.enabled) {
                frame.enabled = false;
                ++frame.version;
            }
            return;
        }
        frame.width = static_cast<u32>(image->width());
        frame.height = static_cast<u32>(image->height());
        frame.pixels.resize(image->sizeInBytes());
        std::memcpy(frame.pixels.data(), image->constBits(), image->sizeInBytes());
        frame.enabled = true;
        ++frame.version;
    };

    static constexpr std::array<QColor, 8> player_colors{
        QColor(79, 142, 247), QColor(239, 91, 91), QColor(47, 191, 113), QColor(245, 184, 46),
        QColor(155, 108, 242), QColor(242, 140, 56), QColor(46, 196, 201), QColor(227, 107, 174),
    };
    const auto player_color = [](int player) { return player_colors[player % 8]; };
    QFont badge_font;
    badge_font.setPixelSize(12);
    badge_font.setBold(true);

    // Hints: a controller that took a player (green flash) or left one (red flash), named for a
    // few seconds in the bottom-left corner, whether the overlay shows or not
    constexpr qint64 hint_ms = 3000;
    for (const auto& row : overlay_rows) {
        const int i = row.player;
        if (row.flash == 1 && row.pad != 0 && row.flash_since > hinted_join[i]) {
            hinted_join[i] = row.flash_since;
            controller_hints.push_back({overlay_row_name[i], i, true, row.flash_since});
        } else if (row.flash == 2 && row.flash_since > hinted_leave[i]) {
            hinted_leave[i] = row.flash_since;
            controller_hints.push_back({overlay_row_name[i], i, false, row.flash_since});
        }
    }
    std::erase_if(controller_hints,
                  [now_ms](const auto& hint) { return now_ms - hint.at >= hint_ms; });
    if (!automatic || !Settings::values.console_hints.GetValue() || controller_hints.empty()) {
        publish(Common::GetControllerHintFrame(), nullptr);
    } else {
        QFont hint_font;
        hint_font.setPixelSize(15);
        hint_font.setBold(true);
        const QFontMetrics hint_fm{hint_font};
        constexpr int hint_h = 30;
        constexpr int hint_gap = 6;
        constexpr int badge_w = 36;
        const auto text_of = [](const ControllerHint& hint) {
            return hint.joined ? tr("%1 joined").arg(hint.name) : tr("%1 left").arg(hint.name);
        };
        int width = 0;
        for (const auto& hint : controller_hints) {
            width = (std::max)(width, 14 + hint_fm.horizontalAdvance(text_of(hint)) + 10 +
                                          badge_w + 6);
        }
        // The widest a hint can be, so the image keeps its size while hints come and go
        width = (std::max)(width, 260) + 2;
        const int count = static_cast<int>(controller_hints.size());
        QImage image{width, count * (hint_h + hint_gap), QImage::Format_RGBA8888};
        image.fill(Qt::transparent);
        QPainter painter{&image};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        // Newest at the bottom, nearest the corner
        int y = 0;
        for (const auto& hint : controller_hints) {
            const QString text = text_of(hint);
            const int w = 14 + hint_fm.horizontalAdvance(text) + 10 + badge_w + 6;
            const qint64 left = hint_ms - (now_ms - hint.at);
            const int alpha = left < 500 ? static_cast<int>(255 * left / 500) : 255;
            painter.setPen(Qt::NoPen);
            painter.setBrush(hint.joined ? QColor(18, 18, 22, 215 * alpha / 255)
                                         : QColor(96, 22, 28, 220 * alpha / 255));
            painter.drawRoundedRect(QRectF(1, y, w, hint_h), hint_h / 2.0, hint_h / 2.0);
            painter.setFont(hint_font);
            painter.setPen(QColor(255, 255, 255, alpha));
            painter.drawText(QRect(15, y, w, hint_h), Qt::AlignVCenter | Qt::AlignLeft, text);
            QColor badge = player_color(hint.player);
            badge.setAlpha(hint.joined ? alpha : alpha / 2);
            painter.setPen(Qt::NoPen);
            painter.setBrush(badge);
            const QRectF badge_rect(1 + w - 6 - badge_w, y + 5, badge_w, hint_h - 10);
            painter.drawRoundedRect(badge_rect, badge_rect.height() / 2, badge_rect.height() / 2);
            painter.setFont(badge_font);
            painter.setPen(QColor(255, 255, 255, alpha));
            painter.drawText(badge_rect, Qt::AlignCenter,
                             QStringLiteral("P%1").arg(hint.player + 1));
            y += hint_h + hint_gap;
        }
        painter.end();
        publish(Common::GetControllerHintFrame(), &image);
    }

    overlay_blink.clear();
    for (const auto& row : overlay_rows) {
        overlay_blink.push_back(row.flash != 0 && BlinkOn(now_ms - row.flash_since));
    }
    if (!Settings::values.console_debug_overlay.GetValue() || !automatic ||
        overlay_rows.empty()) {
        publish(Common::GetControllerOverlayFrame(), nullptr);
        return;
    }

    // The Switch buttons as chips: a label and a colour
    const auto chip = [](int button) -> std::pair<QString, QColor> {
        using namespace Settings::NativeButton;
        switch (button) {
        case A:
            return {QStringLiteral("A"), QColor(231, 76, 60)};
        case B:
            return {QStringLiteral("B"), QColor(230, 180, 20)};
        case X:
            return {QStringLiteral("X"), QColor(52, 152, 219)};
        case Y:
            return {QStringLiteral("Y"), QColor(46, 184, 104)};
        case LStick:
            return {QStringLiteral("LS"), QColor(70, 90, 112)};
        case RStick:
            return {QStringLiteral("RS"), QColor(70, 90, 112)};
        case L:
            return {QStringLiteral("L"), QColor(120, 132, 140)};
        case R:
            return {QStringLiteral("R"), QColor(120, 132, 140)};
        case ZL:
            return {QStringLiteral("ZL"), QColor(98, 108, 116)};
        case ZR:
            return {QStringLiteral("ZR"), QColor(98, 108, 116)};
        case Plus:
            return {QStringLiteral("+"), QColor(142, 88, 182)};
        case Minus:
            return {QStringLiteral("−"), QColor(142, 88, 182)};
        case DLeft:
            return {QStringLiteral("←"), QColor(84, 99, 116)};
        case DUp:
            return {QStringLiteral("↑"), QColor(84, 99, 116)};
        case DRight:
            return {QStringLiteral("→"), QColor(84, 99, 116)};
        case DDown:
            return {QStringLiteral("↓"), QColor(84, 99, 116)};
        case Home:
            return {QStringLiteral("⌂"), QColor(60, 60, 66)};
        case Screenshot:
            return {QStringLiteral("◉"), QColor(60, 60, 66)};
        case SLLeft:
        case SLRight:
            return {QStringLiteral("SL"), QColor(120, 132, 140)};
        default:
            return {QStringLiteral("SR"), QColor(120, 132, 140)};
        }
    };
    QFont name_font;
    name_font.setPixelSize(13);
    QFont chip_font;
    chip_font.setPixelSize(11);
    chip_font.setBold(true);
    const QFontMetrics name_fm{name_font};
    const QFontMetrics chip_fm{chip_font};

    constexpr int row_h = 28;
    constexpr int row_gap = 4;
    constexpr int badge_w = 30;
    constexpr int inner = 6;
    constexpr int bubble_gap = 6;
    constexpr int chip_h = 18;
    constexpr int chip_gap = 3;
    constexpr int widest_chip = 26;
    const auto chip_width = [&](const QString& text) {
        return (std::max)(chip_h, chip_fm.horizontalAdvance(text) + 10);
    };
    // An entry lives 5 seconds, then shrinks away
    constexpr qint64 press_life_ms = 5000;
    constexpr qint64 press_shrink_ms = 400;
    const auto press_scale = [now_ms](const ButtonPress& press) {
        const qint64 over = now_ms - press.at - press_life_ms;
        if (over <= 0) {
            return 1.0;
        }
        return (std::max)(0.0, 1.0 - static_cast<double>(over) / press_shrink_ms);
    };

    int name_w = 0;
    for (const auto& row : overlay_rows) {
        name_w = (std::max)(name_w, name_fm.horizontalAdvance(overlay_row_name[row.player]));
    }
    const int controller_w = inner + badge_w + 8 + name_w + inner + 4;
    // Room for ten of the widest chips, so the image keeps its size as the bubbles change
    const int history_room = inner * 2 + 10 * (widest_chip + chip_gap);
    const int width = controller_w + bubble_gap + history_room + 2;
    const int count = static_cast<int>(overlay_rows.size());
    const int height = count * row_h + (count - 1) * row_gap + 2;

    QImage image{width, height, QImage::Format_RGBA8888};
    image.fill(Qt::transparent);
    QPainter painter{&image};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(Qt::NoPen);
    int y = 1;
    for (std::size_t r = 0; r < overlay_rows.size(); ++r) {
        const auto& row = overlay_rows[r];
        const bool blink = overlay_blink[r];
        const bool gone = row.pad == 0;

        // The controller bubble: its state lights it (blue held, green joined, red left)
        QColor fill(14, 14, 18, 175);
        if (row.flash == 2) {
            if (blink) {
                fill = QColor(190, 50, 50, 200);
            }
        } else if (!gone && active.contains(row.pad)) {
            fill = QColor(45, 95, 200, 200);
        } else if (blink) {
            fill = QColor(45, 160, 80, 200);
        }
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(1, y, controller_w, row_h), row_h / 2.0, row_h / 2.0);
        QColor badge = player_color(row.player);
        if (gone) {
            badge.setAlpha(110);
        }
        painter.setBrush(badge);
        const QRectF badge_rect(1 + inner, y + 5, badge_w, row_h - 10);
        painter.drawRoundedRect(badge_rect, badge_rect.height() / 2, badge_rect.height() / 2);
        painter.setFont(badge_font);
        painter.setPen(Qt::white);
        painter.drawText(badge_rect, Qt::AlignCenter, QStringLiteral("P%1").arg(row.player + 1));
        painter.setFont(name_font);
        painter.setPen(gone ? QColor(255, 255, 255, 120) : QColor(240, 240, 240));
        painter.drawText(QRect(1 + inner + badge_w + 8, y, name_w + 4, row_h),
                         Qt::AlignVCenter | Qt::AlignLeft, overlay_row_name[row.player]);
        painter.setPen(Qt::NoPen);

        // The history bubble: the last presses, newest first and brightest, each shrinking
        // away at the far end as it expires (and the bubble with them)
        const auto& presses = press_history[row.player];
        double chips_w = 0;
        for (const auto& press : presses) {
            chips_w += (chip_width(chip(press.button).first) + chip_gap) * press_scale(press);
        }
        if (chips_w > 0.5) {
            const double bubble_x = 1 + controller_w + bubble_gap;
            const double bubble_w = inner * 2 + chips_w - chip_gap;
            painter.setBrush(QColor(14, 14, 18, 175));
            painter.drawRoundedRect(QRectF(bubble_x, y, (std::max)(bubble_w, double{row_h}), row_h),
                                    row_h / 2.0, row_h / 2.0);
            double cx = bubble_x + inner;
            const int n = static_cast<int>(presses.size());
            for (int k = n - 1; k >= 0; --k) {
                const double scale = press_scale(presses[k]);
                if (scale <= 0.0) {
                    continue;
                }
                const auto& [label, color] = chip(presses[k].button);
                const int alpha = static_cast<int>((255 - (n - 1 - k) * 12) * scale);
                const double w = chip_width(label) * scale;
                const double h = chip_h * scale;
                QColor chip_color = color;
                chip_color.setAlpha(alpha);
                const QRectF rect(cx, y + (row_h - h) / 2.0, w, h);
                painter.setBrush(chip_color);
                painter.drawRoundedRect(rect, h / 2.0, h / 2.0);
                if (scale > 0.6) {
                    painter.setFont(chip_font);
                    painter.setPen(QColor(255, 255, 255, alpha));
                    painter.drawText(rect, Qt::AlignCenter, label);
                    painter.setPen(Qt::NoPen);
                }
                cx += w + chip_gap * scale;
            }
        }
        y += row_h + row_gap;
    }
    painter.end();
    publish(Common::GetControllerOverlayFrame(), &image);
}

bool MainWindow::OverlayAnimating() const {
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (!controller_hints.empty()) {
        return true;
    }
    for (const auto& history : press_history) {
        if (!history.empty() && now_ms - history.front().at > 4900) {
            return true;
        }
    }
    return false;
}

void MainWindow::PollControllerActivity() {
    // Input shows faster than the 250ms assignment tick: redraw only when who is active changes
    // (or while the hints and history animate)
    if (overlay_rows.empty()) {
        if (!controller_hints.empty()) {
            RenderControllerOverlay();
        }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::set<u32> active;
    for (const auto& row : overlay_rows) {
        if (row.pad == 0) {
            continue;
        }
        const auto last =
            input_subsystem ? input_subsystem->GetSDLLastActivity(row.pad) : std::nullopt;
        if (PadInputHeld(row.pad) || (last && now - *last < std::chrono::milliseconds{250})) {
            active.insert(row.pad);
        }
    }
    // ...and when a green/red flash blinks
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    std::vector<bool> blink;
    for (const auto& row : overlay_rows) {
        blink.push_back(row.flash != 0 && BlinkOn(now_ms - row.flash_since));
    }
    const bool pressed = PollButtonHistory();
    if (pressed || OverlayAnimating() || active != overlay_active_pads || blink != overlay_blink) {
        RenderControllerOverlay();
    }
}

bool MainWindow::PollButtonHistory() {
    if (!QtCommon::system || !QtCommon::system->IsPoweredOn()) {
        return false;
    }
    auto& hid = QtCommon::system->HIDCore();
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    bool pressed = false;
    for (const auto& row : overlay_rows) {
        if (row.pad == 0 || row.player < 0 || row.player >= 8) {
            continue;
        }
        const int i = row.player;
        // Another controller took the player: its history starts over
        if (row.flash == 1 && row.flash_since != press_history_since[i]) {
            press_history_since[i] = row.flash_since;
            press_history[i].clear();
            pressed = true;
        }
        const auto& player = Settings::values.players.GetValue()[i];
        auto* controller = i == 0 && player.controller_type == Settings::ControllerType::Handheld
                               ? hid.GetEmulatedController(Core::HID::NpadIdType::Handheld)
                               : hid.GetEmulatedControllerByIndex(i);
        if (!controller) {
            continue;
        }
        // Entries gone through their life and shrink leave the history
        auto& history = press_history[i];
        while (!history.empty() && now_ms - history.front().at > 5400) {
            history.pop_front();
            pressed = true;
        }
        const auto values = controller->GetButtonsValues();
        auto& down = buttons_down[i];
        for (int b = 0; b < Settings::NativeButton::NumButtons; ++b) {
            const bool held = values[b].value;
            if (held && !down[b]) {
                history.push_back({b, now_ms});
                if (history.size() > 10) {
                    history.pop_front();
                }
                pressed = true;
            }
            down[b] = held;
        }
    }
    return pressed;
}

void MainWindow::ApplyConsoleControllerMode() {
    const auto mode = Settings::values.controller_applet_mode.GetValue();
    const bool handheld = mode == Settings::AppletMode::HandheldConsoleMode;
    const bool console = mode == Settings::AppletMode::ConsoleMode;
    // The handheld's own pad is stored once per way of launching, since the pads differ
    // completely: under Steam it is the Steam handle (builtin_pad_steam_handle), launched
    // directly it is the raw pad's identity (builtin_pad_identity, with builtin_pad_manual
    // when declared). Older builds stored the Steam virtual pad's GUID as the identity, which
    // no raw pad ever has: launched directly, drop it so auto-detection finds the deck again.
    if (!LaunchedThroughSteam()) {
        const std::string identity = Settings::values.builtin_pad_identity.GetValue();
        if (identity.size() >= 25 && identity.starts_with("guid:") &&
            identity.compare(5 + 8, 4, "de28") == 0 && identity.compare(5 + 16, 4, "ff11") == 0) {
            Settings::values.builtin_pad_identity.SetValue(std::string{});
            Settings::values.builtin_pad_manual.SetValue(false);
            LOG_INFO(Frontend, "console-mode: stored handheld was a Steam virtual pad ({}), "
                               "back to auto-detect for direct launches",
                     identity);
        }
    }
    // A declaration with no pad behind it (older builds wiped the identity when leaving the
    // mode but kept the flag) would pin the engine to "no handheld" forever: undo it.
    if (Settings::values.builtin_pad_manual.GetValue() &&
        Settings::values.builtin_pad_identity.GetValue().empty()) {
        Settings::values.builtin_pad_manual.SetValue(false);
        LOG_INFO(Frontend, "console-mode: declared handheld had no identity, back to auto");
    }

    // The live set of gamepads. The map key is the connection instance (always unique:
    // two pads can share a serial or GUID through Steam Input and must never collapse),
    // and pad_identity records the stable identity used to reclaim slots.
    //
    // Under Steam Input the virtual pads are not the controllers: every one shares a GUID,
    // and Steam moves physical controllers between them while they stay connected (the
    // deck's own controller moves to the newest virtual pad whenever an external one
    // connects). The Steam handle travels with the physical controller, so it is the
    // identity there. Steam fills it in a moment after a virtual pad appears: until then the
    // pad is not live yet (3 seconds at most, then its GUID identity has to do).
    std::map<u32, std::string> live;
    /// Under Steam: real devices unhidden for their motion (never players)
    std::vector<u32> motion_devices;
    {
        static std::map<u32, std::chrono::steady_clock::time_point> first_seen;
        const auto now = std::chrono::steady_clock::now();
        std::set<u32> present;
        const JoystickApiLock lock;
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int i = 0; i < count; ++i) {
            const u32 id = ids[i];
            present.insert(id);
            first_seen.try_emplace(id, now);
            if (LaunchedThroughSteam() && !IsSteamVirtualPad(id)) {
                // A real device next to Steam's virtual pads, unhidden for its motion: never a
                // player; logged once, with the virtual pads of the same model
                motion_devices.push_back(id);
                static std::set<u32> probed;
                if (probed.insert(id).second) {
                    std::string twins;
                    const Uint16 vendor = SDL_GetJoystickVendorForID(id);
                    const Uint16 product = SDL_GetJoystickProductForID(id);
                    for (int k = 0; k < count; ++k) {
                        if (ids[k] != id && IsSteamVirtualPad(ids[k]) &&
                            SDL_GetJoystickVendorForID(ids[k]) == vendor &&
                            SDL_GetJoystickProductForID(ids[k]) == product) {
                            twins += fmt::format("{} ", ids[k]);
                        }
                    }
                    LOG_INFO(Frontend,
                             "console-mode: real device inst={} {:04x}:{:04x} name={} "
                             "gyro={} path={} same model as virtual inst=[{}]",
                             id, vendor, product, PadName(id).toStdString(), PadHasGyro(id),
                             PadPath(id).toStdString(), twins);
                }
                continue;
            }
            std::string ident = PadIdentity(id);
            if (LaunchedThroughSteam()) {
                if (const Uint64 handle = PadSteamHandle(id)) {
                    ident = fmt::format("steamhandle:{:#x}", handle);
                } else if (now - first_seen[id] < std::chrono::seconds{3}) {
                    continue;
                }
            }
            // What a user's log needs for adding their handheld to the allowlist
            static std::set<std::string> described;
            if (described.insert(ident).second) {
                const Uint16 vendor = SDL_GetJoystickVendorForID(id);
                // (SDL has no name for an unknown type: null, which fmt refuses)
                const char* type = SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(id));
                LOG_INFO(Frontend,
                         "console-mode: device {:04x}:{:04x} name=\"{}\" type={} steam={} "
                         "gyro={} handheld_maker={} controller_maker={} ident={}",
                         vendor, SDL_GetJoystickProductForID(id), PadName(id).toStdString(),
                         type ? type : "unknown",
                         IsSteamVirtualPad(id), PadHasGyro(id), IsHandheldMaker(vendor),
                         IsControllerMaker(vendor), ident);
            }
            live[id] = std::move(ident);
        }
        SDL_free(ids);
        std::erase_if(first_seen, [&present](const auto& e) { return !present.contains(e.first); });
    }

    // The deck's own controller only reports motion while its IMU is on, which Steam Input
    // leaves off unless its layout uses the gyro: keep it on while the real device is open,
    // so its motion is there whenever the deck holds a player
    {
        const auto deck = std::find_if(motion_devices.begin(), motion_devices.end(), [](u32 id) {
            return SDL_GetJoystickVendorForID(id) == 0x28DE &&
                   SDL_GetJoystickProductForID(id) == 0x1205;
        });
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        if (deck != motion_devices.end() && now_ms - deck_motion_sent_at >= 3000) {
            deck_motion_sent_at = now_ms;
            const QString path = PadPath(*deck);
            const bool sent = EnableDeckMotion(path);
            static bool logged = false;
            if (!sent || !logged) {
                logged = true;
                LOG_INFO(Frontend, "console-mode: deck motion on via {}: {}", path.toStdString(),
                         sent ? "sent" : "failed");
            }
        }
    }

    // A controller Steam moved to another virtual pad takes its slot and its handheld latch
    // along: the slot belongs to the controller, not to the virtual pad
    if (LaunchedThroughSteam()) {
        static std::map<std::string, u32> handle_inst;
        std::map<u32, u32> moves; // to <- from
        for (const auto& [id, ident] : live) {
            if (!ident.starts_with("steamhandle:")) {
                continue;
            }
            const auto it = handle_inst.find(ident);
            if (it != handle_inst.end() && it->second != id) {
                moves[id] = it->second;
            }
        }
        if (!moves.empty()) {
            std::map<u32, int> old_slot;
            std::set<u32> involved;
            for (const auto& [to, from] : moves) {
                involved.insert(to);
                involved.insert(from);
            }
            for (const u32 inst : involved) {
                if (const auto it = pad_slots.find(inst); it != pad_slots.end()) {
                    old_slot[inst] = it->second;
                    pad_slots.erase(it);
                }
                pad_identity.erase(inst);
            }
            u32 new_builtin = builtin_pad_id;
            for (const auto& [to, from] : moves) {
                const auto slot = old_slot.find(from);
                LOG_INFO(Frontend, "console-mode: Steam moved {} from inst={} to inst={} (P{})",
                         live[to], from, to, slot != old_slot.end() ? slot->second + 1 : 0);
                if (slot != old_slot.end()) {
                    pad_slots[to] = slot->second;
                    pad_identity[to] = live[to];
                }
                if (builtin_pad_id == from) {
                    new_builtin = to;
                }
            }
            builtin_pad_id = new_builtin;
        }
        handle_inst.clear();
        for (const auto& [id, ident] : live) {
            if (ident.starts_with("steamhandle:")) {
                handle_inst[ident] = id;
            }
        }
    }

    // Publish the live pads to the settings UI, and service the Identify handshake: the
    // next pad with a button held becomes the handheld's own by declaration, not guess.
    // Both run whatever the mode: the applets tab declares the handheld's pad while the
    // mode is only picked in its dropdown, before OK applies it.
    {
        auto& state = Common::GetControllerConsoleState();
        std::scoped_lock lock{state.mutex};
        state.pads.clear();
        for (const auto& [id, ident] : live) {
            // Under Steam every pad shares one GUID identity, so the picker would declare
            // them all at once: its entries carry the Steam handle there instead
            const Uint64 handle = LaunchedThroughSteam() ? PadSteamHandle(id) : 0;
            state.pads.emplace_back(PadName(id).toStdString(),
                                    handle ? fmt::format("steamhandle:{:#x}", handle) : ident);
        }
        // A human tap is shorter than 250ms: poll fast while the Identify handshake is
        // armed so a quick press cannot slip between the samples.
        console_mode_timer.start(std::chrono::milliseconds{state.identify_armed ? 50 : 250});
        if (state.identify_armed) {
            const auto now = std::chrono::steady_clock::now();
            if (now - state.identify_armed_at > std::chrono::seconds{15}) {
                state.identify_armed = false;
            } else {
                for (const auto& [id, ident] : live) {
                    if (PadAnyButtonPressed(id)) {
                        state.identify_armed = false;
                        builtin_pad_id = id;
                        builtin_identity = ident;
                        builtin_seen_first = true;
                        builtin_warned = false;
                        // Each way of launching keeps its own: the other one's stays
                        const Uint64 handle = LaunchedThroughSteam() ? PadSteamHandle(id) : 0;
                        if (LaunchedThroughSteam()) {
                            if (handle) {
                                Settings::values.builtin_pad_steam_handle.SetValue(
                                    fmt::format("{:#x}", handle));
                            }
                        } else {
                            Settings::values.builtin_pad_identity.SetValue(ident);
                            Settings::values.builtin_pad_manual.SetValue(true);
                        }
                        LOG_INFO(Frontend,
                                 "console-mode: identify captured inst={} ident={} "
                                 "steam_handle={:#x}",
                                 id, ident, handle);
                        break;
                    }
                }
            }
        }
    }

    // The applets tab's picker declares a pad by its Steam handle under Steam (see the
    // published entries): keep the handle as such, and the pad's GUID as its identity
    {
        const std::string declared = Settings::values.builtin_pad_identity.GetValue();
        constexpr std::string_view prefix = "steamhandle:";
        if (declared.starts_with(prefix)) {
            const std::string handle = declared.substr(prefix.size());
            for (const auto& [id, ident] : live) {
                if (fmt::format("{:#x}", PadSteamHandle(id)) == handle) {
                    builtin_pad_id = id;
                    break;
                }
            }
            // The identity is the direct launches' own: a Steam pick only sets the handle
            Settings::values.builtin_pad_steam_handle.SetValue(handle);
            Settings::values.builtin_pad_identity.SetValue(std::string{});
            Settings::values.builtin_pad_manual.SetValue(false);
            LOG_INFO(Frontend, "console-mode: picker declared Steam handle {} inst={}", handle,
                     builtin_pad_id);
        }
    }

    // The overlay only exists in the automatic modes: so does its View menu toggle
    ui->action_Show_Controller_Overlay->setVisible(handheld || console);
    if (!handheld && !console) {
        if (!overlay_rows.empty()) {
            overlay_rows.clear();
            RenderControllerOverlay();
        }
        {
            auto& state = Common::GetControllerConsoleState();
            std::scoped_lock lock{state.mutex};
            state.engine_active = false;
            state.players.clear();
            state.handheld_name.clear();
            state.handheld_has_player = false;
            state.swap_requests.clear();
        }
        if (console_profile) {
            // Leaving the console modes: the user's own players come back exactly as they were
            RestoreConsoleProfile();
            pad_slots.clear();
            pad_identity.clear();
            pad_sets.clear();
            slot_set = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
            set_before_profile.clear();
            LOG_INFO(Frontend, "console-mode: left, the user's players are restored");
            if (QtCommon::system && QtCommon::system->IsPoweredOn()) {
                QtCommon::system->HIDCore().ReloadInputDevices();
            }
        }
        if (builtin_seen_first) {
            // Leaving the mode forgets the auto latch: re-selecting it later re-detects. A
            // declared built-in (the picker or Identify) is the user's choice and stays.
            builtin_pad_id = 0;
            builtin_identity.clear();
            builtin_seen_first = false;
            builtin_warned = false;
            // (The Steam handle stays: it is the same controller in every session)
            if (!Settings::values.builtin_pad_manual.GetValue()) {
                Settings::values.builtin_pad_identity.SetValue(std::string{});
            }
        }
        return;
    }
    if (!console_profile) {
        // Entering a console mode: the user's own players are kept aside, and everything the
        // engine changes from here on stays runtime only (see SaveAllConfigValues)
        console_profile = Settings::values.players.GetValue(true);
        game_profiles.reset();
        LOG_INFO(Frontend, "console-mode: entered, the user's players are kept aside "
                           "(PROBE P1 motions {})",
                 MotionTrace((*console_profile)[0]));
    }
    // A game booted with input profiles of its own: those players' sets are the profile's, read
    // before the engine drives the game's copy (a game's copy is read in one go at boot)
    if (Settings::values.players.UsingGlobal()) {
        game_profiles.reset();
    } else if (!game_profiles) {
        game_profiles = Settings::values.players.GetValue();
        for (size_t i = 0; i < game_profiles->size(); ++i) {
            if (!(*game_profiles)[i].profile_name.empty()) {
                LOG_INFO(Frontend, "console-mode: P{} uses the game's input profile \"{}\"",
                         i + 1, (*game_profiles)[i].profile_name);
            }
        }
    }
    if (builtin_identity.empty()) {
        // Console Mode also needs to know the handheld's own pad: it carries player one's set
        builtin_identity = Settings::values.builtin_pad_identity.GetValue();
    }

    // The settings-tab names: the Controls tab lists InputCommon's devices by (guid, port),
    // so the name is the one of the device this pad is bound as - the same text the user
    // picks in that tab, not a guess by connect order.
    std::vector<Common::ParamPackage> input_devices;
    if (input_subsystem) {
        input_devices = input_subsystem->GetInputDevices();
    }
    const auto label_of = [&](u32 id, const std::string&) {
        QString display;
        if (input_subsystem) {
            if (const auto binding = input_subsystem->GetSDLBinding(id)) {
                for (const auto& device : input_devices) {
                    if (device.Get("engine", "") == "sdl" &&
                        device.Get("guid", "") == binding->first &&
                        device.Get("port", -1) == binding->second) {
                        display = QString::fromStdString(device.Get("display", ""));
                        break;
                    }
                }
            }
        }
        return std::pair<QString, QString>{display, PadName(id)};
    };

    // PROBE: trace the pad identities and the slot mapping at every change
    {
        std::string trace;
        for (const auto& [id, ident] : live) {
            const auto it = pad_slots.find(id);
            trace += fmt::format("\n  pad inst={} slot={} idx={} name={} path={} ident={}", id,
                                 it == pad_slots.end() ? -1 : static_cast<int>(it->second),
                                 PadPlayerIndex(id), PadName(id).toStdString(),
                                 PadPath(id).toStdString(), ident);
        }
        LOG_INFO(Frontend, "console-mode: live={}{}, builtin={}{}", trace,
                 live.empty() ? " (none)" : "", builtin_pad_id,
                 builtin_seen_first ? "" : " (unlatched)");

        // PROBE: Steam fills a virtual pad's handle, type and name in a moment after the pad
        // appears; trace every change, to see whether the handle follows the physical pad
        static std::map<u32, std::string> traced_steam;
        for (const auto& [id, ident] : live) {
            const std::string desc =
                fmt::format("steam_handle={:#x} type={} name={}", PadSteamHandle(id),
                            static_cast<int>(SDL_GetGamepadTypeForID(id)),
                            PadName(id).toStdString());
            auto& last = traced_steam[id];
            if (last != desc) {
                LOG_INFO(Frontend, "console-mode: pad inst={} slot={} {}", id,
                         pad_slots.contains(id) ? pad_slots.at(id) + 1 : 0, desc);
                last = desc;
            }
        }

        static std::set<u32> traced_insts;
        for (const auto& [id, ident] : live) {
            if (traced_insts.insert(id).second) {
                LOG_INFO(Frontend,
                         "console-mode: pad connected inst={} ident={} name={} idx={} gyro={} "
                         "steam_handle={:#x} type={} real_type={}",
                         id, ident, PadName(id).toStdString(), PadPlayerIndex(id),
                         PadHasGyro(id), PadSteamHandle(id),
                         static_cast<int>(SDL_GetGamepadTypeForID(id)),
                         static_cast<int>(SDL_GetRealGamepadTypeForID(id)));
            }
        }
        for (auto it = traced_insts.begin(); it != traced_insts.end();) {
            if (live.find(*it) == live.end()) {
                LOG_INFO(Frontend, "console-mode: pad disconnected inst={}", *it);
                it = traced_insts.erase(it);
            } else {
                ++it;
            }
        }
    }

    // The built-in's rules apply to exactly one pad: the one we can PROVE is the
    // handheld's own. Evidence, strongest first:
    //   1. connection continuity - the latched instance is still alive;
    //   2. under Steam, player index 0 (the deck's pad holds it - verified in sessions);
    //   3. a unique identity match (direct launches, where identities are honest).
    // Anything else and the rules PAUSE. Safe beats clever - the lesson of the X-1
    // pattern, where an impostor wearing the identity struck the wrong player row.
    u32 builtin_match_id = 0;
    bool builtin_valid = false;
    const bool steam_handle_known =
        LaunchedThroughSteam() && !Settings::values.builtin_pad_steam_handle.GetValue().empty();
    if (builtin_pad_id && live.find(builtin_pad_id) != live.end() && !steam_handle_known) {
        builtin_match_id = builtin_pad_id;
        builtin_valid = true;
    } else if (LaunchedThroughSteam()) {
        // Under Steam every virtual pad shares one GUID, and Steam's player index is no
        // evidence either: a session gave the deck's pad index 4 and an Xbox pad index 0,
        // which then took the deck's place. The deck's own pad keeps its Steam handle across
        // sessions (0xff42501d27 in every deck log), so that is the identity. With none
        // stored yet, a lone pad is the handheld's own, and gets latched below.
        const std::string want = Settings::values.builtin_pad_steam_handle.GetValue();
        int count = 0;
        u32 holder = 0;
        if (!want.empty()) {
            for (const auto& [id, ident] : live) {
                if (fmt::format("{:#x}", PadSteamHandle(id)) == want) {
                    ++count;
                    holder = id;
                }
            }
        }
        bool adopted = false;
        if (count == 0) {
            // No pad holds the stored handle (Steam may hand out a new one after a reboot or
            // an update), or none is stored yet: the deck's own controller is the one Steam
            // reports with no gamepad type ("#SettingsController_SteamController"), every
            // other controller has a real one. Adopt it when exactly one pad fits.
            int fits = 0;
            u32 fit = 0;
            for (const auto& [id, ident] : live) {
                if (SDL_GetGamepadTypeForID(id) == SDL_GAMEPAD_TYPE_UNKNOWN &&
                    PadSteamHandle(id) != 0) {
                    ++fits;
                    fit = id;
                }
            }
            if (fits == 1) {
                count = 1;
                holder = fit;
                adopted = true;
            } else if (want.empty() && live.size() == 1 &&
                       !IsStandaloneControllerType(SDL_GetGamepadTypeForID(live.begin()->first))) {
                count = 1;
                holder = live.begin()->first;
            }
        }
        if (adopted) {
            const std::string handle = fmt::format("{:#x}", PadSteamHandle(holder));
            if (handle != want) {
                Settings::values.builtin_pad_steam_handle.SetValue(handle);
                LOG_INFO(Frontend,
                         "console-mode: builtin adopted inst={} as the deck's controller, "
                         "Steam handle {} -> {}",
                         holder, want.empty() ? std::string{"(none)"} : want, handle);
            }
        }
        static std::string last_resolved;
        const std::string resolved =
            count == 1 ? fmt::format("inst={}", holder) : fmt::format("{} matches", count);
        if (resolved != last_resolved) {
            last_resolved = resolved;
            LOG_INFO(Frontend, "console-mode: builtin by Steam handle {}: {}",
                     want.empty() ? std::string{"(none stored)"} : want, resolved);
        }
        if (count == 1) {
            builtin_match_id = holder;
            builtin_valid = true;
        }
    } else if (!builtin_identity.empty()) {
        int identity_count = 0;
        u32 identity_holder = 0;
        for (const auto& [id, ident] : live) {
            if (ident == builtin_identity) {
                ++identity_count;
                identity_holder = id;
            }
        }
        if (identity_count == 1) {
            builtin_match_id = identity_holder;
            builtin_valid = true;
        }
        if (!builtin_valid) {
            static int last_count = -1;
            if (identity_count != last_count) {
                last_count = identity_count;
                LOG_INFO(Frontend,
                         "console-mode: builtin unresolved ({} pads match identity {}), "
                         "built-in rules paused",
                         identity_count, builtin_identity);
            }
        }
    }
    if (builtin_valid) {
        builtin_pad_id = builtin_match_id;
        if (LaunchedThroughSteam() &&
            Settings::values.builtin_pad_steam_handle.GetValue().empty()) {
            // Latch the handheld's Steam handle as soon as Steam has filled it in
            if (const Uint64 handle = PadSteamHandle(builtin_match_id)) {
                Settings::values.builtin_pad_steam_handle.SetValue(fmt::format("{:#x}", handle));
                LOG_INFO(Frontend, "console-mode: builtin Steam handle latched {:#x} inst={}",
                         handle, builtin_match_id);
            }
        }
    }

    if (handheld) {
        const std::string stored{Settings::values.builtin_pad_identity.GetValue()};
        const bool manual = Settings::values.builtin_pad_manual.GetValue();
        if (manual) {
            // A declaration is the truth: the picker or the Identify button chose this
            // pad, so the engine obeys it and simply resolves its current instance.
            builtin_identity = stored;
            builtin_seen_first = true;
            builtin_pad_id = 0;
            if (builtin_valid) {
                builtin_pad_id = builtin_match_id;
            }
        } else if (!builtin_pad_id && !stored.empty() && !LaunchedThroughSteam()) {
            // (Under Steam the stored GUID is every pad's: the Steam handle resolves it)
            for (const auto& [id, ident] : live) {
                if (ident == stored) {
                    builtin_pad_id = id;
                    builtin_identity = ident;
                    builtin_seen_first = true;
                    builtin_match_id = id;
                    builtin_valid = true;
                    break;
                }
            }
        }
        // (Under Steam the Steam handle resolves the handheld above; this latch stores the
        // direct launches' identity and must not take a Steam pad's)
        if (!builtin_pad_id && !manual && !LaunchedThroughSteam()) {
            auto handheld_candidate = [&]() -> u32 {
                std::vector<u32> makers;
                std::vector<u32> decks;
                for (const auto& [id, ident] : live) {
                    if (IsSteamVirtualPad(id)) {
                        // A virtual pad carries its launcher's vendor (Valve under Steam
                        // Input), which is not evidence of handheld hardware at all
                        continue;
                    }
                    const Uint16 vendor = PadVendor(id);
                    if (IsHandheldMaker(vendor)) {
                        makers.push_back(id);
                        // Valve also makes standalone controllers: the deck's own wins
                        if (vendor == 0x28DE && PadProduct(id) == 0x1205) {
                            decks.push_back(id);
                        }
                    }
                }
                if (makers.size() == 1) {
                    return makers.front();
                }
                return decks.size() == 1 ? decks.front() : 0;
            };
            const u32 candidate = handheld_candidate();
            // A pad identified here is the handheld's own from this very tick on: the claims
            // below must already skip it, or a launch with the externals on gives the deck
            // P1 and the externals P2.., and releasing the deck's P1 a tick later leaves P1
            // empty (deck log: "player 1 deactivated", externals on P2-P5)
            if (candidate) {
                LOG_INFO(Frontend, "console-mode: handheld {:04x}:{:04x} detected by its maker",
                         PadVendor(candidate), PadProduct(candidate));
                builtin_pad_id = candidate;
                builtin_identity = live[candidate];
                builtin_seen_first = true;
                Settings::values.builtin_pad_identity.SetValue(live[candidate]);
                builtin_warned = false;
                builtin_match_id = candidate;
                builtin_valid = true;
            } else if (live.size() == 1 && !IsControllerMaker(PadVendor(live.begin()->first))) {
                LOG_INFO(Frontend, "console-mode: handheld {:04x}:{:04x} assumed: the only pad",
                         PadVendor(live.begin()->first), PadProduct(live.begin()->first));
                builtin_match_id = live.begin()->first;
                builtin_valid = true;
                builtin_pad_id = live.begin()->first;
                builtin_identity = live.begin()->second;
                builtin_seen_first = true;
                Settings::values.builtin_pad_identity.SetValue(live.begin()->second);
                // A lone Steam-virtual pad at the ritual is the handheld's own by
                // definition (Steam Input wraps it); only raw unknown hardware gets the
                // note, where the mislabel risk is real.
                if (!builtin_warned && !IsSteamVirtualPad(live.begin()->first)) {
                    QMessageBox::information(
                        this, tr("Assign Handheld Controller"),
                        tr("Unknown handheld detected.\n\n"
                           "Careful: a less popular controller could be labeled as a handheld."));
                    builtin_warned = true;
                }
            }
            // Nothing identified: this tick works like Modern, and the mode stays chosen
            // (the next tick looks again, e.g. once the pads have enumerated)
        }
    }

    auto& players = Settings::values.players.GetValue();
    bool changed = false;

    // A disconnected pad keeps its reservation so a quick blip (Steam virtual pads churn)
    // reclaims its player - but not forever: after the grace period the slot is free again,
    // so plugging a DIFFERENT controller cannot leave an earlier player dead for the rest
    // of the session.
    {
        static std::map<u32, std::chrono::steady_clock::time_point> pad_last_seen;
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [id, ident] : live) {
            pad_last_seen[id] = now;
        }
        for (auto it = pad_slots.begin(); it != pad_slots.end();) {
            const bool is_live = live.find(it->first) != live.end();
            const auto seen = pad_last_seen.find(it->first);
            const bool expired =
                !is_live && seen != pad_last_seen.end() &&
                now - seen->second > std::chrono::seconds{45};
            if (expired) {
                LOG_INFO(Frontend,
                         "console-mode: reservation expired inst={} (slot {} free again)",
                         it->first, it->second);
                pad_identity.erase(it->first);
                pad_last_seen.erase(it->first);
                it = pad_slots.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }

    // The handheld's own pad takes no player while external controllers are around: if it
    // claimed one while alone, release it so the externals fill the range from player one.
    // When the externals leave, the built-in claims slot one again - no tiptoeing needed.
    if (handheld && builtin_valid && live.size() > 1) {
        for (auto it = pad_slots.begin(); it != pad_slots.end();) {
            if (it->first == builtin_match_id) {
                pad_identity.erase(it->first);
                it = pad_slots.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }

    // Alone, the handheld's own pad is player one. The departed externals' reservations
    // would otherwise hold the low slots for their grace period and push it to P4 (seen on
    // the deck: three pads off in a row, ambiguous under Steam), so drop them: externals
    // that return fill from player one again, in connect order.
    if (handheld && builtin_valid && live.size() == 1) {
        for (auto it = pad_slots.begin(); it != pad_slots.end();) {
            const bool own = it->first == builtin_match_id;
            if (!own || it->second != 0) {
                if (!own) {
                    LOG_INFO(Frontend,
                             "console-mode: handheld alone, dropping reservation inst={} "
                             "(slot {})",
                             it->first, it->second);
                }
                pad_identity.erase(it->first);
                it = pad_slots.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }

    // New pads claim the lowest free slot; the handheld's own pad is skipped while
    // others are around. The claim order follows the pads' player index where one exists:
    // under Steam that is the ordering players expect, not instance-number luck.
    std::set<int> used;
    for (const auto& [id, slot] : pad_slots) {
        used.insert(slot);
    }
    std::vector<u32> claim_order;
    claim_order.reserve(live.size());
    for (const auto& [id, ident] : live) {
        claim_order.push_back(id);
    }
    std::sort(claim_order.begin(), claim_order.end(), [](u32 a, u32 b) {
        const int ia = PadPlayerIndex(a);
        const int ib = PadPlayerIndex(b);
        if (ia != ib) {
            return ia >= 0 && (ib < 0 || ia < ib);
        }
        return a < b;
    });
    for (const u32 id : claim_order) {
        const std::string& ident = live.at(id);
        // The built-in is excluded by IDENTITY: its connection instance changes when the
        // pad cycles, and matching by instance let a returning built-in claim a slot (P4)
        if (handheld && builtin_valid && id == builtin_match_id && live.size() > 1) {
            continue;
        }
        // Steam Input pads used to take their RAW player index as the slot, but Steam's
        // numbering is not a slot map: a session showed indices 1 and 2 landing on P2 and
        // P3 with P1 left dead. The index orders the claims (the sort above); the slots
        // always fill lowest-free below.
        if (pad_slots.find(id) == pad_slots.end()) {
            // Reclaim a dangling reservation when the identity matches. A unique match is the
            // pad's own slot. Identical pads (several Xbox pads through Steam Input) share an
            // identity, so which reservation is whose cannot be known - but skipping them all
            // pushed returning pads behind the gaps (P2 and P3 left, came back as P5 and P6).
            // An ambiguous pad takes the lowest of those reservations, or a lower free slot,
            // like a console filling the first open player.
            auto match = pad_slots.end();
            int matches = 0;
            for (auto it = pad_slots.begin(); it != pad_slots.end(); ++it) {
                if (live.find(it->first) == live.end() && pad_identity[it->first] == ident) {
                    if (match == pad_slots.end() || it->second < match->second) {
                        match = it;
                    }
                    ++matches;
                }
            }
            if (matches == 0 && LaunchedThroughSteam()) {
                // Steam may hand a returning controller a new handle: then no reservation is
                // provably its own, and it takes the lowest one, like an ambiguous pad
                for (auto it = pad_slots.begin(); it != pad_slots.end(); ++it) {
                    if (live.find(it->first) == live.end() &&
                        (match == pad_slots.end() || it->second < match->second)) {
                        match = it;
                    }
                }
                if (match != pad_slots.end()) {
                    matches = 2;
                }
            }
            if (matches > 1) {
                int lowest_free = -1;
                for (int slot = 0; slot < static_cast<int>(players.size()); ++slot) {
                    if (used.find(slot) == used.end()) {
                        lowest_free = slot;
                        break;
                    }
                }
                if (lowest_free < 0 || match->second < lowest_free) {
                    LOG_INFO(Frontend,
                             "console-mode: ambiguous reclaim inst={} ident={} ({} candidates) "
                             "takes the lowest, slot {} from dead inst={}",
                             id, ident, matches, match->second, match->first);
                    matches = 1;
                } else {
                    match = pad_slots.end();
                    matches = 0;
                }
            }
            if (matches == 1) {
                LOG_INFO(Frontend,
                         "console-mode: reclaim inst={} ident={} takes slot {} from dead inst={}",
                         id, ident, match->second, match->first);
                pad_slots[id] = match->second;
                pad_identity[id] = ident;
                pad_identity.erase(match->first);
                pad_slots.erase(match);
                changed = true;
                continue;
            }
            // Claim the lowest free slot
            bool assigned = false;
            for (int slot = 0; slot < static_cast<int>(players.size()) && !assigned; ++slot) {
                if (used.find(slot) == used.end()) {
                    LOG_INFO(Frontend, "console-mode: claim inst={} ident={} -> slot {}", id,
                             ident, slot);
                    pad_slots[id] = slot;
                    pad_identity[id] = ident;
                    used.insert(slot);
                    assigned = true;
                    changed = true;
                }
            }
            if (!assigned) {
                // Retire reservations (pads that left long ago) and retry, never a live
                // pad's slot
                for (auto it = pad_slots.begin(); it != pad_slots.end();) {
                    const bool live_pad = live.find(it->first) != live.end();
                    if (!live_pad) {
                        pad_identity.erase(it->first);
                        it = pad_slots.erase(it);
                    } else {
                        ++it;
                    }
                }
                used.clear();
                for (const auto& [k, slot] : pad_slots) {
                    used.insert(slot);
                }
                for (int slot = 0; slot < static_cast<int>(players.size()); ++slot) {
                    if (used.find(slot) == used.end()) {
                        LOG_INFO(Frontend, "console-mode: claim inst={} ident={} -> slot {}", id,
                                 ident, slot);
                        pad_slots[id] = slot;
                        pad_identity[id] = ident;
                        used.insert(slot);
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    // Swaps requested from the Players tab: the two players' pads trade slots (or one moves,
    // when the other player has none), for this session only. The routing below re-points
    // them at once.
    {
        std::vector<std::pair<int, int>> requests;
        {
            auto& state = Common::GetControllerConsoleState();
            std::scoped_lock lock{state.mutex};
            requests.swap(state.swap_requests);
        }
        for (const auto& [a, b] : requests) {
            u32 pad_a = 0;
            u32 pad_b = 0;
            for (const auto& [id, slot] : pad_slots) {
                if (live.find(id) == live.end()) {
                    continue;
                }
                if (slot == a) {
                    pad_a = id;
                }
                if (slot == b) {
                    pad_b = id;
                }
            }
            if (!pad_a && !pad_b) {
                continue;
            }
            if (pad_a) {
                pad_slots[pad_a] = b;
            }
            if (pad_b) {
                pad_slots[pad_b] = a;
            }
            LOG_INFO(Frontend, "console-mode: Players tab swap P{} <-> P{} (inst={} inst={})",
                     a + 1, b + 1, pad_a, pad_b);
            changed = true;
        }
    }

    // Profiles picked in the Controls page: their mappings go on the set the player's
    // controller carries (part of the user's own setup, so they are kept), on its device
    {
        std::vector<Common::ControllerConsoleState::ProfileRequest> picks;
        {
            auto& state = Common::GetControllerConsoleState();
            std::scoped_lock lock{state.mutex};
            picks.swap(state.profile_requests);
        }
        std::optional<InputProfiles> input_profiles;
        for (const auto& [player, name, restore] : picks) {
            if (!console_profile || player < 0 || player >= static_cast<int>(players.size())) {
                continue;
            }
            const int set = slot_set[player];
            const bool game_set = game_profiles && !(*game_profiles)[set].profile_name.empty();
            auto& target = game_set ? (*game_profiles)[set] : (*console_profile)[set];
            if (name.empty() && restore) {
                if (const auto before = set_before_profile.find(set);
                    before != set_before_profile.end()) {
                    target.buttons = before->second.buttons;
                    target.analogs = before->second.analogs;
                    target.motions = before->second.motions;
                    set_before_profile.erase(before);
                }
            }
            if (!name.empty()) {
                set_before_profile.try_emplace(set, target);
                if (!input_profiles) {
                    input_profiles.emplace();
                }
                const auto read = input_profiles->PeekProfile(name, player);
                if (!read) {
                    LOG_WARNING(Frontend, "console-mode: input profile \"{}\" not found", name);
                    continue;
                }
                ApplyProfileMappings(target, *read);
            }
            target.profile_name = name;
            LOG_INFO(Frontend, "console-mode: P{} input profile \"{}\" on the P{} set{}",
                     player + 1, name, set + 1, game_set ? " (the game's)" : "");
            changed = true;
        }
    }

    // Rumble requested from the Controllers tab: a short pulse on the player's controller
    {
        std::vector<int> rumbles;
        {
            auto& state = Common::GetControllerConsoleState();
            std::scoped_lock lock{state.mutex};
            rumbles.swap(state.rumble_requests);
        }
        for (const int player : rumbles) {
            for (const auto& [id, slot] : pad_slots) {
                if (slot != player || live.find(id) == live.end()) {
                    continue;
                }
                // Sent twice: a single request through Steam's virtual pads is sometimes lost
                const u32 pad = id;
                PadRumble(pad, 0xC000, 300);
                QTimer::singleShot(150, this, [pad] { PadRumble(pad, 0xC000, 200); });
                break;
            }
        }
    }

    // Each live pad's player gets that pad's binding set, pointed at the pad. The slot map
    // alone is not the routing: the profile binds player N to whatever (guid, port) it was
    // mapped with, and the driver's ports follow attach order with freed ports reused - so a
    // slot's pad and the pad that drives the player drifted apart (P1 kept the deck while
    // the Xbox pad sat on a dead P2). The sets come from the user's own profile and follow
    // the pad, not the slot: the handheld's own pad carries player one's set (its gyro too,
    // even when that is a DSU server rather than SDL) to whatever slot it sits on; an
    // external pad takes its slot's set, minus the handheld's non-SDL motion, plus its own
    // SDL gyro when it has one and the set has no motion. A pad the driver has not opened
    // yet is retried next tick.
    slot_set = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    if (input_subsystem && console_profile) {
        const auto profile = ConsoleBindingSets();
        const auto is_handheld_motion = [&profile](const std::string& raw) {
            if (raw.empty() || Common::ParamPackage{raw}.Get("engine", "") == "sdl") {
                return false;
            }
            return std::find(profile[0].motions.begin(), profile[0].motions.end(), raw) !=
                   profile[0].motions.end();
        };
        for (const auto& [id, ident] : live) {
            const auto it = pad_slots.find(id);
            if (it == pad_slots.end() || it->second < 0 ||
                it->second >= static_cast<int>(players.size())) {
                continue;
            }
            const auto binding = input_subsystem->GetSDLBinding(id);
            if (!binding) {
                continue;
            }
            const int slot = it->second;
            const bool is_builtin = builtin_valid && id == builtin_match_id;
            // A controller carries the set it first got for the session: its slot's, unless
            // another controller has that one (after a swap), then the first set nobody has
            if (!is_builtin && !pad_sets.contains(ident)) {
                std::set<int> taken;
                for (const auto& [other, set] : pad_sets) {
                    taken.insert(set);
                }
                int set = slot;
                for (int k = 0; taken.contains(set) && k < static_cast<int>(players.size());
                     ++k) {
                    set = k;
                }
                if (taken.contains(set)) {
                    set = slot;
                }
                pad_sets[ident] = set;
                LOG_INFO(Frontend, "console-mode: inst={} carries the P{} set", id, set + 1);
            }
            // A slot the user never bound a controller to (keyboard or empty, P6+ typically)
            // would leave its pad dead: borrow the nearest lower external slot's set.
            const auto has_sdl = [](const Settings::PlayerInput& p) {
                return std::any_of(p.buttons.begin(), p.buttons.end(), [](const std::string& b) {
                    return !b.empty() && Common::ParamPackage{b}.Get("engine", "") == "sdl";
                });
            };
            int set_index = is_builtin ? 0 : pad_sets[ident];
            if (!is_builtin && !has_sdl(profile[set_index])) {
                for (int k = set_index - 1; k >= 0; --k) {
                    if (has_sdl(profile[k]) && (k > 0 || !builtin_valid || !handheld)) {
                        set_index = k;
                        break;
                    }
                }
            }
            const auto& own = profile[set_index];
            slot_set[slot] = set_index;
            Settings::PlayerInput want = players[slot];
            want.buttons = own.buttons;
            want.analogs = own.analogs;
            want.motions = own.motions;
            want.profile_name = own.profile_name;
            RouteSdlBindings(want, binding->first, binding->second);
            // Under Steam the virtual pad has no motion: take the paired real device's. The
            // deck's own controller is unique (28DE:1205); another model pairs when only one
            // of it is connected (the virtual pad reports the real controller's ids).
            u32 motion_source = 0;
            if (LaunchedThroughSteam()) {
                const Uint16 vendor = is_builtin ? 0x28DE : SDL_GetJoystickVendorForID(id);
                const Uint16 product = is_builtin ? 0x1205 : SDL_GetJoystickProductForID(id);
                const auto same_model = [vendor, product](u32 other) {
                    return SDL_GetJoystickVendorForID(other) == vendor &&
                           SDL_GetJoystickProductForID(other) == product;
                };
                const auto real = std::count_if(motion_devices.begin(), motion_devices.end(),
                                                same_model);
                const auto virtuals = std::count_if(live.begin(), live.end(), [&](const auto& e) {
                    return same_model(e.first);
                });
                if (real == 1 && (is_builtin || virtuals == 1)) {
                    const u32 device =
                        *std::find_if(motion_devices.begin(), motion_devices.end(), same_model);
                    if (PadHasGyro(device)) {
                        motion_source = device;
                    }
                }
            }
            if (motion_source != overlay_motion_source[slot]) {
                overlay_motion_source[slot] = motion_source;
                LOG_INFO(Frontend, "console-mode: P{} motion from real device inst={}", slot + 1,
                         motion_source);
            }
            if (motion_source) {
                if (const auto source = input_subsystem->GetSDLBinding(motion_source)) {
                    want.motions.fill(SdlMotionParam(source->first, source->second));
                }
            } else if (!is_builtin) {
                for (std::string& motion : want.motions) {
                    if (is_handheld_motion(motion)) {
                        motion.clear();
                    }
                }
                const bool no_motion =
                    std::all_of(want.motions.begin(), want.motions.end(),
                                [](const std::string& m) { return m.empty(); });
                if (no_motion && PadHasGyro(id)) {
                    want.motions.fill(SdlMotionParam(binding->first, binding->second));
                }
            }
            auto& player = players[slot];
            player.profile_name = want.profile_name;
            if (player.buttons != want.buttons || player.analogs != want.analogs ||
                player.motions != want.motions) {
                player.buttons = want.buttons;
                player.analogs = want.analogs;
                player.motions = want.motions;
                LOG_INFO(Frontend,
                         "console-mode: P{} <- inst={} guid={} port={} set=P{}{}", slot + 1, id,
                         binding->first, binding->second, set_index + 1,
                         is_builtin ? " (handheld)" : "");
                changed = true;
            }
        }
    }

    // The configuration follows identity: a pad that left keeps its reservation, so it
    // reclaims its own player when it returns. Only the leaving player is deactivated.
    for (int i = 0; i < static_cast<int>(players.size()); ++i) {
        bool want = false;
        for (const auto& [id, ident] : live) {
            const auto it = pad_slots.find(id);
            if (it != pad_slots.end() && it->second == i) {
                want = true;
                break;
            }
        }
        if (players[i].connected != want) {
            players[i].connected = want;
            changed = true;
        }
    }

    if (changed) {
        std::string out;
        for (int i = 0; i < static_cast<int>(players.size()); ++i) {
            out += players[i].connected ? "1" : "0";
        }

        static std::string last_written;
        if (out != last_written) {
            for (size_t i = 0; i < out.size(); ++i) {
                if (last_written.size() != out.size() || last_written[i] != out[i]) {
                    LOG_INFO(Frontend, "console-mode: player {} {}", i + 1,
                             out[i] == '1' ? "activated" : "deactivated");
                }
            }
            last_written = out;
        }
        LOG_INFO(Frontend, "console-mode: players written={}", out);
    }

    // The activation contract: a live pad holds at most one slot, slots stay in range and
    // never collide. If this ever logs, the engine has a real bug and the log is the proof.
    {
        std::set<int> seen_slots;
        for (const auto& [id, slot] : pad_slots) {
            const bool in_range = slot >= 0 && slot < static_cast<int>(players.size());
            const bool unique = in_range && seen_slots.insert(slot).second;
            if (live.find(id) != live.end() && (!in_range || !unique)) {
                LOG_ERROR(Frontend,
                          "console-mode: INVARIANT BROKEN inst={} slot={} in_range={} "
                          "unique={} - two pads fighting over one player or a corrupt slot",
                          id, slot, in_range, unique);
            }
        }
    }

    // The debug overlay is rendered INTO the presented frame (see UpdateRawImage in the
    // present path): a Qt window can never composite over the game surface in gamescope,
    // so the text is drawn here and alpha-blended into the frame before the GPU copy.
    {
        // One row per player: it pops in green when its pad connects, turns red when the
        // pad leaves and then goes. Blue (input held) is RenderControllerOverlay's, which also
        // runs on the fast activity timer. The name drops InputCommon's port number, which
        // shifts whenever Steam moves a controller to another virtual pad.
        constexpr qint64 flash_ms = 1500;
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        static const QRegularExpression port_number{
            QStringLiteral(" \\d+(?=( \\(Steam\\))?$)")};
        overlay_rows.clear();
        std::vector<Common::ControllerConsoleState::PlayerRow> tab_rows;
        // Flashes follow the controller behind a player, not just the player: in handheld mode
        // P1 stays connected while the controller behind it changes. Green when a controller
        // takes a player; red when one leaves, then green if another takes its place at once.
        for (int i = 0; i < static_cast<int>(players.size()); ++i) {
            u32 pad = 0;
            for (const auto& [id, slot] : pad_slots) {
                if (slot == i && live.find(id) != live.end()) {
                    pad = id;
                    break;
                }
            }
            auto& since = overlay_row_since[i];
            auto& gone = overlay_row_gone[i];
            auto& flash = overlay_flash[i];
            auto& flash_since = overlay_flash_since[i];
            auto& green_at = overlay_green_at[i];
            if (players[i].connected && pad) {
                const auto [display, raw] = label_of(pad, std::string{});
                QString name = display.isEmpty() ? raw : display;
                name.remove(port_number);
                overlay_row_name[i] = name;
                if (since == 0 || gone != 0) {
                    since = now_ms;
                    gone = 0;
                    flash = 1;
                    flash_since = now_ms;
                    green_at = 0;
                } else if (live[pad] != overlay_row_ident[i]) {
                    const bool old_left =
                        std::none_of(live.begin(), live.end(), [&](const auto& entry) {
                            return entry.second == overlay_row_ident[i];
                        });
                    if (old_left) {
                        flash = 2;
                        flash_since = now_ms;
                        green_at = now_ms + flash_ms / 2;
                    } else {
                        flash = 1;
                        flash_since = now_ms;
                        green_at = 0;
                    }
                }
                overlay_row_ident[i] = live[pad];
            } else if (since != 0 && gone == 0) {
                gone = now_ms;
                flash = 2;
                flash_since = now_ms;
                green_at = 0;
            }
            if (since == 0) {
                continue;
            }
            if (gone != 0 && now_ms - gone > flash_ms) {
                since = 0;
                gone = 0;
                flash = 0;
                overlay_row_ident[i].clear();
                continue;
            }
            if (green_at != 0 && now_ms >= green_at) {
                flash = 1;
                flash_since = green_at;
                green_at = 0;
            }
            if (flash != 0 && gone == 0 && green_at == 0 && now_ms - flash_since >= flash_ms) {
                flash = 0;
            }
            overlay_rows.push_back({i, QStringLiteral("P%1: %2").arg(i + 1).arg(
                                           overlay_row_name[i]),
                                    gone != 0 ? 0u : pad, flash, flash_since});
            tab_rows.push_back({i, overlay_row_name[i].toStdString(), false, gone != 0, flash,
                                flash_since, ConsoleBindingSets()[slot_set[i]].profile_name});
        }
        {
            std::string handheld_name;
            bool handheld_has_player = false;
            if (builtin_valid && live.find(builtin_match_id) != live.end()) {
                const auto [display, raw] = label_of(builtin_match_id, std::string{});
                QString name = display.isEmpty() ? raw : display;
                name.remove(port_number);
                handheld_name = name.toStdString();
                handheld_has_player = pad_slots.contains(builtin_match_id);
            }
            auto& state = Common::GetControllerConsoleState();
            std::scoped_lock lock{state.mutex};
            state.engine_active = true;
            state.players = std::move(tab_rows);
            state.handheld_name = std::move(handheld_name);
            state.handheld_has_player = handheld_has_player;
        }
        RenderControllerOverlay();
    }

    if (!changed) {
        return;
    }
    // Reloaded whether a game runs or not: the Controls tab reads the players back from the
    // emulated controllers, so stale ones (the last state of a stopped game) would be written
    // over the players when it opens, and taken for the user's edits when it closes
    if (QtCommon::system) {
        QtCommon::system->HIDCore().ReloadInputDevices();
    }
    auto& state = Common::GetControllerConsoleState();
    std::scoped_lock lock{state.mutex};
    ++state.generation;
}

void MainWindow::ConnectMenuEvents() {
    const auto connect_menu = [&]<typename Fn>(QAction* action, const Fn& event_fn) {
        connect(action, &QAction::triggered, this, event_fn);
        // Add actions to this window so that hiding menus in fullscreen won't disable them
        addAction(action);
        // Add actions to the render window so that they work outside of single window mode
        render_window->addAction(action);
    };

    // File
    connect_menu(ui->action_Load_File, &MainWindow::OnMenuLoadFile);
    connect_menu(ui->action_Load_Folder, &MainWindow::OnMenuLoadFolder);
    connect_menu(ui->action_Install_File_NAND, &MainWindow::OnMenuInstallToNAND);
    connect_menu(ui->action_Exit, &QMainWindow::close);
    connect_menu(ui->action_Load_Amiibo, &MainWindow::OnLoadAmiibo);

    // Emulation
    connect_menu(ui->action_Pause, &MainWindow::OnPauseContinueGame);
    connect_menu(ui->action_Stop, &MainWindow::OnStopGame);
    connect_menu(ui->action_Open_Mods_Page, &MainWindow::OnOpenModsPage);
    connect_menu(ui->action_Open_UserHandbook, &MainWindow::OnOpenUserHandbook);
    connect_menu(ui->action_Restart, &MainWindow::OnRestartGame);
    connect_menu(ui->action_Configure, &MainWindow::OnConfigure);
    connect_menu(ui->action_Configure_Current_Game, &MainWindow::OnConfigurePerGame);

    // View
    connect_menu(ui->action_Fullscreen, &MainWindow::ToggleFullscreen);
    connect_menu(ui->action_Single_Window_Mode, &MainWindow::ToggleWindowMode);
    connect_menu(ui->action_Show_Filter_Bar, &MainWindow::OnToggleFilterBar);
    connect_menu(ui->action_Show_Status_Bar, &MainWindow::OnToggleStatusBar);
    connect_menu(ui->action_Show_Performance_Overlay, &MainWindow::OnTogglePerfOverlay);
    connect(ui->action_Show_Controller_Overlay, &QAction::toggled, this,
            [](bool shown) { Settings::values.console_debug_overlay.SetValue(shown); });
#ifdef HAS_RESHADE
    connect_menu(ui->action_Post_Processing_Shaders, &MainWindow::OnPostProcessingShaders);
#else
    ui->action_Post_Processing_Shaders->setVisible(false);
#endif

    connect_menu(ui->action_Reset_Window_Size_720, &MainWindow::ResetWindowSize720);
    connect_menu(ui->action_Reset_Window_Size_900, &MainWindow::ResetWindowSize900);
    connect_menu(ui->action_Reset_Window_Size_1080, &MainWindow::ResetWindowSize1080);
    ui->menu_Reset_Window_Size->addActions({ui->action_Reset_Window_Size_720,
                                            ui->action_Reset_Window_Size_900,
                                            ui->action_Reset_Window_Size_1080});

    connect_menu(ui->action_Grid_View, &MainWindow::SetGridView);
    connect_menu(ui->action_Tree_View, &MainWindow::SetTreeView);
    connect_menu(ui->action_Carousel_View, &MainWindow::SetCarouselView);

    game_size_actions = new QActionGroup(this);
    game_size_actions->setExclusive(true);

    for (size_t i = 0; i < default_game_icon_sizes.size(); i++) {
        const auto current_size = UISettings::values.game_icon_size.GetValue();
        const auto size = default_game_icon_sizes[i].first;
        QAction* action = ui->menuGame_Icon_Size->addAction(GetTranslatedGameIconSize(i));
        action->setCheckable(true);

        if (current_size == size)
            action->setChecked(true);

        game_size_actions->addAction(action);

        connect(action, &QAction::triggered, this, [this, size](bool checked) {
            if (checked) {
                UISettings::values.game_icon_size.SetValue(size);
                CheckIconSize();

                game_list->UpdateIconSizes();
                game_list->RefreshGameDirectory();
            }
        });
    }

    CheckIconSize();

    ui->action_Show_Game_Name->setChecked(UISettings::values.show_game_name.GetValue());
    connect(ui->action_Show_Game_Name, &QAction::triggered, this, &MainWindow::ToggleShowGameName);

    // Multiplayer
    connect(ui->action_View_Lobby, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnViewLobby);
    connect(ui->action_Start_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnCreateRoom);
    connect(ui->action_Leave_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnCloseRoom);
    connect(ui->action_Connect_To_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnDirectConnectToRoom);
    connect(ui->action_Show_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnOpenNetworkRoom);
    connect(multiplayer_state, &MultiplayerState::SaveConfig, this, &MainWindow::OnSaveConfig);

    // Tools
    connect_menu(ui->action_Launch_PhotoViewer, [this] {
        LaunchFirmwareApplet(u64(Service::AM::AppletProgramId::PhotoViewer), std::nullopt);
    });
    connect_menu(ui->action_Launch_MiiEdit, [this] {
        LaunchFirmwareApplet(u64(Service::AM::AppletProgramId::MiiEdit), std::nullopt);
    });
    connect_menu(ui->action_Launch_Controller, [this] {
        LaunchFirmwareApplet(u64(Service::AM::AppletProgramId::Controller), std::nullopt);
    });
    connect_menu(ui->action_Launch_QLaunch, [this] {
        LaunchFirmwareApplet(u64(Service::AM::AppletProgramId::QLaunch), std::nullopt);
    });
    // Tools (cabinet)
    connect_menu(ui->action_Launch_Cabinet_Nickname_Owner, [this] {
        LaunchFirmwareApplet(u64(Service::AM::AppletProgramId::Cabinet),
                             {Service::NFP::CabinetMode::StartNicknameAndOwnerSettings});
    });
    connect_menu(ui->action_Launch_Cabinet_Eraser, [this] {
        LaunchFirmwareApplet(u64(Service::AM::AppletProgramId::Cabinet),
                             {Service::NFP::CabinetMode::StartGameDataEraser});
    });
    connect_menu(ui->action_Launch_Cabinet_Restorer, [this] {
        LaunchFirmwareApplet(u64(Service::AM::AppletProgramId::Cabinet),
                             {Service::NFP::CabinetMode::StartRestorer});
    });
    connect_menu(ui->action_Launch_Cabinet_Formatter, [this] {
        LaunchFirmwareApplet(u64(Service::AM::AppletProgramId::Cabinet),
                             {Service::NFP::CabinetMode::StartFormatter});
    });

    connect_menu(ui->action_Desktop, &MainWindow::OnCreateHomeMenuDesktopShortcut);
    connect_menu(ui->action_Application_Menu, &MainWindow::OnCreateHomeMenuApplicationMenuShortcut);
    connect_menu(ui->action_Capture_Screenshot, &MainWindow::OnCaptureScreenshot);

    // TAS
    connect_menu(ui->action_TAS_Start, &MainWindow::OnTasStartStop);
    connect_menu(ui->action_TAS_Record, &MainWindow::OnTasRecord);
    connect_menu(ui->action_TAS_Reset, &MainWindow::OnTasReset);
    connect_menu(ui->action_Configure_Tas, &MainWindow::OnConfigureTas);

    // Help
    connect_menu(ui->action_Root_Data_Folder, &MainWindow::OnOpenRootDataFolder);
    connect_menu(ui->action_NAND_Folder, &MainWindow::OnOpenNANDFolder);
    connect_menu(ui->action_SDMC_Folder, &MainWindow::OnOpenSDMCFolder);
    connect_menu(ui->action_Mod_Folder, &MainWindow::OnOpenModFolder);
    connect_menu(ui->action_Log_Folder, &MainWindow::OnOpenLogFolder);

    connect_menu(ui->action_Verify_installed_contents, &MainWindow::OnVerifyInstalledContents);
    connect_menu(ui->action_Firmware_From_Folder, &MainWindow::OnInstallFirmware);
    connect_menu(ui->action_Firmware_From_ZIP, &MainWindow::OnInstallFirmwareFromZIP);
    connect_menu(ui->action_Install_Keys, &MainWindow::OnInstallDecryptionKeys);
    connect_menu(ui->action_About, &MainWindow::OnAbout);
    connect_menu(ui->action_Eden_Dependencies, &MainWindow::OnEdenDependencies);
    connect_menu(ui->action_Data_Manager, &MainWindow::OnDataDialog);
}

void MainWindow::UpdateMenuState() {
    const bool is_paused = QtCommon::emu_thread == nullptr || !QtCommon::emu_thread->IsRunning();
    const bool is_firmware_available = CheckFirmwarePresence();

    const std::array running_actions{
        ui->action_Stop,
        ui->action_Restart,
        ui->action_Configure_Current_Game,
        ui->action_Load_Amiibo,
        ui->action_Pause,
    };

    const std::array applet_actions{
        ui->action_Launch_PhotoViewer,       ui->action_Launch_Cabinet_Nickname_Owner,
        ui->action_Launch_Cabinet_Eraser,    ui->action_Launch_Cabinet_Restorer,
        ui->action_Launch_Cabinet_Formatter, ui->action_Launch_MiiEdit,
        ui->action_Launch_QLaunch,           ui->action_Launch_Controller};

    for (QAction* action : running_actions) {
        action->setEnabled(emulation_running);
    }

    ui->action_Firmware_From_Folder->setEnabled(!emulation_running);
    ui->action_Firmware_From_ZIP->setEnabled(!emulation_running);
    ui->action_Install_Keys->setEnabled(!emulation_running);

    for (QAction* action : applet_actions) {
        action->setEnabled(is_firmware_available && !emulation_running);
    }

    ui->action_Capture_Screenshot->setEnabled(emulation_running && !is_paused);

    if (emulation_running && is_paused) {
        ui->action_Pause->setText(tr("&Continue"));
    } else {
        ui->action_Pause->setText(tr("&Pause"));
    }

    multiplayer_state->UpdateNotificationStatus();
}

void MainWindow::SetupPrepareForSleep() {
#ifdef __unix__
    if (auto bus = QDBusConnection::systemBus(); bus.isConnected()) {
        // See https://github.com/ConsoleKit2/ConsoleKit2/issues/150
#ifdef __linux__
        const auto dbus_logind_service = QStringLiteral("org.freedesktop.login1");
        const auto dbus_logind_path = QStringLiteral("/org/freedesktop/login1");
        const auto dbus_logind_manager_if = QStringLiteral("org.freedesktop.login1.Manager");
        // const auto dbus_logind_session_if = QStringLiteral("org.freedesktop.login1.Session");
#else
        const auto dbus_logind_service = QStringLiteral("org.freedesktop.ConsoleKit");
        const auto dbus_logind_path = QStringLiteral("/org/freedesktop/ConsoleKit/Manager");
        const auto dbus_logind_manager_if = QStringLiteral("org.freedesktop.ConsoleKit.Manager");
        // const auto dbus_logind_session_if = QStringLiteral("org.freedesktop.ConsoleKit.Session");
#endif
        const bool success = bus.connect(dbus_logind_service, dbus_logind_path,
                                         dbus_logind_manager_if, QStringLiteral("PrepareForSleep"),
                                         QStringLiteral("b"), this, SLOT(OnPrepareForSleep(bool)));
        if (!success)
            LOG_WARNING(Frontend, "Couldn't register PrepareForSleep signal");
    } else {
        LOG_WARNING(Frontend, "QDBusConnection system bus is not connected");
    }
#endif // __unix__
}

void MainWindow::OnPrepareForSleep(bool prepare_sleep) {
    if (QtCommon::emu_thread == nullptr)
        return;

    if (prepare_sleep) {
        if (QtCommon::emu_thread->IsRunning()) {
            auto_paused = true;
            OnPauseGame();
        }
    } else {
        if (!QtCommon::emu_thread->IsRunning() && auto_paused) {
            auto_paused = false;
            OnStartGame();
        }
    }
}

#ifdef __unix__
std::array<int, 3> MainWindow::sig_interrupt_fds{0, 0, 0};

void MainWindow::SetupSigInterrupts() {
    if (sig_interrupt_fds[2] == 1) {
        return;
    }
    socketpair(AF_UNIX, SOCK_STREAM, 0, sig_interrupt_fds.data());
    sig_interrupt_fds[2] = 1;

    struct sigaction sa;
    sa.sa_handler = &MainWindow::HandleSigInterrupt;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    sig_interrupt_notifier = new QSocketNotifier(sig_interrupt_fds[1], QSocketNotifier::Read, this);
    connect(sig_interrupt_notifier, &QSocketNotifier::activated, this,
            &MainWindow::OnSigInterruptNotifierActivated);
    connect(this, &MainWindow::SigInterrupt, this, &MainWindow::close);
}

void MainWindow::HandleSigInterrupt(int sig) {
    if (sig == SIGINT) {
        _exit(1);
    }

    // Calling into Qt directly from a signal handler is not safe,
    // so wake up a QSocketNotifier with this hacky write call instead.
    char a = 1;
    int ret = write(sig_interrupt_fds[0], &a, sizeof(a));
    (void)ret;
}

void MainWindow::OnSigInterruptNotifierActivated() {
    sig_interrupt_notifier->setEnabled(false);

    char a;
    int ret = read(sig_interrupt_fds[1], &a, sizeof(a));
    (void)ret;

    sig_interrupt_notifier->setEnabled(true);

    emit SigInterrupt();
}
#endif // __unix__

void MainWindow::PreventOSSleep() {
#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#else
    SDL_DisableScreenSaver();
#endif
}

void MainWindow::AllowOSSleep() {
#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS);
#else
    SDL_EnableScreenSaver();
#endif
}

bool MainWindow::LoadROM(const QString& filename, Service::AM::FrontendAppletParameters params) {
    // Shutdown previous session if the emu thread is still active...
    if (QtCommon::emu_thread != nullptr)
        ShutdownGame();

    if (!render_window->InitRenderTarget())
        return false;

    QtCommon::system->SetFilesystem(QtCommon::vfs);

    if (params.launch_type == Service::AM::LaunchType::FrontendInitiated)
        QtCommon::system->GetUserChannel().clear();

    QtCommon::system->SetFrontendAppletSet({
        std::make_unique<QtAmiiboSettings>(*this), // Amiibo Settings
        // The console modes own the controllers and suppress the applet, like Disabled
        (Settings::values.controller_applet_mode.GetValue() == Settings::AppletMode::Disabled ||
         Settings::values.controller_applet_mode.GetValue() == Settings::AppletMode::ConsoleMode ||
         Settings::values.controller_applet_mode.GetValue() ==
             Settings::AppletMode::HandheldConsoleMode)
            ? nullptr
            : std::make_unique<QtControllerSelector>(*this), // Controller Selector
        std::make_unique<QtErrorDisplay>(*this),             // Error Display
        nullptr,                                             // Mii Editor
        nullptr,                                             // Parental Controls
        nullptr,                                             // Photo Viewer
        std::make_unique<QtProfileSelector>(*this),          // Profile Selector
        std::make_unique<QtSoftwareKeyboard>(*this),         // Software Keyboard
        std::make_unique<QtWebBrowser>(*this),               // Web Browser
        nullptr,                                             // Net Connect
    });

    /** firmware check */

    if (!QtCommon::Content::CheckGameFirmware(params.program_id))
        return false;

    /** Exec */
    const Core::SystemResultStatus result{
        QtCommon::system->Load(*render_window, filename.toStdString(), params)};

    const auto drd_callout = (UISettings::values.callout_flags.GetValue() &
                              static_cast<u32>(CalloutFlag::DRDDeprecation)) == 0;

    if (result == Core::SystemResultStatus::Success &&
        QtCommon::system->GetAppLoader().GetFileType() ==
            Loader::FileType::DeconstructedRomDirectory &&
        drd_callout) {
        UISettings::values.callout_flags = UISettings::values.callout_flags.GetValue() |
                                           static_cast<u32>(CalloutFlag::DRDDeprecation);
        QMessageBox::warning(
            this, tr("Warning: Outdated Game Format"),
            tr("You are using the deconstructed ROM directory format for this game, which is an "
               "outdated format that has been superseded by others such as NCA, NAX, XCI, or "
               "NSP. Deconstructed ROM directories lack icons, metadata, and update "
               "support.<br>For an explanation of the various Switch formats Eden supports, "
               "out our user handbook. This message will not be shown again."));
    }

    if (result != Core::SystemResultStatus::Success) {
        switch (result) {
        case Core::SystemResultStatus::ErrorGetLoader:
            LOG_CRITICAL(Frontend, "Failed to obtain loader for {}!", filename.toStdString());
            QMessageBox::critical(this, tr("Error while loading ROM!"),
                                  tr("The ROM format is not supported."));
            break;
        case Core::SystemResultStatus::ErrorVideoCore:
            QMessageBox::critical(
                this, tr("An error occurred initializing the video core."),
                tr("This is usually caused by outdated GPU drivers. "
                   "Please see the log for more details. See: "
                   "<a href='https://git.eden-emu.dev/eden-emu/eden/src/branch/master/docs/user/HowToAccessLogs.md'>"
                   "How to access log files</a>."));
            break;
        default:
            if (result > Core::SystemResultStatus::ErrorLoader) {
                const u16 loader_id = static_cast<u16>(Core::SystemResultStatus::ErrorLoader);
                const u16 error_id = static_cast<u16>(result) - loader_id;
                const std::string error_code = fmt::format("({:04X}-{:04X})", loader_id, error_id);
                LOG_CRITICAL(Frontend, "Failed to load ROM! {}", error_code);

                const auto title =
                    tr("Error while loading ROM! %1", "%1 signifies a numeric error code.")
                        .arg(QString::fromStdString(error_code));
                const auto description =
                    tr("%1<br>Please redump your files or ask on Discord/Stoat for help.",
                       "%1 signifies an error string.")
                        .arg(QString::fromStdString(
                            GetResultStatusString(static_cast<Loader::ResultStatus>(error_id))));

                QMessageBox::critical(this, title, description);
            } else {
                QMessageBox::critical(
                    this, tr("Error while loading ROM!"),
                    tr("An unknown error occurred. Please see the log for more details."));
            }
            break;
        }
        return false;
    }
    current_game_path = filename;

    return true;
}

bool MainWindow::SelectAndSetCurrentUser(
    const Core::Frontend::ProfileSelectParameters& parameters) {
    QtProfileSelectionDialog dialog(*QtCommon::system, this, parameters);
    dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                          Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    dialog.setWindowModality(Qt::WindowModal);

    if (dialog.exec() == QDialog::Rejected) {
        return false;
    }

    Settings::values.current_user = dialog.GetIndex();
    return true;
}

void MainWindow::BootGame(const QString& filename, Service::AM::FrontendAppletParameters params,
                          StartGameType type) {
    LOG_INFO(Frontend, "Eden starting...");

    if (params.program_id == 0 ||
        params.program_id > static_cast<u64>(Service::AM::AppletProgramId::MaxProgramId)) {
        StoreRecentFile(filename); // Put the filename on top of the list
    }

    // Save configurations
    UpdateUISettings();
    game_list->SaveInterfaceLayout();
    SaveAllConfigValues();

    u64 title_id{0};

    last_filename_booted = filename;

    QtCommon::Content::configureFilesystemProvider(filename.toStdString());
    const auto v_file = Core::GetGameFileFromPath(QtCommon::vfs, filename.toUtf8().constData());
    const auto loader =
        Loader::GetLoader(*QtCommon::system, v_file, params.program_id, params.program_index);

    if (loader != nullptr && loader->ReadProgramId(title_id) == Loader::ResultStatus::Success &&
        type == StartGameType::Normal) {
        // Load per game settings
        const auto file_path =
            std::filesystem::path{Common::U16StringFromBuffer(filename.utf16(), filename.size())};
        const auto config_file_name = title_id == 0
                                          ? Common::FS::PathToUTF8String(file_path.filename())
                                          : fmt::format("{:016X}", title_id);
        QtConfig per_game_config(config_file_name, Config::ConfigType::PerGameConfig);
        QtCommon::system->HIDCore().ReloadInputDevices();
        QtCommon::system->ApplySettings();
    }

    Settings::LogSettings();

    if (UISettings::values.select_user_on_boot && !user_flag_cmd_line) {
        const Core::Frontend::ProfileSelectParameters parameters{
            .mode = Service::AM::Frontend::UiMode::UserSelector,
            .invalid_uid_list = {},
            .display_options = {},
            .purpose = Service::AM::Frontend::UserSelectionPurpose::General,
        };
        if (SelectAndSetCurrentUser(parameters) == false) {
            return;
        }
    }

    // If the user specifies -u (successfully) on the cmd line, don't prompt for a user on first
    // game startup only. If the user stops emulation and starts a new one, go back to the expected
    // behavior of asking.
    user_flag_cmd_line = false;

    if (!LoadROM(filename, params)) {
        return;
    }

    QtCommon::system->SetShuttingDown(false);
    game_list->setDisabled(true);

    // Create and start the emulation thread
    QtCommon::emu_thread = std::make_unique<EmuThread>();
    emit EmulationStarting();
    QtCommon::emu_thread->start();

    // Register an ExecuteProgram callback such that Core can execute a sub-program
    QtCommon::system->RegisterExecuteProgramCallback(
        [this](std::size_t program_index_) { render_window->ExecuteProgram(program_index_); });

    QtCommon::system->RegisterExitCallback([this] {
        QtCommon::emu_thread->ForceStop();
        render_window->Exit();
    });

    QtCommon::system->RegisterApplicationChangedCallback([this](u64 changed_program_id) {
        if (QtCommon::emu_thread)
            QtCommon::emu_thread->RequestDiskShaderCacheReload(changed_program_id);

        QMetaObject::invokeMethod(
            this, [this, changed_program_id] { this->OnApplicationChanged(changed_program_id); },
            Qt::QueuedConnection);
    });

    connect(render_window, &GRenderWindow::Closed, this, &MainWindow::OnStopGame);
    connect(render_window, &GRenderWindow::MouseActivity, this, &MainWindow::OnMouseActivity);

    connect(QtCommon::emu_thread.get(), &EmuThread::LoadProgress, loading_screen,
            &LoadingScreen::OnLoadProgress, Qt::QueuedConnection);

    connect(
        QtCommon::emu_thread.get(), &EmuThread::ShaderCacheReloadStarted, this,
        [this] {
            loading_screen->Prepare(QtCommon::system->GetAppLoader());
            loading_screen->show();
            render_window->hide();
        },
        Qt::QueuedConnection);

    connect(
        QtCommon::emu_thread.get(), &EmuThread::ShaderCacheReloadFinished, this,
        [this] {
            loading_screen->OnLoadComplete();
        },
        Qt::QueuedConnection);

    // Update the GUI
    UpdateStatusButtons();
    if (ui->action_Single_Window_Mode->isChecked()) {
        game_list->hide();
        game_list_placeholder->hide();
    }
    status_bar_update_timer.start(500);
    renderer_status_button->setDisabled(true);
    refresh_button->setDisabled(true);
    SetFPSSuffix();

    if (UISettings::values.hide_mouse || Settings::values.mouse_panning) {
        render_window->installEventFilter(render_window);
        render_window->setAttribute(Qt::WA_Hover, true);
    }

    if (UISettings::values.hide_mouse) {
        mouse_hide_timer.start();
    }

    render_window->InitializeCamera();

    std::string title_name;
    std::string title_version;
    const auto res = QtCommon::system->GetGameName(title_name);

    const auto metadata = [title_id] {
        const FileSys::PatchManager pm(title_id, QtCommon::system->GetFileSystemController(),
                                       QtCommon::system->GetContentProvider());
        return pm.GetControlMetadata();
    }();
    if (metadata.first != nullptr) {
        title_version = metadata.first->GetVersionString();
        title_name = metadata.first->GetApplicationName();
    }
    if (res != Loader::ResultStatus::Success || title_name.empty()) {
        title_name = Common::FS::PathToUTF8String(
            std::filesystem::path{Common::U16StringFromBuffer(filename.utf16(), filename.size())}
                .filename());
    }
    const bool is_64bit = QtCommon::system->Kernel().ApplicationProcess()->Is64Bit();
    const auto instruction_set_suffix = is_64bit ? tr("(64-bit)") : tr("(32-bit)");
    title_name = tr("%1 %2", "%1 is the title name. %2 indicates if the title is 64-bit or 32-bit")
                     .arg(QString::fromStdString(title_name), instruction_set_suffix)
                     .toStdString();
    LOG_INFO(Frontend, "Booting game: {:016X} | {} | {}", title_id, title_name, title_version);
    const auto gpu_vendor = QtCommon::system->GPU().Renderer().GetDeviceVendor();
    UpdateWindowTitle(title_name, title_version, gpu_vendor);

    loading_screen->Prepare(QtCommon::system->GetAppLoader());
    loading_screen->show();

    emulation_running = true;
    if (ui->action_Fullscreen->isChecked()) {
        ShowFullscreen();
    }
    OnStartGame();
}

void MainWindow::BootGameFromList(const QString& filename, StartGameType with_config) {
    BootGame(filename, ApplicationAppletParameters(), with_config);
}

bool MainWindow::OnShutdownBegin() {
    if (!emulation_running) {
        return false;
    }

    if (ui->action_Fullscreen->isChecked()) {
        HideFullscreen();
    }

    AllowOSSleep();

    // Disable unlimited frame rate and turbo/slow modes
    Settings::values.use_speed_limit.SetValue(true);
    Settings::values.current_speed_mode = Settings::SpeedMode::Standard;

    if (QtCommon::system->IsShuttingDown()) {
        return false;
    }

    if (perf_overlay) {
        perf_overlay->hide();
        perf_overlay->deleteLater();
        perf_overlay = nullptr;
    }

    QtCommon::system->SetShuttingDown(true);
    discord_rpc->Pause();

    RequestGameExit();
    QtCommon::emu_thread->disconnect();
    QtCommon::emu_thread->SetRunning(true);

    connect(QtCommon::emu_thread.get(), &QThread::finished, this, &MainWindow::OnEmulationStopped);
    emit EmulationStopping();

    int shutdown_time = 1000;

    if (QtCommon::system->DebuggerEnabled()) {
        shutdown_time = 0;
    } else if (QtCommon::system->GetExitLocked()) {
        shutdown_time = 5000;
    }

    shutdown_timer.setSingleShot(true);
    shutdown_timer.start(shutdown_time);
    connect(&shutdown_timer, &QTimer::timeout, this, &MainWindow::OnEmulationStopTimeExpired);

    // Disable everything to prevent anything from being triggered here
    ui->action_Pause->setEnabled(false);
    ui->action_Restart->setEnabled(false);
    ui->action_Stop->setEnabled(false);

    return true;
}

void MainWindow::OnShutdownBeginDialog() {
    shutdown_dialog =
        new OverlayDialog(this, *QtCommon::system, QString{}, tr("Closing software..."), QString{},
                          QString{}, Qt::AlignHCenter | Qt::AlignVCenter);
    shutdown_dialog->open();
}

void MainWindow::OnEmulationStopTimeExpired() {
    if (QtCommon::emu_thread) {
        QtCommon::emu_thread->ForceStop();
    }
}

void MainWindow::OnEmulationStopped() {
    shutdown_timer.stop();
    if (QtCommon::emu_thread) {
        QtCommon::emu_thread->disconnect();
        QtCommon::emu_thread->wait();
        QtCommon::emu_thread.reset();
    }

    if (shutdown_dialog) {
        shutdown_dialog->deleteLater();
        shutdown_dialog = nullptr;
    }

    emulation_running = false;

    discord_rpc->Update();
    Common::FeralGamemode::Stop();

    // The emulation is stopped, so closing the window or not does not matter anymore
    disconnect(render_window, &GRenderWindow::Closed, this, &MainWindow::OnStopGame);

    // Update the GUI
    UpdateMenuState();

    render_window->hide();
    loading_screen->hide();
    loading_screen->Clear();
    if (game_list->IsEmpty()) {
        game_list_placeholder->show();
    } else {
        game_list->show();
    }
    game_list->SetFilterFocus();
    tas_label->clear();
    input_subsystem->GetTas()->Stop();
    OnTasStateChanged();
    render_window->FinalizeCamera();

    QtCommon::system->GetFrontendAppletHolder().SetCurrentAppletId(Service::AM::AppletId::None);

    // Enable all controllers
    QtCommon::system->HIDCore().SetSupportedStyleTag({Core::HID::NpadStyleSet::All});

    render_window->removeEventFilter(render_window);
    render_window->setAttribute(Qt::WA_Hover, false);

    UpdateWindowTitle();

    // Disable status bar updates
    status_bar_update_timer.stop();
    shader_building_label->setVisible(false);
    res_scale_label->setVisible(false);
    emu_speed_label->setVisible(false);
    game_fps_label->setVisible(false);
    emu_frametime_label->setVisible(false);
    renderer_status_button->setEnabled(!UISettings::values.has_broken_vulkan);
    refresh_button->setEnabled(true);

    if (!firmware_label->text().isEmpty()) {
        firmware_label->setVisible(true);
    }

    current_game_path.clear();

    // When closing the game, destroy the GLWindow to clear the context after the game is closed
    render_window->ReleaseRenderTarget();

    // Enable game list
    game_list->setEnabled(true);

    Settings::RestoreGlobalState(QtCommon::system->IsPoweredOn());
    QtCommon::system->HIDCore().ReloadInputDevices();
    UpdateStatusButtons();
}

void MainWindow::ShutdownGame() {
    if (!emulation_running) {
        return;
    }

    // TODO(crueter): make this common as well (frontend_common?)
    play_time_manager->Stop();
    OnShutdownBegin();
    OnEmulationStopTimeExpired();
    OnEmulationStopped();
}

void MainWindow::StoreRecentFile(const QString& filename) {
    UISettings::values.recent_files.prepend(filename);
    UISettings::values.recent_files.removeDuplicates();
    while (UISettings::values.recent_files.size() > max_recent_files_item) {
        UISettings::values.recent_files.removeLast();
    }

    UpdateRecentFiles();
}

void MainWindow::UpdateRecentFiles() {
    const int num_recent_files =
        (std::min)(static_cast<int>(UISettings::values.recent_files.size()), max_recent_files_item);

    for (int i = 0; i < num_recent_files; i++) {
        const QString text = QStringLiteral("&%1. %2").arg(i + 1).arg(
            QFileInfo(UISettings::values.recent_files[i]).fileName());
        actions_recent_files[i]->setText(text);
        actions_recent_files[i]->setData(UISettings::values.recent_files[i]);
        actions_recent_files[i]->setToolTip(UISettings::values.recent_files[i]);
        actions_recent_files[i]->setVisible(true);
    }

    for (int j = num_recent_files; j < max_recent_files_item; ++j) {
        actions_recent_files[j]->setVisible(false);
    }

    // Enable the recent files menu if the list isn't empty
    ui->menu_recent_files->setEnabled(num_recent_files != 0);
}

void MainWindow::OnGameListLoadFile(QString game_path, u64 program_id) {
    auto params = ApplicationAppletParameters();
    params.program_id = program_id;

    BootGame(game_path, params);
}

// TODO(crueter): Common profile selector
void MainWindow::OnGameListOpenFolder(u64 program_id, GameListOpenTarget target,
                                      const std::string& game_path) {
    std::filesystem::path path;
    QString open_target;

    const auto [user_save_size, device_save_size] = [&game_path, &program_id] {
        const FileSys::PatchManager pm{program_id, QtCommon::system->GetFileSystemController(),
                                       QtCommon::system->GetContentProvider()};
        const auto control = pm.GetControlMetadata().first;
        if (control != nullptr) {
            return std::make_pair(control->GetDefaultNormalSaveSize(),
                                  control->GetDeviceSaveDataSize());
        } else {
            const auto file = Core::GetGameFileFromPath(QtCommon::vfs, game_path);
            const auto loader = Loader::GetLoader(*QtCommon::system, file);

            FileSys::NACP nacp{};
            loader->ReadControlData(nacp);
            return std::make_pair(nacp.GetDefaultNormalSaveSize(), nacp.GetDeviceSaveDataSize());
        }
    }();

    const bool has_user_save{user_save_size > 0};
    const bool has_device_save{device_save_size > 0};

    ASSERT_MSG(has_user_save != has_device_save, "Game uses both user and device savedata?");

    switch (target) {
    case GameListOpenTarget::SaveData: {
        open_target = tr("Save Data");
        const auto save_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir);
        auto vfs_save_dir = QtCommon::vfs->OpenDirectory(Common::FS::PathToUTF8String(save_dir),
                                                         FileSys::OpenMode::Read);

        if (has_user_save) {
            // User save data
            const auto user_id = GetProfileID();
            assert(user_id);

            const auto user_save_data_path = FileSys::SaveDataFactory::GetFullPath(
                {}, vfs_save_dir, FileSys::SaveDataSpaceId::User, FileSys::SaveDataType::Account,
                program_id, user_id->AsU128(), 0);

            path = Common::FS::ConcatPathSafe(save_dir, user_save_data_path);
        } else {
            // Device save data
            const auto device_save_data_path = FileSys::SaveDataFactory::GetFullPath(
                {}, vfs_save_dir, FileSys::SaveDataSpaceId::User, FileSys::SaveDataType::Account,
                program_id, {}, 0);

            path = Common::FS::ConcatPathSafe(save_dir, device_save_data_path);
        }

        if (!Common::FS::CreateDirs(path)) {
            LOG_ERROR(Frontend, "Unable to create the directories for save data");
        }

        break;
    }
    case GameListOpenTarget::ModData: {
        open_target = tr("Mod Data");
        path = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) /
               fmt::format("{:016X}", program_id);
        break;
    }
    default:
        UNIMPLEMENTED();
        break;
    }

    const QString qpath = QString::fromStdString(Common::FS::PathToUTF8String(path));
    const QDir dir(qpath);
    if (!dir.exists()) {
        QMessageBox::warning(this, tr("Error Opening %1 Folder").arg(open_target),
                             tr("Folder does not exist!"));
        return;
    }
    LOG_INFO(Frontend, "Opening {} path for program_id={:016x}", open_target.toStdString(),
             program_id);
    QDesktopServices::openUrl(QUrl::fromLocalFile(qpath));
}

static bool RomFSRawCopy(size_t total_size, size_t& read_size, QProgressDialog& dialog,
                         const FileSys::VirtualDir& src, const FileSys::VirtualDir& dest,
                         bool full) {
    if (src == nullptr || dest == nullptr || !src->IsReadable() || !dest->IsWritable())
        return false;
    if (dialog.wasCanceled())
        return false;

    std::vector<u8> buffer(CopyBufferSize);
    auto last_timestamp = std::chrono::steady_clock::now();

    const auto QtRawCopy = [&](const FileSys::VirtualFile& src_file,
                               const FileSys::VirtualFile& dest_file) {
        if (src_file == nullptr || dest_file == nullptr) {
            return false;
        }
        if (!dest_file->Resize(src_file->GetSize())) {
            return false;
        }

        for (std::size_t i = 0; i < src_file->GetSize(); i += buffer.size()) {
            if (dialog.wasCanceled()) {
                dest_file->Resize(0);
                return false;
            }

            using namespace std::literals::chrono_literals;
            const auto new_timestamp = std::chrono::steady_clock::now();

            if ((new_timestamp - last_timestamp) > 33ms) {
                last_timestamp = new_timestamp;
                dialog.setValue(
                    static_cast<int>((std::min)(read_size, total_size) * 100 / total_size));
                QCoreApplication::processEvents();
            }

            const auto read = src_file->Read(buffer.data(), buffer.size(), i);
            dest_file->Write(buffer.data(), read, i);

            read_size += read;
        }

        return true;
    };

    if (full) {
        for (const auto& file : src->GetFiles()) {
            const auto out = VfsDirectoryCreateFileWrapper(dest, file->GetName());
            if (!QtRawCopy(file, out))
                return false;
        }
    }

    for (const auto& dir : src->GetSubdirectories()) {
        const auto out = dest->CreateSubdirectory(dir->GetName());
        if (!RomFSRawCopy(total_size, read_size, dialog, dir, out, full))
            return false;
    }

    return true;
}

// TODO(crueter): All this can be transfered to qt_common
// Aldoe I need to decide re: message boxes for QML
// translations_common? strings_common? qt_strings? who knows
void MainWindow::OnGameListRemoveInstalledEntry(u64 program_id,
                                                QtCommon::Game::InstalledEntryType type) {
    const QString entry_question = [type] {
        switch (type) {
        case QtCommon::Game::InstalledEntryType::Game:
            return tr("Remove Installed Game Contents?");
        case QtCommon::Game::InstalledEntryType::Update:
            return tr("Remove Installed Game Update?");
        case QtCommon::Game::InstalledEntryType::AddOnContent:
            return tr("Remove Installed Game DLC?");
        default:
            return QStringLiteral("Remove Installed Game <Invalid Type>?");
        }
    }();

    if (!question(this, tr("Remove Entry"), entry_question, QMessageBox::Yes | QMessageBox::No,
                  QMessageBox::No)) {
        return;
    }

    // TODO(crueter): move this to QtCommon (populate async?)
    switch (type) {
    case QtCommon::Game::InstalledEntryType::Game:
        QtCommon::Game::RemoveBaseContent(program_id, type);
        [[fallthrough]];
    case QtCommon::Game::InstalledEntryType::Update:
        QtCommon::Game::RemoveUpdateContent(program_id, type);
        if (type != QtCommon::Game::InstalledEntryType::Game) {
            break;
        }
        [[fallthrough]];
    case QtCommon::Game::InstalledEntryType::AddOnContent:
        QtCommon::Game::RemoveAddOnContent(program_id, type);
        break;
    }
    Common::FS::RemoveDirRecursively(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                                     "game_list");
    game_list->PopulateAsync(UISettings::values.game_dirs);
}

void MainWindow::OnGameListRemoveFile(u64 program_id, QtCommon::Game::GameListRemoveTarget target,
                                      const std::string& game_path) {
    const QString question = [target] {
        switch (target) {
        case QtCommon::Game::GameListRemoveTarget::GlShaderCache:
            return tr("Delete OpenGL Transferable Shader Cache?");
        case QtCommon::Game::GameListRemoveTarget::VkShaderCache:
            return tr("Delete Vulkan Transferable Shader Cache?");
        case QtCommon::Game::GameListRemoveTarget::AllShaderCache:
            return tr("Delete All Transferable Shader Caches?");
        case QtCommon::Game::GameListRemoveTarget::CustomConfiguration:
            return tr("Remove Custom Game Configuration?");
        case QtCommon::Game::GameListRemoveTarget::CacheStorage:
            return tr("Remove Cache Storage?");
        default:
            return QString{};
        }
    }();

    if (!MainWindow::question(this, tr("Remove File"), question, QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No)) {
        return;
    }

    switch (target) {
    case QtCommon::Game::GameListRemoveTarget::VkShaderCache:
        QtCommon::Game::RemoveVulkanDriverPipelineCache(program_id);
        [[fallthrough]];
    case QtCommon::Game::GameListRemoveTarget::GlShaderCache:
        QtCommon::Game::RemoveTransferableShaderCache(program_id, target);
        break;
    case QtCommon::Game::GameListRemoveTarget::AllShaderCache:
        QtCommon::Game::RemoveAllTransferableShaderCaches(program_id);
        break;
    case QtCommon::Game::GameListRemoveTarget::CustomConfiguration:
        QtCommon::Game::RemoveCustomConfiguration(program_id, game_path);
        break;
    case QtCommon::Game::GameListRemoveTarget::CacheStorage:
        QtCommon::Game::RemoveCacheStorage(program_id);
        break;
    }
}

void MainWindow::OnGameListSetPlayTime(u64 program_id) {
    const u64 current_play_time = play_time_manager->GetPlayTime(program_id);

    SetPlayTimeDialog dialog(this, current_play_time);

    if (dialog.exec() == QDialog::Accepted) {
        const u64 total_seconds = dialog.GetTotalSeconds();
        play_time_manager->SetPlayTime(program_id, total_seconds);
        game_list->PopulateAsync(UISettings::values.game_dirs);
    }
}

void MainWindow::OnGameListRemovePlayTimeData(u64 program_id) {
    if (QMessageBox::question(this, tr("Remove Play Time Data"), tr("Reset play time?"),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    play_time_manager->ResetProgramPlayTime(program_id);
    game_list->PopulateAsync(UISettings::values.game_dirs);
}

void MainWindow::OnGameListDumpRomFS(u64 program_id, const std::string& game_path,
                                     DumpRomFSTarget target) {
    const auto failed = [this] {
        QMessageBox::warning(this, tr("RomFS Extraction Failed!"),
                             tr("There was an error copying the RomFS files or the user "
                                "cancelled the operation."));
    };

    const auto loader = Loader::GetLoader(
        *QtCommon::system, QtCommon::vfs->OpenFile(game_path, FileSys::OpenMode::Read));
    if (loader == nullptr) {
        failed();
        return;
    }

    FileSys::VirtualFile packed_update_raw{};
    loader->ReadUpdateRaw(packed_update_raw);

    const auto& installed = QtCommon::system->GetContentProvider();

    u64 title_id{};
    u8 raw_type{};
    if (!SelectRomFSDumpTarget(installed, program_id, &title_id, &raw_type)) {
        failed();
        return;
    }

    const auto type = static_cast<FileSys::ContentRecordType>(raw_type);
    const auto base_nca = installed.GetEntry(title_id, type);
    if (!base_nca) {
        failed();
        return;
    }

    const FileSys::NCA update_nca{packed_update_raw, nullptr};
    if (type != FileSys::ContentRecordType::Program ||
        update_nca.GetStatus() != Loader::ResultStatus::ErrorMissingBKTRBaseRomFS ||
        update_nca.GetTitleId() != FileSys::GetUpdateTitleID(title_id)) {
        packed_update_raw = {};
    }

    const auto base_romfs = base_nca->GetRomFS();
    const auto dump_dir =
        target == DumpRomFSTarget::Normal
            ? Common::FS::GetEdenPath(Common::FS::EdenPath::DumpDir)
            : Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir) / "atmosphere" / "contents";
    const auto romfs_dir = fmt::format("{:016X}/romfs", title_id);

    const auto path = Common::FS::PathToUTF8String(dump_dir / romfs_dir);

    const FileSys::PatchManager pm{title_id, QtCommon::system->GetFileSystemController(),
                                   installed};
    auto romfs = pm.PatchRomFS(base_nca.get(), base_romfs, type, packed_update_raw, false);

    const auto out = VfsFilesystemCreateDirectoryWrapper(path, FileSys::OpenMode::ReadWrite);

    if (out == nullptr) {
        failed();
        QtCommon::vfs->DeleteDirectory(path);
        return;
    }

    bool ok = false;
    const QStringList selections{tr("Full"), tr("Skeleton")};
    const auto res = QInputDialog::getItem(
        this, tr("Select RomFS Dump Mode"),
        tr("Please select the how you would like the RomFS dumped.<br>Full will copy all of the "
           "files into the new directory while <br>skeleton will only create the directory "
           "structure."),
        selections, 0, false, &ok);
    if (!ok) {
        failed();
        QtCommon::vfs->DeleteDirectory(path);
        return;
    }

    const auto extracted = FileSys::ExtractRomFS(romfs);
    if (extracted == nullptr) {
        failed();
        return;
    }

    const auto full = res == selections.constFirst();

    // The expected required space is the size of the RomFS + 1 GiB
    const auto minimum_free_space = romfs->GetSize() + 0x40000000;

    if (full && Common::FS::GetFreeSpaceSize(path) < minimum_free_space) {
        QMessageBox::warning(this, tr("RomFS Extraction Failed!"),
                             tr("There is not enough free space at %1 to extract the RomFS. Please "
                                "free up space or select a different dump directory at "
                                "Emulation > Configure > System > Filesystem > Dump Root")
                                 .arg(QString::fromStdString(path)));
        return;
    }

    QProgressDialog progress(tr("Extracting RomFS..."), tr("Cancel"), 0, 100, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(100);
    progress.setAutoClose(false);
    progress.setAutoReset(false);

    size_t read_size = 0;

    if (RomFSRawCopy(romfs->GetSize(), read_size, progress, extracted, out, full)) {
        progress.close();
        QMessageBox::information(this, tr("RomFS Extraction Succeeded!"),
                                 tr("The operation completed successfully."));
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(path)));
    } else {
        progress.close();
        failed();
        QtCommon::vfs->DeleteDirectory(path);
    }
}

// END
void MainWindow::OnGameListVerifyIntegrity(const std::string& game_path) {
    QtCommon::Content::VerifyGameContents(game_path);
}

void MainWindow::OnGameListCopyTID(u64 program_id) {
    QClipboard* clipboard = QGuiApplication::clipboard();
    clipboard->setText(QString::fromStdString(fmt::format("{:016X}", program_id)));
}

void MainWindow::OnGameListCreateShortcut(u64 program_id, const std::string& game_path,
                                          const QtCommon::Game::ShortcutTarget target) {
    // Create shortcu
    std::string arguments = fmt::format("-g \"{:s}\"", game_path);

    QtCommon::Game::CreateShortcut(game_path, program_id, "", target, arguments, true);
}

void MainWindow::OnGameListOpenDirectory(const QString& directory) {
    // TODO(crueter): QtCommon
    std::filesystem::path fs_path;
    if (directory == QStringLiteral("SDMC")) {
        fs_path =
            Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir) / "Nintendo/Contents/registered";
    } else if (directory == QStringLiteral("UserNAND")) {
        fs_path =
            Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "user/Contents/registered";
    } else if (directory == QStringLiteral("SysNAND")) {
        fs_path =
            Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "system/Contents/registered";
    } else {
        fs_path = directory.toStdString();
    }

    const auto qt_path = QString::fromStdString(Common::FS::PathToUTF8String(fs_path));

    if (!Common::FS::IsDir(fs_path)) {
        QMessageBox::critical(this, tr("Error Opening %1").arg(qt_path),
                              tr("Folder does not exist!"));
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(qt_path));
}

void MainWindow::OnGameListAddDirectory() {
    const QString dir_path = QFileDialog::getExistingDirectory(this, tr("Select Directory"));
    if (dir_path.isEmpty()) {
        return;
    }

    UISettings::GameDir game_dir{dir_path.toStdString(), false, true};
    if (!UISettings::values.game_dirs.contains(game_dir)) {
        UISettings::values.game_dirs.append(game_dir);
        game_list->PopulateAsync(UISettings::values.game_dirs);
    } else {
        LOG_WARNING(Frontend, "Selected directory is already in the game list");
    }

    OnSaveConfig();
}

void MainWindow::OnGameListShowList(bool show) {
    if (emulation_running && ui->action_Single_Window_Mode->isChecked())
        return;
    game_list->setVisible(show);
    game_list_placeholder->setVisible(!show);
};

void MainWindow::OnGameListOpenPerGameProperties(const std::string& file) {
    u64 title_id{};
    const auto v_file = Core::GetGameFileFromPath(QtCommon::vfs, file);
    const auto loader = Loader::GetLoader(*QtCommon::system, v_file);

    if (loader == nullptr || loader->ReadProgramId(title_id) != Loader::ResultStatus::Success) {
        QMessageBox::information(this, tr("Properties"),
                                 tr("The game properties could not be loaded."));
        return;
    }

    OpenPerGameConfiguration(title_id, file);
}

void MainWindow::OnLinkToRyujinx(const u64& program_id) {
    namespace fs = std::filesystem;

    fs::path ryu_dir;

    // find an existing Ryujinx linked path in config.ini; if it exists, use it as a "hint"
    // If it's not defined in config.ini, use default
    const fs::path existing_path =
        UISettings::values.ryujinx_link_paths
            .value(program_id, QDir(Common::FS::GetLegacyPath(Common::FS::RyujinxDir)))
            .filesystemAbsolutePath();

    // this function also prompts the user to manually specify a portable location
    ryu_dir = QtCommon::FS::GetRyujinxSavePath(existing_path, program_id);

    if (ryu_dir.empty())
        return;

    const std::string user_id = GetProfileIDString();
    if (user_id.empty())
        return;

    const std::string hex_program = fmt::format("{:016X}", program_id);

    const fs::path eden_dir = FrontendCommon::DataManager::GetDataDir(
                                  FrontendCommon::DataManager::DataDir::Saves, user_id) /
                              hex_program;

    // CheckUnlink basically just checks to see if one or both are linked, and prompts the user to
    // unlink if this is the case.
    // If it returns false, neither dir is linked so it's fine to continue
    if (!QtCommon::FS::CheckUnlink(eden_dir, ryu_dir)) {
        RyujinxDialog dialog(eden_dir, ryu_dir, this);
        if (dialog.exec() == QDialog::Accepted) {
            UISettings::values.ryujinx_link_paths.insert(
                program_id,
                QString::fromStdString(Common::FS::GetRyuPathFromSavePath(ryu_dir).string()));
        }
    } else {
        UISettings::values.ryujinx_link_paths.remove(program_id);
    }

    SaveAllConfigValues();
}

void MainWindow::OnMenuLoadFile() {
    if (is_load_file_select_active) {
        return;
    }

    is_load_file_select_active = true;
    const QString extensions =
        QStringLiteral("*.")
            .append(QtCommon::supported_file_extensions.join(QStringLiteral(" *.")))
            .append(QStringLiteral(" main"));
    const QString file_filter = tr("Switch Executable (%1);;All Files (*.*)",
                                   "%1 is an identifier for the Switch executable file extensions.")
                                    .arg(extensions);
    const QString filename = QFileDialog::getOpenFileName(
        this, tr("Load File"), QString::fromStdString(UISettings::values.roms_path), file_filter);
    is_load_file_select_active = false;

    if (filename.isEmpty()) {
        return;
    }

    UISettings::values.roms_path = QFileInfo(filename).path().toStdString();
    BootGame(filename, ApplicationAppletParameters());
}

void MainWindow::OnMenuLoadFolder() {
    const QString dir_path =
        QFileDialog::getExistingDirectory(this, tr("Open Extracted ROM Directory"));

    if (dir_path.isNull()) {
        return;
    }

    const QDir dir{dir_path};
    const QStringList matching_main = dir.entryList({QStringLiteral("main")}, QDir::Files);
    if (matching_main.size() == 1) {
        BootGame(dir.path() + QDir::separator() + matching_main[0], ApplicationAppletParameters());
    } else {
        QMessageBox::warning(this, tr("Invalid Directory Selected"),
                             tr("The directory you have selected does not contain a 'main' file."));
    }
}

void MainWindow::IncrementInstallProgress() {
    install_progress->setValue(install_progress->value() + 1);
}

void MainWindow::OnMenuInstallToNAND() {
    const QString file_filter =
        tr("Installable Switch File (*.nca *.nsp *.nsz *.xci *.xcz);;Nintendo Content Archive "
           "(*.nca);;Nintendo Submission Package (*.nsp *.nsz);;NX Cartridge "
           "Image (*.xci *.xcz)");

    QStringList filenames = QFileDialog::getOpenFileNames(
        this, tr("Install Files"), QString::fromStdString(UISettings::values.roms_path),
        file_filter);

    if (filenames.isEmpty()) {
        return;
    }

    InstallDialog installDialog(this, filenames);
    if (installDialog.exec() == QDialog::Rejected) {
        return;
    }

    const QStringList files = installDialog.GetFiles();

    if (files.isEmpty()) {
        return;
    }

    // Save folder location of the first selected file
    UISettings::values.roms_path = QFileInfo(filenames[0]).path().toStdString();

    int remaining = filenames.size();

    // This would only overflow above 2^51 bytes (2.252 PB)
    int total_size = 0;
    for (const QString& file : files) {
        total_size += static_cast<int>(QFile(file).size() / CopyBufferSize);
    }
    if (total_size < 0) {
        LOG_CRITICAL(Frontend, "Attempting to install too many files, aborting.");
        return;
    }

    QStringList new_files{};         // Newly installed files that do not yet exist in the NAND
    QStringList overwritten_files{}; // Files that overwrote those existing in the NAND
    QStringList failed_files{};      // Files that failed to install due to errors
    bool detected_base_install{};    // Whether a base game was attempted to be installed

    ui->action_Install_File_NAND->setEnabled(false);

    install_progress = new QProgressDialog(QString{}, tr("Cancel"), 0, total_size, this);
    install_progress->setWindowFlags(windowFlags() & ~Qt::WindowMaximizeButtonHint);
    install_progress->setAttribute(Qt::WA_DeleteOnClose, true);
    install_progress->setFixedWidth(installDialog.GetMinimumWidth() + 40);
    install_progress->show();

    for (const QString& file : files) {
        install_progress->setWindowTitle(tr("%n file(s) remaining", "", remaining));
        install_progress->setLabelText(
            tr("Installing file \"%1\"...").arg(QFileInfo(file).fileName()));

        QFuture<ContentManager::InstallResult> future;
        ContentManager::InstallResult result;

        if (file.endsWith(QStringLiteral("nsp"), Qt::CaseInsensitive) ||
            file.endsWith(QStringLiteral("nsz"), Qt::CaseInsensitive) ||
            file.endsWith(QStringLiteral("xci"), Qt::CaseInsensitive) ||
            file.endsWith(QStringLiteral("xcz"), Qt::CaseInsensitive)) {
            const auto progress_callback = [this](size_t size, size_t progress) {
                emit UpdateInstallProgress();
                if (install_progress->wasCanceled()) {
                    return true;
                }
                return false;
            };
            future = QtConcurrent::run([&file, progress_callback] {
                return ContentManager::InstallNSP(*QtCommon::system, *QtCommon::vfs,
                                                  file.toStdString(), progress_callback);
            });

            while (!future.isFinished()) {
                QCoreApplication::processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            result = future.result();

        } else {
            result = InstallNCA(file);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        switch (result) {
        case ContentManager::InstallResult::Success:
            new_files.append(QFileInfo(file).fileName());
            break;
        case ContentManager::InstallResult::Overwrite:
            overwritten_files.append(QFileInfo(file).fileName());
            break;
        case ContentManager::InstallResult::Failure:
            failed_files.append(QFileInfo(file).fileName());
            break;
        case ContentManager::InstallResult::BaseInstallAttempted:
            failed_files.append(QFileInfo(file).fileName());
            detected_base_install = true;
            break;
        }

        --remaining;
    }

    install_progress->close();

    if (detected_base_install) {
        QMessageBox::warning(
            this, tr("Install Results"),
            tr("To avoid possible conflicts, we discourage users from installing base games to the "
               "NAND.\nPlease, only use this feature to install updates and DLC."));
    }

    const QString install_results =
        (new_files.isEmpty() ? QString{}
                             : tr("%n file(s) were newly installed\n", "", new_files.size())) +
        (overwritten_files.isEmpty()
             ? QString{}
             : tr("%n file(s) were overwritten\n", "", overwritten_files.size())) +
        (failed_files.isEmpty() ? QString{}
                                : tr("%n file(s) failed to install\n", "", failed_files.size()));

    QMessageBox::information(this, tr("Install Results"), install_results);
    Common::FS::RemoveDirRecursively(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                                     "game_list");
    game_list->PopulateAsync(UISettings::values.game_dirs);
    ui->action_Install_File_NAND->setEnabled(true);
}

ContentManager::InstallResult MainWindow::InstallNCA(const QString& filename) {
    const QStringList tt_options{tr("System Application"),
                                 tr("System Archive"),
                                 tr("System Application Update"),
                                 tr("Firmware Package (Type A)"),
                                 tr("Firmware Package (Type B)"),
                                 tr("Game"),
                                 tr("Game Update"),
                                 tr("Game DLC"),
                                 tr("Delta Title")};
    bool ok;
    const auto item = QInputDialog::getItem(
        this, tr("Select NCA Install Type..."),
        tr("Please select the type of title you would like to install this NCA as:\n(In "
           "most instances, the default 'Game' is fine.)"),
        tt_options, 5, false, &ok);

    auto index = tt_options.indexOf(item);
    if (!ok || index == -1) {
        QMessageBox::warning(this, tr("Failed to Install"),
                             tr("The title type you selected for the NCA is invalid."));
        return ContentManager::InstallResult::Failure;
    }

    // If index is equal to or past Game, add the jump in TitleType.
    if (index >= 5) {
        index += static_cast<size_t>(FileSys::TitleType::Application) -
                 static_cast<size_t>(FileSys::TitleType::FirmwarePackageB);
    }

    const bool is_application = index >= static_cast<s32>(FileSys::TitleType::Application);
    const auto& fs_controller = QtCommon::system->GetFileSystemController();
    auto* registered_cache = is_application ? fs_controller.GetUserNANDContents()
                                            : fs_controller.GetSystemNANDContents();

    const auto progress_callback = [this](size_t size, size_t progress) {
        emit UpdateInstallProgress();
        if (install_progress->wasCanceled()) {
            return true;
        }
        return false;
    };
    return ContentManager::InstallNCA(*QtCommon::vfs, filename.toStdString(), *registered_cache,
                                      static_cast<FileSys::TitleType>(index), progress_callback);
}

void MainWindow::OnMenuRecentFile() {
    QAction* action = qobject_cast<QAction*>(sender());
    assert(action);

    const QString filename = action->data().toString();
    if (QFileInfo::exists(filename)) {
        BootGame(filename, ApplicationAppletParameters());
    } else {
        // Display an error message and remove the file from the list.
        QMessageBox::information(this, tr("File not found"),
                                 tr("File \"%1\" not found").arg(filename));

        UISettings::values.recent_files.removeOne(filename);
        UpdateRecentFiles();
    }
}

void MainWindow::OnStartGame() {
    PreventOSSleep();

    QtCommon::emu_thread->SetRunning(true);

    UpdateMenuState();
    OnTasStateChanged();

    play_time_manager->SetProgramId(QtCommon::system->GetApplicationProcessProgramID());
    play_time_manager->Start();

    discord_rpc->Update();
    Common::FeralGamemode::Start();
}

void MainWindow::OnRestartGame() {
    if (!QtCommon::system->IsPoweredOn()) {
        return;
    }

    if (ConfirmShutdownGame()) {
        // Make a copy since ShutdownGame edits game_path
        const auto current_game = QString(current_game_path);
        ShutdownGame();
        BootGame(current_game, ApplicationAppletParameters());
    }
}

void MainWindow::OnPauseGame() {
    QtCommon::emu_thread->SetRunning(false);
    play_time_manager->Stop();
    UpdateMenuState();
    AllowOSSleep();
    Common::FeralGamemode::Stop();
}

void MainWindow::OnPauseContinueGame() {
    if (emulation_running) {
        if (QtCommon::emu_thread->IsRunning()) {
            OnPauseGame();
        } else {
            OnStartGame();
        }
    }
}

void MainWindow::OnStopGame() {
    if (ConfirmShutdownGame()) {
        play_time_manager->Stop();
        // Update game list to show new play time
        game_list->PopulateAsync(UISettings::values.game_dirs);
        if (OnShutdownBegin()) {
            OnShutdownBeginDialog();
        } else {
            OnEmulationStopped();
        }
    }
}

bool MainWindow::ConfirmShutdownGame() {
    if (UISettings::values.confirm_before_stopping.GetValue() == ConfirmStop::Ask_Always) {
        if (QtCommon::system->GetExitLocked()) {
            if (!ConfirmForceLockedExit()) {
                return false;
            }
        } else {
            if (!ConfirmChangeGame()) {
                return false;
            }
        }
    } else {
        if (UISettings::values.confirm_before_stopping.GetValue() ==
                ConfirmStop::Ask_Based_On_Game &&
            QtCommon::system->GetExitLocked()) {
            if (!ConfirmForceLockedExit()) {
                return false;
            }
        }
    }
    return true;
}

void MainWindow::OnLoadComplete() {
    loading_screen->OnLoadComplete();

    perf_overlay = new PerformanceOverlay(this);
    perf_overlay->setVisible(ui->action_Show_Performance_Overlay->isChecked());

    connect(perf_overlay, &PerformanceOverlay::closed, perf_overlay,
            [this]() { ui->action_Show_Performance_Overlay->setChecked(false); });
}

void MainWindow::OnExecuteProgram(std::size_t program_index) {
    const u64 previous_program_id = QtCommon::system->GetApplicationProcessProgramID();

    const auto current_path =
        QString::fromStdString(QtCommon::system->GetCurrentApplicationFilePath());

    LOG_INFO(Frontend, "ExecuteProgram requested, program_index={} previous_program_id={:016X}",
             program_index, previous_program_id);

    ShutdownGame();

    auto params = ApplicationAppletParameters();
    params.program_index = static_cast<s32>(program_index);
    params.launch_type = Service::AM::LaunchType::ApplicationInitiated;

    if (previous_program_id > static_cast<u64>(Service::AM::AppletProgramId::MaxProgramId)) {
        params.previous_program_index =
            static_cast<s32>(previous_program_id - FileSys::GetBaseTitleID(previous_program_id));
        params.program_id = previous_program_id;
    }

    const auto filename = current_path.isEmpty() ? last_filename_booted : current_path;

    LOG_DEBUG(Frontend, "ExecuteProgram booting from path: {}", filename.toStdString());

    BootGame(filename, params);
}

void MainWindow::OnExit() {
    ShutdownGame();
}

void MainWindow::OnSaveConfig() {
    QtCommon::system->ApplySettings();
    SaveAllConfigValues();
}

void MainWindow::ErrorDisplayDisplayError(QString error_code, QString error_text) {
    error_applet = new OverlayDialog(render_window, *QtCommon::system, error_code, error_text,
                                     QString{}, tr("OK"), Qt::AlignLeft | Qt::AlignVCenter);
    SCOPE_EXIT {
        error_applet->deleteLater();
        error_applet = nullptr;
    };
    error_applet->exec();

    emit ErrorDisplayFinished();
}

void MainWindow::ErrorDisplayRequestExit() {
    if (error_applet) {
        error_applet->reject();
    }
}

void MainWindow::OnMenuReportCompatibility() {
    QtCommon::Frontend::Critical(
        tr("Function Disabled"),
        tr("Compatibility list reporting is currently disabled. Check back later!"));

    // #if defined(ARCHITECTURE_x86_64) && !defined(__APPLE__)
    //     const auto& caps = g_cpu_caps;
    //     const bool has_fma = caps.fma;
    //     const auto processor_count = std::thread::hardware_concurrency();
    //     const bool has_4threads = processor_count == 0 || processor_count >= 4;
    //     const bool has_8gb_ram = Common::GetMemInfo().TotalPhysicalMemory >= 8_GiB;
    //     const bool has_broken_vulkan = UISettings::values.has_broken_vulkan;

    //     if (!has_fma || !has_4threads || !has_8gb_ram || has_broken_vulkan) {
    //         QMessageBox::critical(this, tr("Hardware requirements not met"),
    //                               tr("Your system does not meet the recommended hardware
    //                               requirements. "
    //                                  "Compatibility reporting has been disabled."));
    //         return;
    //     }

    //     if (!Settings::values.eden_token.GetValue().empty() &&
    //         !Settings::values.eden_username.GetValue().empty()) {
    //     } else {
    //         QMessageBox::critical(
    //             this, tr("Missing yuzu Account"),
    //             tr("In order to submit a game compatibility test case, you must set up your web
    //             token "
    //                "and "
    //                "username.<br><br/>To link your eden account, go to Emulation &gt;
    //                Configuration "
    //                "&gt; "
    //                "Web."));
    //     }
    // #else
    //     QMessageBox::critical(this, tr("Hardware requirements not met"),
    //                           tr("Your system does not meet the recommended hardware
    //                           requirements. "
    //                              "Compatibility reporting has been disabled."));
    // #endif
}

void MainWindow::OpenURL(const QUrl& url) {
    const bool open = QDesktopServices::openUrl(url);
    if (!open) {
        QMessageBox::warning(this, tr("Error opening URL"),
                             tr("Unable to open the URL \"%1\".").arg(url.toString()));
    }
}

void MainWindow::OnOpenModsPage() {
    OpenURL(QUrl(QStringLiteral("https://github.com/eden-emulator/yuzu-mod-archive")));
}

void MainWindow::OnOpenUserHandbook() {
    OpenURL(QUrl(QStringLiteral("https://git.eden-emu.dev/eden-emu/eden/src/branch/master/docs/user/README.md")));
}

void MainWindow::ToggleFullscreen() {
    if (!emulation_running) {
        return;
    }
    if (ui->action_Fullscreen->isChecked()) {
        ShowFullscreen();
    } else {
        HideFullscreen();
    }
}

// We're going to return the screen that the given window has the most pixels on
static QScreen* GuessCurrentScreen(QWidget* window) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    return *std::max_element(
        screens.cbegin(), screens.cend(), [window](const QScreen* left, const QScreen* right) {
            const QSize left_size = left->geometry().intersected(window->geometry()).size();
            const QSize right_size = right->geometry().intersected(window->geometry()).size();
            return (left_size.height() * left_size.width()) <
                   (right_size.height() * right_size.width());
        });
}

bool MainWindow::UsingExclusiveFullscreen() {
    return Settings::values.fullscreen_mode.GetValue() == Settings::FullscreenMode::Exclusive ||
           QGuiApplication::platformName() == QStringLiteral("wayland") ||
           QGuiApplication::platformName() == QStringLiteral("wayland-egl");
}

void MainWindow::ShowFullscreen() {
    const auto show_fullscreen = [this](QWidget* window) {
        if (UsingExclusiveFullscreen()) {
            window->showFullScreen();
            return;
        }
        window->hide();
        window->setWindowFlags(window->windowFlags() | Qt::FramelessWindowHint);
        const auto screen_geometry = GuessCurrentScreen(window)->geometry();
        window->setGeometry(screen_geometry.x(), screen_geometry.y(), screen_geometry.width(),
                            screen_geometry.height() + 1);
        window->raise();
        window->showNormal();
    };

    if (ui->action_Single_Window_Mode->isChecked()) {
        UISettings::values.geometry = saveGeometry();

        ui->menubar->hide();
        statusBar()->hide();

        show_fullscreen(this);
    } else {
        UISettings::values.renderwindow_geometry = render_window->saveGeometry();
        show_fullscreen(render_window);
    }
}

void MainWindow::HideFullscreen() {
    if (ui->action_Single_Window_Mode->isChecked()) {
        if (UsingExclusiveFullscreen()) {
            showNormal();
            restoreGeometry(UISettings::values.geometry);
        } else {
            hide();
            setWindowFlags(windowFlags() & ~Qt::FramelessWindowHint);
            restoreGeometry(UISettings::values.geometry);
            raise();
            show();
        }

        statusBar()->setVisible(ui->action_Show_Status_Bar->isChecked());
        ui->menubar->show();
    } else {
        if (UsingExclusiveFullscreen()) {
            render_window->showNormal();
            render_window->restoreGeometry(UISettings::values.renderwindow_geometry);
        } else {
            render_window->hide();
            render_window->setWindowFlags(windowFlags() & ~Qt::FramelessWindowHint);
            render_window->restoreGeometry(UISettings::values.renderwindow_geometry);
            render_window->raise();
            render_window->show();
        }
    }
}

void MainWindow::ToggleWindowMode() {
    if (ui->action_Single_Window_Mode->isChecked()) {
        // Render in the main window...
        render_window->BackupGeometry();
        ui->horizontalLayout->addWidget(render_window);
        render_window->setFocusPolicy(Qt::StrongFocus);
        if (emulation_running) {
            render_window->setVisible(true);
            render_window->setFocus();
            game_list->hide();
        }

    } else {
        // Render in a separate window...
        ui->horizontalLayout->removeWidget(render_window);
        render_window->setParent(nullptr);
        render_window->setFocusPolicy(Qt::NoFocus);
        if (emulation_running) {
            render_window->setVisible(true);
            render_window->RestoreGeometry();
            game_list->show();
        }
    }
}

void MainWindow::ResetWindowSize(u32 width, u32 height) {
    const auto aspect_ratio = Layout::EmulationAspectRatio(Settings::values.aspect_ratio.GetValue(),
                                                           float(height) / width);
    if (!ui->action_Single_Window_Mode->isChecked()) {
        render_window->resize(height / aspect_ratio, height);
    } else {
        const bool show_status_bar = ui->action_Show_Status_Bar->isChecked();
        const auto status_bar_height = show_status_bar ? statusBar()->height() : 0;
        resize(height / aspect_ratio, height + menuBar()->height() + status_bar_height);
    }
}

void MainWindow::ResetWindowSize720() {
    ResetWindowSize(Layout::ScreenUndocked::Width, Layout::ScreenUndocked::Height);
}

void MainWindow::ResetWindowSize900() {
    ResetWindowSize(1600U, 900U);
}

void MainWindow::ResetWindowSize1080() {
    ResetWindowSize(Layout::ScreenDocked::Width, Layout::ScreenDocked::Height);
}

void MainWindow::SetGameListMode(Settings::GameListMode mode) {
    ui->action_Grid_View->setChecked(mode == Settings::GameListMode::GridView);
    ui->action_Tree_View->setChecked(mode == Settings::GameListMode::TreeView);
    ui->action_Carousel_View->setChecked(mode == Settings::GameListMode::CarouselView);

    UISettings::values.game_list_mode = mode;
    ui->action_Show_Game_Name->setEnabled(mode != Settings::GameListMode::TreeView);

    CheckIconSize();
    game_list->ResetViewMode();
}

void MainWindow::SetGridView() {
    SetGameListMode(Settings::GameListMode::GridView);
}

void MainWindow::SetTreeView() {
    SetGameListMode(Settings::GameListMode::TreeView);
}

void MainWindow::SetCarouselView() {
    SetGameListMode(Settings::GameListMode::CarouselView);
}

void MainWindow::CheckIconSize() {
    // When in grid/carousel view mode, with text off
    // there is no point in having icons turned off
    auto actions = game_size_actions->actions();
    if (UISettings::values.game_list_mode.GetValue() != Settings::GameListMode::TreeView &&
        !UISettings::values.show_game_name.GetValue()) {
        u32 newSize = UISettings::values.game_icon_size.GetValue();
        if (newSize == 0) {
            newSize = 64;
            UISettings::values.game_icon_size.SetValue(newSize);
        }

        // Then disable the "none" action and update that menu.
        for (size_t i = 0; i < default_game_icon_sizes.size(); i++) {
            const auto current_size = newSize;
            const auto size = default_game_icon_sizes[i].first;
            if (current_size == size)
                actions.at(i)->setChecked(true);
        }

        // Update this if you add anything before None.
        actions.at(0)->setEnabled(false);
    } else {
        actions.at(0)->setEnabled(true);
    }
}

void MainWindow::ToggleShowGameName() {
    auto& setting = UISettings::values.show_game_name;
    const bool newValue = !setting.GetValue();
    ui->action_Show_Game_Name->setChecked(newValue);
    setting.SetValue(newValue);

    CheckIconSize();

    game_list->UpdateIconSizes();
    game_list->RefreshGameDirectory();
}

void MainWindow::OnConfigure() {
    const auto old_theme = UISettings::values.theme;
    const bool old_discord_presence = UISettings::values.enable_discord_presence.GetValue();
    const auto old_language_index = Settings::values.language_index.GetValue();
    const bool old_gamemode = UISettings::values.enable_gamemode.GetValue();
#ifdef __unix__
    const bool old_force_x11 = UISettings::values.gui_force_x11.GetValue();
#endif

    // Taken before the dialog exists: building its Controls tab already writes the players.
    // The copy in use, not the global one: during a game the tab shows the live players
    // (from the emulated controllers), while the global copy still holds the routing from
    // before the game booted - a device comparison against that would invent swaps.
    const auto players_before = Settings::values.players.GetValue();
    Settings::SetConfiguringGlobal(true);
    ConfigureDialog configure_dialog(this, hotkey_registry, input_subsystem.get(),
                                     vk_device_records, *QtCommon::system,
                                     !multiplayer_state->IsHostingPublicRoom());
    connect(&configure_dialog, &ConfigureDialog::LanguageChanged, this,
            &MainWindow::OnLanguageChanged);
    connect(&configure_dialog, &ConfigureDialog::ExternalContentDirsChanged, this,
            &MainWindow::OnGameListRefresh);

    const auto result = configure_dialog.exec();
    if (result != QDialog::Accepted && !UISettings::values.configuration_applied &&
        !UISettings::values.reset_to_defaults) {
        // Runs if the user hit Cancel or closed the window, and did not ever press the Apply button
        // or `Reset to Defaults` button
        return;
    } else if (result == QDialog::Accepted) {
        // Only apply new changes if user hit Okay
        // This is here to avoid applying changes if the user hit Apply, made some changes, then hit
        // Cancel
        configure_dialog.ApplyConfiguration();
    } else if (UISettings::values.reset_to_defaults) {
        LOG_INFO(Frontend, "Resetting all settings to defaults");
        if (!Common::FS::RemoveFile(config->GetConfigFilePath())) {
            LOG_WARNING(Frontend, "Failed to remove configuration file");
        }
        if (!Common::FS::RemoveDirContentsRecursively(
                Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "custom")) {
            LOG_WARNING(Frontend, "Failed to remove custom configuration files");
        }
        if (!Common::FS::RemoveDirRecursively(
                Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "game_list")) {
            LOG_WARNING(Frontend, "Failed to remove game metadata cache files");
        }

        // Explicitly save the game directories, since reinitializing config does not explicitly do
        // so.
        QVector<UISettings::GameDir> old_game_dirs = std::move(UISettings::values.game_dirs);
        QVector<u64> old_favorited_ids = std::move(UISettings::values.favorited_ids);

        Settings::values.disabled_addons.clear();

        RestoreConsoleProfile();
        config = std::make_unique<QtConfig>();
        UISettings::values.reset_to_defaults = false;

        UISettings::values.game_dirs = std::move(old_game_dirs);
        UISettings::values.favorited_ids = std::move(old_favorited_ids);

        InitializeRecentFileMenuActions();

        SetDefaultUIGeometry();
        RestoreUIState();
    }
    MergeConsoleProfileEdits(players_before);
    ui->action_Show_Controller_Overlay->setChecked(
        Settings::values.console_debug_overlay.GetValue());
    InitializeHotkeys();

    if (UISettings::values.theme != old_theme) {
        UpdateUITheme();
    }
    if (UISettings::values.enable_discord_presence.GetValue() != old_discord_presence) {
        SetDiscordEnabled(UISettings::values.enable_discord_presence.GetValue());
    }
    if (UISettings::values.enable_gamemode.GetValue() != old_gamemode) {
        SetGamemodeEnabled(UISettings::values.enable_gamemode.GetValue());
    }
#ifdef __unix__
    if (UISettings::values.gui_force_x11.GetValue() != old_force_x11) {
        GraphicsBackend::SetForceX11(UISettings::values.gui_force_x11.GetValue());
    }
#endif

    if (!multiplayer_state->IsHostingPublicRoom()) {
        multiplayer_state->UpdateCredentials();
    }

    emit UpdateThemedIcons();

    const auto reload = UISettings::values.is_game_list_reload_pending.exchange(false);
    if (reload || Settings::values.language_index.GetValue() != old_language_index) {
        game_list->PopulateAsync(UISettings::values.game_dirs);
    }

    UISettings::values.configuration_applied = false;

    SaveAllConfigValues();

    if ((UISettings::values.hide_mouse || Settings::values.mouse_panning) && emulation_running) {
        render_window->installEventFilter(render_window);
        render_window->setAttribute(Qt::WA_Hover, true);
    } else {
        render_window->removeEventFilter(render_window);
        render_window->setAttribute(Qt::WA_Hover, false);
    }

    if (UISettings::values.hide_mouse) {
        mouse_hide_timer.start();
    }

    // Restart camera config
    if (emulation_running) {
        render_window->FinalizeCamera();
        render_window->InitializeCamera();
    }

    if (!UISettings::values.has_broken_vulkan) {
        renderer_status_button->setEnabled(!emulation_running);
    }

    UpdateStatusButtons();
    controller_dialog->refreshConfiguration();
    QtCommon::system->ApplySettings();
}

void MainWindow::OnConfigureTas() {
    ConfigureTasDialog dialog(this);
    const auto result = dialog.exec();

    if (result != QDialog::Accepted && !UISettings::values.configuration_applied) {
        Settings::RestoreGlobalState(QtCommon::system->IsPoweredOn());
        return;
    } else if (result == QDialog::Accepted) {
        dialog.ApplyConfiguration();
        OnSaveConfig();
    }
}

void MainWindow::OnTasStartStop() {
    if (!emulation_running) {
        return;
    }

    // Disable system buttons to prevent TAS from executing a hotkey
    auto* controller =
        QtCommon::system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller->ResetSystemButtons();

    input_subsystem->GetTas()->StartStop();
    OnTasStateChanged();
}

void MainWindow::OnTasRecord() {
    if (!emulation_running) {
        return;
    }
    if (is_tas_recording_dialog_active) {
        return;
    }

    // Disable system buttons to prevent TAS from recording a hotkey
    auto* controller =
        QtCommon::system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller->ResetSystemButtons();

    const bool is_recording = input_subsystem->GetTas()->Record();
    if (!is_recording) {
        if (Settings::values.tas_show_recording_dialog.GetValue()) {
            is_tas_recording_dialog_active = true;

            bool answer = question(this, tr("TAS Recording"), tr("Overwrite file of player 1?"),
                                   QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

            input_subsystem->GetTas()->SaveRecording(answer);
            is_tas_recording_dialog_active = false;
        } else {
            input_subsystem->GetTas()->SaveRecording(true);
        }
    }
    OnTasStateChanged();
}

void MainWindow::OnTasReset() {
    input_subsystem->GetTas()->Reset();
}

void MainWindow::OnToggleDockedMode() {
    const bool is_docked = Settings::IsDockedMode();
    auto* player_1 =
        QtCommon::system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    auto* handheld =
        QtCommon::system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Handheld);

    if (!is_docked && handheld->IsConnected()) {
        QMessageBox::warning(this, tr("Invalid config detected"),
                             tr("Handheld controller can't be used on docked mode. Pro "
                                "controller will be selected."));
        handheld->Disconnect();
        player_1->SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
        player_1->Connect();
        controller_dialog->refreshConfiguration();
    }

    Settings::values.use_docked_mode.SetValue(is_docked ? Settings::ConsoleMode::Handheld
                                                        : Settings::ConsoleMode::Docked);
    UpdateDockedButton();
    OnDockedModeChanged(is_docked, !is_docked, *QtCommon::system);
}

void MainWindow::OnToggleGpuAccuracy() {
    switch (Settings::values.gpu_accuracy.GetValue()) {
    case Settings::GpuAccuracy::Low:
        Settings::values.gpu_accuracy.SetValue(Settings::GpuAccuracy::High);
        break;
    case Settings::GpuAccuracy::High:
        Settings::values.gpu_accuracy.SetValue(Settings::GpuAccuracy::Low);
        break;
    }

    QtCommon::system->ApplySettings();
    UpdateGPUAccuracyButton();
}

void MainWindow::OnMute() {
    Settings::values.audio_muted = !Settings::values.audio_muted;
    UpdateVolumeUI();
}

void MainWindow::OnDecreaseVolume() {
    Settings::values.audio_muted = false;
    const auto current_volume = static_cast<s32>(Settings::values.volume.GetValue());
    int step = 5;
    if (current_volume <= 30) {
        step = 2;
    }
    if (current_volume <= 6) {
        step = 1;
    }
    Settings::values.volume.SetValue((std::max)(current_volume - step, 0));
    UpdateVolumeUI();
}

void MainWindow::OnIncreaseVolume() {
    Settings::values.audio_muted = false;
    const auto current_volume = static_cast<s32>(Settings::values.volume.GetValue());
    int step = 5;
    if (current_volume < 30) {
        step = 2;
    }
    if (current_volume < 6) {
        step = 1;
    }
    Settings::values.volume.SetValue(current_volume + step);
    UpdateVolumeUI();
}

void MainWindow::OnToggleAdaptingFilter() {
    auto filter = Settings::values.scaling_filter.GetValue();
    filter = Settings::ScalingFilter(u32(filter) + 1);
    if (u32(filter) > u32(Settings::EnumMetadata<Settings::ScalingFilter>::GetLast()))
        filter = Settings::EnumMetadata<Settings::ScalingFilter>::GetFirst();
    Settings::values.scaling_filter.SetValue(filter);
    filter_status_button->setChecked(true);
    UpdateFilterText();
}

void MainWindow::OnToggleGraphicsAPI() {
    auto api = Settings::values.renderer_backend.GetValue();
    switch (api) {
#ifdef HAS_OPENGL
    case Settings::RendererBackend::Vulkan:
        api = Settings::RendererBackend::OpenGL_GLSL;
        break;
    case Settings::RendererBackend::OpenGL_GLSL:
        api = Settings::RendererBackend::OpenGL_GLASM;
        break;
    case Settings::RendererBackend::OpenGL_GLASM:
        api = Settings::RendererBackend::OpenGL_SPIRV;
        break;
    case Settings::RendererBackend::OpenGL_SPIRV:
        api = Settings::RendererBackend::Null;
        break;
#else
    case Settings::RendererBackend::Vulkan:
        api = Settings::RendererBackend::Null;
        break;
#endif
    case Settings::RendererBackend::Null:
        api = Settings::RendererBackend::Vulkan;
        break;
    default:
        break;
    }
    Settings::values.renderer_backend.SetValue(api);
    renderer_status_button->setChecked(api == Settings::RendererBackend::Vulkan);
    UpdateAPIText();
}

void MainWindow::OnConfigurePerGame() {
    const u64 title_id = QtCommon::system->GetApplicationProcessProgramID();
    OpenPerGameConfiguration(title_id, current_game_path.toStdString());
}

void MainWindow::OpenPerGameConfiguration(u64 title_id, const std::string& file_name) {
    const auto v_file = Core::GetGameFileFromPath(QtCommon::vfs, file_name);

    Settings::SetConfiguringGlobal(false);
    ConfigurePerGame dialog(this, title_id, file_name, vk_device_records, *QtCommon::system);
    dialog.LoadFromFile(v_file);

    const auto result = dialog.exec();

    if (result != QDialog::Accepted && !UISettings::values.configuration_applied) {
        Settings::RestoreGlobalState(QtCommon::system->IsPoweredOn());
        return;
    } else if (result == QDialog::Accepted) {
        dialog.ApplyConfiguration();
    }

    const auto reload = UISettings::values.is_game_list_reload_pending.exchange(false);
    if (reload) {
        OnGameListRefresh();
    }

    // Do not cause the global config to write local settings into the config file
    const bool is_powered_on = QtCommon::system->IsPoweredOn();
    Settings::RestoreGlobalState(is_powered_on);
    QtCommon::system->HIDCore().ReloadInputDevices();

    UISettings::values.configuration_applied = false;

    if (!is_powered_on) {
        SaveAllConfigValues();
    }
}

void MainWindow::OnLoadAmiibo() {
    if (QtCommon::emu_thread == nullptr || !QtCommon::emu_thread->IsRunning() ||
        is_amiibo_file_select_active)
        return;

    auto* virtual_amiibo = input_subsystem->GetVirtualAmiibo();

    // Remove amiibo if one is connected
    if (virtual_amiibo->GetCurrentState() == InputCommon::VirtualAmiibo::State::TagNearby) {
        virtual_amiibo->CloseAmiibo();
        QMessageBox::warning(this, tr("Amiibo"), tr("The current amiibo has been removed"));
        return;
    }

    if (virtual_amiibo->GetCurrentState() != InputCommon::VirtualAmiibo::State::WaitingForAmiibo) {
        QMessageBox::warning(this, tr("Error"), tr("The current game is not looking for amiibos"));
        return;
    }

    is_amiibo_file_select_active = true;
    const QString extensions{QStringLiteral("*.bin")};
    const QString file_filter = tr("Amiibo File (%1);; All Files (*.*)").arg(extensions);
    const QString filename = QFileDialog::getOpenFileName(this, tr("Load Amiibo"), {}, file_filter);
    is_amiibo_file_select_active = false;

    if (filename.isEmpty()) {
        return;
    }

    LoadAmiibo(filename);
}

// TODO(crueter): does this need to be ported to QML?
bool MainWindow::question(QWidget* parent, const QString& title, const QString& text,
                          QMessageBox::StandardButtons buttons,
                          QMessageBox::StandardButton defaultButton) {
    QMessageBox* box_dialog = new QMessageBox(parent);
    box_dialog->setWindowTitle(title);
    box_dialog->setText(text);
    box_dialog->setStandardButtons(buttons);
    box_dialog->setDefaultButton(defaultButton);

    ControllerNavigation* controller_navigation =
        new ControllerNavigation(QtCommon::system->HIDCore(), box_dialog);
    connect(controller_navigation, &ControllerNavigation::TriggerKeyboardEvent,
            [box_dialog](Qt::Key key) {
                QKeyEvent* event = new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier);
                QCoreApplication::postEvent(box_dialog, event);
            });
    int res = box_dialog->exec();

    controller_navigation->UnloadController();
    return res == QMessageBox::Yes;
}

void MainWindow::LoadAmiibo(const QString& filename) {
    auto* virtual_amiibo = input_subsystem->GetVirtualAmiibo();
    const QString title = tr("Error loading Amiibo data");
    // Remove amiibo if one is connected
    if (virtual_amiibo->GetCurrentState() == InputCommon::VirtualAmiibo::State::TagNearby) {
        virtual_amiibo->CloseAmiibo();
        QMessageBox::warning(this, tr("Amiibo"), tr("The current amiibo has been removed"));
        return;
    }

    switch (virtual_amiibo->LoadAmiibo(filename.toStdString())) {
    case InputCommon::VirtualAmiibo::Info::NotAnAmiibo:
        QMessageBox::warning(this, title, tr("The selected file is not a valid amiibo"));
        break;
    case InputCommon::VirtualAmiibo::Info::UnableToLoad:
        QMessageBox::warning(this, title, tr("The selected file is already on use"));
        break;
    case InputCommon::VirtualAmiibo::Info::WrongDeviceState:
        QMessageBox::warning(this, title, tr("The current game is not looking for amiibos"));
        break;
    case InputCommon::VirtualAmiibo::Info::Unknown:
        QMessageBox::warning(this, title, tr("An unknown error occurred"));
        break;
    default:
        break;
    }
}

void MainWindow::OnOpenRootDataFolder() {
    QtCommon::Game::OpenRootDataFolder();
}

void MainWindow::OnOpenNANDFolder() {
    QtCommon::Game::OpenNANDFolder();
}

void MainWindow::OnOpenSDMCFolder() {
    QtCommon::Game::OpenSDMCFolder();
}

void MainWindow::OnOpenModFolder() {
    QtCommon::Game::OpenModFolder();
}

void MainWindow::OnOpenLogFolder() {
    QtCommon::Game::OpenLogFolder();
}

void MainWindow::OnVerifyInstalledContents() {
    QtCommon::Content::VerifyInstalledContents();
}

void MainWindow::OnInstallFirmware() {
    QtCommon::Content::InstallFirmware();
    OnCheckFirmwareDecryption();
}

void MainWindow::OnInstallFirmwareFromZIP() {
    QtCommon::Content::InstallFirmwareZip();
    OnCheckFirmwareDecryption();
}

// TODO(crueter): QtCommon this: game list populate can be a signal?
void MainWindow::OnInstallDecryptionKeys() {
    // Don't do this while emulation is running.
    if (QtCommon::emu_thread != nullptr && QtCommon::emu_thread->IsRunning())
        return;

    QtCommon::Content::InstallKeys();

    game_list->PopulateAsync(UISettings::values.game_dirs);
    OnCheckFirmwareDecryption();
}

void MainWindow::OnAbout() {
    AboutDialog aboutDialog(this);
    aboutDialog.exec();
}

void MainWindow::OnEdenDependencies() {
    DepsDialog depsDialog(this);
    depsDialog.exec();
}

void MainWindow::OnDataDialog() {
    DataDialog dataDialog(this);
    dataDialog.exec();

    // refresh stuff in case it was cleared
    OnGameListRefresh();
}

void MainWindow::OnToggleFilterBar() {
    game_list->SetFilterVisible(ui->action_Show_Filter_Bar->isChecked());
    if (ui->action_Show_Filter_Bar->isChecked())
        game_list->SetFilterFocus();
    else
        game_list->ClearFilter();
}

void MainWindow::OnToggleStatusBar() {
    statusBar()->setVisible(ui->action_Show_Status_Bar->isChecked());
}

void MainWindow::OnTogglePerfOverlay() {
    if (perf_overlay)
        perf_overlay->setVisible(ui->action_Show_Performance_Overlay->isChecked());
}

#ifdef HAS_RESHADE
void MainWindow::OnPostProcessingShaders() {
    if (post_processing_dialog == nullptr) {
        post_processing_dialog = new ConfigurePostProcessing(this);
        connect(post_processing_dialog, &QDialog::finished, post_processing_dialog, [this]() {
            post_processing_dialog->deleteLater();
            post_processing_dialog = nullptr;
            UpdatePostShaderText();
        });
    }

    post_processing_dialog->show();
    post_processing_dialog->raise();
    post_processing_dialog->activateWindow();
}
#endif

void MainWindow::OnGameListRefresh() {
    // Resets metadata cache and reloads
    QtCommon::Game::ResetMetadata(false);
    game_list->RefreshGameDirectory();
    game_list->RefreshExternalContent();
    SetFirmwareVersion();
}

void MainWindow::LaunchFirmwareApplet(u64 raw_program_id,
                                      std::optional<Service::NFP::CabinetMode> cabinet_mode) {
    auto const program_id = Service::AM::AppletProgramId(raw_program_id);
    auto result = FirmwareManager::VerifyFirmware(*QtCommon::system.get());
    using namespace QtCommon::StringLookup;
    switch (result) {
    case FirmwareManager::ErrorFirmwareMissing:
        QMessageBox::warning(this, tr("No firmware available"),
                             Lookup(FwCheckErrorFirmwareMissing));
        return;
    case FirmwareManager::ErrorFirmwareCorrupted:
        QMessageBox::warning(this, tr("Firmware Corrupted"), Lookup(FwCheckErrorFirmwareCorrupted));
        return;
    default:
        break;
    }
    auto bis_system = QtCommon::system->GetFileSystemController().GetSystemNANDContents();
    if (auto applet_nca =
            bis_system->GetEntry(u64(program_id), FileSys::ContentRecordType::Program);
        applet_nca) {
        if (auto const applet_id =
                [program_id] {
                    using namespace Service::AM;
                    switch (program_id) {
                    case AppletProgramId::OverlayDisplay:
                        return AppletId::OverlayDisplay;
                    case AppletProgramId::QLaunch:
                        return AppletId::QLaunch;
                    case AppletProgramId::Starter:
                        return AppletId::Starter;
                    case AppletProgramId::Auth:
                        return AppletId::Auth;
                    case AppletProgramId::Cabinet:
                        return AppletId::Cabinet;
                    case AppletProgramId::Controller:
                        return AppletId::Controller;
                    case AppletProgramId::DataErase:
                        return AppletId::DataErase;
                    case AppletProgramId::Error:
                        return AppletId::Error;
                    case AppletProgramId::NetConnect:
                        return AppletId::NetConnect;
                    case AppletProgramId::ProfileSelect:
                        return AppletId::ProfileSelect;
                    case AppletProgramId::SoftwareKeyboard:
                        return AppletId::SoftwareKeyboard;
                    case AppletProgramId::MiiEdit:
                        return AppletId::MiiEdit;
                    case AppletProgramId::Web:
                        return AppletId::Web;
                    case AppletProgramId::Shop:
                        return AppletId::Shop;
                    case AppletProgramId::PhotoViewer:
                        return AppletId::PhotoViewer;
                    case AppletProgramId::Settings:
                        return AppletId::Settings;
                    case AppletProgramId::OfflineWeb:
                        return AppletId::OfflineWeb;
                    case AppletProgramId::LoginShare:
                        return AppletId::LoginShare;
                    case AppletProgramId::WebAuth:
                        return AppletId::WebAuth;
                    case AppletProgramId::MyPage:
                        return AppletId::MyPage;
                    default:
                        return AppletId::None;
                    }
                }();
            applet_id != Service::AM::AppletId::None) {
            QtCommon::system->GetFrontendAppletHolder().SetCurrentAppletId(applet_id);
            if (cabinet_mode)
                QtCommon::system->GetFrontendAppletHolder().SetCabinetMode(*cabinet_mode);
            // ?
            auto const filename = QString::fromStdString((applet_nca->GetFullPath()));
            UISettings::values.roms_path = QFileInfo(filename).path().toStdString();
            BootGame(filename, LibraryAppletParameters(u64(program_id), applet_id));
        } else {
            QMessageBox::warning(this, tr("Unknown applet"),
                                 tr("Applet doesn't map to a known value."));
        }
    } else {
        QMessageBox::warning(this, tr("Record not found"),
                             tr("Applet not found. Please reinstall firmware."));
    }
}

void MainWindow::OnCreateHomeMenuDesktopShortcut() {
    QtCommon::Game::CreateHomeMenuShortcut(QtCommon::Game::ShortcutTarget::Desktop);
}

void MainWindow::OnCreateHomeMenuApplicationMenuShortcut() {
    QtCommon::Game::CreateHomeMenuShortcut(QtCommon::Game::ShortcutTarget::Applications);
}

void MainWindow::OnCaptureScreenshot() {
    if (QtCommon::emu_thread == nullptr || !QtCommon::emu_thread->IsRunning())
        return;

    const u64 title_id = QtCommon::system->GetApplicationProcessProgramID();
    const auto screenshot_path =
        QString::fromStdString(Common::FS::GetEdenPathString(Common::FS::EdenPath::ScreenshotsDir));
    const auto date =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_hh-mm-ss-zzz"));
    QString filename = QStringLiteral("%1/%2_%3.png")
                           .arg(screenshot_path)
                           .arg(title_id, 16, 16, QLatin1Char{'0'})
                           .arg(date);

    if (!Common::FS::CreateDir(screenshot_path.toStdString()))
        return;

#ifdef _WIN32
    if (UISettings::values.enable_screenshot_save_as) {
        OnPauseGame();
        filename = QFileDialog::getSaveFileName(this, tr("Capture Screenshot"), filename,
                                                tr("PNG Image (*.png)"));
        OnStartGame();
        if (filename.isEmpty()) {
            return;
        }
    }
#endif
    render_window->CaptureScreenshot(filename);
}

#ifdef ENABLE_UPDATE_CHECKER
void MainWindow::OnEmulatorUpdateAvailable() {
    std::optional<Common::Net::Release> version = update_future.result();
    if (!version)
        return;

    UpdateDialog dialog(version.value(), this);
    dialog.exec();
}
#endif

void MainWindow::OnApplicationChanged(u64 program_id) {
    if (!emulation_running || program_id == 0)
        return;

    std::string title_name;
    std::string title_version;

    const FileSys::PatchManager pm(program_id, QtCommon::system->GetFileSystemController(),
                                   QtCommon::system->GetContentProvider());
    if (const auto metadata = pm.GetControlMetadata(); metadata.first != nullptr) {
        title_version = metadata.first->GetVersionString();
        title_name = metadata.first->GetApplicationName();
    }
    if (title_name.empty()) {
        title_name = fmt::format("{:016X}", program_id);
    }

    if (const auto* process = QtCommon::system->Kernel().ApplicationProcess(); process != nullptr) {
        const auto instruction_set_suffix = process->Is64Bit() ? tr("(64-bit)") : tr("(32-bit)");
        title_name =
            tr("%1 %2", "%1 is the title name. %2 indicates if the title is 64-bit or 32-bit")
                .arg(QString::fromStdString(title_name), instruction_set_suffix)
                .toStdString();
    }

    LOG_INFO(Frontend, "Now running: {:016X} | {} | {}", program_id, title_name, title_version);

    UpdateWindowTitle(title_name, title_version,
                      QtCommon::system->GPU().Renderer().GetDeviceVendor());

    // Switch to record playtime for the running program
    if (play_time_manager)
        play_time_manager->SetProgramId(program_id);
}

void MainWindow::UpdateWindowTitle(std::string_view title_name, std::string_view title_version,
                                   std::string_view gpu_vendor) {
    static const std::string build_id = std::string{Common::g_build_id};
    static const std::string yuzu_title =
        fmt::format("{} | {} | {}", std::string{Common::g_build_name},
                    std::string{Common::g_build_version}, std::string{Common::g_compiler_id});

    const auto override_title =
        fmt::format(fmt::runtime(std::string(Common::g_title_bar_format_idle)), build_id);
    const auto window_title = override_title.empty() ? yuzu_title : override_title;

    if (title_name.empty()) {
        setWindowTitle(QString::fromStdString(window_title));
    } else {
        const auto run_title = [window_title, title_name, title_version, gpu_vendor]() {
            if (title_version.empty()) {
                return fmt::format("{} | {} | {}", window_title, title_name, gpu_vendor);
            }
            return fmt::format("{} | {} | {} | {}", window_title, title_name, title_version,
                               gpu_vendor);
        }();
        setWindowTitle(QString::fromStdString(run_title));
    }
}

std::string MainWindow::CreateTASFramesString(
    std::array<size_t, InputCommon::TasInput::PLAYER_NUMBER> frames) const {
    std::string string = "";
    size_t maxPlayerIndex = 0;
    for (size_t i = 0; i < frames.size(); i++) {
        if (frames[i] != 0) {
            if (maxPlayerIndex != 0)
                string += ", ";
            while (maxPlayerIndex++ != i)
                string += "0, ";
            string += std::to_string(frames[i]);
        }
    }
    return string;
}

QString MainWindow::GetTasStateDescription() const {
    auto [tas_status, current_tas_frame, total_tas_frames] = input_subsystem->GetTas()->GetStatus();
    std::string tas_frames_string = CreateTASFramesString(total_tas_frames);
    switch (tas_status) {
    case InputCommon::TasInput::TasState::Running:
        return tr("TAS state: Running %1/%2")
            .arg(current_tas_frame)
            .arg(QString::fromStdString(tas_frames_string));
    case InputCommon::TasInput::TasState::Recording:
        return tr("TAS state: Recording %1").arg(total_tas_frames[0]);
    case InputCommon::TasInput::TasState::Stopped:
        return tr("TAS state: Idle %1/%2")
            .arg(current_tas_frame)
            .arg(QString::fromStdString(tas_frames_string));
    default:
        return tr("TAS State: Invalid");
    }
}

void MainWindow::OnTasStateChanged() {
    bool is_running = false;
    bool is_recording = false;
    if (emulation_running) {
        const InputCommon::TasInput::TasState tas_status =
            std::get<0>(input_subsystem->GetTas()->GetStatus());
        is_running = tas_status == InputCommon::TasInput::TasState::Running;
        is_recording = tas_status == InputCommon::TasInput::TasState::Recording;
    }

    ui->action_TAS_Start->setText(is_running ? tr("&Stop Running") : tr("&Start"));
    ui->action_TAS_Record->setText(is_recording ? tr("Stop R&ecording") : tr("R&ecord"));

    ui->action_TAS_Start->setEnabled(emulation_running);
    ui->action_TAS_Record->setEnabled(emulation_running);
    ui->action_TAS_Reset->setEnabled(emulation_running);
}

void MainWindow::UpdateStatusBar() {
    if (QtCommon::emu_thread == nullptr || !QtCommon::system->IsPoweredOn()) {
        status_bar_update_timer.stop();
        return;
    }

    if (Settings::values.tas_enable)
        tas_label->setText(GetTasStateDescription());
    else
        tas_label->clear();

    auto results = QtCommon::system->GetAndResetPerfStats();
    auto& shader_notify = QtCommon::system->GPU().ShaderNotify();
    const int shaders_building = shader_notify.ShadersBuilding();

    emit statsUpdated(results, shader_notify);

    if (shaders_building > 0) {
        shader_building_label->setText(tr("Building: %n shader(s)", "", shaders_building));
        shader_building_label->setVisible(true);
    } else {
        shader_building_label->setVisible(false);
    }

    const auto res_info = Settings::values.resolution_info;
    const auto res_scale = res_info.up_factor;
    res_scale_label->setText(
        tr("Scale: %1x", "%1 is the resolution scaling factor").arg(res_scale));

    if (Settings::values.use_speed_limit.GetValue()) {
        emu_speed_label->setText(tr("Speed: %1% / %2%")
                                     .arg(results.emulation_speed * 100.0, 0, 'f', 0)
                                     .arg(Settings::SpeedLimit()));
    } else {
        emu_speed_label->setText(tr("Speed: %1%").arg(results.emulation_speed * 100.0, 0, 'f', 0));
    }

    QString fpsText = tr("Game: %1 FPS").arg(std::round(results.average_game_fps), 0, 'f', 0);
    if (!m_fpsSuffix.isEmpty())
        fpsText = fpsText % QStringLiteral(" (%1)").arg(m_fpsSuffix);

    game_fps_label->setText(fpsText);

    emu_frametime_label->setText(tr("Frame: %1 ms").arg(results.frametime * 1000.0, 0, 'f', 2));

    res_scale_label->setVisible(true);
    emu_speed_label->setVisible(!Settings::values.use_multi_core.GetValue());
    game_fps_label->setVisible(true);
    emu_frametime_label->setVisible(true);
    firmware_label->setVisible(false);
}

void MainWindow::UpdateGPUAccuracyButton() {
    const auto gpu_accuracy = Settings::values.gpu_accuracy.GetValue();
    const auto gpu_accuracy_text =
        ConfigurationShared::gpu_accuracy_texts_map.find(gpu_accuracy)->second;
    gpu_accuracy_button->setText(gpu_accuracy_text.toUpper());
    gpu_accuracy_button->setChecked(gpu_accuracy != Settings::GpuAccuracy::Low);
}

void MainWindow::UpdateDockedButton() {
    const auto console_mode = Settings::values.use_docked_mode.GetValue();
    dock_status_button->setChecked(Settings::IsDockedMode());
    dock_status_button->setText(
        ConfigurationShared::use_docked_mode_texts_map.find(console_mode)->second.toUpper());
}

void MainWindow::UpdateAPIText() {
    const auto api = Settings::values.renderer_backend.GetValue();
    const auto renderer_status_text =
        ConfigurationShared::renderer_backend_texts_map.find(api)->second;
    renderer_status_button->setText(renderer_status_text.toUpper());
}

void MainWindow::UpdateFilterText() {
    const auto filter = Settings::values.scaling_filter.GetValue();
    const auto filter_text = ConfigurationShared::scaling_filter_texts_map.find(filter)->second;
    filter_status_button->setText(filter_text.toUpper());
}

void MainWindow::UpdateAAText() {
    const auto aa_mode = Settings::values.anti_aliasing.GetValue();
    const auto aa_text = ConfigurationShared::anti_aliasing_texts_map.find(aa_mode)->second;
    aa_status_button->setText(aa_mode == Settings::AntiAliasing::None
                                  ? QStringLiteral(QT_TRANSLATE_NOOP("MainWindow", "NO AA"))
                                  : aa_text.toUpper());
}

#ifdef HAS_RESHADE
void MainWindow::UpdatePostShaderText() {
    const bool enabled = Settings::values.post_shader_enabled.GetValue();
    post_shader_status_button->setChecked(enabled);

    if (!enabled) {
        post_shader_status_button->setText(tr("NO FX"));
        return;
    }

    const std::string preset = VideoCore::GetActiveFxPreset();
    if (preset.empty()) {
        post_shader_status_button->setText(tr("FX"));
        return;
    }

    post_shader_status_button->setText(QString::fromStdString(preset).toUpper());
}
#endif

void MainWindow::UpdateVolumeUI() {
    const auto volume_value = static_cast<int>(Settings::values.volume.GetValue());
    volume_slider->setValue(volume_value);
    if (Settings::values.audio_muted) {
        volume_button->setChecked(false);
        volume_button->setText(tr("VOLUME: MUTE"));
    } else {
        volume_button->setChecked(true);
        volume_button->setText(tr("VOLUME: %1%", "Volume percentage (e.g. 50%)").arg(volume_value));
    }
}

void MainWindow::UpdateStatusButtons() {
    renderer_status_button->setChecked(Settings::values.renderer_backend.GetValue() ==
                                       Settings::RendererBackend::Vulkan);
    UpdateAPIText();
    UpdateGPUAccuracyButton();
    UpdateDockedButton();
    UpdateFilterText();
    UpdateAAText();
    UpdateVolumeUI();
}

// TODO(crueter): Use this for game list stuff
void MainWindow::UpdateUISettings() {
    if (!ui->action_Fullscreen->isChecked()) {
        UISettings::values.geometry = saveGeometry();
        UISettings::values.renderwindow_geometry = render_window->saveGeometry();
    }
    UISettings::values.state = saveState();
    UISettings::values.single_window_mode = ui->action_Single_Window_Mode->isChecked();
    UISettings::values.fullscreen = ui->action_Fullscreen->isChecked();
    UISettings::values.show_filter_bar = ui->action_Show_Filter_Bar->isChecked();
    UISettings::values.show_status_bar = ui->action_Show_Status_Bar->isChecked();
    UISettings::values.show_perf_overlay = ui->action_Show_Performance_Overlay->isChecked();

    UISettings::values.first_start = false;

    Settings::values.enable_overlay = ui->action_Enable_Overlay_Applet->isChecked();
}

void MainWindow::UpdateInputDrivers() {
    if (!input_subsystem)
        return;
    input_subsystem->PumpEvents();
}

void MainWindow::HideMouseCursor() {
    if (QtCommon::emu_thread == nullptr && UISettings::values.hide_mouse) {
        mouse_hide_timer.stop();
        ShowMouseCursor();
        return;
    }
    render_window->setCursor(QCursor(Qt::BlankCursor));
}

void MainWindow::ShowMouseCursor() {
    render_window->unsetCursor();
    if (QtCommon::emu_thread != nullptr && UISettings::values.hide_mouse)
        mouse_hide_timer.start();
}

void MainWindow::OnMouseActivity() {
    if (!Settings::values.mouse_panning) {
        ShowMouseCursor();
    }
}

void MainWindow::OnCheckFirmwareDecryption() {
    if (!ContentManager::AreKeysPresent()) {
        const auto res = QtCommon::Frontend::Warning(
            tr("Derivation Components Missing"),
            tr("Decryption keys are missing. Install them now?"),
            QtCommon::Frontend::StandardButton::Yes | QtCommon::Frontend::StandardButton::No);

        if (res == QtCommon::Frontend::StandardButton::Yes)
            OnInstallDecryptionKeys();
    }

    SetFirmwareVersion();
    UpdateMenuState();
}

#ifdef __unix__
void MainWindow::OnCheckGraphicsBackend() {
    const QString platformName = QGuiApplication::platformName();
    const QByteArray qtPlatform = qgetenv("QT_QPA_PLATFORM");

    if (platformName == QStringLiteral("xcb") || qtPlatform == "xcb")
        return;

    const bool isWayland =
        platformName.startsWith(QStringLiteral("wayland"), Qt::CaseInsensitive) ||
        qtPlatform.startsWith("wayland");
    if (!isWayland)
        return;

    const bool currently_hidden = UISettings::values.gui_hide_backend_warning.GetValue();
    if (currently_hidden)
        return;

    QMessageBox msgbox(this);
    msgbox.setWindowTitle(tr("Wayland Detected!"));
    msgbox.setText(
        tr("Wayland is known to have significant performance issues and mysterious bugs.\n"
           "It's recommended to use X11 instead.\n\n"
           "Would you like to force it for future launches?"));
    msgbox.setIcon(QMessageBox::Warning);

    QPushButton* okButton = msgbox.addButton(tr("Use X11"), QMessageBox::AcceptRole);
    msgbox.addButton(tr("Continue with Wayland"), QMessageBox::RejectRole);
    msgbox.setDefaultButton(okButton);

    QCheckBox* cb = new QCheckBox(tr("Don't show again"), &msgbox);
    cb->setChecked(currently_hidden);
    msgbox.setCheckBox(cb);

    msgbox.exec();

    const bool hide = cb->isChecked();
    if (hide != currently_hidden) {
        UISettings::values.gui_hide_backend_warning.SetValue(hide);
    }

    if (msgbox.clickedButton() == okButton) {
        UISettings::values.gui_force_x11.SetValue(true);
        GraphicsBackend::SetForceX11(true);
        QMessageBox::information(this, tr("Restart Required"),
                                 tr("Restart Eden to apply the X11 backend."));
    }
}
#endif

bool MainWindow::CheckFirmwarePresence() {
    return FirmwareManager::CheckFirmwarePresence(*QtCommon::system.get());
}

void MainWindow::SetFirmwareVersion() {
    const auto pair = FirmwareManager::GetFirmwareVersion(*QtCommon::system.get());
    const auto firmware_data = pair.first;
    const auto result = pair.second;

    if (result.IsError() || !CheckFirmwarePresence()) {
        LOG_INFO(Frontend, "Installed firmware: No firmware available");
        ui->menu_Applets->setEnabled(false);
        ui->menu_Create_Shortcuts->setEnabled(false);
        firmware_label->setVisible(false);
        return;
    }

    firmware_label->setVisible(true);
    ui->menu_Applets->setEnabled(true);
    ui->menu_Create_Shortcuts->setEnabled(true);

    const std::string display_version(firmware_data.display_version.data());
    const std::string display_title(firmware_data.display_title.data());

    LOG_INFO(Frontend, "Installed firmware: {}", display_version);

    firmware_label->setText(QString::fromStdString(display_version));
    firmware_label->setToolTip(QString::fromStdString(display_title));
}

void MainWindow::SetFPSSuffix() {
    switch (Settings::values.current_speed_mode.GetValue()) {
    case Settings::SpeedMode::Slow:
        m_fpsSuffix = tr("Slow");
        break;
    case Settings::SpeedMode::Turbo:
        m_fpsSuffix = tr("Turbo");
        break;
    case Settings::SpeedMode::Standard:
        const bool limited = Settings::values.use_speed_limit.GetValue();
        m_fpsSuffix = limited ? QString{} : tr("Unlocked");
        break;
    }
}

bool MainWindow::SelectRomFSDumpTarget(const FileSys::ContentProvider& installed, u64 program_id,
                                       u64* selected_title_id, u8* selected_content_record_type) {
    using ContentInfo = std::tuple<u64, FileSys::TitleType, FileSys::ContentRecordType>;
    boost::container::flat_set<ContentInfo> available_title_ids;

    const auto RetrieveEntries = [&](FileSys::TitleType title_type,
                                     FileSys::ContentRecordType record_type) {
        const auto entries = installed.ListEntriesFilter(title_type, record_type);
        for (const auto& entry : entries) {
            if (FileSys::GetBaseTitleID(entry.title_id) == program_id &&
                installed.GetEntry(entry)->GetStatus() == Loader::ResultStatus::Success) {
                available_title_ids.insert({entry.title_id, title_type, record_type});
            }
        }
    };

    RetrieveEntries(FileSys::TitleType::Application, FileSys::ContentRecordType::Program);
    RetrieveEntries(FileSys::TitleType::Application, FileSys::ContentRecordType::HtmlDocument);
    RetrieveEntries(FileSys::TitleType::Application, FileSys::ContentRecordType::LegalInformation);
    RetrieveEntries(FileSys::TitleType::AOC, FileSys::ContentRecordType::Data);

    if (available_title_ids.empty()) {
        return false;
    }

    size_t title_index = 0;

    if (available_title_ids.size() > 1) {
        QStringList list;
        for (auto& [title_id, title_type, record_type] : available_title_ids) {
            const auto hex_title_id = QString::fromStdString(fmt::format("{:X}", title_id));
            if (record_type == FileSys::ContentRecordType::Program) {
                list.push_back(QStringLiteral("Program [%1]").arg(hex_title_id));
            } else if (record_type == FileSys::ContentRecordType::HtmlDocument) {
                list.push_back(QStringLiteral("HTML document [%1]").arg(hex_title_id));
            } else if (record_type == FileSys::ContentRecordType::LegalInformation) {
                list.push_back(QStringLiteral("Legal information [%1]").arg(hex_title_id));
            } else {
                list.push_back(
                    QStringLiteral("DLC %1 [%2]").arg(title_id & 0x7FF).arg(hex_title_id));
            }
        }

        bool ok;
        const auto res = QInputDialog::getItem(
            this, tr("Select RomFS Dump Target"),
            tr("Please select which RomFS you would like to dump."), list, 0, false, &ok);
        if (!ok) {
            return false;
        }

        title_index = list.indexOf(res);
    }

    const auto& [title_id, title_type, record_type] = *available_title_ids.nth(title_index);
    *selected_title_id = title_id;
    *selected_content_record_type = static_cast<u8>(record_type);
    return true;
}

bool MainWindow::ConfirmClose() {
    if (QtCommon::emu_thread == nullptr ||
        UISettings::values.confirm_before_stopping.GetValue() == ConfirmStop::Ask_Never)
        return true;

    if (!QtCommon::system->GetExitLocked() &&
        UISettings::values.confirm_before_stopping.GetValue() == ConfirmStop::Ask_Based_On_Game)
        return true;

    const auto text = tr("Are you sure you want to close Eden?");
    return question(this, tr("Eden"), text);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!ConfirmClose()) {
        event->ignore();
        return;
    }

    UpdateUISettings();
    game_list->SaveInterfaceLayout();
    UISettings::SaveWindowState();
    hotkey_registry.SaveHotkeys();

    // Unload controllers early
    controller_dialog->UnloadController();
    game_list->UnloadController();

    // Shutdown session if the emu thread is active...
    if (QtCommon::emu_thread != nullptr)
        ShutdownGame();

    render_window->close();
    multiplayer_state->Close();
    QtCommon::system->HIDCore().UnloadInputDevices();
    Network::Shutdown();

    QWidget::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    emit sizeChanged(event->size());
}

void MainWindow::moveEvent(QMoveEvent* event) {
    auto window_frame_height = frameGeometry().height() - geometry().height();
    emit positionChanged(event->pos() - QPoint{0, window_frame_height});
}

static bool IsSingleFileDropEvent(const QMimeData* mime) {
    return mime->hasUrls() && mime->urls().length() == 1;
}

void MainWindow::AcceptDropEvent(QDropEvent* event) {
    if (IsSingleFileDropEvent(event->mimeData())) {
        event->setDropAction(Qt::DropAction::LinkAction);
        event->accept();
    }
}

bool MainWindow::DropAction(QDropEvent* event) {
    if (!IsSingleFileDropEvent(event->mimeData())) {
        return false;
    }

    const QMimeData* mime_data = event->mimeData();
    const QString& filename = mime_data->urls().at(0).toLocalFile();

    if (emulation_running && QFileInfo(filename).suffix() == QStringLiteral("bin")) {
        // Amiibo
        LoadAmiibo(filename);
    } else {
        // Game
        if (ConfirmChangeGame()) {
            BootGame(filename, ApplicationAppletParameters());
        }
    }
    return true;
}

void MainWindow::dropEvent(QDropEvent* event) {
    DropAction(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    AcceptDropEvent(event);
}

void MainWindow::dragMoveEvent(QDragMoveEvent* event) {
    AcceptDropEvent(event);
}

bool MainWindow::ConfirmChangeGame() {
    if (QtCommon::emu_thread == nullptr)
        return true;

    // Use custom question to link controller navigation
    return question(
        this, tr("Eden"),
        tr("Are you sure you want to stop the emulation? Any unsaved progress will be lost."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
}

bool MainWindow::ConfirmForceLockedExit() {
    if (QtCommon::emu_thread == nullptr)
        return true;

    const auto text = tr("The currently running application has requested Eden to not exit.\n\n"
                         "Would you like to bypass this and exit anyway?");

    return question(this, tr("Eden"), text);
}

void MainWindow::RequestGameExit() {
    if (!QtCommon::system->IsPoweredOn())
        return;

    QtCommon::system->SetExitRequested(true);
    QtCommon::system->GetAppletManager().RequestExit();
}

void MainWindow::filterBarSetChecked(bool state) {
    ui->action_Show_Filter_Bar->setChecked(state);
    emit(OnToggleFilterBar());
}

static void AdjustLinkColor() {
    QPalette new_pal(qApp->palette());

    if (UISettings::IsDarkTheme())
        new_pal.setColor(QPalette::Link, QColor(0, 190, 255, 255));
    else
        new_pal.setColor(QPalette::Link, QColor(0, 140, 200, 255));

    if (qApp->palette().color(QPalette::Link) != new_pal.color(QPalette::Link))
        qApp->setPalette(new_pal);
}

void MainWindow::UpdateUITheme() {
    const QString default_theme = QString::fromUtf8(UISettings::themes[size_t(UISettings::default_theme)].second);
    QString current_theme = QString::fromStdString(UISettings::values.theme);

    if (current_theme.isEmpty())
        current_theme = default_theme;

#ifdef _WIN32
    QIcon::setThemeName(current_theme);
    AdjustLinkColor();
#else
    if (current_theme == QStringLiteral("default") || current_theme == QStringLiteral("colorful")) {
        QIcon::setThemeName(current_theme == QStringLiteral("colorful") ? current_theme : startup_icon_theme);
        QIcon::setThemeSearchPaths(QStringList(default_theme_paths));
        if (isDarkMode()) {
            current_theme = QStringLiteral("default_dark");
        }
    } else {
        QIcon::setThemeName(current_theme);
        QIcon::setThemeSearchPaths(QStringList(QStringLiteral(":/icons")));
        AdjustLinkColor();
    }
#endif

    if (current_theme != default_theme) {
        QString theme_uri{QStringLiteral(":%1/style.qss").arg(current_theme)};
        QFile f(theme_uri);
        if (!f.open(QFile::ReadOnly | QFile::Text)) {
            LOG_ERROR(Frontend, "Unable to open style \"{}\", fallback to the default theme",
                      UISettings::values.theme);
            current_theme = default_theme;
        }
    }

    QString theme_uri{QStringLiteral(":%1/style.qss").arg(current_theme)};
    QFile f(theme_uri);
    if (f.open(QFile::ReadOnly | QFile::Text)) {
        QTextStream ts(&f);
        qApp->setStyleSheet(ts.readAll());
        setStyleSheet(ts.readAll());
    } else {
        LOG_ERROR(Frontend, "Unable to set style \"{}\", stylesheet file not found",
                  UISettings::values.theme);
        qApp->setStyleSheet({});
        setStyleSheet({});
    }

#ifdef _WIN32
    RemoveTitlebarFilter();
    ApplyGlobalDarkTitlebar(UISettings::IsDarkTheme());
#endif
}

void MainWindow::LoadTranslation() {
    bool loaded;

    if (UISettings::values.language.GetValue().empty()) {
        // If the selected language is empty, use system locale
        loaded = translator.load(QLocale(), {}, {}, QStringLiteral(":/languages/"));
    } else {
        // Otherwise load from the specified file
        loaded = translator.load(QString::fromStdString(UISettings::values.language.GetValue()),
                                 QStringLiteral(":/languages/"));
    }

    if (loaded)
        qApp->installTranslator(&translator);
    else
        UISettings::values.language = std::string("en");
}

void MainWindow::OnLanguageChanged(const QString& locale) {
    if (UISettings::values.language.GetValue() != std::string("en"))
        qApp->removeTranslator(&translator);

    QList<QAction*> actions = game_size_actions->actions();
    for (size_t i = 0; i < default_game_icon_sizes.size(); i++) {
        actions.at(i)->setText(GetTranslatedGameIconSize(i));
    }

    UISettings::values.language = locale.toStdString();
    LoadTranslation();
    ui->retranslateUi(this);
    multiplayer_state->retranslateUi();
    UpdateWindowTitle();
}

void MainWindow::SetDiscordEnabled([[maybe_unused]] bool state) {
#ifdef USE_DISCORD_PRESENCE
    if (state)
        discord_rpc = std::make_unique<DiscordRPC::DiscordImpl>(*QtCommon::system);
    else
        discord_rpc = std::make_unique<DiscordRPC::NullImpl>();
#else
    discord_rpc = std::make_unique<DiscordRPC::NullImpl>();
#endif
    discord_rpc->Update();
}

void MainWindow::SetGamemodeEnabled(bool state) {
    if (emulation_running) {
        if (state)
            Common::FeralGamemode::Start();
        else
            Common::FeralGamemode::Stop();
    }
}

void MainWindow::changeEvent(QEvent* event) {
#ifdef __unix__
    // PaletteChange event appears to only reach so far into the GUI, explicitly asking to
    // UpdateUITheme is a decent work around
    if (event->type() == QEvent::PaletteChange) {
        const QPalette test_palette(qApp->palette());
        const QString current_theme = QString::fromStdString(UISettings::values.theme);
        // Keeping eye on QPalette::Window to avoid looping. QPalette::Text might be useful too
        static QColor last_window_color;
        const QColor window_color = test_palette.color(QPalette::Active, QPalette::Window);
        if (last_window_color != window_color && (current_theme == QStringLiteral("default") ||
                                                  current_theme == QStringLiteral("colorful"))) {
            UpdateUITheme();
        }
        last_window_color = window_color;
    }
#endif // __unix__
    QWidget::changeEvent(event);
}

Service::AM::FrontendAppletParameters MainWindow::ApplicationAppletParameters() {
    return Service::AM::FrontendAppletParameters{
        .applet_id = Service::AM::AppletId::Application,
        .applet_type = Service::AM::AppletType::Application,
    };
}

Service::AM::FrontendAppletParameters MainWindow::LibraryAppletParameters(
    u64 program_id, Service::AM::AppletId applet_id) {
    return Service::AM::FrontendAppletParameters{
        .program_id = program_id,
        .applet_id = applet_id,
        .applet_type = Service::AM::AppletType::LibraryApplet,
    };
}

void VolumeButton::wheelEvent(QWheelEvent* event) {

    int num_degrees = event->angleDelta().y() / 8;
    int num_steps = (num_degrees / 15) * scroll_multiplier;
    // Stated in QT docs: Most mouse types work in steps of 15 degrees, in which case the delta
    // value is a multiple of 120; i.e., 120 units * 1/8 = 15 degrees.

    if (num_steps > 0) {
        Settings::values.volume.SetValue(
            (std::min)(200, Settings::values.volume.GetValue() + num_steps));
    } else {
        Settings::values.volume.SetValue(
            (std::max)(0, Settings::values.volume.GetValue() + num_steps));
    }

    scroll_multiplier = (std::min)(MaxMultiplier, scroll_multiplier * 2);
    scroll_timer.start(100); // reset the multiplier if no scroll event occurs within 100 ms

    emit VolumeChanged();
    event->accept();
}

void VolumeButton::ResetMultiplier() {
    scroll_multiplier = 1;
}

#ifdef main
#undef main
#endif

#if !defined(QT_STATICPLUGIN) || defined(__APPLE__)
#define VMA_IMPLEMENTATION 1
#include "video_core/vulkan_common/vma.h"
#endif

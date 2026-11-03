// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCheckBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSpinBox>
#include <QUrl>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "qt_common/config/uisettings.h"
#include "ui_configure_debug.h"
#include "yuzu/configuration/configure_debug.h"
#include "yuzu/debugger/console.h"

ConfigureDebug::ConfigureDebug(const Core::System& system_, QWidget* parent)
    : QScrollArea(parent), ui{std::make_unique<Ui::ConfigureDebug>()}, system{system_} {
    ui->setupUi(this);
#ifdef YUZU_ENABLE_MCP
    SetupMcpWidgets();
#endif
    SetConfiguration();

    connect(ui->open_log_button, &QPushButton::clicked, []() {
        const auto path =
            QString::fromStdString(Common::FS::GetEdenPathString(Common::FS::EdenPath::LogDir));
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });

    connect(ui->toggle_gdbstub, &QCheckBox::toggled,
            [&]() { ui->gdbport_spinbox->setEnabled(ui->toggle_gdbstub->isChecked()); });
}

ConfigureDebug::~ConfigureDebug() = default;

#ifdef YUZU_ENABLE_MCP
void ConfigureDebug::SetupMcpWidgets() {
    auto* layout = ui->debug_widget->layout();
    auto* title = new QLabel(tr("MCP server (for AI agents and scripts):"), ui->debug_widget);
    title->setContentsMargins(0, 8, 0, 0);
    layout->addWidget(title);

    mcp_enable = new QCheckBox(tr("Enable MCP Server"), ui->debug_widget);
    layout->addWidget(mcp_enable);

    auto* port_row = new QWidget(ui->debug_widget);
    auto* port_layout = new QHBoxLayout(port_row);
    port_layout->setContentsMargins(0, 0, 0, 0);
    port_layout->addWidget(new QLabel(tr("Port:"), port_row));
    mcp_port = new QSpinBox(port_row);
    mcp_port->setRange(1024, 65535);
    port_layout->addWidget(mcp_port);
    layout->addWidget(port_row);

    mcp_writes = new QCheckBox(tr("Allow Writes (input, memory writes, settings)"),
                               ui->debug_widget);
    layout->addWidget(mcp_writes);

    auto* token_row = new QWidget(ui->debug_widget);
    auto* token_layout = new QHBoxLayout(token_row);
    token_layout->setContentsMargins(0, 0, 0, 0);
    token_layout->addWidget(new QLabel(tr("Token:"), token_row));
    mcp_token = new QLineEdit(token_row);
    mcp_token->setEchoMode(QLineEdit::Password);
    mcp_token->setPlaceholderText(tr("Optional: clients must send it as a bearer token"));
    token_layout->addWidget(mcp_token);
    layout->addWidget(token_row);

    connect(mcp_enable, &QCheckBox::toggled, this, [this, port_row, token_row](bool on) {
        port_row->setEnabled(on);
        mcp_writes->setEnabled(on);
        token_row->setEnabled(on);
    });
}
#endif

void ConfigureDebug::SetConfiguration() {
    const bool runtime_lock = !system.IsPoweredOn();
    ui->toggle_gdbstub->setChecked(Settings::values.use_gdbstub.GetValue());
    ui->gdbport_spinbox->setEnabled(Settings::values.use_gdbstub.GetValue());
    ui->gdbport_spinbox->setValue(Settings::values.gdbstub_port.GetValue());
    ui->log_filter_edit->setText(QString::fromStdString(Settings::values.log_filter.GetValue()));
    ui->flush_line->setChecked(Settings::values.log_flush_line.GetValue());
    ui->censor_username->setChecked(Settings::values.censor_username.GetValue());
    ui->reporting_services->setChecked(Settings::values.reporting_services.GetValue());
    ui->dump_audio_commands->setChecked(Settings::values.dump_audio_commands.GetValue());
    ui->quest_flag->setChecked(Settings::values.quest_flag.GetValue());
    ui->use_dev_keys->setChecked(Settings::values.use_dev_keys.GetValue());
    ui->use_debug_asserts->setChecked(Settings::values.use_debug_asserts.GetValue());
    ui->use_auto_stub->setChecked(Settings::values.use_auto_stub.GetValue());
#ifdef YUZU_ENABLE_MCP
    mcp_enable->setChecked(Settings::values.enable_mcp_server.GetValue());
    mcp_port->setValue(Settings::values.mcp_port.GetValue());
    mcp_writes->setChecked(Settings::values.mcp_allow_writes.GetValue());
    mcp_token->setText(QString::fromStdString(Settings::values.mcp_token.GetValue()));
    emit mcp_enable->toggled(mcp_enable->isChecked());
#endif
    ui->enable_all_controllers->setChecked(Settings::values.enable_all_controllers.GetValue());
    ui->extended_logging->setChecked(Settings::values.extended_logging.GetValue());
    ui->perform_vulkan_check->setChecked(Settings::values.perform_vulkan_check.GetValue());
    ui->serial_battery_edit->setText(
        QString::number(Settings::values.serial_battery.GetValue())
    );
    ui->serial_board_edit->setText(
        QString::number(Settings::values.serial_unit.GetValue())
    );
#ifdef YUZU_USE_QT_WEB_ENGINE
    ui->disable_web_applet->setChecked(Settings::values.disable_web_applet.GetValue());
#else
    ui->disable_web_applet->setChecked(true);
#endif

    // Immutable after starting
    ui->homebrew_args_edit->setEnabled(runtime_lock);
    ui->homebrew_args_edit->setText(
        QString::fromStdString(Settings::values.program_args.GetValue()));
    ui->toggle_console->setEnabled(runtime_lock);
    ui->toggle_console->setChecked(UISettings::values.show_console.GetValue());
    ui->fs_access_log->setEnabled(runtime_lock);
    ui->fs_access_log->setChecked(Settings::values.enable_fs_access_log.GetValue());
    ui->enable_renderdoc_hotkey->setEnabled(runtime_lock);
    ui->enable_renderdoc_hotkey->setChecked(Settings::values.enable_renderdoc_hotkey.GetValue());
    ui->disable_buffer_reorder->setEnabled(runtime_lock);
    ui->disable_buffer_reorder->setChecked(Settings::values.disable_buffer_reorder.GetValue());
    ui->enable_graphics_debugging->setEnabled(runtime_lock);
    ui->enable_graphics_debugging->setChecked(Settings::values.renderer_debug.GetValue());
    ui->enable_shader_feedback->setEnabled(runtime_lock);
    ui->enable_shader_feedback->setChecked(Settings::values.renderer_shader_feedback.GetValue());
    ui->enable_nsight_aftermath->setEnabled(runtime_lock);
    ui->enable_nsight_aftermath->setChecked(Settings::values.enable_nsight_aftermath.GetValue());
    ui->dump_guest_shaders->setEnabled(runtime_lock);
    ui->dump_guest_shaders->setChecked(Settings::values.dump_guest_shaders.GetValue());
    ui->dump_macros->setEnabled(runtime_lock);
    ui->dump_macros->setChecked(Settings::values.dump_macros.GetValue());
    ui->disable_macro_jit->setEnabled(runtime_lock);
    ui->disable_macro_jit->setChecked(Settings::values.disable_macro_jit.GetValue());
    ui->disable_macro_hle->setEnabled(runtime_lock);
    ui->disable_macro_hle->setChecked(Settings::values.disable_macro_hle.GetValue());
    ui->disable_loop_safety_checks->setEnabled(runtime_lock);
    ui->disable_loop_safety_checks->setChecked(
        Settings::values.disable_shader_loop_safety_checks.GetValue());
    ui->perform_vulkan_check->setChecked(Settings::values.perform_vulkan_check.GetValue());
    ui->debug_knobs_spinbox->setValue(Settings::values.debug_knobs.GetValue());

    ui->gpu_log_level->setEnabled(runtime_lock);
    ui->gpu_log_level->setCurrentIndex(
        static_cast<int>(Settings::values.gpu_log_level.GetValue()));
    ui->gpu_log_shader_dumps->setEnabled(runtime_lock);
    ui->gpu_log_shader_dumps->setChecked(Settings::values.gpu_log_shader_dumps.GetValue());
#ifdef YUZU_USE_QT_WEB_ENGINE
    ui->disable_web_applet->setChecked(Settings::values.disable_web_applet.GetValue());
#else
    ui->disable_web_applet->setChecked(true);
    ui->disable_web_applet->setVisible(false);
#endif
}

void ConfigureDebug::ApplyConfiguration() {
    Settings::values.use_gdbstub = ui->toggle_gdbstub->isChecked();
    Settings::values.gdbstub_port = ui->gdbport_spinbox->value();
    UISettings::values.show_console = ui->toggle_console->isChecked();
    Settings::values.log_filter = ui->log_filter_edit->text().toStdString();
    Settings::values.log_flush_line = ui->flush_line->isChecked();
    Settings::values.censor_username = ui->censor_username->isChecked();
    Settings::values.program_args = ui->homebrew_args_edit->text().toStdString();
    Settings::values.enable_fs_access_log = ui->fs_access_log->isChecked();
    Settings::values.reporting_services = ui->reporting_services->isChecked();
    Settings::values.dump_audio_commands = ui->dump_audio_commands->isChecked();
    Settings::values.quest_flag = ui->quest_flag->isChecked();
    Settings::values.use_dev_keys = ui->use_dev_keys->isChecked();
    Settings::values.use_debug_asserts = ui->use_debug_asserts->isChecked();
    Settings::values.use_auto_stub = ui->use_auto_stub->isChecked();
#ifdef YUZU_ENABLE_MCP
    // The main window restarts the server if these changed (see MainWindow::OnConfigure).
    Settings::values.enable_mcp_server = mcp_enable->isChecked();
    Settings::values.mcp_port = static_cast<u16>(mcp_port->value());
    Settings::values.mcp_allow_writes = mcp_writes->isChecked();
    Settings::values.mcp_token = mcp_token->text().toStdString();
#endif
    Settings::values.enable_all_controllers = ui->enable_all_controllers->isChecked();
    Settings::values.renderer_debug = ui->enable_graphics_debugging->isChecked();
    Settings::values.enable_renderdoc_hotkey = ui->enable_renderdoc_hotkey->isChecked();
    Settings::values.disable_buffer_reorder = ui->disable_buffer_reorder->isChecked();
    Settings::values.renderer_shader_feedback = ui->enable_shader_feedback->isChecked();
    Settings::values.enable_nsight_aftermath = ui->enable_nsight_aftermath->isChecked();
    Settings::values.dump_guest_shaders = ui->dump_guest_shaders->isChecked();
    Settings::values.dump_macros = ui->dump_macros->isChecked();
    Settings::values.disable_shader_loop_safety_checks =
        ui->disable_loop_safety_checks->isChecked();
    Settings::values.disable_macro_jit = ui->disable_macro_jit->isChecked();
    Settings::values.disable_macro_hle = ui->disable_macro_hle->isChecked();
    Settings::values.extended_logging = ui->extended_logging->isChecked();
    Settings::values.perform_vulkan_check = ui->perform_vulkan_check->isChecked();
    Settings::values.disable_web_applet = ui->disable_web_applet->isChecked();
    Settings::values.serial_battery = ui->serial_battery_edit->text().toUInt();
    Settings::values.serial_unit = ui->serial_board_edit->text().toUInt();
    Settings::values.debug_knobs = ui->debug_knobs_spinbox->value();
    Settings::values.gpu_log_level =
        static_cast<Settings::GpuLogLevel>(ui->gpu_log_level->currentIndex());
    Settings::values.gpu_log_shader_dumps = ui->gpu_log_shader_dumps->isChecked();
    Debugger::ToggleConsole();
    Common::Log::Filter filter;
    filter.ParseFilterString(Settings::values.log_filter.GetValue());
    Common::Log::SetGlobalFilter(filter);
    Core::Crypto::KeyManager::Instance().ReloadKeys();
}

void ConfigureDebug::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureDebug::RetranslateUI() {
    ui->retranslateUi(this);
}

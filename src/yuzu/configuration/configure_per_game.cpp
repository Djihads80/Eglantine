// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <QAbstractButton>
#include <QFileInfo>
#include <QIcon>
#include <QImageReader>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QString>
#include <QTimer>

#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "common/settings_enums.h"
#include "common/settings_input.h"
#include "common/string_util.h"
#include "configuration/shared_widget.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/xts_archive.h"
#include "core/loader/loader.h"
#include "frontend_common/config.h"
#include "qt_common/abstract/frontend.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/vk.h"
#include "ui_configure_per_game.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_applets.h"
#include "yuzu/configuration/configure_audio.h"
#include "yuzu/configuration/configure_cpu.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/configuration/configure_graphics_advanced.h"
#include "yuzu/configuration/configure_graphics_extensions.h"
#include "yuzu/configuration/configure_input_per_game.h"
#include "yuzu/configuration/configure_network.h"
#include "yuzu/configuration/configure_per_game.h"
#include "yuzu/configuration/configure_per_game_addons.h"
#include "yuzu/configuration/configure_system.h"
#include "yuzu/nup_metadata.h"
#include "yuzu/util/util.h"

ConfigurePerGame::ConfigurePerGame(QWidget* parent, u64 title_id_, const std::string& file_name,
                                   std::vector<VkDeviceInfo::Record>& vk_device_records,
                                   Core::System& system_)
    : QDialog(parent), ui(std::make_unique<Ui::ConfigurePerGame>()),
      file_path(std::filesystem::path(Common::FS::ToU8String(file_name))), title_id{title_id_},
      system{system_},
      builder{std::make_unique<ConfigurationShared::Builder>(this, !system_.IsPoweredOn())},
      tab_group{std::make_shared<std::vector<ConfigurationShared::Tab*>>()} {
    const auto config_file_name = title_id == 0 ? Common::FS::PathToUTF8String(file_path.filename())
                                                : fmt::format("{:016X}", title_id);
    game_config = std::make_unique<QtConfig>(config_file_name, Config::ConfigType::PerGameConfig);
    addons_tab = std::make_unique<ConfigurePerGameAddons>(system_, this);
    audio_tab = std::make_unique<ConfigureAudio>(system_, tab_group, *builder, this);
    cpu_tab = std::make_unique<ConfigureCpu>(system_, tab_group, *builder, this);
    graphics_advanced_tab =
        std::make_unique<ConfigureGraphicsAdvanced>(system_, tab_group, *builder, this);
    graphics_extensions_tab =
        std::make_unique<ConfigureGraphicsExtensions>(system_, tab_group, *builder, this);
    graphics_tab = std::make_unique<ConfigureGraphics>(
        system_, vk_device_records, [&]() { graphics_advanced_tab->ExposeComputeOption(); },
        [](Settings::AspectRatio, Settings::ResolutionSetup) {}, tab_group, *builder, this);
    input_tab = std::make_unique<ConfigureInputPerGame>(system_, game_config.get(), this);
    system_tab = std::make_unique<ConfigureSystem>(system_, tab_group, *builder, this);
    network_tab = std::make_unique<ConfigureNetwork>(system_, this);
    applets_tab = std::make_unique<ConfigureApplets>(system_, tab_group, *builder, this);

    ui->setupUi(this);

    ui->tabWidget->addTab(addons_tab.get(), tr("Add-Ons"));
    ui->tabWidget->addTab(system_tab.get(), tr("System"));
    ui->tabWidget->addTab(cpu_tab.get(), tr("CPU"));
    ui->tabWidget->addTab(graphics_tab.get(), tr("Graphics"));
    ui->tabWidget->addTab(graphics_advanced_tab.get(), tr("Adv. Graphics"));
    ui->tabWidget->addTab(graphics_extensions_tab.get(), tr("Ext. Graphics"));
    ui->tabWidget->addTab(audio_tab.get(), tr("Audio"));
    ui->tabWidget->addTab(input_tab.get(), tr("Input Profiles"));
    ui->tabWidget->addTab(network_tab.get(), tr("Network"));
    ui->tabWidget->addTab(applets_tab.get(), tr("Applets"));

    setFocusPolicy(Qt::ClickFocus);
    setWindowTitle(tr("Properties"));

    addons_tab->SetTitleId(title_id);

    scene = new QGraphicsScene;
    ui->icon_view->setScene(scene);
    ui->button_detect_icon->setIcon(
        QIcon::fromTheme(QStringLiteral("edit-find"), QIcon::fromTheme(QStringLiteral("search"))));
    ui->button_upload_icon->setIcon(QIcon::fromTheme(
        QStringLiteral("document-open"), QIcon::fromTheme(QStringLiteral("insert-image"))));

    connect(ui->button_upload_icon, &QPushButton::clicked, this,
            &ConfigurePerGame::OnUploadNupIcon);
    connect(ui->button_detect_icon, &QPushButton::clicked, this,
            &ConfigurePerGame::OnDetectNupIcon);
    connect(ui->display_name, &QLineEdit::textEdited, this, [this](const QString&) {
        has_pending_nup_changes = true;
    });

    if (system.IsPoweredOn()) {
        QPushButton* apply_button = ui->buttonBox->addButton(QDialogButtonBox::Apply);
        connect(apply_button, &QAbstractButton::clicked, this,
                &ConfigurePerGame::HandleApplyButtonClicked);
    }

    LoadConfiguration();
}

ConfigurePerGame::~ConfigurePerGame() {
    if (detect_cancel_token) {
        detect_cancel_token->store(true);
    }
}

void ConfigurePerGame::ApplyConfiguration() {
    SavePendingNupMetadata(true);

    for (const auto tab : *tab_group) {
        tab->ApplyConfiguration();
    }
    addons_tab->ApplyConfiguration();
    input_tab->ApplyConfiguration();
    network_tab->ApplyConfiguration();
    applets_tab->ApplyConfiguration();

    if (Settings::IsDockedMode() && Settings::values.players.GetValue()[0].controller_type ==
                                        Settings::ControllerType::Handheld) {
        Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
        Settings::values.use_docked_mode.SetGlobal(true);
    }

    system.ApplySettings();
    Settings::LogSettings();

    game_config->SaveAllValues();
}
void ConfigurePerGame::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QDialog::changeEvent(event);
}

void ConfigurePerGame::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigurePerGame::HandleApplyButtonClicked() {
    UISettings::values.configuration_applied = true;
    ApplyConfiguration();
}

void ConfigurePerGame::LoadFromFile(FileSys::VirtualFile file_) {
    file = std::move(file_);
    LoadConfiguration();
}

void ConfigurePerGame::LoadConfiguration() {
    if (file == nullptr) {
        return;
    }

    addons_tab->LoadFromFile(file);

    ui->display_title_id->setText(
        QStringLiteral("%1").arg(title_id, 16, 16, QLatin1Char{'0'}).toUpper());

    const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto control = pm.GetControlMetadata();
    const auto loader = Loader::GetLoader(system, file);

    is_nup_file = loader != nullptr && loader->GetFileType() == Loader::FileType::NUP;
    ui->nup_icon_widget->setVisible(is_nup_file);
    ui->display_name->setClearButtonEnabled(is_nup_file);

    QString perm_error;
    nup_writable = is_nup_file && NupMetadata::IsArchiveWritable(file_path, &perm_error);
    const bool is_running = is_nup_file && NupMetadata::IsArchiveRunning(system, file_path);

    if (is_nup_file) {
        if (is_running) {
            ui->display_name->setReadOnly(true);
            ui->button_detect_icon->setEnabled(false);
            ui->button_upload_icon->setEnabled(false);
            ui->display_name->setToolTip(tr("Cannot modify NUP metadata while the game is running."));
        } else if (!nup_writable) {
            ui->display_name->setReadOnly(true);
            ui->button_detect_icon->setEnabled(false);
            ui->button_upload_icon->setEnabled(false);
            ui->display_name->setToolTip(perm_error);
        } else {
            ui->display_name->setReadOnly(false);
            ui->button_detect_icon->setEnabled(true);
            ui->button_upload_icon->setEnabled(true);
            ui->display_name->setPlaceholderText(tr("Enter game name..."));
            ui->display_name->setToolTip(tr("Edit the game name stored in the NUP metadata (saved on OK)"));
        }
    } else {
        ui->display_name->setReadOnly(true);
        ui->display_name->setPlaceholderText(QString{});
        ui->display_name->setToolTip(QString{});
    }

    if (!is_nup_file && control.first != nullptr) {
        ui->display_version->setText(QString::fromStdString(control.first->GetVersionString()));
        ui->display_name->setText(QString::fromStdString(control.first->GetApplicationName()));
        ui->display_developer->setText(QString::fromStdString(control.first->GetDeveloperName()));
    } else if (loader != nullptr) {
        std::string title;
        if (loader->ReadTitle(title) == Loader::ResultStatus::Success) {
            ui->display_name->setText(QString::fromStdString(title));
        } else if (is_nup_file) {
            ui->display_name->setText(
                QFileInfo(QString::fromStdString(file->GetName())).completeBaseName());
        }

        FileSys::NACP nacp;
        if (loader->ReadControlData(nacp) == Loader::ResultStatus::Success) {
            ui->display_developer->setText(QString::fromStdString(nacp.GetDeveloperName()));
            const auto ver = nacp.GetVersionString();
            ui->display_version->setText(ver.empty() ? QStringLiteral("1.0.0")
                                                     : QString::fromStdString(ver));
        } else {
            ui->display_version->setText(QStringLiteral("1.0.0"));
        }
    }
    initial_nup_title = ui->display_name->text().trimmed().toStdString();
    pending_nup_icon.reset();
    has_pending_nup_changes = false;

    scene->clear();
    if (!is_nup_file && control.second != nullptr) {
        QPixmap map;
        const auto bytes = control.second->ReadAllBytes();
        if (map.loadFromData(bytes.data(), static_cast<u32>(bytes.size()))) {
            UpdateIconPreview(map);
        }
    } else if (loader != nullptr) {
        std::vector<u8> bytes;
        if (loader->ReadIcon(bytes) == Loader::ResultStatus::Success) {
            QPixmap map;
            if (map.loadFromData(bytes.data(), static_cast<u32>(bytes.size()))) {
                UpdateIconPreview(map);
            }
        }
    }

    ui->display_filename->setText(QString::fromStdString(file->GetName()));

    if (loader != nullptr) {
        ui->display_format->setText(
            QString::fromStdString(Loader::GetFileTypeString(loader->GetFileType())));
    }

    const auto valueText = QtCommon::ReadableByteSize(file->GetSize());
    ui->display_size->setText(valueText);
}

void ConfigurePerGame::UpdateIconPreview(const QPixmap& map) {
    scene->clear();
    if (map.isNull()) {
        return;
    }
    scene->addPixmap(map.scaled(ui->icon_view->width(), ui->icon_view->height(),
                                Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

void ConfigurePerGame::OnUploadNupIcon() {
    if (!is_nup_file || !nup_writable) {
        return;
    }
    const QString default_dir =
        file_path.has_parent_path()
            ? QString::fromStdString(Common::FS::PathToUTF8String(file_path.parent_path()))
            : QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString selected = QtCommon::Frontend::GetOpenFileName(
        tr("Select Game Icon"), default_dir,
        tr("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All Files (*)"));
    if (selected.isEmpty()) {
        return;
    }
    QImageReader reader(selected);
    reader.setAllocationLimit(64);
    const QImage img = reader.read();
    if (img.isNull()) {
        QMessageBox::warning(this, tr("Invalid Image"),
                             tr("The selected file could not be loaded as an image."));
        return;
    }
    const auto encoded = NupMetadata::NormalizeAndEncodeIcon(img);
    if (encoded.empty()) {
        QMessageBox::warning(this, tr("Invalid Image"),
                             tr("Failed to encode image to 256x256 icon."));
        return;
    }
    QPixmap map;
    if (map.loadFromData(encoded.data(), static_cast<u32>(encoded.size()))) {
        UpdateIconPreview(map);
    }
    pending_nup_icon = encoded;
    has_pending_nup_changes = true;
}

void ConfigurePerGame::OnDetectNupIcon() {
    if (!is_nup_file || !nup_writable) {
        return;
    }
    if (detect_cancel_token) {
        detect_cancel_token->store(true);
    }
    detect_cancel_token = std::make_shared<std::atomic_bool>(false);

    QString query_qstr = ui->display_name->text().trimmed();
    if (query_qstr.isEmpty() && file != nullptr) {
        query_qstr = QFileInfo(QString::fromStdString(file->GetName())).completeBaseName();
    }
    const std::string raw_query = query_qstr.toStdString();
    if (raw_query.empty() && title_id == 0) {
        QMessageBox::information(this, tr("Detect Icon"),
                                 tr("Please enter a game name first so the icon can be detected."));
        return;
    }
    ui->button_detect_icon->setEnabled(false);
    ui->button_detect_icon->setText(tr("Detecting..."));

    QTimer::singleShot(6000, this, [guard = QPointer<ConfigurePerGame>(this), token = detect_cancel_token] {
        if (guard && guard->detect_cancel_token == token && !token->load()) {
            token->store(true);
            if (guard->ui->button_detect_icon) {
                guard->ui->button_detect_icon->setEnabled(guard->nup_writable);
                guard->ui->button_detect_icon->setText(QObject::tr("Detect Icon"));
            }
        }
    });

    NupMetadata::DetectIconAsync(
        raw_query, title_id, file_path, detect_cancel_token,
        [guard = QPointer<ConfigurePerGame>(this), token = detect_cancel_token](
            NupMetadata::DetectResult r) {
            if (!guard || (token && token->load())) {
                return;
            }
            guard->ui->button_detect_icon->setEnabled(guard->nup_writable);
            guard->ui->button_detect_icon->setText(QObject::tr("Detect Icon"));

            if (!r.success) {
                QMessageBox::information(guard, QObject::tr("Detect Icon"), r.error_message);
                return;
            }
            if (!r.resolved_title.empty()) {
                guard->ui->display_name->setText(QString::fromStdString(r.resolved_title));
            }
            QPixmap map;
            if (map.loadFromData(r.icon_bytes.data(), static_cast<u32>(r.icon_bytes.size()))) {
                guard->UpdateIconPreview(map);
            }
            guard->pending_nup_icon = std::move(r.icon_bytes);
            guard->has_pending_nup_changes = true;
        });
}

bool ConfigurePerGame::SavePendingNupMetadata(bool show_error) {
    if (!is_nup_file || file_path.empty()) {
        return true;
    }
    const std::string current_title = ui->display_name->text().trimmed().toStdString();
    std::optional<std::string> new_title;
    if (current_title != initial_nup_title) {
        new_title = current_title;
    }
    if (!new_title && !pending_nup_icon) {
        has_pending_nup_changes = false;
        return true;
    }

    QString error;
    const bool ok =
        NupMetadata::CommitMetadataChanges(file_path, new_title, pending_nup_icon, &error);
    if (!ok) {
        if (show_error) {
            QMessageBox::warning(
                this, tr("Update NUP Metadata"),
                tr("Failed to update NUP metadata:\n%1").arg(error));
        }
        return false;
    }

    initial_nup_title = current_title;
    pending_nup_icon.reset();
    has_pending_nup_changes = false;
    UISettings::values.is_game_list_reload_pending.exchange(true);
    return true;
}

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <thread>

#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/sm/sm.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "qt_common/qt_compat.h"
#include "ui_configure_input.h"
#include "ui_configure_input_advanced.h"
#include "ui_configure_input_player.h"
#include "yuzu/configuration/configure_camera.h"
#include "yuzu/configuration/configure_debug_controller.h"
#include "yuzu/configuration/configure_input.h"
#include "yuzu/configuration/configure_input_advanced.h"
#include "yuzu/configuration/configure_player_assignment.h"
#include "common/controller_console_state.h"
#include <mutex>
#include <QTimer>
#include <vector>
#include <algorithm>
#include <QTabWidget>
#include <QRadioButton>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QTabBar>
#include <QDateTime>
#include "yuzu/configuration/configure_input_player.h"
#include "yuzu/configuration/configure_motion_touch.h"
#include "yuzu/configuration/configure_ringcon.h"
#include "yuzu/configuration/configure_touchscreen_advanced.h"
#include "yuzu/configuration/configure_vibration.h"
#include "yuzu/configuration/input_profiles.h"

namespace {
template <typename Dialog, typename... Args>
void CallConfigureDialog(ConfigureInput& parent, Args&&... args) {
    Dialog dialog(&parent, std::forward<Args>(args)...);

    const auto res = dialog.exec();
    if (res == QDialog::Accepted) {
        dialog.ApplyConfiguration();
    }
}
} // Anonymous namespace

void OnDockedModeChanged(bool last_state, bool new_state, Core::System& system) {
    if (last_state == new_state) {
        return;
    }

    if (!system.IsPoweredOn()) {
        return;
    }

    system.GetAppletManager().OperationModeChanged();
}

ConfigureInput::ConfigureInput(Core::System& system_, QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureInput>()),
      profiles(std::make_unique<InputProfiles>()), system{system_} {
    ui->setupUi(this);
}

ConfigureInput::~ConfigureInput() = default;

void ConfigureInput::Initialize(InputCommon::InputSubsystem* input_subsystem,
                                std::size_t max_players) {
    const bool is_powered_on = system.IsPoweredOn();
    auto& hid_core = system.HIDCore();
    player_controllers = {
        new ConfigureInputPlayer(this, 0, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 1, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 2, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 3, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 4, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 5, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 6, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 7, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
    };

    player_tabs = {
        ui->tabPlayer1, ui->tabPlayer2, ui->tabPlayer3, ui->tabPlayer4,
        ui->tabPlayer5, ui->tabPlayer6, ui->tabPlayer7, ui->tabPlayer8,
    };

    connected_controller_checkboxes = {
        ui->checkboxPlayer1Connected, ui->checkboxPlayer2Connected, ui->checkboxPlayer3Connected,
        ui->checkboxPlayer4Connected, ui->checkboxPlayer5Connected, ui->checkboxPlayer6Connected,
        ui->checkboxPlayer7Connected, ui->checkboxPlayer8Connected,
    };

    std::array<QLabel*, 8> connected_controller_labels = {
        ui->label,   ui->label_3, ui->label_4, ui->label_5,
        ui->label_6, ui->label_7, ui->label_8, ui->label_9,
    };

    for (std::size_t i = 0; i < player_tabs.size(); ++i) {
        QHBoxLayout* tab_layout = new QHBoxLayout(player_tabs[i]);
        tab_layout->addWidget(player_controllers[i]);
        connect(player_controllers[i], &ConfigureInputPlayer::Connected, [this, i](bool checked) {
            // Ensures that connecting a controller changes the number of players
            if (connected_controller_checkboxes[i]->isChecked() != checked) {
                // Ensures that the players are always connected in sequential order
                PropagatePlayerNumberChanged(i, checked);
            }
        });
        connect(connected_controller_checkboxes[i], &QCheckBox::clicked, [this, i](bool checked) {
            // Reconnect current controller if it was the last one checked
            // (player number was reduced by more than one)
            const bool reconnect_first = !checked &&
                                         i < connected_controller_checkboxes.size() - 1 &&
                                         connected_controller_checkboxes[i + 1]->isChecked();

            // Ensures that the players are always connected in sequential order
            PropagatePlayerNumberChanged(i, checked, reconnect_first);
        });
        connect(player_controllers[i], &ConfigureInputPlayer::RefreshInputDevices, this,
                &ConfigureInput::UpdateAllInputDevices);
        connect(player_controllers[i], &ConfigureInputPlayer::RefreshInputProfiles, this,
                &ConfigureInput::UpdateAllInputProfiles, Qt::QueuedConnection);
        connect(connected_controller_checkboxes[i], &QCheckBox::STATE_CHANGED,
                [this, i](int state) {
                    // Keep activated controllers synced with the "Connected Controllers" checkboxes
                    player_controllers[i]->ConnectPlayer(state == Qt::Checked);
                });

        // Remove/hide all the elements that exceed max_players, if applicable.
        if (i >= max_players) {
            ui->tabWidget->removeTab(static_cast<int>(max_players));
            connected_controller_checkboxes[i]->hide();
            connected_controller_labels[i]->hide();
        }
    }
    // Only the first player can choose handheld mode so connect the signal just to player 1
    connect(player_controllers[0], &ConfigureInputPlayer::HandheldStateChanged,
            [this](bool is_handheld) { UpdateDockedState(is_handheld); });

    // The Players tab comes first: how players are assigned, and who is which player now
    assignment = new ConfigurePlayerAssignment(this);
    assignment->setAccessibleName(tr("Controllers"));
    assignment->SetOpenPlayerTab([this](int player) {
        for (QWidget* parent = player_tabs[player]->parentWidget(); parent;
             parent = parent->parentWidget()) {
            if (auto* tabs = qobject_cast<QTabWidget*>(parent)) {
                tabs->setCurrentWidget(player_tabs[player]);
                return;
            }
        }
    });

    // Console Mode first in the Controllers tab: a mirror of the player tabs' Docked/Handheld
    // choice, which stays the one applied (and the one player 1's Handheld type locks)
    {
        auto* console_box = new QGroupBox(tr("Console Mode"), assignment);
        auto* console_layout = new QHBoxLayout(console_box);
        auto* docked = new QRadioButton(tr("Docked"), console_box);
        auto* handheld = new QRadioButton(tr("Handheld"), console_box);
        console_layout->addWidget(docked);
        console_layout->addWidget(handheld);
        console_layout->addStretch(1);
        assignment->AddTopWidget(console_box);
        const auto mirror = [this, docked, handheld] {
            docked->setChecked(ui->radioDocked->isChecked());
            handheld->setChecked(ui->radioUndocked->isChecked());
            docked->setEnabled(ui->radioDocked->isEnabled());
            handheld->setEnabled(ui->radioUndocked->isEnabled());
        };
        connect(ui->radioDocked, &QRadioButton::toggled, this, mirror);
        connect(ui->radioUndocked, &QRadioButton::toggled, this, mirror);
        connect(docked, &QRadioButton::clicked, this, [this] { ui->radioDocked->setChecked(true); });
        connect(handheld, &QRadioButton::clicked, this,
                [this] { ui->radioUndocked->setChecked(true); });
        mirror_console_mode = mirror;
        mirror();
    }

    {
        auto& state = Common::GetControllerConsoleState();
        std::scoped_lock lock{state.mutex};
        followed_generation = state.generation;
    }
    follow_timer = new QTimer(this);
    connect(follow_timer, &QTimer::timeout, this, [this] {
        unsigned long long generation = 0;
        std::vector<Common::ControllerConsoleState::PlayerRow> rows;
        bool engine_active = false;
        {
            auto& state = Common::GetControllerConsoleState();
            std::scoped_lock lock{state.mutex};
            generation = state.generation;
            engine_active = state.engine_active;
            rows = state.players;
        }
        // Automatic assignment owns the players: only their tabs show, and their connection
        // cannot be toggled by hand
        const auto mode = Settings::values.controller_applet_mode.GetValue();
        const bool automatic = mode == Settings::AppletMode::ConsoleMode ||
                               mode == Settings::AppletMode::HandheldConsoleMode;
        QTabWidget* tabs = nullptr;
        for (QWidget* parent = player_tabs[0]->parentWidget(); parent && !tabs;
             parent = parent->parentWidget()) {
            tabs = qobject_cast<QTabWidget*>(parent);
        }
        // The tabs follow the engine's rows and flashes, like the overlay: a green label when a
        // controller takes the player, red when one leaves (the tab hides once its row is gone)
        constexpr long long flash_ms = 1500;
        const long long now = QDateTime::currentMSecsSinceEpoch();
        // The player tabs' choice can be locked (player 1 set to Handheld) without toggling
        if (mirror_console_mode) {
            mirror_console_mode();
        }
        for (std::size_t i = 0; i < player_tabs.size(); ++i) {
            const auto row = std::find_if(rows.begin(), rows.end(), [i](const auto& r) {
                return r.player == static_cast<int>(i);
            });
            bool shown = true;
            QColor label;
            if (automatic) {
                shown = engine_active && row != rows.end();
                // Red (leaving) wins, then blue (input held), then the green connect blink
                const long long elapsed = shown ? now - row->flash_since : 0;
                const bool blink = shown && row->flash != 0 && elapsed < flash_ms &&
                                   (elapsed / 125) % 2 == 0;
                if (blink && row->flash == 2) {
                    label = QColor(200, 50, 50);
                } else if (shown && row->active && !row->leaving) {
                    label = QColor(50, 110, 220);
                } else if (blink) {
                    label = QColor(40, 160, 70);
                }
            }
            if (tabs) {
                if (const int index = tabs->indexOf(player_tabs[i]); index >= 0) {
                    tabs->setTabVisible(index, shown);
                    tabs->tabBar()->setTabTextColor(index, label);
                }
            }
            player_controllers[i]->SetConnectionLocked(automatic);
        }
        // Clicking a player's tab rumbles its controller in the automatic modes, so the user
        // can tell which controller the tab is about (tab changes made by the code do not)
        if (tabs && tabs != rumble_tabs) {
            rumble_tabs = tabs;
            connect(tabs->tabBar(), &QTabBar::tabBarClicked, this, [this, tabs](int index) {
                const auto mode = Settings::values.controller_applet_mode.GetValue();
                if (mode != Settings::AppletMode::ConsoleMode &&
                    mode != Settings::AppletMode::HandheldConsoleMode) {
                    return;
                }
                const auto it = std::find(player_tabs.begin(), player_tabs.end(),
                                          tabs->widget(index));
                if (it == player_tabs.end()) {
                    return;
                }
                const auto player = static_cast<std::size_t>(it - player_tabs.begin());
                if (!Settings::values.vibration_enabled.GetValue() ||
                    !Settings::values.players.GetValue()[player].vibration_enabled) {
                    return;
                }
                auto& state = Common::GetControllerConsoleState();
                std::scoped_lock lock{state.mutex};
                state.rumble_requests.push_back(static_cast<int>(player));
            });
        }

        // The Controllers tab carries Console Mode in the automatic modes
        ui->handheldGroup->setVisible(!automatic);
        ui->connectedControllers->setVisible(!automatic);

        if (generation == followed_generation) {
            return;
        }
        followed_generation = generation;
        for (auto* controller : player_controllers) {
            controller->RefreshFromController();
        }
        // The boxes mirror the emulated controllers without acting on them
        for (auto* checkbox : connected_controller_checkboxes) {
            checkbox->blockSignals(true);
        }
        LoadPlayerControllerIndices();
        for (auto* checkbox : connected_controller_checkboxes) {
            checkbox->blockSignals(false);
        }
    });
    follow_timer->start(50);
    ui->tabWidget->insertTab(0, assignment, tr("Controllers"));
    ui->tabWidget->setCurrentIndex(0);

    advanced = new ConfigureInputAdvanced(hid_core, this);

    QHBoxLayout* advancedLayout = new QHBoxLayout(ui->tabAdvanced);
    advancedLayout->addWidget(advanced);

    connect(advanced, &ConfigureInputAdvanced::CallDebugControllerDialog,
            [this, input_subsystem, &hid_core, is_powered_on] {
                CallConfigureDialog<ConfigureDebugController>(
                    *this, input_subsystem, profiles.get(), hid_core, is_powered_on);
            });
    connect(advanced, &ConfigureInputAdvanced::CallTouchscreenConfigDialog,
            [this] { CallConfigureDialog<ConfigureTouchscreenAdvanced>(*this); });
    connect(advanced, &ConfigureInputAdvanced::CallMotionTouchConfigDialog,
            [this, input_subsystem] {
                CallConfigureDialog<ConfigureMotionTouch>(*this, input_subsystem);
            });
    connect(advanced, &ConfigureInputAdvanced::CallRingControllerDialog,
            [this, input_subsystem, &hid_core] {
                CallConfigureDialog<ConfigureRingController>(*this, input_subsystem, hid_core);
            });
    connect(advanced, &ConfigureInputAdvanced::CallCameraDialog, [this, input_subsystem] {
        CallConfigureDialog<ConfigureCamera>(*this, input_subsystem);
    });

    connect(ui->vibrationButton, &QPushButton::clicked,
            [this, &hid_core] { CallConfigureDialog<ConfigureVibration>(*this, hid_core); });

    connect(ui->motionButton, &QPushButton::clicked, [this, input_subsystem] {
        CallConfigureDialog<ConfigureMotionTouch>(*this, input_subsystem);
    });

    connect(ui->buttonClearAll, &QPushButton::clicked, [this] { ClearAll(); });
    connect(ui->buttonRestoreDefaults, &QPushButton::clicked, [this] { RestoreDefaults(); });

    RetranslateUI();
    LoadConfiguration();
}

void ConfigureInput::PropagatePlayerNumberChanged(size_t player_index, bool checked,
                                                  bool reconnect_current) {
    connected_controller_checkboxes[player_index]->setChecked(checked);

    if (checked) {
        // Check all previous buttons when checked
        if (player_index > 0) {
            PropagatePlayerNumberChanged(player_index - 1, checked);
        }
    } else {
        // Unchecked all following buttons when unchecked
        if (player_index < connected_controller_checkboxes.size() - 1) {
            PropagatePlayerNumberChanged(player_index + 1, checked);
        }
    }

    if (reconnect_current) {
        connected_controller_checkboxes[player_index]->setCheckState(Qt::Checked);
    }
}

QList<QWidget*> ConfigureInput::GetSubTabs() const {
    // The settings dialog shows these, not this widget's own tab bar
    return {
        assignment,     ui->tabAdvanced,  ui->tabPlayer1, ui->tabPlayer2, ui->tabPlayer3,
        ui->tabPlayer4, ui->tabPlayer5, ui->tabPlayer6, ui->tabPlayer7, ui->tabPlayer8,
    };
}

void ConfigureInput::ApplyConfiguration() {
    const bool was_global = Settings::values.players.UsingGlobal();
    Settings::values.players.SetGlobal(true);
    for (auto* controller : player_controllers) {
        controller->ApplyConfiguration();
    }

    advanced->ApplyConfiguration();
    assignment->ApplyConfiguration();

    const bool pre_docked_mode = Settings::IsDockedMode();
    const bool docked_mode_selected = ui->radioDocked->isChecked();
    Settings::values.use_docked_mode.SetValue(
        docked_mode_selected ? Settings::ConsoleMode::Docked : Settings::ConsoleMode::Handheld);
    OnDockedModeChanged(pre_docked_mode, docked_mode_selected, system);

    Settings::values.vibration_enabled.SetValue(ui->vibrationGroup->isChecked());
    Settings::values.motion_enabled.SetValue(ui->motionGroup->isChecked());
    Settings::values.players.SetGlobal(was_global);
}

void ConfigureInput::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureInput::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureInput::LoadConfiguration() {
    const auto* handheld = system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Handheld);

    LoadPlayerControllerIndices();
    UpdateDockedState(handheld->IsConnected());

    ui->vibrationGroup->setChecked(Settings::values.vibration_enabled.GetValue());
    ui->motionGroup->setChecked(Settings::values.motion_enabled.GetValue());
}

void ConfigureInput::LoadPlayerControllerIndices() {
    for (std::size_t i = 0; i < connected_controller_checkboxes.size(); ++i) {
        if (i == 0) {
            auto* handheld =
                system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Handheld);
            if (handheld->IsConnected()) {
                connected_controller_checkboxes[i]->setChecked(true);
                continue;
            }
        }
        const auto* controller = system.HIDCore().GetEmulatedControllerByIndex(i);
        connected_controller_checkboxes[i]->setChecked(controller->IsConnected());
    }
}

void ConfigureInput::ClearAll() {
    // We don't have a good way to know what tab is active, but we can find out by getting the
    // parent of the consoleInputSettings
    auto* player_tab = static_cast<ConfigureInputPlayer*>(ui->consoleInputSettings->parent());
    player_tab->ClearAll();
}

void ConfigureInput::RestoreDefaults() {
    // We don't have a good way to know what tab is active, but we can find out by getting the
    // parent of the consoleInputSettings
    auto* player_tab = static_cast<ConfigureInputPlayer*>(ui->consoleInputSettings->parent());
    player_tab->RestoreDefaults();

    ui->radioDocked->setChecked(true);
    ui->radioUndocked->setChecked(false);
    ui->vibrationGroup->setChecked(true);
    ui->motionGroup->setChecked(true);
}

void ConfigureInput::UpdateDockedState(bool is_handheld) {
    // Disallow changing the console mode if the controller type is handheld.
    ui->radioDocked->setEnabled(!is_handheld);
    ui->radioUndocked->setEnabled(!is_handheld);

    ui->radioDocked->setChecked(Settings::IsDockedMode());
    ui->radioUndocked->setChecked(!Settings::IsDockedMode());

    // Also force into undocked mode if the controller type is handheld.
    if (is_handheld) {
        ui->radioUndocked->setChecked(true);
    }
}

void ConfigureInput::UpdateAllInputDevices() {
    for (const auto& player : player_controllers) {
        player->UpdateInputDeviceCombobox();
    }
}

void ConfigureInput::UpdateAllInputProfiles(std::size_t player_index) {
    for (std::size_t i = 0; i < player_controllers.size(); ++i) {
        if (i == player_index) {
            continue;
        }

        player_controllers[i]->UpdateInputProfiles();
    }
}

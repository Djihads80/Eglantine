// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QKeyEvent>
#include <QList>
#include <QPointer>
#include <QToolButton>
#include <QWidget>

#include "common/controller_console_state.h"

namespace Core {
class System;
}

class QCheckBox;
class QString;
class QTimer;
class QTabWidget;

class ConfigureInputAdvanced;
class ConfigureInputPlayer;
class ConfigurePlayerAssignment;

class InputProfiles;

namespace InputCommon {
class InputSubsystem;
}

namespace Ui {
class ConfigureInput;
}

void OnDockedModeChanged(bool last_state, bool new_state, Core::System& system);

class ConfigureInput : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureInput(Core::System& system_, QWidget* parent = nullptr);
    ~ConfigureInput() override;

    /// Initializes the input dialog with the given input subsystem.
    void Initialize(InputCommon::InputSubsystem* input_subsystem_, std::size_t max_players = 8);

    /// Save all button configurations to settings file.
    void ApplyConfiguration();

    QList<QWidget*> GetSubTabs() const;

private:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;

    /// Player tabs dropped in a new order: the controllers take that order, the tabs go back
    void FinishTabDrag();
    /// The arrows on each player tab, swapping with the player before or after it
    void UpdateTabArrows();
    void SwapPlayers(int a, int b);
    void RetranslateUI();
    void ClearAll();

    void UpdateDockedState(bool is_handheld);
    void UpdateAllInputDevices();
    void UpdateAllInputProfiles(std::size_t player_index);
    // Enable preceding controllers or disable following ones
    void PropagatePlayerNumberChanged(size_t player_index, bool checked,
                                      bool reconnect_current = false);

    /// Load configuration settings.
    void LoadConfiguration();
    void LoadPlayerControllerIndices();

    /// Restore all buttons to their default values.
    void RestoreDefaults();

    std::unique_ptr<Ui::ConfigureInput> ui;

    std::unique_ptr<InputProfiles> profiles;

    std::array<ConfigureInputPlayer*, 8> player_controllers{};
    ConfigurePlayerAssignment* assignment = nullptr;
    /// Follows the automatic player assignment: the player tabs and the connected boxes are
    /// redrawn when it reloads the controllers while the dialog is open
    QTimer* follow_timer = nullptr;
    unsigned long long followed_generation = 0;
    std::function<void()> mirror_console_mode;
    /// The settings dialog's tab widget whose tab clicks rumble the players' controllers
    QTabWidget* rumble_tabs = nullptr;
    std::array<QWidget*, 8> player_tabs{};
    /// Dragging the player tabs (the automatic modes): the tab bar watched, and the tab order
    /// when the drag began
    QTabWidget* drag_tabs = nullptr;
    QList<QWidget*> tab_order;
    QWidget* dragged_tab = nullptr;
    bool tab_dragging = false;
    std::array<QString, 8> player_tab_titles{};
    std::vector<Common::ControllerConsoleState::PlayerRow> last_rows;
    std::array<QPointer<QToolButton>, 8> tab_lefts;
    std::array<QPointer<QToolButton>, 8> tab_rights;
    /// The tab to select once its player shows (the controller a drag or an arrow moved)
    int pending_select = -1;
    long long pending_select_until = 0;
    // Checkboxes representing the "Connected Controllers".
    std::array<QCheckBox*, 8> connected_controller_checkboxes{};
    ConfigureInputAdvanced* advanced = nullptr;

    Core::System& system;
};

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <QString>
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QFrame;
class QVBoxLayout;
class QLabel;
class QPushButton;
class QTimer;
class QWidget;

/// The Controllers tab of the Controls settings: how players are assigned (by hand, or
/// automatically from the connected controllers), which controller is the handheld's own
/// built-in one, and the live list of players with a swap per row.
class ConfigurePlayerAssignment : public QWidget {
    Q_OBJECT

public:
    explicit ConfigurePlayerAssignment(QWidget* parent = nullptr);
    ~ConfigurePlayerAssignment() override;

    void ApplyConfiguration();

    /// How to bring a player's tab to the front (the settings dialog owns the tabs)
    void SetOpenPlayerTab(std::function<void(int)> open);

    /// Puts a widget above everything else in the tab
    void AddTopWidget(QWidget* widget);

private:
    void LoadConfiguration();
    /// The controller applet mode the controls select
    int SelectedMode() const;
    void OnModeChanged();
    void UpdateModeUi();
    void Refresh();
    void RefreshBuiltin();
    void RefreshPlayers();
    void OnBuiltinPicked(int index);
    void OnIdentifyPressed();
    void OnSwapPressed(int player);
    void RequestSwap(int a, int b);
    void RequestRumble(int player);

    /// Custom frontend or Real applet; the frontend's modes are radio buttons below it
    QComboBox* mode = nullptr;
    QWidget* frontend_row = nullptr;
    QButtonGroup* frontend_modes = nullptr;
    QLabel* mode_info = nullptr;
    QCheckBox* overlay_toggle = nullptr;
    bool original_overlay = false;
    QWidget* now_box = nullptr;

    QWidget* builtin_row = nullptr;
    QComboBox* builtin_picker = nullptr;
    QPushButton* builtin_identify = nullptr;
    QLabel* builtin_status = nullptr;
    QString builtin_signature;

    QWidget* players_box = nullptr;
    QVBoxLayout* players_list = nullptr;
    QLabel* players_note = nullptr;
    QString players_signature;
    /// The player whose Swap was pressed first, waiting for the second (-1: none)
    int swap_from = -1;
    /// The rows and the player each shows, for restyling them while a controller is active
    std::vector<QFrame*> player_rows;
    std::vector<int> row_players;
    /// When each player's row was clicked to rumble (blue for a moment)
    std::map<int, long long> rumbled_at;
    std::function<void(int)> open_player_tab;

    QTimer* timer = nullptr;
    /// The mode when the dialog opened, put back unless the dialog applies
    int original_mode = 0;
    bool applied = false;
};

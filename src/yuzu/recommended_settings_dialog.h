// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>
#include <QDialog>

class ControllerNavigation;
class QCheckBox;
class QGridLayout;
class QPushButton;

namespace Core::HID {
class HIDCore;
}

namespace Settings {
struct GamePreset;
}

// Lists a game's recommended settings next to the values in use, each with a checkbox to pick it.
class RecommendedSettingsList : public QWidget {
    Q_OBJECT

public:
    explicit RecommendedSettingsList(const Settings::GamePreset& preset,
                                     QWidget* parent = nullptr);
    ~RecommendedSettingsList() override;

    // Re-reads the values in use.
    void Refresh();

    std::set<std::string> SelectedKeys() const;
    // Whether any recommendation differs from the value in use.
    bool HasChanges() const;
    void SetAllChecked(bool checked);

signals:
    void SelectionChanged();

private:
    const Settings::GamePreset& preset;
    QGridLayout* grid = nullptr;
    std::vector<std::pair<std::string, QCheckBox*>> rows;
};

// Asks whether to apply a game's recommended settings, listing each one next to the value in use
// so the user can pick which to take.
class RecommendedSettingsDialog : public QDialog {
    Q_OBJECT

public:
    // exec() results; QDialog::Rejected means the window was closed.
    enum Choice { Apply = QDialog::Accepted, Keep = QDialog::Accepted + 1 };

    explicit RecommendedSettingsDialog(const Settings::GamePreset& preset, bool can_save,
                                       Core::HID::HIDCore& hid_core, QWidget* parent = nullptr);
    ~RecommendedSettingsDialog() override;

    std::set<std::string> SelectedKeys() const;
    bool SaveForGame() const;
    bool SkipPrompt() const;

private:
    void UpdateApplyButton();

    RecommendedSettingsList* table = nullptr;
    QCheckBox* save_for_game = nullptr;
    QCheckBox* skip_prompt = nullptr;
    QPushButton* apply_button = nullptr;
    ControllerNavigation* controller_navigation = nullptr;
};

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
class QPushButton;

namespace Core::HID {
class HIDCore;
}

namespace Settings {
struct GamePreset;
}

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

    std::vector<std::pair<std::string, QCheckBox*>> rows;
    QCheckBox* save_for_game = nullptr;
    QCheckBox* skip_prompt = nullptr;
    QPushButton* apply_button = nullptr;
    ControllerNavigation* controller_navigation = nullptr;
};

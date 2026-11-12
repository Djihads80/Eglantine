// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>
#include <QWidget>
#include "common/common_types.h"

class QLabel;
class QPushButton;
class RecommendedSettingsList;

namespace Core {
class System;
}

namespace ConfigurationShared {
class Builder;
}

// Per-game tab with the game's recommended settings and the options that only affect that game.
class ConfigurePerGameTweaks : public QWidget {
    Q_OBJECT

public:
    explicit ConfigurePerGameTweaks(u64 title_id, const Core::System& system_,
                                    const ConfigurationShared::Builder& builder,
                                    QWidget* parent = nullptr);
    ~ConfigurePerGameTweaks() override;

    // Whether the game has recommended settings or tweaks to show.
    static bool HasContent(u64 title_id);

    void ApplyConfiguration();
    // Re-reads the values in use for the recommended settings.
    void RefreshRecommendations();

signals:
    void ApplyRecommendedRequested(const std::set<std::string>& keys);

private:
    void UpdateRecommendedState();

    const Core::System& system;
    std::vector<std::function<void(bool)>> apply_funcs;

    RecommendedSettingsList* recommended_list = nullptr;
    QLabel* recommended_status = nullptr;
    QPushButton* select_all_button = nullptr;
    QPushButton* select_none_button = nullptr;
    QPushButton* apply_button = nullptr;
};

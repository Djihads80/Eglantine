// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>
#include <QObject>
#include <QWidget>
#include "qt_common/util/vk.h"
#include "yuzu/configuration/configuration_shared.h"

class QCheckBox;
class QLabel;

namespace Core {
class System;
}

namespace ConfigurationShared {
class Builder;
}

namespace Performance {

enum class Platform { Nvidia, Amd, Intel, MacOs };

/// Which launch arguments (EDEN_NX2_NAME=VALUE) are switched on. The four platform tabs share it,
/// since the same argument can be recommended on more than one. They are kept in a block of the
/// user's nx2_switches.txt, which the emulator reads at start; lines outside the block are left
/// alone.
class State : public QObject {
    Q_OBJECT

public:
    explicit State(QObject* parent = nullptr);

    [[nodiscard]] bool IsEnabled(const std::string& line) const;
    void SetEnabled(const std::string& line, bool enabled);
    /// Writes the block (called from the settings dialog's OK / Apply).
    void Apply() const;
    [[nodiscard]] static std::filesystem::path SwitchFilePath();

signals:
    void Changed();

private:
    std::set<std::string> enabled;
};

} // namespace Performance

/// One platform's tab: its recommended launch arguments, with what the primary graphics device and
/// this OS support.
class ConfigurePerformance : public QWidget {
    Q_OBJECT

public:
    explicit ConfigurePerformance(Performance::Platform platform_, Performance::State& state_,
                                  const std::vector<VkDeviceInfo::Record>& records,
                                  QWidget* parent = nullptr);
    ~ConfigurePerformance() override;

private:
    struct Row {
        std::string line; ///< NAME=VALUE
        QCheckBox* box{};
    };

    void AddDefaults();
    void Refresh();

    Performance::Platform platform;
    Performance::State& state;
    std::vector<Row> rows;
    std::vector<std::string> recommended_lines; ///< Recommended and supported
};

/// The RAM tab: settings that trade memory use for smoothness (Category::RendererMemory).
class ConfigureRam : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    explicit ConfigureRam(const Core::System& system_, const ConfigurationShared::Builder& builder,
                          QWidget* parent = nullptr);
    ~ConfigureRam() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

private:
    const Core::System& system;
    std::vector<std::function<void(bool)>> apply_funcs;
};

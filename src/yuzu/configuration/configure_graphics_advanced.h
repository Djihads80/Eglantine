// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>
#include <QWidget>
#include "common/common_types.h"
#include "yuzu/configuration/configuration_shared.h"

class QLabel;

namespace Core {
class System;
}

namespace Ui {
class ConfigureGraphicsAdvanced;
}

namespace ConfigurationShared {
class Builder;
class Widget;
}

class ConfigureGraphicsAdvanced : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    explicit ConfigureGraphicsAdvanced(
        const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
        const ConfigurationShared::Builder& builder, QWidget* parent = nullptr);
    ~ConfigureGraphicsAdvanced() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

    void ExposeComputeOption();

    /// Sets the memory of the selected GPU the texture cache memory limit is checked against.
    void SetDeviceMemory(u64 bytes);

private:
    void Setup(const ConfigurationShared::Builder& builder);
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    std::unique_ptr<Ui::ConfigureGraphicsAdvanced> ui;

    const Core::System& system;

    std::vector<std::function<void(bool)>> apply_funcs;

    void UpdateMemoryLimit();

    QWidget* checkbox_enable_compute_pipelines{};
    ConfigurationShared::Widget* memory_limit_widget{};
    QLabel* memory_limit_note{};
    u64 device_memory{};
};

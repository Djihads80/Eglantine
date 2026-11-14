// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <QWidget>
#include "common/common_types.h"
#include "yuzu/configuration/configuration_shared.h"

class QCheckBox;
class QLineEdit;
class QComboBox;
class QDateTimeEdit;
namespace Core {
class System;
}

namespace Ui {
class ConfigureSystem;
}

namespace ConfigurationShared {
class Builder;
class Widget;
}

class ConfigureSystem : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    explicit ConfigureSystem(Core::System& system_,
                             std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
                             const ConfigurationShared::Builder& builder,
                             QWidget* parent = nullptr, u64 program_id = 0);
    ~ConfigureSystem() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    void Setup(const ConfigurationShared::Builder& builder);

    void UpdateRtcTime();

    std::vector<std::function<void(bool)>> apply_funcs{};

    std::unique_ptr<Ui::ConfigureSystem> ui;
    bool enabled = false;

    Core::System& system;
    const u64 program_id;

    QComboBox* combo_region;
    QComboBox* combo_language;
    ConfigurationShared::Widget* widget_rtc{nullptr};
    QCheckBox* checkbox_rtc{nullptr};
    QDateTimeEdit* date_rtc{nullptr};
    s64 rtc_offset{0};
    bool rtc_using_global{true};
    u64 previous_time;
};

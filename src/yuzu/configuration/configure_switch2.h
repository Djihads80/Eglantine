// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <QString>
#include <QWidget>
#include "common/settings_common.h"
#include "yuzu/configuration/configuration_shared.h"

namespace Core {
class System;
}

namespace ConfigurationShared {
class Builder;
}

/// The settings of one category that only Switch 2 titles use, in a group box (the Switch 2 and
/// Switch 2 debug tabs of the Switch 2 Settings page)
class ConfigureSwitch2 : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    explicit ConfigureSwitch2(const Core::System& system_, Settings::Category category,
                              const QString& name, const QString& title,
                              const QString& description,
                              std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
                              const ConfigurationShared::Builder& builder,
                              QWidget* parent = nullptr);
    ~ConfigureSwitch2() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

private:
    const Core::System& system;

    std::vector<std::function<void(bool)>> apply_funcs;
};

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <QWidget>
#include "common/common_types.h"
#include "yuzu/configuration/configuration_shared.h"

namespace Core {
class System;
}

namespace ConfigurationShared {
class Builder;
}

class ConfigureFrameGeneration : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    explicit ConfigureFrameGeneration(
        const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
        const ConfigurationShared::Builder& builder, QWidget* parent = nullptr);
    ~ConfigureFrameGeneration() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

    // Settings shown here rather than on the Graphics tab.
    static bool IsFrameGenerationSetting(u32 id);

private:
    const Core::System& system;
    std::vector<std::function<void(bool)>> apply_funcs;
};

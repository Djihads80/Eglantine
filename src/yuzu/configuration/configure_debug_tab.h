// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <QWidget>

class ConfigureDebug;
class ConfigureCpuDebug;
class ConfigureMcp;

namespace Core {
class System;
}

namespace Ui {
class ConfigureDebugTab;
}

class ConfigureDebugTab : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureDebugTab(const Core::System& system_, QWidget* parent = nullptr);
    ~ConfigureDebugTab() override;

    void ApplyConfiguration();

    void SetCurrentIndex(int index);

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    void SetConfiguration();

    std::unique_ptr<Ui::ConfigureDebugTab> ui;

    std::unique_ptr<ConfigureDebug> debug_tab;
    std::unique_ptr<ConfigureCpuDebug> cpu_debug_tab;
#ifdef YUZU_ENABLE_MCP
    std::unique_ptr<ConfigureMcp> mcp_tab;
#endif
};

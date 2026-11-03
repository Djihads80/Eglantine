// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <QScrollArea>

class QCheckBox;
class QLineEdit;
class QSpinBox;

namespace Core {
class System;
}

namespace Ui {
class ConfigureDebug;
}

class ConfigureDebug : public QScrollArea {
    Q_OBJECT

public:
    explicit ConfigureDebug(const Core::System& system_, QWidget* parent = nullptr);
    ~ConfigureDebug() override;

    void ApplyConfiguration();

private:
    void changeEvent(QEvent* event) override;

    void RetranslateUI();
    void SetConfiguration();

    std::unique_ptr<Ui::ConfigureDebug> ui;

    const Core::System& system;

    bool crash_dump_warning_shown{false};

#ifdef YUZU_ENABLE_MCP
    /// Adds the MCP server settings under the GDB stub in the Debugger group.
    void SetupMcpWidgets();

    QCheckBox* mcp_enable = nullptr;
    QSpinBox* mcp_port = nullptr;
    QCheckBox* mcp_writes = nullptr;
    QLineEdit* mcp_token = nullptr;
#endif
};

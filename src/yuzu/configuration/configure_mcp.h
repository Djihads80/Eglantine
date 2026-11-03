// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

class QCheckBox;
class QLineEdit;
class QSpinBox;
class QWidget;

/// The MCP tab of Configure > Debug: built-in MCP server on/off, port, Allow Writes and token.
/// The main window applies changes to a running server when the dialog closes.
class ConfigureMcp : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureMcp(QWidget* parent = nullptr);
    ~ConfigureMcp() override;

    /// Writes the fields to the settings.
    void ApplyConfiguration();

private:
    /// Fills the fields from the settings.
    void SetConfiguration();
    /// Enables the dependent fields only while the server is enabled.
    void UpdateEnabledState();

    QCheckBox* enable = nullptr;
    QSpinBox* port = nullptr;
    QCheckBox* writes = nullptr;
    QLineEdit* token = nullptr;
    QWidget* details = nullptr;
};

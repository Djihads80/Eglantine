// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

#include "common/settings.h"
#include "yuzu/configuration/configure_mcp.h"

ConfigureMcp::ConfigureMcp(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* group = new QGroupBox(tr("MCP Server"), this);
    auto* group_layout = new QVBoxLayout(group);

    auto* intro = new QLabel(
        tr("A local server that lets AI agents and scripts drive and inspect the emulator (Model "
           "Context Protocol). Only programs on this computer can connect."),
        group);
    intro->setWordWrap(true);
    group_layout->addWidget(intro);

    enable = new QCheckBox(tr("Enable MCP Server"), group);
    group_layout->addWidget(enable);

    details = new QWidget(group);
    auto* form = new QFormLayout(details);
    form->setContentsMargins(0, 0, 0, 0);
    port = new QSpinBox(details);
    port->setRange(1, 65535);
    form->addRow(tr("Port:"), port);
    writes = new QCheckBox(tr("Allow Writes (input, memory writes, settings)"), details);
    form->addRow(writes);
    token = new QLineEdit(details);
    token->setEchoMode(QLineEdit::Password);
    token->setPlaceholderText(tr("Optional: clients must send it as a bearer token"));
    form->addRow(tr("Token:"), token);
    group_layout->addWidget(details);

    auto* note = new QLabel(
        tr("Without Allow Writes, agents can look at the emulator and boot, pause or stop games, "
           "but not send input or change memory and settings. Changes apply when you press OK."),
        group);
    note->setWordWrap(true);
    group_layout->addWidget(note);

    layout->addWidget(group);
    layout->addStretch();

    connect(enable, &QCheckBox::toggled, this, [this] { UpdateEnabledState(); });
    SetConfiguration();
}

ConfigureMcp::~ConfigureMcp() = default;

void ConfigureMcp::SetConfiguration() {
    enable->setChecked(Settings::values.enable_mcp_server.GetValue());
    port->setValue(Settings::values.mcp_port.GetValue());
    writes->setChecked(Settings::values.mcp_allow_writes.GetValue());
    token->setText(QString::fromStdString(Settings::values.mcp_token.GetValue()));
    UpdateEnabledState();
}

void ConfigureMcp::UpdateEnabledState() {
    details->setEnabled(enable->isChecked());
}

void ConfigureMcp::ApplyConfiguration() {
    Settings::values.enable_mcp_server = enable->isChecked();
    Settings::values.mcp_port = static_cast<u16>(port->value());
    Settings::values.mcp_allow_writes = writes->isChecked();
    Settings::values.mcp_token = token->text().toStdString();
}

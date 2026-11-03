// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>
#include "common/settings.h"
#include "core/core.h"
#include "yuzu/configuration/configure_switch2.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureSwitch2::ConfigureSwitch2(const Core::System& system_, Settings::Category category,
                                   const QString& name, const QString& title,
                                   const QString& description,
                                   std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
                                   const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), system{system_} {
    setAccessibleName(name);

    auto* const layout = new QVBoxLayout(this);
    auto* const group_box = new QGroupBox(title, this);
    auto* const group_layout = new QVBoxLayout(group_box);

    if (!description.isEmpty()) {
        auto* const label = new QLabel(description, group_box);
        label->setWordWrap(true);
        group_layout->addWidget(label);
    }

    // A map keeps the settings in their declaration order
    std::map<u32, QWidget*> hold;
    for (auto* const setting : Settings::values.linkage.by_category[category]) {
        auto* const widget = builder.BuildWidget(setting, apply_funcs);
        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }
        hold.emplace(setting->Id(), widget);
    }
    for (const auto& [id, widget] : hold) {
        group_layout->addWidget(widget);
    }

    layout->addWidget(group_box);
    layout->addStretch();
}

ConfigureSwitch2::~ConfigureSwitch2() = default;

void ConfigureSwitch2::SetConfiguration() {}

void ConfigureSwitch2::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}

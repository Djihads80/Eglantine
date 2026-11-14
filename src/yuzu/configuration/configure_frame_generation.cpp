// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <map>
#include <QCheckBox>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include "common/settings.h"
#include "core/core.h"
#include "yuzu/configuration/configure_frame_generation.h"
#include "yuzu/configuration/shared_widget.h"

namespace {

// Settings that only matter while frame generation is on.
std::array<u32, 6> DependentSettings() {
    return {Settings::values.frame_gen_type.Id(),         Settings::values.frame_gen_multiplier.Id(),
            Settings::values.frame_gen_target_rate.Id(),  Settings::values.frame_gen_flow_scale_auto.Id(),
            Settings::values.frame_gen_flow_scale.Id(),   Settings::values.frame_gen_queue_target.Id()};
}

} // namespace

ConfigureFrameGeneration::ConfigureFrameGeneration(
    const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), system{system_} {
    setAccessibleName(tr("Frame Generation"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    scroll->setWidget(content);

    auto* box = new QGroupBox(tr("Frame Generation"), content);
    auto* box_layout = new QVBoxLayout(box);
    auto* description = new QLabel(
        tr("Inserts generated frames between the frames a game renders, for smoother motion at "
           "the cost of some latency and artifacts around fast-moving edges."),
        box);
    description->setWordWrap(true);
    box_layout->addWidget(description);
    layout->addWidget(box);
    layout->addStretch(1);

    std::map<u32, ConfigurationShared::Widget*> widgets;
    for (auto* setting : Settings::values.linkage.by_category[Settings::Category::Renderer]) {
        if (!IsFrameGenerationSetting(setting->Id())) {
            continue;
        }
        auto* widget = builder.BuildWidget(setting, apply_funcs);
        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }
        widgets.emplace(setting->Id(), widget);
    }
    for (const auto& [id, widget] : widgets) {
        box_layout->addWidget(widget);
    }

    // Hide rather than disable, a disabled widget would read as using the global value
    if (const auto it = widgets.find(Settings::values.frame_gen.Id());
        it != widgets.end() && it->second->checkbox != nullptr) {
        QCheckBox* enabled = it->second->checkbox;
        for (const u32 id : DependentSettings()) {
            if (const auto dep = widgets.find(id); dep != widgets.end()) {
                dep->second->setVisible(enabled->isChecked());
                connect(enabled, &QCheckBox::toggled, dep->second, &QWidget::setVisible);
            }
        }
    }
}

ConfigureFrameGeneration::~ConfigureFrameGeneration() = default;

bool ConfigureFrameGeneration::IsFrameGenerationSetting(u32 id) {
    if (id == Settings::values.frame_gen.Id() || id == Settings::values.frame_gen_dump_flow.Id()) {
        return true;
    }
    for (const u32 dependent : DependentSettings()) {
        if (id == dependent) {
            return true;
        }
    }
    return false;
}

void ConfigureFrameGeneration::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}

void ConfigureFrameGeneration::SetConfiguration() {}

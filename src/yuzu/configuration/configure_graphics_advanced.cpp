// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>
#include <QLabel>
#include <QSpinBox>
#include <qnamespace.h>
#include "common/settings.h"
#include "core/core.h"
#include "qt_common/config/shared_translation.h"
#include "ui_configure_graphics_advanced.h"
#include "video_core/texture_cache/memory_limits.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_graphics_advanced.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureGraphicsAdvanced::ConfigureGraphicsAdvanced(
    const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui{std::make_unique<Ui::ConfigureGraphicsAdvanced>()}, system{system_} {

    ui->setupUi(this);

    Setup(builder);

    SetConfiguration();

    checkbox_enable_compute_pipelines->setVisible(false);
}

ConfigureGraphicsAdvanced::~ConfigureGraphicsAdvanced() = default;

void ConfigureGraphicsAdvanced::SetConfiguration() {}

void ConfigureGraphicsAdvanced::Setup(const ConfigurationShared::Builder& builder) {
    auto& normal_layout = *ui->normal_target->layout();

    // A map will sort the data for us
    std::map<u32, QWidget*> normal_hold{};

    for (auto setting :
         Settings::values.linkage.by_category[Settings::Category::RendererAdvanced]) {
        const bool is_memory_limit = setting->Id() == Settings::values.texture_cache_memory_limit.Id();
        ConfigurationShared::Widget* widget =
            is_memory_limit
                ? builder.BuildWidget(setting, apply_funcs, ConfigurationShared::RequestType::Default,
                                      true, 1.0f, nullptr, tr(" MiB"))
                : builder.BuildWidget(setting, apply_funcs);

        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }

        const auto id = setting->Id();

        normal_hold.emplace(id, widget);

        if (is_memory_limit && widget->spinbox != nullptr) {
            memory_limit_widget = widget;
            memory_limit_widget->spinbox->setSpecialValueText(tr("Auto"));
            memory_limit_widget->spinbox->setSingleStep(256);
            memory_limit_note = new QLabel(this);
            memory_limit_note->setWordWrap(true);
            memory_limit_note->setTextFormat(Qt::RichText);
            connect(memory_limit_widget->spinbox, qOverload<int>(&QSpinBox::valueChanged), this,
                    [this] { UpdateMemoryLimit(); });
        }

        // Keep track of enable_compute_pipelines so we can display it when needed
        if (setting->Id() == Settings::values.enable_compute_pipelines.Id()) {
            checkbox_enable_compute_pipelines = widget;
        }
    }

    for (const auto& [id, widget] : normal_hold) {
        normal_layout.addWidget(widget);
        if (widget == memory_limit_widget) {
            normal_layout.addWidget(memory_limit_note);
        }
    }
    UpdateMemoryLimit();
}

void ConfigureGraphicsAdvanced::SetDeviceMemory(u64 bytes) {
    device_memory = bytes;
    UpdateMemoryLimit();
}

void ConfigureGraphicsAdvanced::UpdateMemoryLimit() {
    if (memory_limit_widget == nullptr) {
        return;
    }
    QSpinBox* const spinbox = memory_limit_widget->spinbox;
    if (device_memory == 0) {
        memory_limit_note->hide();
        return;
    }
    const int device_mib = static_cast<int>((std::min<u64>)(device_memory >> 20, 1048576));
    const int recommended_mib =
        static_cast<int>(VideoCommon::TextureCacheMemory::RecommendedLimit(device_memory) >> 20);
    // The spinbox keeps the setting's range until a device is known; then the GPU is the ceiling.
    spinbox->setMaximum(device_mib);

    const int value = spinbox->value();
    const QString below =
        tr("<span style=\"color:#d29922\">&#9888;</span> Below the recommended %1 MiB for this "
           "GPU: textures may be removed and loaded again, which can cause stutter.")
            .arg(recommended_mib);
    const QString auto_note = tr("Auto uses %1 MiB (three quarters of this GPU's %2 MiB).")
                                  .arg(recommended_mib)
                                  .arg(device_mib);
    const QString low_note = tr("Values under 1024 MiB are raised to 1024 MiB.");
    if (value == 0) {
        memory_limit_note->setText(auto_note);
    } else if (value < recommended_mib) {
        memory_limit_note->setText(value < 1024 ? below + QStringLiteral(" ") + low_note : below);
    } else {
        memory_limit_note->setText(QString{});
    }
    memory_limit_note->setVisible(!memory_limit_note->text().isEmpty());
}

void ConfigureGraphicsAdvanced::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}

void ConfigureGraphicsAdvanced::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGraphicsAdvanced::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureGraphicsAdvanced::ExposeComputeOption() {
    checkbox_enable_compute_pipelines->setVisible(true);
}

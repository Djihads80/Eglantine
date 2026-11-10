// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <optional>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QFileDialog>
#include <QGraphicsItem>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>

#include "common/settings.h"
#include "core/core.h"
#include "qt_common/qt_compat.h"
#include "ui_configure_system.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_system.h"
#include "yuzu/configuration/shared_widget.h"

constexpr std::array<u32, 7> LOCALE_BLOCKLIST{
    // pzzefezrpnkzeidfej
    // thhsrnhutlohsternp
    // BHH4CG          U
    // Raa1AB          S
    //  nn9
    //  ts
    0b0100011100001100000, // Japan
    0b0000001101001100100, // Americas
    0b0100110100001000010, // Europe
    0b0100110100001000010, // Australia
    0b0000000000000000000, // China
    0b0100111100001000000, // Korea
    0b0100111100001000000, // Taiwan
};

static bool IsValidLocale(u32 region_index, u32 language_index) {
    if (region_index >= LOCALE_BLOCKLIST.size()) {
        return false;
    }
    return ((LOCALE_BLOCKLIST.at(region_index) >> language_index) & 1) == 0;
}

ConfigureSystem::ConfigureSystem(Core::System& system_,
                                 std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
                                 const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui{std::make_unique<Ui::ConfigureSystem>()}, system{system_} {
    ui->setupUi(this);

    if (!Settings::IsConfiguringGlobal()) {
        const bool using_global = Settings::values.custom_rtc_offset.UsingGlobal() &&
                                  Settings::values.custom_rtc_enabled.UsingGlobal();
        Settings::values.custom_rtc.SetGlobal(using_global);
    }

    rtc_offset = Settings::values.custom_rtc_offset.GetValue();

    const auto posix_time = std::chrono::system_clock::now().time_since_epoch();
    const auto current_time_s =
        std::chrono::duration_cast<std::chrono::seconds>(posix_time).count();
    previous_time = static_cast<u64>((std::max<s64>)(0, current_time_s + rtc_offset));

    Setup(builder);

    const auto locale_check = [this]() {
        const auto region_index = combo_region->currentIndex();
        const auto language_index = combo_language->currentIndex();
        const bool valid_locale = IsValidLocale(region_index, language_index);
        ui->label_warn_invalid_locale->setVisible(!valid_locale);
        if (!valid_locale) {
            ui->label_warn_invalid_locale->setText(
                tr("Warning: \"%1\" is not a valid language for region \"%2\"")
                    .arg(combo_language->currentText())
                    .arg(combo_region->currentText()));
        }
    };

    const auto update_date_offset = [this]() {
        if (!checkbox_rtc || !checkbox_rtc->isChecked()) {
            return;
        }
        const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        const auto selected_s = date_rtc->dateTime().toSecsSinceEpoch();
        rtc_offset = static_cast<s64>(selected_s - now_s);
        previous_time = static_cast<u64>((std::max<s64>)(0, selected_s));

        if (!Settings::IsConfiguringGlobal() && widget_rtc && widget_rtc->restore_button) {
            widget_rtc->restore_button->setEnabled(true);
            widget_rtc->restore_button->setVisible(true);
        }
    };
    const auto update_rtc_date = [this](int _) {
        UpdateRtcTime();
    };

    connect(combo_language, qOverload<int>(&QComboBox::currentIndexChanged), this, locale_check);
    connect(combo_region, qOverload<int>(&QComboBox::currentIndexChanged), this, locale_check);
    if (checkbox_rtc) {
        connect(checkbox_rtc, qOverload<CHECKSTATE_TYPE>(&QCheckBox::STATE_CHANGED), this,
                update_rtc_date);
    }
    if (date_rtc) {
        connect(date_rtc, &QDateTimeEdit::dateTimeChanged, this, update_date_offset);
    }

    if (!Settings::IsConfiguringGlobal() && widget_rtc && widget_rtc->restore_button) {
        connect(widget_rtc->restore_button, &QAbstractButton::clicked, this, [this]() {
            rtc_offset = static_cast<s64>(std::strtoll(
                Settings::values.custom_rtc_offset.ToStringGlobal().c_str(), nullptr, 0));
            const bool global_enabled =
                Settings::values.custom_rtc_enabled.ToStringGlobal() == "true";
            if (checkbox_rtc) {
                const QSignalBlocker blocker(checkbox_rtc);
                checkbox_rtc->setChecked(global_enabled);
            }
            UpdateRtcTime();
        });
    }

    ui->label_warn_invalid_locale->setVisible(false);
    locale_check();

    SetConfiguration();
    UpdateRtcTime();
}

ConfigureSystem::~ConfigureSystem() = default;

void ConfigureSystem::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureSystem::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureSystem::Setup(const ConfigurationShared::Builder& builder) {
    auto& core_layout = *ui->core_widget->layout();
    auto& system_layout = *ui->system_widget->layout();

    std::map<u32, QWidget*> core_hold{};
    std::map<u32, QWidget*> system_hold{};

    std::vector<Settings::BasicSetting*> settings;
    auto push = [&settings](auto& list) {
        for (auto setting : list) {
            settings.push_back(setting);
        }
    };

    push(Settings::values.linkage.by_category[Settings::Category::Core]);
    push(Settings::values.linkage.by_category[Settings::Category::System]);

    for (auto setting : settings) {
        if (setting->Id() == Settings::values.use_docked_mode.Id() &&
            Settings::IsConfiguringGlobal()) {
            continue;
        }

        // custom_rtc_offset is managed directly in ConfigureSystem alongside custom_rtc
        if (setting->Id() == Settings::values.custom_rtc_offset.Id()) {
            continue;
        }

        ConfigurationShared::Widget* widget = builder.BuildWidget(setting, apply_funcs);

        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }

        // Keep track of the region_index (and language_index) combobox to validate the selected
        // settings
        if (setting->Id() == Settings::values.region_index.Id()) {
            combo_region = widget->combobox;
        }

        if (setting->Id() == Settings::values.language_index.Id()) {
            combo_language = widget->combobox;
        }

        if (setting->Id() == Settings::values.custom_rtc.Id()) {
            widget_rtc = widget;
            checkbox_rtc = widget->checkbox;
            date_rtc = widget->date_time_edit;
        }

        switch (setting->GetCategory()) {
        case Settings::Category::Core:
            core_hold.emplace(setting->Id(), widget);
            break;
        case Settings::Category::System:
            system_hold.emplace(setting->Id(), widget);
            break;
        default:
            widget->deleteLater();
        }
    }
    for (const auto& [label, widget] : core_hold) {
        core_layout.addWidget(widget);
    }
    for (const auto& [id, widget] : system_hold) {
        system_layout.addWidget(widget);
    }
}

void ConfigureSystem::UpdateRtcTime() {
    if (!checkbox_rtc || !date_rtc) {
        return;
    }

    const auto posix_time = std::chrono::system_clock::now().time_since_epoch();
    const auto current_time_s =
        std::chrono::duration_cast<std::chrono::seconds>(posix_time).count();

    s64 target_time = current_time_s;
    if (checkbox_rtc->isChecked()) {
        target_time += rtc_offset;
    }
    previous_time = static_cast<u64>((std::max<s64>)(0, target_time));

    const auto date = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(previous_time));
    const QSignalBlocker blocker(date_rtc);
    date_rtc->setDateTime(date);
}

void ConfigureSystem::SetConfiguration() {}

void ConfigureSystem::ApplyConfiguration() {
    const bool powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(powered_on);
    }

    if (checkbox_rtc && checkbox_rtc->isChecked() && date_rtc) {
        const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        rtc_offset = static_cast<s64>(date_rtc->dateTime().toSecsSinceEpoch() - now_s);
    }

    if (Settings::IsConfiguringGlobal()) {
        Settings::values.custom_rtc_offset.SetValue(rtc_offset);
    } else {
        const bool using_global = widget_rtc && widget_rtc->restore_button
                                      ? !widget_rtc->restore_button->isEnabled()
                                      : true;
        Settings::values.custom_rtc_offset.SetGlobal(using_global);
        if (!using_global) {
            Settings::values.custom_rtc_offset.SetValue(rtc_offset);
        }
    }

    UpdateRtcTime();
}

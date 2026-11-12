// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <numeric>
#include <QAbstractButton>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include "common/game_presets.h"
#include "common/settings.h"
#include "qt_common/config/shared_translation.h"
#include "yuzu/recommended_settings_dialog.h"
#include "yuzu/util/controller_navigation.h"

namespace {

struct SettingText {
    QString label;
    QString current;
    QString recommended;
    bool already_set;
};

std::vector<SettingText> DescribeSettings(const Settings::GamePreset& preset) {
    const auto labels = ConfigurationShared::InitializeTranslations(nullptr);
    const auto enums = ConfigurationShared::ComboboxEnumeration(nullptr);
    // The graphics configuration populates VSync choices separately from the shared translations.
    enums->emplace(Settings::EnumMetadata<Settings::VSyncMode>::Index(),
                   ConfigurationShared::ComboboxTranslations{
                       {static_cast<u32>(Settings::VSyncMode::Immediate),
                        QCoreApplication::translate("RecommendedSettingsDialog", "Immediate")},
                       {static_cast<u32>(Settings::VSyncMode::Mailbox), QStringLiteral("Mailbox")},
                       {static_cast<u32>(Settings::VSyncMode::Fifo), QStringLiteral("FIFO")},
                       {static_cast<u32>(Settings::VSyncMode::FifoRelaxed),
                        QStringLiteral("FIFO Relaxed")}});

    std::vector<SettingText> result;
    result.reserve(preset.settings.size());
    for (const auto& recommendation : preset.settings) {
        const auto* setting = Settings::values.linkage.by_key.at(recommendation.key);
        QString label = QString::fromStdString(recommendation.key);
        if (const auto it = labels->find(setting->Id());
            it != labels->end() && !it->second.first.isEmpty()) {
            label = it->second.first;
        }
        if (label.endsWith(QLatin1Char(':'))) {
            label.chop(1);
        }

        const auto format = [&](const std::string& encoded) {
            if (setting->IsEnum()) {
                if (const auto it = enums->find(setting->EnumIndex()); it != enums->end()) {
                    const auto enum_value = static_cast<u32>(std::stoul(encoded));
                    for (const auto& [id, text] : it->second) {
                        if (id == enum_value) {
                            return text;
                        }
                    }
                }
            } else if (setting->TypeId() == "bool") {
                return encoded == "true"
                           ? QCoreApplication::translate("RecommendedSettingsDialog", "On")
                           : QCoreApplication::translate("RecommendedSettingsDialog", "Off");
            }
            return QString::fromStdString(encoded);
        };

        const std::string current = setting->ToString();
        result.push_back({label, format(current), format(recommendation.value),
                          current == recommendation.value});
    }
    return result;
}

} // namespace

RecommendedSettingsList::RecommendedSettingsList(const Settings::GamePreset& preset_,
                                                 QWidget* parent)
    : QWidget(parent), preset{preset_} {
    grid = new QGridLayout(this);
    grid->setContentsMargins(12, 8, 12, 8);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(0, 1);
    Refresh();
}

RecommendedSettingsList::~RecommendedSettingsList() = default;

void RecommendedSettingsList::Refresh() {
    while (QLayoutItem* item = grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    rows.clear();

    // Setting | Current | -> | Recommended
    const auto add_header = [&](int column, const QString& text) {
        auto* header = new QLabel(text, this);
        QFont font = header->font();
        font.setBold(true);
        header->setFont(font);
        grid->addWidget(header, 0, column);
    };
    add_header(0, tr("Setting"));
    add_header(1, tr("Current"));
    add_header(3, tr("Recommended"));
    auto* rule = new QFrame(this);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Sunken);
    grid->addWidget(rule, 1, 0, 1, 4);

    const auto descriptions = DescribeSettings(preset);
    // List the settings that would change first.
    std::vector<size_t> order(descriptions.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::ranges::stable_partition(order,
                                  [&](size_t i) { return !descriptions[i].already_set; });
    int row = 2;
    for (const size_t i : order) {
        const auto& text = descriptions[i];
        auto* check = new QCheckBox(text.label, this);
        auto* current = new QLabel(text.current, this);
        auto* arrow = new QLabel(QStringLiteral("\u2192"), this);
        auto* recommended = new QLabel(text.recommended, this);
        if (text.already_set) {
            check->setChecked(false);
            check->setEnabled(false);
            check->setToolTip(tr("Already using the recommended value."));
            current->setEnabled(false);
            arrow->setEnabled(false);
            recommended->setText(tr("Already set"));
            recommended->setEnabled(false);
        } else {
            check->setChecked(true);
            QFont font = recommended->font();
            font.setBold(true);
            recommended->setFont(font);
            connect(check, &QCheckBox::toggled, this, [this, arrow, recommended](bool on) {
                arrow->setEnabled(on);
                recommended->setEnabled(on);
                emit SelectionChanged();
            });
        }
        grid->addWidget(check, row, 0);
        grid->addWidget(current, row, 1);
        grid->addWidget(arrow, row, 2);
        grid->addWidget(recommended, row, 3);
        rows.emplace_back(preset.settings[i].key, check);
        ++row;
    }
    emit SelectionChanged();
}

std::set<std::string> RecommendedSettingsList::SelectedKeys() const {
    std::set<std::string> keys;
    for (const auto& [key, check] : rows) {
        if (check->isEnabled() && check->isChecked()) {
            keys.insert(key);
        }
    }
    return keys;
}

bool RecommendedSettingsList::HasChanges() const {
    return std::ranges::any_of(rows, [](const auto& entry) { return entry.second->isEnabled(); });
}

void RecommendedSettingsList::SetAllChecked(bool checked) {
    for (const auto& [key, check] : rows) {
        if (check->isEnabled()) {
            check->setChecked(checked);
        }
    }
}

RecommendedSettingsDialog::RecommendedSettingsDialog(const Settings::GamePreset& preset,
                                                     bool can_save, Core::HID::HIDCore& hid_core,
                                                     QWidget* parent)
    : QDialog(parent) {
    const QString game_name = QString::fromUtf8(preset.name);
    setWindowTitle(tr("Recommended settings for %1").arg(game_name));

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 20, 20, 16);

    auto* title = new QLabel(tr("Recommended settings for %1").arg(game_name), this);
    QFont title_font = title->font();
    title_font.setPointSizeF(title_font.pointSizeF() * 1.35);
    title_font.setBold(true);
    title->setFont(title_font);
    layout->addWidget(title);

    auto* intro = new QLabel(tr("These settings are known to work well with %1. Untick any "
                                "setting you want to keep as it is.")
                                 .arg(game_name),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    table = new RecommendedSettingsList(preset);
    connect(table, &RecommendedSettingsList::SelectionChanged, this,
            &RecommendedSettingsDialog::UpdateApplyButton);

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(table);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Show every row without scrolling unless the list outgrows the screen.
    const QSize table_size = table->sizeHint();
    const int frame = scroll->frameWidth() * 2;
    scroll->setMinimumWidth(table_size.width() + frame +
                            scroll->verticalScrollBar()->sizeHint().width());
    scroll->setMinimumHeight(std::min(table_size.height() + frame, 420));
    layout->addWidget(scroll, 1);

    if (!table->HasChanges()) {
        auto* note = new QLabel(tr("All recommended settings are already in use."), this);
        note->setWordWrap(true);
        layout->addWidget(note);
    }

    auto* options = new QGroupBox(tr("Options"), this);
    auto* options_layout = new QVBoxLayout(options);
    save_for_game = new QCheckBox(tr("Save the selected settings to this game's configuration"),
                                  options);
    save_for_game->setEnabled(can_save);
    save_for_game->setToolTip(
        can_save ? tr("Otherwise they are only used for this launch.")
                 : tr("This game has no per-game configuration to save to."));
    options_layout->addWidget(save_for_game);
    skip_prompt = new QCheckBox(tr("Don't ask again for this game"), options);
    skip_prompt->setToolTip(tr("Future launches use your saved settings without asking."));
    options_layout->addWidget(skip_prompt);
    layout->addWidget(options);

    auto* buttons = new QDialogButtonBox(this);
    auto* keep_button = buttons->addButton(tr("Keep Current Settings"), QDialogButtonBox::RejectRole);
    apply_button = buttons->addButton(tr("Apply Selected"), QDialogButtonBox::AcceptRole);
    keep_button->setDefault(true);
    // Closing the window (reject) cancels the launch, so keeping settings has its own result.
    connect(apply_button, &QPushButton::clicked, this, [this] { done(Apply); });
    connect(keep_button, &QPushButton::clicked, this, [this] { done(Keep); });
    layout->addWidget(buttons);
    UpdateApplyButton();

    controller_navigation = new ControllerNavigation(hid_core, this);
    connect(controller_navigation, &ControllerNavigation::TriggerKeyboardEvent, this,
            [this](Qt::Key key) {
                if (key == Qt::Key_Down || key == Qt::Key_Right) {
                    key = Qt::Key_Tab;
                } else if (key == Qt::Key_Up || key == Qt::Key_Left) {
                    key = Qt::Key_Backtab;
                } else if (key == Qt::Key_Enter || key == Qt::Key_Return) {
                    if (auto* button = qobject_cast<QAbstractButton*>(focusWidget())) {
                        button->click();
                        return;
                    }
                }
                QWidget* target = focusWidget();
                QCoreApplication::postEvent(target ? target : this,
                                            new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier));
            });

    // Size to the content, but never past most of the screen; the setting list scrolls instead.
    layout->activate();
    QSize size = sizeHint().expandedTo(QSize(560, 0));
    if (const QScreen* screen = parent ? parent->screen() : this->screen()) {
        const QSize available = screen->availableGeometry().size() * 0.85;
        size = size.boundedTo(available);
        scroll->setMinimumHeight(std::min(scroll->minimumHeight(), available.height() / 3));
        scroll->setMinimumWidth(std::min(scroll->minimumWidth(), available.width() - 40));
    }
    resize(size);
    keep_button->setFocus();
}

RecommendedSettingsDialog::~RecommendedSettingsDialog() = default;

std::set<std::string> RecommendedSettingsDialog::SelectedKeys() const {
    return table->SelectedKeys();
}

bool RecommendedSettingsDialog::SaveForGame() const {
    return save_for_game->isEnabled() && save_for_game->isChecked();
}

bool RecommendedSettingsDialog::SkipPrompt() const {
    return skip_prompt->isChecked();
}

void RecommendedSettingsDialog::UpdateApplyButton() {
    apply_button->setEnabled(!SelectedKeys().empty());
}

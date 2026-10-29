// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <map>
#include <memory>
#include <thread>

#include "common/settings.h"
#include "qt_common/config/uisettings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/sm/sm.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "qt_common/qt_compat.h"
#include "ui_configure_input.h"
#include "ui_configure_input_advanced.h"
#include "ui_configure_input_player.h"
#include "yuzu/configuration/configure_camera.h"
#include "yuzu/configuration/configure_debug_controller.h"
#include "yuzu/configuration/configure_input.h"
#include "yuzu/configuration/configure_input_advanced.h"
#include "yuzu/configuration/configure_player_assignment.h"
#include "common/controller_console_state.h"
#include <mutex>
#include <QTimer>
#include <vector>
#include <algorithm>
#include <QTabWidget>
#include <QButtonGroup>
#include <QRadioButton>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QTabBar>
#include <QCursor>
#include <QApplication>
#include <QPainter>
#include <QStyleOptionTab>
#include <QProxyStyle>
#include <QDateTime>
#include <QEvent>
#include <QMouseEvent>
#include <QToolButton>
#include "yuzu/configuration/configure_input_player.h"
#include "yuzu/configuration/configure_motion_touch.h"
#include "yuzu/configuration/configure_ringcon.h"
#include "yuzu/configuration/configure_touchscreen_advanced.h"
#include "yuzu/configuration/configure_vibration.h"
#include "yuzu/configuration/input_profiles.h"

namespace {
template <typename Dialog, typename... Args>
void CallConfigureDialog(ConfigureInput& parent, Args&&... args) {
    Dialog dialog(&parent, std::forward<Args>(args)...);

    const auto res = dialog.exec();
    if (res == QDialog::Accepted) {
        dialog.ApplyConfiguration();
    }
}
} // Anonymous namespace

void OnDockedModeChanged(bool last_state, bool new_state, Core::System& system) {
    if (last_state == new_state) {
        return;
    }

    if (!system.IsPoweredOn()) {
        return;
    }

    system.GetAppletManager().OperationModeChanged();
}

ConfigureInput::ConfigureInput(Core::System& system_, QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureInput>()),
      profiles(std::make_unique<InputProfiles>()), system{system_} {
    ui->setupUi(this);
}

ConfigureInput::~ConfigureInput() = default;

void ConfigureInput::Initialize(InputCommon::InputSubsystem* input_subsystem,
                                std::size_t max_players) {
    const bool is_powered_on = system.IsPoweredOn();
    auto& hid_core = system.HIDCore();
    player_controllers = {
        new ConfigureInputPlayer(this, 0, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 1, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 2, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 3, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 4, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 5, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 6, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
        new ConfigureInputPlayer(this, 7, ui->consoleInputSettings, input_subsystem, profiles.get(),
                                 hid_core, is_powered_on),
    };

    player_tabs = {
        ui->tabPlayer1, ui->tabPlayer2, ui->tabPlayer3, ui->tabPlayer4,
        ui->tabPlayer5, ui->tabPlayer6, ui->tabPlayer7, ui->tabPlayer8,
    };

    connected_controller_checkboxes = {
        ui->checkboxPlayer1Connected, ui->checkboxPlayer2Connected, ui->checkboxPlayer3Connected,
        ui->checkboxPlayer4Connected, ui->checkboxPlayer5Connected, ui->checkboxPlayer6Connected,
        ui->checkboxPlayer7Connected, ui->checkboxPlayer8Connected,
    };

    std::array<QLabel*, 8> connected_controller_labels = {
        ui->label,   ui->label_3, ui->label_4, ui->label_5,
        ui->label_6, ui->label_7, ui->label_8, ui->label_9,
    };

    for (std::size_t i = 0; i < player_tabs.size(); ++i) {
        QHBoxLayout* tab_layout = new QHBoxLayout(player_tabs[i]);
        tab_layout->addWidget(player_controllers[i]);
        connect(player_controllers[i], &ConfigureInputPlayer::Connected, [this, i](bool checked) {
            // Ensures that connecting a controller changes the number of players
            if (connected_controller_checkboxes[i]->isChecked() != checked) {
                // Ensures that the players are always connected in sequential order
                PropagatePlayerNumberChanged(i, checked);
            }
        });
        connect(connected_controller_checkboxes[i], &QCheckBox::clicked, [this, i](bool checked) {
            // Reconnect current controller if it was the last one checked
            // (player number was reduced by more than one)
            const bool reconnect_first = !checked &&
                                         i < connected_controller_checkboxes.size() - 1 &&
                                         connected_controller_checkboxes[i + 1]->isChecked();

            // Ensures that the players are always connected in sequential order
            PropagatePlayerNumberChanged(i, checked, reconnect_first);
        });
        connect(player_controllers[i], &ConfigureInputPlayer::RefreshInputDevices, this,
                &ConfigureInput::UpdateAllInputDevices);
        connect(player_controllers[i], &ConfigureInputPlayer::RefreshInputProfiles, this,
                &ConfigureInput::UpdateAllInputProfiles, Qt::QueuedConnection);
        connect(connected_controller_checkboxes[i], &QCheckBox::STATE_CHANGED,
                [this, i](int state) {
                    // Keep activated controllers synced with the "Connected Controllers" checkboxes
                    player_controllers[i]->ConnectPlayer(state == Qt::Checked);
                });

        // Remove/hide all the elements that exceed max_players, if applicable.
        if (i >= max_players) {
            ui->tabWidget->removeTab(static_cast<int>(max_players));
            connected_controller_checkboxes[i]->hide();
            connected_controller_labels[i]->hide();
        }
    }
    // Only the first player can choose handheld mode so connect the signal just to player 1
    connect(player_controllers[0], &ConfigureInputPlayer::HandheldStateChanged,
            [this](bool is_handheld) { UpdateDockedState(is_handheld); });

    // The Players tab comes first: how players are assigned, and who is which player now
    assignment = new ConfigurePlayerAssignment(this);
    assignment->setAccessibleName(tr("Controllers"));
    assignment->SetOpenPlayerTab([this](int player) {
        for (QWidget* parent = player_tabs[player]->parentWidget(); parent;
             parent = parent->parentWidget()) {
            if (auto* tabs = qobject_cast<QTabWidget*>(parent)) {
                tabs->setCurrentWidget(player_tabs[player]);
                return;
            }
        }
    });

    // Console Mode first in the Controllers tab: a mirror of the player tabs' Docked/Handheld
    // choice, which stays the one applied (and the one player 1's Handheld type locks)
    {
        auto* console_box = new QGroupBox(tr("Console Mode"), assignment);
        auto* console_layout = new QHBoxLayout(console_box);
        auto* console_group = new QButtonGroup(console_box);
        console_layout->addWidget(ConfigurePlayerAssignment::MakeSegmented(
            console_box, {tr("Docked"), tr("Handheld")}, {0, 1}, console_group));
        console_layout->addStretch(1);
        auto* docked = console_group->button(0);
        auto* handheld = console_group->button(1);
        assignment->AddTopWidget(console_box);
        const auto mirror = [this, docked, handheld] {
            docked->setChecked(ui->radioDocked->isChecked());
            handheld->setChecked(ui->radioUndocked->isChecked());
            docked->setEnabled(ui->radioDocked->isEnabled());
            handheld->setEnabled(ui->radioUndocked->isEnabled());
        };
        connect(ui->radioDocked, &QRadioButton::toggled, this, mirror);
        connect(ui->radioUndocked, &QRadioButton::toggled, this, mirror);
        connect(docked, &QAbstractButton::clicked, this,
                [this] { ui->radioDocked->setChecked(true); });
        connect(handheld, &QAbstractButton::clicked, this,
                [this] { ui->radioUndocked->setChecked(true); });
        mirror_console_mode = mirror;
        mirror();
    }

    {
        auto& state = Common::GetControllerConsoleState();
        std::scoped_lock lock{state.mutex};
        followed_generation = state.generation;
    }
    follow_timer = new QTimer(this);
    connect(follow_timer, &QTimer::timeout, this, [this] {
        unsigned long long generation = 0;
        std::vector<Common::ControllerConsoleState::PlayerRow> rows;
        bool engine_active = false;
        {
            auto& state = Common::GetControllerConsoleState();
            std::scoped_lock lock{state.mutex};
            generation = state.generation;
            engine_active = state.engine_active;
            rows = state.players;
        }
        last_rows = rows;
        // Automatic assignment owns the players: only their tabs show, and their connection
        // cannot be toggled by hand
        const auto mode = Settings::values.controller_applet_mode.GetValue();
        const bool automatic = mode == Settings::AppletMode::ConsoleMode ||
                               mode == Settings::AppletMode::HandheldConsoleMode;
        QTabWidget* tabs = nullptr;
        for (QWidget* parent = player_tabs[0]->parentWidget(); parent && !tabs;
             parent = parent->parentWidget()) {
            tabs = qobject_cast<QTabWidget*>(parent);
        }
        // The tabs follow the engine's rows and flashes, like the overlay: a green label when a
        // controller takes the player, red when one leaves (the tab hides once its row is gone)
        constexpr long long flash_ms = 1500;
        const long long now = QDateTime::currentMSecsSinceEpoch();
        // The player tabs' choice can be locked (player 1 set to Handheld) without toggling
        if (mirror_console_mode) {
            mirror_console_mode();
        }
        for (std::size_t i = 0; i < player_tabs.size(); ++i) {
            const auto row = std::find_if(rows.begin(), rows.end(), [i](const auto& r) {
                return r.player == static_cast<int>(i);
            });
            bool shown = true;
            QColor label;
            if (automatic) {
                shown = engine_active && row != rows.end();
                // Red (leaving) wins, then blue (input held), then the green connect blink
                const long long elapsed = shown ? now - row->flash_since : 0;
                const bool blink = shown && row->flash != 0 && elapsed < flash_ms &&
                                   (elapsed / 125) % 2 == 0;
                if (blink && row->flash == 2) {
                    label = QColor(200, 50, 50);
                } else if (shown && row->active && !row->leaving) {
                    label = QColor(50, 110, 220);
                } else if (blink) {
                    label = QColor(40, 160, 70);
                }
            }
            if (tabs && !tab_dragging) {
                if (const int index = tabs->indexOf(player_tabs[i]); index >= 0) {
                    if (player_tab_titles[i].isEmpty()) {
                        player_tab_titles[i] = tabs->tabText(index);
                    }
                    // Only what changed: each of these lays the tab bar out again, and done every
                    // pass that shook it under the Midnight Blue themes
                    const auto set_text = [tabs, index](const QString& text) {
                        if (tabs->tabText(index) != text) {
                            tabs->setTabText(index, text);
                        }
                    };
                    const auto set_tip = [tabs, index](const QString& tip) {
                        if (tabs->tabToolTip(index) != tip) {
                            tabs->setTabToolTip(index, tip);
                        }
                    };
                    if (tabs->isTabVisible(index) != shown) {
                        tabs->setTabVisible(index, shown);
                    }
                    if (tabs->tabBar()->tabTextColor(index) != label) {
                        tabs->tabBar()->setTabTextColor(index, label);
                    }
                    if (!automatic) {
                        set_text(player_tab_titles[i]);
                        set_tip({});
                    } else if (shown) {
                        // The grip says the tab drags, like the Controllers tab's rows
                        set_text(QStringLiteral("\u283F P%1").arg(i + 1));
                        set_tip(tr("P%1: %2\nDrag left or right to change the player order")
                                    .arg(i + 1)
                                    .arg(QString::fromStdString(row->name)));
                    }
                }
            }
            player_controllers[i]->SetConnectionLocked(automatic);
        }
        // In the automatic modes the player tabs drag to change the player order, and the
        // selected tab reads bold
        if (tabs) {
            const bool controls_page = tabs->indexOf(player_tabs[0]) >= 0;
            tabs_editable = automatic && engine_active && controls_page;
            // Under the dark themes Qt's drag would show the theme's tab: Eden drags it instead
            tabs->setMovable(tabs_editable && !UISettings::IsDarkTheme());
            // Pill tabs in the players' colours, the selected one filled (the tab bar is the
            // settings dialog's, shared with the other pages: only on this page, in these modes)
            QTabBar* bar = tabs->tabBar();
            const bool pills = automatic && controls_page;
            // (The bar is remembered: under a style sheet its style() is a wrapper, not ours)
            if (pills && pill_bar != bar) {
                auto* style = new PlayerTabStyle(QApplication::style()->name());
                style->setParent(bar);
                bar->setStyle(style);
                pill_bar = bar;
            } else if (!pills && pill_bar == bar) {
                bar->setStyle(nullptr);
                pill_bar = nullptr;
            }
            // The Midnight Blue themes give a hovered tab a 1-pixel border on three sides and
            // narrow its padding: the tab and the whole tab bar change size, the page below
            // moves, and with the hover lost it moves back, shaking. A hovered tab keeps its
            // size: their padding (8 at the sides, 2 above and below) less that border (the
            // bottom one grows from 2 to 3). (Every page of the settings dialog shares this bar.)
            const bool midnight =
                QString::fromStdString(UISettings::values.theme).contains(QStringLiteral("midnight"));
            const QString steady =
                midnight ? QStringLiteral("QTabBar::tab:top:!selected:hover { padding-left: 7px; "
                                          "padding-right: 7px; padding-top: 1px; "
                                          "padding-bottom: 1px; }")
                         : QString{};
            if (bar->styleSheet() != steady) {
                bar->setStyleSheet(steady);
            }
            // The dark themes' style sheet draws the tabs itself: the bubbles are painted over it
            const bool painted = pills && UISettings::IsDarkTheme();
            if (painted) {
                if (!tab_overlay || tab_overlay->parentWidget() != bar) {
                    delete tab_overlay;
                    tab_overlay = new PlayerTabOverlay(
                        bar, QString::fromStdString(UISettings::values.theme)
                                     .contains(QStringLiteral("midnight"))
                                 ? QColor(0x19, 0x23, 0x2D)
                                 : QColor(0x31, 0x36, 0x3B));
                }
                if (tab_overlay->geometry() != bar->rect()) {
                    tab_overlay->setGeometry(bar->rect());
                }
                if (!tab_overlay->isVisible()) {
                    tab_overlay->lower();
                    tab_overlay->show();
                }
                tab_overlay->update();
            } else if (tab_overlay) {
                tab_overlay->hide();
            }
            // A player tab's colour is what makes it a bubble: none outside these modes
            for (std::size_t i = 0; i < player_tabs.size(); ++i) {
                if (const int index = tabs->indexOf(player_tabs[i]); index >= 0) {
                    const QVariant tab_data =
                        pills ? QVariant(ConfigurePlayerAssignment::PlayerColor(static_cast<int>(i)))
                              : QVariant{};
                    if (bar->tabData(index) != tab_data) {
                        bar->setTabData(index, tab_data);
                    }
                }
            }
            if (tabs != drag_tabs) {
                drag_tabs = tabs;
                tabs->tabBar()->installEventFilter(this);
                connect(tabs, &QTabWidget::currentChanged, this,
                        &ConfigureInput::UpdateTabArrows);
            }
            if (pending_select >= 0) {
                const int index = tabs->indexOf(player_tabs[pending_select]);
                if (index >= 0 && tabs->isTabVisible(index)) {
                    tabs->setCurrentIndex(index);
                    pending_select = -1;
                } else if (now > pending_select_until) {
                    pending_select = -1;
                }
            }
            if (!tab_dragging) {
                UpdateTabArrows();
            }
        }

        // Clicking a player's tab rumbles its controller in the automatic modes, so the user
        // can tell which controller the tab is about (tab changes made by the code do not)
        if (tabs && tabs != rumble_tabs) {
            rumble_tabs = tabs;
            connect(tabs->tabBar(), &QTabBar::tabBarClicked, this, [this, tabs](int index) {
                const auto applet_mode = Settings::values.controller_applet_mode.GetValue();
                if (applet_mode != Settings::AppletMode::ConsoleMode &&
                    applet_mode != Settings::AppletMode::HandheldConsoleMode) {
                    return;
                }
                const auto it = std::find(player_tabs.begin(), player_tabs.end(),
                                          tabs->widget(index));
                if (it == player_tabs.end()) {
                    return;
                }
                const auto player = static_cast<std::size_t>(it - player_tabs.begin());
                if (!Settings::values.vibration_enabled.GetValue() ||
                    !Settings::values.players.GetValue()[player].vibration_enabled) {
                    return;
                }
                auto& state = Common::GetControllerConsoleState();
                std::scoped_lock lock{state.mutex};
                state.rumble_requests.push_back(static_cast<int>(player));
            });
        }

        // The Controllers tab carries Console Mode in the automatic modes
        ui->handheldGroup->setVisible(!automatic);
        ui->connectedControllers->setVisible(!automatic);

        if (generation == followed_generation) {
            return;
        }
        followed_generation = generation;
        for (auto* controller : player_controllers) {
            controller->RefreshFromController();
        }
        // The boxes mirror the emulated controllers without acting on them
        for (auto* checkbox : connected_controller_checkboxes) {
            checkbox->blockSignals(true);
        }
        LoadPlayerControllerIndices();
        for (auto* checkbox : connected_controller_checkboxes) {
            checkbox->blockSignals(false);
        }
    });
    follow_timer->start(50);
    ui->tabWidget->insertTab(0, assignment, tr("Controllers"));
    ui->tabWidget->setCurrentIndex(0);

    advanced = new ConfigureInputAdvanced(hid_core, this);

    QHBoxLayout* advancedLayout = new QHBoxLayout(ui->tabAdvanced);
    advancedLayout->addWidget(advanced);

    connect(advanced, &ConfigureInputAdvanced::CallDebugControllerDialog,
            [this, input_subsystem, &hid_core, is_powered_on] {
                CallConfigureDialog<ConfigureDebugController>(
                    *this, input_subsystem, profiles.get(), hid_core, is_powered_on);
            });
    connect(advanced, &ConfigureInputAdvanced::CallTouchscreenConfigDialog,
            [this] { CallConfigureDialog<ConfigureTouchscreenAdvanced>(*this); });
    connect(advanced, &ConfigureInputAdvanced::CallMotionTouchConfigDialog,
            [this, input_subsystem] {
                CallConfigureDialog<ConfigureMotionTouch>(*this, input_subsystem);
            });
    connect(advanced, &ConfigureInputAdvanced::CallRingControllerDialog,
            [this, input_subsystem, &hid_core] {
                CallConfigureDialog<ConfigureRingController>(*this, input_subsystem, hid_core);
            });
    connect(advanced, &ConfigureInputAdvanced::CallCameraDialog, [this, input_subsystem] {
        CallConfigureDialog<ConfigureCamera>(*this, input_subsystem);
    });

    connect(ui->vibrationButton, &QPushButton::clicked,
            [this, &hid_core] { CallConfigureDialog<ConfigureVibration>(*this, hid_core); });

    connect(ui->motionButton, &QPushButton::clicked, [this, input_subsystem] {
        CallConfigureDialog<ConfigureMotionTouch>(*this, input_subsystem);
    });

    connect(ui->buttonClearAll, &QPushButton::clicked, [this] { ClearAll(); });
    connect(ui->buttonRestoreDefaults, &QPushButton::clicked, [this] { RestoreDefaults(); });

    RetranslateUI();
    LoadConfiguration();
}

void ConfigureInput::PropagatePlayerNumberChanged(size_t player_index, bool checked,
                                                  bool reconnect_current) {
    connected_controller_checkboxes[player_index]->setChecked(checked);

    if (checked) {
        // Check all previous buttons when checked
        if (player_index > 0) {
            PropagatePlayerNumberChanged(player_index - 1, checked);
        }
    } else {
        // Unchecked all following buttons when unchecked
        if (player_index < connected_controller_checkboxes.size() - 1) {
            PropagatePlayerNumberChanged(player_index + 1, checked);
        }
    }

    if (reconnect_current) {
        connected_controller_checkboxes[player_index]->setCheckState(Qt::Checked);
    }
}

PlayerTabStyle::PlayerTabStyle(const QString& base) : QProxyStyle(base) {}

void PlayerTabStyle::drawControl(ControlElement element, const QStyleOption* option,
                                 QPainter* painter, const QWidget* widget) const {
    const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
    const auto* bar = qobject_cast<const QTabBar*>(widget);
    if (!tab || !bar || (element != CE_TabBarTabShape && element != CE_TabBarTabLabel)) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }
    // The tab is found by its text: a dragged tab is painted into an image of its own, where
    // its position says nothing
    int index = -1;
    for (int i = 0; i < bar->count() && index < 0; ++i) {
        if (bar->tabText(i) == tab->text) {
            index = i;
        }
    }
    // Only a player tab is a bubble, in its player's colour; the others stay as they were
    const QVariant data = index >= 0 ? bar->tabData(index) : QVariant{};
    if (data.metaType() != QMetaType::fromType<QColor>()) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }
    const QColor accent = data.value<QColor>();
    const bool selected = tab->state & State_Selected;
    if (element == CE_TabBarTabShape) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRectF pill = QRectF(tab->rect).adjusted(2, 3, -2, -3);
        if (selected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(accent);
        } else {
            QColor edge = accent;
            edge.setAlpha(170);
            painter->setPen(QPen(edge, 1.5));
            QColor fill = accent;
            fill.setAlpha(tab->state & State_MouseOver ? 45 : 0);
            painter->setBrush(fill);
        }
        painter->drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
        painter->restore();
        return;
    }
    // The label: white and bold on the filled tab, else in the player's colour, which reads on
    // a light tab bar and a dark one (Windows in dark mode keeps light tabs with white text);
    // a flash colour wins
    QStyleOptionTab label{*tab};
    const QColor flash = bar->tabTextColor(index);
    const QColor text = selected ? QColor(Qt::white) : (flash.isValid() ? flash : accent);
    label.palette.setColor(QPalette::WindowText, text);
    label.palette.setColor(QPalette::ButtonText, text);
    painter->save();
    QFont font = painter->font();
    font.setBold(selected);
    painter->setFont(font);
    QProxyStyle::drawControl(element, &label, painter, widget);
    painter->restore();
}

QSize PlayerTabStyle::sizeFromContents(ContentsType type, const QStyleOption* option,
                                       const QSize& size, const QWidget* widget) const {
    QSize result = QProxyStyle::sizeFromContents(type, option, size, widget);
    // Room for a bubble's rounded ends and bold label (a player tab starts with its grip)
    const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
    if (type == CT_TabBarTab && tab && tab->text.startsWith(QChar(0x283F))) {
        result += QSize(14, 4);
    }
    return result;
}

PlayerTabOverlay::PlayerTabOverlay(QTabBar* bar_, QColor background_)
    : QWidget(bar_), bar{bar_}, background{background_} {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    // The dark themes give a plain widget a solid background, which would cover the tab bar
    setStyleSheet(QStringLiteral("background: transparent;"));
}

void PlayerTabOverlay::SetDrag(int index, int offset) {
    drag_index = index;
    drag_offset = offset;
    update();
}

void PlayerTabOverlay::paintEvent(QPaintEvent*) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing);
    const int hovered = bar->underMouse() ? bar->tabAt(bar->mapFromGlobal(QCursor::pos())) : -1;
    const auto bubble = [&](int i, QRect tab, bool around_arrows) {
        const QColor accent = bar->tabData(i).value<QColor>();
        const bool selected = i == bar->currentIndex();
        const QRectF pill = QRectF(tab).adjusted(2, 3, -2, -3);
        if (selected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(accent);
        } else {
            QColor edge = accent;
            edge.setAlpha(170);
            painter.setPen(QPen(edge, 1.5));
            QColor fill = accent;
            fill.setAlpha(i == hovered ? 45 : 0);
            painter.setBrush(fill);
        }
        painter.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
        // The label, between the tab's arrows; a flash colours it as on the tabs it covers
        QRect text = tab;
        if (around_arrows) {
            if (const QWidget* left = bar->tabButton(i, QTabBar::LeftSide)) {
                text.setLeft(left->geometry().right() + 1);
            }
            if (const QWidget* right = bar->tabButton(i, QTabBar::RightSide)) {
                text.setRight(right->geometry().left() - 1);
            }
        }
        QFont font = bar->font();
        font.setBold(selected);
        painter.setFont(font);
        const QColor flash = bar->tabTextColor(i);
        painter.setPen(selected ? QColor(Qt::white)
                                : (flash.isValid() ? flash : QColor(0xEF, 0xF0, 0xF1)));
        painter.drawText(text, Qt::AlignCenter, bar->tabText(i));
    };
    for (int i = 0; i < bar->count(); ++i) {
        const QVariant tab_data = bar->tabData(i);
        if (tab_data.metaType() != QMetaType::fromType<QColor>() || !bar->isTabVisible(i)) {
            continue;
        }
        const QRect tab = bar->tabRect(i);
        // The theme's tab goes under the theme's background, and the bubble goes on top
        painter.fillRect(tab, background);
        if (i == drag_index) {
            // Where the dragged tab came from: an empty slot
            QColor edge = tab_data.value<QColor>();
            edge.setAlpha(90);
            painter.setPen(QPen(edge, 1, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            const QRectF slot = QRectF(tab).adjusted(3, 4, -3, -4);
            painter.drawRoundedRect(slot, slot.height() / 2, slot.height() / 2);
            continue;
        }
        bubble(i, tab, true);
    }
    // The dragged tab, last, over the others
    if (drag_index >= 0 && drag_index < bar->count()) {
        bubble(drag_index, bar->tabRect(drag_index).translated(drag_offset, 0), false);
    }
}

void ConfigureInput::SwapPlayers(int a, int b) {
    if (a == b || a < 0 || b < 0 || a >= 8 || b >= 8) {
        return;
    }
    auto& state = Common::GetControllerConsoleState();
    std::scoped_lock lock{state.mutex};
    state.swap_requests.emplace_back(a, b);
}

bool ConfigureInput::eventFilter(QObject* object, QEvent* event) {
    if (drag_tabs && object == drag_tabs->tabBar() && tab_overlay && tab_overlay->isVisible() &&
        DragPlayerTab(event)) {
        return true;
    }
    if (drag_tabs && object == drag_tabs->tabBar() && drag_tabs->isMovable()) {
        if (event->type() == QEvent::MouseButtonPress &&
            static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            tab_order.clear();
            for (int k = 0; k < drag_tabs->count(); ++k) {
                tab_order.push_back(drag_tabs->widget(k));
            }
            const QPoint at = static_cast<QMouseEvent*>(event)->position().toPoint();
            dragged_tab = drag_tabs->widget(drag_tabs->tabBar()->tabAt(at));
            tab_dragging = true;
        } else if (event->type() == QEvent::MouseButtonRelease && tab_dragging) {
            tab_dragging = false;
            // After the tab bar has finished its own drop
            QTimer::singleShot(0, this, &ConfigureInput::FinishTabDrag);
        }
    }
    return QWidget::eventFilter(object, event);
}

void ConfigureInput::FinishTabDrag() {
    QTabWidget* tabs = drag_tabs;
    if (!tabs || tab_order.isEmpty()) {
        return;
    }
    // The players in the order their tabs were dropped in
    std::vector<int> dropped;
    for (int k = 0; k < tabs->count(); ++k) {
        const auto it = std::find(player_tabs.begin(), player_tabs.end(), tabs->widget(k));
        if (it != player_tabs.end() && tabs->isTabVisible(k)) {
            dropped.push_back(static_cast<int>(it - player_tabs.begin()));
        }
    }
    // The tabs go back in player order: the controllers move instead
    for (int k = 0; k < static_cast<int>(tab_order.size()); ++k) {
        const int from = tabs->indexOf(tab_order[k]);
        if (from >= 0 && from != k) {
            tabs->tabBar()->moveTab(from, k);
        }
    }
    tab_order.clear();
    const auto dragged = std::find(player_tabs.begin(), player_tabs.end(), dragged_tab);
    ApplyDroppedOrder(dropped, dragged != player_tabs.end()
                                   ? static_cast<int>(dragged - player_tabs.begin())
                                   : -1);
}

void ConfigureInput::ApplyDroppedOrder(const std::vector<int>& dropped, int dragged_player) {
    std::vector<int> places = dropped;
    std::sort(places.begin(), places.end());
    if (dropped == places) {
        return;
    }

    // Place k takes the controller player dropped[k] has now, as one swap at a time (the
    // engine applies them in order)
    std::map<int, int> holder;
    for (const int place : places) {
        holder[place] = place;
    }
    for (std::size_t k = 0; k < places.size(); ++k) {
        int from = places[k];
        for (const auto& [place, original] : holder) {
            if (original == dropped[k]) {
                from = place;
            }
        }
        if (from != places[k]) {
            SwapPlayers(places[k], from);
            std::swap(holder[places[k]], holder[from]);
        }
    }

    // The selected tab follows the controller that was dragged
    const auto at = std::find(dropped.begin(), dropped.end(), dragged_player);
    if (at != dropped.end()) {
        pending_select = places[at - dropped.begin()];
        pending_select_until = QDateTime::currentMSecsSinceEpoch() + 2000;
    }
}

bool ConfigureInput::DragPlayerTab(QEvent* event) {
    QTabBar* bar = drag_tabs->tabBar();
    const auto type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove &&
        type != QEvent::MouseButtonRelease) {
        return false;
    }
    const auto* mouse = static_cast<QMouseEvent*>(event);
    const QPoint at = mouse->position().toPoint();
    const auto set_arrows = [bar](int index, bool shown) {
        for (const auto side : {QTabBar::LeftSide, QTabBar::RightSide}) {
            if (QWidget* button = bar->tabButton(index, side)) {
                button->setVisible(shown);
            }
        }
    };
    if (type == QEvent::MouseButtonPress) {
        const int index = bar->tabAt(at);
        own_drag_index = mouse->button() == Qt::LeftButton && index >= 0 &&
                                 bar->tabData(index).metaType() == QMetaType::fromType<QColor>()
                             ? index
                             : -1;
        own_drag_press_x = at.x();
        own_dragging = false;
        return false; // the tab bar still selects the tab
    }
    if (own_drag_index < 0) {
        return false;
    }
    const int offset = at.x() - own_drag_press_x;
    if (type == QEvent::MouseMove) {
        if (!own_dragging && std::abs(offset) < QApplication::startDragDistance()) {
            return false;
        }
        if (!own_dragging) {
            own_dragging = true;
            set_arrows(own_drag_index, false);
        }
        tab_overlay->SetDrag(own_drag_index, offset);
        return true;
    }
    // Released: the players take the order of the tabs with the dragged one where it was let go
    const int index = own_drag_index;
    const bool dragged = own_dragging;
    own_drag_index = -1;
    own_dragging = false;
    if (!dragged) {
        return false;
    }
    set_arrows(index, true);
    tab_overlay->SetDrag(-1, 0);
    std::vector<std::pair<int, int>> centres; // (x, player)
    int dragged_player = -1;
    for (int k = 0; k < bar->count(); ++k) {
        const auto it = std::find(player_tabs.begin(), player_tabs.end(), drag_tabs->widget(k));
        if (it == player_tabs.end() || !bar->isTabVisible(k)) {
            continue;
        }
        const int player = static_cast<int>(it - player_tabs.begin());
        const int x = bar->tabRect(k).center().x() + (k == index ? offset : 0);
        if (k == index) {
            dragged_player = player;
        }
        centres.emplace_back(x, player);
    }
    std::stable_sort(centres.begin(), centres.end());
    std::vector<int> dropped;
    for (const auto& [x, player] : centres) {
        dropped.push_back(player);
    }
    ApplyDroppedOrder(dropped, dragged_player);
    return true;
}

void ConfigureInput::UpdateTabArrows() {
    QTabWidget* tabs = drag_tabs;
    if (!tabs) {
        return;
    }
    const bool movable = tabs_editable;
    int last_player = -1;
    for (const auto& row : last_rows) {
        if (!row.leaving) {
            last_player = (std::max)(last_player, row.player);
        }
    }
    for (int player = 0; player < static_cast<int>(player_tabs.size()); ++player) {
        const int index = tabs->indexOf(player_tabs[player]);
        if (index < 0) {
            continue;
        }
        // (A tab's buttons go when the tab is removed, as on leaving the Controls page)
        auto& left = tab_lefts[player];
        auto& right = tab_rights[player];
        // Only a shown tab carries arrows: a hidden tab's buttons would float over the tab bar
        // and swap a controller onto a player nobody holds
        if (!movable || !tabs->isTabVisible(index)) {
            if (left || right) {
                tabs->tabBar()->setTabButton(index, QTabBar::LeftSide, nullptr);
                tabs->tabBar()->setTabButton(index, QTabBar::RightSide, nullptr);
                delete left;
                delete right;
            }
            continue;
        }
        if (!left || !right) {
            const auto make = [this, player](Qt::ArrowType arrow, int step) {
                auto* button = new QToolButton;
                button->setArrowType(arrow);
                button->setAutoRaise(true);
                button->setFixedSize(16, 16);
                // The size a style sheet reports is the size the tab makes room for (the dark
                // themes say 12 pixels, and the tab text went under the buttons)
                button->setStyleSheet(QStringLiteral("QToolButton { width: 16px; height: 16px; "
                                                     "padding: 0px; border: none; "
                                                     "background: transparent; }"));
                connect(button, &QToolButton::clicked, this, [this, player, step] {
                    SwapPlayers(player, player + step);
                    pending_select = player + step;
                    pending_select_until = QDateTime::currentMSecsSinceEpoch() + 2000;
                });
                return button;
            };
            left = make(Qt::LeftArrow, -1);
            right = make(Qt::RightArrow, 1);
            tabs->tabBar()->setTabButton(index, QTabBar::LeftSide, left);
            tabs->tabBar()->setTabButton(index, QTabBar::RightSide, right);
        }
        left->setEnabled(player > 0);
        left->setToolTip(tr("Swap with P%1").arg(player));
        right->setEnabled(player < last_player);
        right->setToolTip(tr("Swap with P%1").arg(player + 2));
    }
}

QList<QWidget*> ConfigureInput::GetSubTabs() const {
    // The settings dialog shows these, not this widget's own tab bar
    return {
        assignment,     ui->tabAdvanced,  ui->tabPlayer1, ui->tabPlayer2, ui->tabPlayer3,
        ui->tabPlayer4, ui->tabPlayer5, ui->tabPlayer6, ui->tabPlayer7, ui->tabPlayer8,
    };
}

void ConfigureInput::ApplyConfiguration() {
    const bool was_global = Settings::values.players.UsingGlobal();
    Settings::values.players.SetGlobal(true);
    for (auto* controller : player_controllers) {
        controller->ApplyConfiguration();
    }

    advanced->ApplyConfiguration();
    assignment->ApplyConfiguration();

    const bool pre_docked_mode = Settings::IsDockedMode();
    const bool docked_mode_selected = ui->radioDocked->isChecked();
    Settings::values.use_docked_mode.SetValue(
        docked_mode_selected ? Settings::ConsoleMode::Docked : Settings::ConsoleMode::Handheld);
    OnDockedModeChanged(pre_docked_mode, docked_mode_selected, system);

    Settings::values.vibration_enabled.SetValue(ui->vibrationGroup->isChecked());
    Settings::values.motion_enabled.SetValue(ui->motionGroup->isChecked());
    Settings::values.players.SetGlobal(was_global);
}

void ConfigureInput::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureInput::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureInput::LoadConfiguration() {
    const auto* handheld = system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Handheld);

    LoadPlayerControllerIndices();
    UpdateDockedState(handheld->IsConnected());

    ui->vibrationGroup->setChecked(Settings::values.vibration_enabled.GetValue());
    ui->motionGroup->setChecked(Settings::values.motion_enabled.GetValue());
}

void ConfigureInput::LoadPlayerControllerIndices() {
    for (std::size_t i = 0; i < connected_controller_checkboxes.size(); ++i) {
        if (i == 0) {
            auto* handheld =
                system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Handheld);
            if (handheld->IsConnected()) {
                connected_controller_checkboxes[i]->setChecked(true);
                continue;
            }
        }
        const auto* controller = system.HIDCore().GetEmulatedControllerByIndex(i);
        connected_controller_checkboxes[i]->setChecked(controller->IsConnected());
    }
}

void ConfigureInput::ClearAll() {
    // We don't have a good way to know what tab is active, but we can find out by getting the
    // parent of the consoleInputSettings
    auto* player_tab = static_cast<ConfigureInputPlayer*>(ui->consoleInputSettings->parent());
    player_tab->ClearAll();
}

void ConfigureInput::RestoreDefaults() {
    // We don't have a good way to know what tab is active, but we can find out by getting the
    // parent of the consoleInputSettings
    auto* player_tab = static_cast<ConfigureInputPlayer*>(ui->consoleInputSettings->parent());
    player_tab->RestoreDefaults();

    ui->radioDocked->setChecked(true);
    ui->radioUndocked->setChecked(false);
    ui->vibrationGroup->setChecked(true);
    ui->motionGroup->setChecked(true);
}

void ConfigureInput::UpdateDockedState(bool is_handheld) {
    // Disallow changing the console mode if the controller type is handheld.
    ui->radioDocked->setEnabled(!is_handheld);
    ui->radioUndocked->setEnabled(!is_handheld);

    ui->radioDocked->setChecked(Settings::IsDockedMode());
    ui->radioUndocked->setChecked(!Settings::IsDockedMode());

    // Also force into undocked mode if the controller type is handheld.
    if (is_handheld) {
        ui->radioUndocked->setChecked(true);
    }
}

void ConfigureInput::UpdateAllInputDevices() {
    for (const auto& player : player_controllers) {
        player->UpdateInputDeviceCombobox();
    }
}

void ConfigureInput::UpdateAllInputProfiles(std::size_t player_index) {
    for (std::size_t i = 0; i < player_controllers.size(); ++i) {
        if (i == player_index) {
            continue;
        }

        player_controllers[i]->UpdateInputProfiles();
    }
}

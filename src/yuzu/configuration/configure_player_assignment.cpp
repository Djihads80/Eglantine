// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFrame>
#include <QMimeData>
#include <QMouseEvent>
#include <QPen>
#include <QPainter>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDir>
#include <QRadioButton>
#include <QScrollArea>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "common/controller_console_state.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "yuzu/configuration/configure_player_assignment.h"
#include "yuzu/configuration/input_profiles.h"

namespace {

bool LaunchedThroughSteam() {
    return std::getenv("SteamGameId") != nullptr || std::getenv("SteamAppId") != nullptr;
}

bool IsAutomatic(Settings::AppletMode mode) {
    return mode == Settings::AppletMode::ConsoleMode ||
           mode == Settings::AppletMode::HandheldConsoleMode;
}

constexpr int MaxPlayers = 8;

/// The players' colours, as the overlay draws their badges
constexpr std::array<QColor, 8> PlayerColors{
    QColor(79, 142, 247), QColor(239, 91, 91), QColor(47, 191, 113), QColor(245, 184, 46),
    QColor(155, 108, 242), QColor(242, 140, 56), QColor(46, 196, 201), QColor(227, 107, 174),
};

/// An on/off switch: a rounded track with a knob, like a phone's settings
class ToggleSwitch : public QAbstractButton {
public:
    explicit ToggleSwitch(QWidget* parent) : QAbstractButton(parent) {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
    }

    QSize sizeHint() const override {
        return {42, 24};
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        const QRectF track = QRectF(rect()).adjusted(1, 1, -1, -1);
        QColor fill = isChecked() ? palette().highlight().color() : palette().mid().color();
        if (!isEnabled()) {
            fill.setAlpha(110);
        }
        painter.setBrush(fill);
        painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);
        const qreal knob = track.height() - 6;
        const qreal x = isChecked() ? track.right() - 3 - knob : track.left() + 3;
        painter.setBrush(Qt::white);
        painter.drawEllipse(QRectF(x, track.top() + 3, knob, knob));
    }
};
constexpr char PlayerMime[] = "application/x-eden-player";

/// One player in the list: a click rumbles its controller, and a long press (or a drag) picks
/// it up to drop on another player, which swaps the two
class PlayerRow : public QFrame {
public:
    PlayerRow(int player_, QWidget* parent) : QFrame(parent), player{player_} {
        setObjectName(QStringLiteral("playerRow"));
        setAcceptDrops(true);
        setCursor(Qt::OpenHandCursor);
        long_press.setSingleShot(true);
        long_press.setInterval(400);
        QObject::connect(&long_press, &QTimer::timeout, this, [this] { StartDrag(); });
    }

    std::function<void(int)> on_click;
    std::function<void(int, int)> on_drop;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            press_pos = event->position().toPoint();
            pressed = true;
            long_press.start();
        }
        QFrame::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (pressed && (event->position().toPoint() - press_pos).manhattanLength() >=
                           QApplication::startDragDistance()) {
            StartDrag();
        }
        QFrame::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (pressed && long_press.isActive() && on_click) {
            on_click(player);
        }
        pressed = false;
        long_press.stop();
        QFrame::mouseReleaseEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasFormat(QString::fromLatin1(PlayerMime))) {
            event->acceptProposedAction();
            drop_hover = true;
            update();
        }
    }

    void dragLeaveEvent(QDragLeaveEvent* event) override {
        drop_hover = false;
        update();
        QFrame::dragLeaveEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        QFrame::paintEvent(event);
        if (drop_hover) {
            // Where the dragged player lands
            QPainter painter{this};
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(palette().highlight().color(), 2, Qt::DashLine));
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
        }
    }

    void dropEvent(QDropEvent* event) override {
        drop_hover = false;
        update();
        const int from = event->mimeData()->data(QString::fromLatin1(PlayerMime)).toInt();
        if (from != player && on_drop) {
            on_drop(from, player);
        }
        event->acceptProposedAction();
    }

private:
    void StartDrag() {
        if (!pressed) {
            return;
        }
        pressed = false;
        long_press.stop();
        auto* mime = new QMimeData;
        mime->setData(QString::fromLatin1(PlayerMime), QByteArray::number(player));
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(grab());
        drag->setHotSpot(press_pos);
        setCursor(Qt::ClosedHandCursor);
        drag->exec(Qt::MoveAction);
        setCursor(Qt::OpenHandCursor);
    }

    int player;
    bool pressed = false;
    bool drop_hover = false;
    QPoint press_pos;
    QTimer long_press;
};

} // Anonymous namespace

QColor ConfigurePlayerAssignment::PlayerColor(int player) {
    return PlayerColors[static_cast<std::size_t>(player) % PlayerColors.size()];
}

QString ConfigurePlayerAssignment::ModernComboStyle() {
    return QStringLiteral(
        "QComboBox { border: 1px solid palette(mid); border-radius: 6px; padding: 3px 10px; "
        "background: palette(base); }"
        "QComboBox:hover, QComboBox:focus { border-color: palette(highlight); }"
        "QComboBox:disabled { color: palette(mid); }"
        // The open list: readable rows, the hovered one in the highlight colour
        "QComboBox QAbstractItemView { background: palette(base); color: palette(text); "
        "border: 1px solid palette(mid); outline: 0; padding: 2px; }"
        "QComboBox QAbstractItemView::item { min-height: 24px; padding: 0 8px; }"
        "QComboBox QAbstractItemView::item:hover, QComboBox QAbstractItemView::item:selected { "
        "background: palette(highlight); color: palette(highlighted-text); }");
}

void ConfigurePlayerAssignment::ModernizeCombos(QWidget* root) {
    // A combo box's own list painter ignores the style sheet and paints the hovered row white:
    // the standard painter follows it
    for (auto* combo : root->findChildren<QComboBox*>()) {
        if (!combo->property("modern_list").toBool()) {
            combo->setItemDelegate(new QStyledItemDelegate(combo));
            combo->setProperty("modern_list", true);
        }
    }
}

QString ConfigurePlayerAssignment::OutlinedButtonStyle() {
    return QStringLiteral(
        "QPushButton, QToolButton { border: 1px solid palette(highlight); border-radius: 6px; "
        "padding: 3px 12px; color: palette(highlight); font-weight: bold; "
        "background: palette(base); }"
        "QPushButton:hover, QToolButton:hover { background: palette(highlight); "
        "color: palette(highlighted-text); }"
        "QPushButton:disabled, QToolButton:disabled { border-color: palette(mid); "
        "color: palette(mid); }");
}

QWidget* ConfigurePlayerAssignment::MakeSegmented(QWidget* parent, const QStringList& labels,
                                                 const std::vector<int>& ids,
                                                 QButtonGroup* group) {
    auto* bar = new QWidget(parent);
    auto* bar_layout = new QHBoxLayout(bar);
    bar_layout->setContentsMargins(0, 0, 0, 0);
    bar_layout->setSpacing(0);
    for (int k = 0; k < labels.size(); ++k) {
        auto* button = new QPushButton(labels[k], bar);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        QString style = QStringLiteral(
            "QPushButton { padding: 5px 14px; border: 1px solid palette(mid); "
            "background: palette(button); }"
            "QPushButton:checked { background: palette(highlight); "
            "color: palette(highlighted-text); border-color: palette(highlight); }"
            "QPushButton:disabled { color: palette(mid); }");
        if (k == 0) {
            style += QStringLiteral("QPushButton { border-top-left-radius: 6px; "
                                    "border-bottom-left-radius: 6px; }");
        } else {
            style += QStringLiteral("QPushButton { border-left-width: 0px; }");
        }
        if (k == labels.size() - 1) {
            style += QStringLiteral("QPushButton { border-top-right-radius: 6px; "
                                    "border-bottom-right-radius: 6px; }");
        }
        button->setStyleSheet(style);
        group->addButton(button, ids[static_cast<std::size_t>(k)]);
        bar_layout->addWidget(button);
    }
    return bar;
}

ConfigurePlayerAssignment::ConfigurePlayerAssignment(QWidget* parent) : QWidget(parent) {
    // The cards keep their natural height and the page scrolls when they do not fit (squeezed,
    // a card clips its controls)
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    auto* content = new QWidget(scroll);
    content->setAutoFillBackground(false);
    scroll->setWidget(content);
    outer->addWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    content_layout = layout;
    layout->setSpacing(8);
    // Each section is a rounded card with its title on the edge; drop-downs are rounded fields
    // (kept tight: the Deck's screen is 800 pixels tall)
    setStyleSheet(QStringLiteral(
        "QGroupBox { border: 1px solid palette(mid); border-radius: 10px; margin-top: 10px; "
        "padding: 8px 8px 4px 8px; background: palette(alternate-base); }"
        "QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 4px; "
        "font-weight: bold; }") +
                  ModernComboStyle());

    // The controller applet's mode, as a segmented control: Manual (players set by hand),
    // Modern Console (automatic) and Real Hardware (the console's own applet)
    auto* mode_box = new QGroupBox(tr("Controller Assignment Handler"), this);
    auto* mode_layout = new QVBoxLayout(mode_box);
    handler_group = new QButtonGroup(this);
    auto* handler_row = new QHBoxLayout();
    handler_row->addWidget(MakeSegmented(mode_box,
                                     {tr("Manual"), tr("Real Hardware"), tr("Modern Console")},
                                     {HandlerManual, HandlerReal, HandlerModern}, handler_group));
    handler_row->addStretch(1);
    mode_layout->addLayout(handler_row);

    // Under Modern Console: whether this is a handheld with its own controls (Docked Handheld)
    handheld_panel = new QFrame(mode_box);
    handheld_panel->setObjectName(QStringLiteral("handheldPanel"));
    handheld_panel->setStyleSheet(QStringLiteral(
        "#handheldPanel { border: 1px solid palette(mid); border-radius: 8px; "
        "background: palette(base); }"));
    auto* panel_layout = new QHBoxLayout(handheld_panel);
    panel_layout->setContentsMargins(12, 8, 12, 8);
    auto* panel_text = new QVBoxLayout();
    auto* panel_title =
        new QLabel(tr("Portable Handheld Gaming Device with Built-in Controller"), handheld_panel);
    QFont title_font = panel_title->font();
    title_font.setBold(true);
    panel_title->setFont(title_font);
    panel_title->setWordWrap(true);
    auto* panel_note = new QLabel(tr("Like a Steam Deck, Legion Go or ROG Ally on a TV: other "
                                     "controllers come first, and the built-in controls play "
                                     "only when none is connected."),
                                  handheld_panel);
    panel_note->setWordWrap(true);
    panel_note->setEnabled(false);
    panel_text->addWidget(panel_title);
    panel_text->addWidget(panel_note);
    panel_layout->addLayout(panel_text, 1);
    handheld_switch = new ToggleSwitch(handheld_panel);
    handheld_switch->setToolTip(tr("On: this is a handheld with its own controls"));
    panel_layout->addWidget(handheld_switch, 0, Qt::AlignVCenter);
    auto* indent = new QHBoxLayout();
    indent->setContentsMargins(16, 0, 0, 0);
    indent->addWidget(handheld_panel);
    mode_layout->addLayout(indent);
    mode_info = new QLabel(mode_box);
    mode_info->setWordWrap(true);
    mode_layout->addWidget(mode_info);
    layout->addWidget(mode_box);

    // Assign Handheld Controller: declare the handheld's own controller instead of guessing it.
    // Shown for that mode only.
    builtin_row = new QGroupBox(tr("Assign Handheld Controller"), this);
    auto* builtin_layout = new QVBoxLayout(builtin_row);
    auto* picker_row = new QHBoxLayout();
    picker_row->addWidget(new QLabel(tr("Handheld's controller:"), builtin_row));
    builtin_picker = new QComboBox(builtin_row);
    builtin_picker->addItem(tr("Auto-detect"), QString{});
    picker_row->addWidget(builtin_picker, 1);
    builtin_identify = new QPushButton(tr("Identify"), builtin_row);
    builtin_identify->setCursor(Qt::PointingHandCursor);
    builtin_identify->setStyleSheet(OutlinedButtonStyle());
    picker_row->addWidget(builtin_identify);
    builtin_layout->addLayout(picker_row);
    builtin_status = new QLabel(builtin_row);
    builtin_status->setWordWrap(true);
    builtin_layout->addWidget(builtin_status);
    layout->addWidget(builtin_row);

    // The players right now, as the engine assigned them
    now_box = new QGroupBox(tr("Connected Players"), this);
    auto* now_layout = new QVBoxLayout(now_box);
    players_box = new QWidget(now_box);
    players_list = new QVBoxLayout(players_box);
    players_list->setContentsMargins(0, 0, 0, 0);
    players_list->setSpacing(6);
    now_layout->addWidget(players_box);
    players_note = new QLabel(now_box);
    players_note->setWordWrap(true);
    now_layout->addWidget(players_note);
    layout->addWidget(now_box);

    // What shows over the game: a title, a line about it and a switch per item
    auto* display = new QGroupBox(tr("On-Screen Display"), this);
    display_box = display;
    auto* display_layout = new QVBoxLayout(display);
    const auto add_switch = [display, display_layout](const QString& title,
                                                      const QString& note) {
        auto* line = new QHBoxLayout();
        auto* text = new QVBoxLayout();
        text->setSpacing(1);
        auto* title_label = new QLabel(title, display);
        QFont font = title_label->font();
        font.setBold(true);
        title_label->setFont(font);
        auto* note_label = new QLabel(note, display);
        note_label->setWordWrap(true);
        note_label->setEnabled(false);
        text->addWidget(title_label);
        text->addWidget(note_label);
        line->addLayout(text, 1);
        auto* toggle = new ToggleSwitch(display);
        line->addWidget(toggle, 0, Qt::AlignVCenter);
        display_layout->addLayout(line);
        return toggle;
    };
    // The same switch as View > Show Controller Overlay
    overlay_toggle = add_switch(tr("Controller Overlay"),
                                tr("The players, their controllers and their last buttons in "
                                   "the top-left corner."));
    original_overlay = Settings::values.console_debug_overlay.GetValue();
    overlay_toggle->setChecked(original_overlay);
    connect(overlay_toggle, &QAbstractButton::toggled, this,
            [](bool shown) { Settings::values.console_debug_overlay.SetValue(shown); });
    hints_toggle = add_switch(tr("Controller Hints"),
                              tr("Names a controller in the bottom-left corner for a moment "
                                 "when it joins as a player or leaves."));
    original_hints = Settings::values.console_hints.GetValue();
    hints_toggle->setChecked(original_hints);
    connect(hints_toggle, &QAbstractButton::toggled, this,
            [](bool shown) { Settings::values.console_hints.SetValue(shown); });
    layout->addWidget(display);
    layout->addStretch(1);

    // The mode applies at once, so the players switch while the dialog is open; closing it
    // without applying puts the previous mode back (see the destructor)
    original_mode = static_cast<int>(Settings::values.controller_applet_mode.GetValue());
    connect(handler_group, &QButtonGroup::idClicked, this, [this] {
        // Picking any handler leaves the hidden Legacy Applet
        legacy_applet = false;
        OnModeChanged();
    });
    connect(handheld_switch, &QAbstractButton::clicked, this,
            &ConfigurePlayerAssignment::OnModeChanged);
    connect(builtin_picker, &QComboBox::currentIndexChanged, this,
            &ConfigurePlayerAssignment::OnBuiltinPicked);
    connect(builtin_identify, &QPushButton::clicked, this,
            &ConfigurePlayerAssignment::OnIdentifyPressed);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &ConfigurePlayerAssignment::Refresh);
    // Fast enough for the highlight to follow a button press
    timer->start(50);

    LoadConfiguration();
    Refresh();
}

ConfigurePlayerAssignment::~ConfigurePlayerAssignment() {
    if (!applied) {
        Settings::values.controller_applet_mode.SetValue(
            static_cast<Settings::AppletMode>(original_mode));
        Settings::values.console_debug_overlay.SetValue(original_overlay);
        Settings::values.console_hints.SetValue(original_hints);
    }
}

void ConfigurePlayerAssignment::LoadConfiguration() {
    const auto current = Settings::values.controller_applet_mode.GetValue();
    // Legacy Applet has no button for now: it shows as Manual and stays until another pick
    legacy_applet = current == Settings::AppletMode::HLE;
    int handler = HandlerManual;
    if (current == Settings::AppletMode::ConsoleMode ||
        current == Settings::AppletMode::HandheldConsoleMode) {
        handler = HandlerModern;
    } else if (current == Settings::AppletMode::LLE) {
        handler = HandlerReal;
    }
    handler_group->button(handler)->setChecked(true);
    handheld_switch->setChecked(current == Settings::AppletMode::HandheldConsoleMode);
    UpdateModeUi();
}

int ConfigurePlayerAssignment::SelectedMode() const {
    switch (handler_group->checkedId()) {
    case HandlerModern:
        return static_cast<int>(handheld_switch->isChecked()
                                    ? Settings::AppletMode::HandheldConsoleMode
                                    : Settings::AppletMode::ConsoleMode);
    case HandlerReal:
        return static_cast<int>(Settings::AppletMode::LLE);
    default:
        return static_cast<int>(legacy_applet ? Settings::AppletMode::HLE
                                              : Settings::AppletMode::Disabled);
    }
}

void ConfigurePlayerAssignment::OnModeChanged() {
    Settings::values.controller_applet_mode.SetValue(
        static_cast<Settings::AppletMode>(SelectedMode()));
    UpdateModeUi();
}

void ConfigurePlayerAssignment::ApplyConfiguration() {
    Settings::values.controller_applet_mode.SetValue(
        static_cast<Settings::AppletMode>(SelectedMode()));
    applied = true;
}

void ConfigurePlayerAssignment::UpdateModeUi() {
    const auto selected = static_cast<Settings::AppletMode>(SelectedMode());
    handheld_panel->setVisible(handler_group->checkedId() == HandlerModern);
    // Only the automatic modes have live players to show
    const bool automatic = IsAutomatic(selected);
    switch (selected) {
    case Settings::AppletMode::ConsoleMode:
        mode_info->setText(tr("Controller assignment is automatic. Each controller is assigned "
                              "to the next available player when it connects."));
        break;
    case Settings::AppletMode::HandheldConsoleMode:
        mode_info->setText(tr("Controller assignment is automatic, with the handheld's own "
                              "controls last."));
        break;
    case Settings::AppletMode::Disabled:
        mode_info->setText(tr("Controllers are assigned in each player's tab. Games use the "
                              "configured players without showing a controller applet."));
        break;
    case Settings::AppletMode::LLE:
        mode_info->setText(tr("Controllers are assigned in each player's tab. When a game "
                              "requests controller setup, the console's own controller applet "
                              "is shown. This requires the system firmware."));
        break;
    default:
        mode_info->setText(tr("Controllers are assigned in each player's tab. When a game "
                              "requests controller setup, Eden's controller applet is shown."));
        break;
    }
    now_box->setVisible(automatic);
    display_box->setVisible(automatic);
    builtin_row->setVisible(selected == Settings::AppletMode::HandheldConsoleMode);
    Refresh();
}

void ConfigurePlayerAssignment::Refresh() {
    RefreshBuiltin();
    RefreshPlayers();
}

void ConfigurePlayerAssignment::RefreshBuiltin() {
    auto& state = Common::GetControllerConsoleState();
    std::vector<std::pair<std::string, std::string>> pads;
    bool armed = false;
    std::chrono::steady_clock::time_point armed_at{};
    std::string detected_name;
    {
        std::scoped_lock lock{state.mutex};
        pads = state.pads;
        armed = state.identify_armed;
        armed_at = state.identify_armed_at;
        detected_name = state.handheld_name;
    }

    // The entries name the connected controllers; under Steam their data is the Steam handle
    // (every Steam virtual pad shares one GUID), otherwise the pad's identity
    QString signature;
    for (const auto& [display, ident] : pads) {
        signature += QString::fromStdString(ident) + QLatin1Char('|');
    }
    if (signature != builtin_signature) {
        builtin_signature = signature;
        const QString current = builtin_picker->currentData().toString();
        const bool blocked = builtin_picker->blockSignals(true);
        builtin_picker->clear();
        builtin_picker->addItem(tr("Auto-detect"), QString{});
        for (const auto& [display, ident] : pads) {
            builtin_picker->addItem(QString::fromStdString(display),
                                    QString::fromStdString(ident));
        }
        const int keep = builtin_picker->findData(current);
        builtin_picker->setCurrentIndex(keep > 0 ? keep : 0);
        builtin_picker->blockSignals(blocked);
    }

    if (armed) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::seconds{15} - (std::chrono::steady_clock::now() - armed_at))
                              .count();
        builtin_status->setText(tr("Waiting for a button press (%1 seconds)...")
                                    .arg(std::max<qint64>(0, left)));
        // (The line below counts down)
        builtin_identify->setText(tr("Press a button..."));
        return;
    }
    builtin_identify->setText(tr("Identify"));

    // What is stored for the way Eden was launched: the Steam handle under Steam, the raw
    // pad's identity otherwise (each launch kind keeps its own)
    const bool steam = LaunchedThroughSteam();
    const QString identity =
        QString::fromStdString(Settings::values.builtin_pad_identity.GetValue());
    const QString handle =
        QString::fromStdString(Settings::values.builtin_pad_steam_handle.GetValue());
    const QString stored = steam ? (handle.isEmpty() ? QString{}
                                                     : QStringLiteral("steamhandle:") + handle)
                                 : identity;
    const bool declared = !steam && Settings::values.builtin_pad_manual.GetValue();

    // Keep the picker on a declared pad, else on automatic detection
    const int stored_index = stored.isEmpty() ? -1 : builtin_picker->findData(stored);
    const int want_index = declared && stored_index > 0 ? stored_index : 0;
    if (builtin_picker->currentIndex() != want_index) {
        const bool blocked = builtin_picker->blockSignals(true);
        builtin_picker->setCurrentIndex(want_index);
        builtin_picker->blockSignals(blocked);
    }

    bool engine_active = false;
    {
        std::scoped_lock lock{state.mutex};
        engine_active = state.engine_active;
    }
    if (engine_active && detected_name.empty() &&
        Settings::values.controller_applet_mode.GetValue() ==
            Settings::AppletMode::HandheldConsoleMode) {
        builtin_status->setText(tr("Handheld controller not detected \u2014 working like Modern "
                                   "until it is. Pick it above, or use Identify."));
    } else if (stored.isEmpty() && detected_name.empty()) {
        builtin_status->setText(
            tr("Not set. Pick the handheld's controller above, or use Identify."));
    } else {
        const QString name = stored_index > 0 ? builtin_picker->itemText(stored_index)
                             : !detected_name.empty() ? QString::fromStdString(detected_name)
                                                      : tr("not set");
        builtin_status->setText(
            tr("Handheld: %1 (%2). %3")
                .arg(name)
                .arg(want_index > 0 ? tr("declared") : tr("auto-detected"))
                .arg(!detected_name.empty()
                         ? tr("Takes no player while external controllers are connected.")
                         : tr("Not currently connected.")));
    }
}

void ConfigurePlayerAssignment::RefreshPlayers() {
    auto& state = Common::GetControllerConsoleState();
    std::vector<Common::ControllerConsoleState::PlayerRow> players;
    std::string handheld_name;
    bool handheld_has_player = false;
    bool engine_active = false;
    {
        std::scoped_lock lock{state.mutex};
        players = state.players;
        handheld_name = state.handheld_name;
        handheld_has_player = state.handheld_has_player;
        engine_active = state.engine_active;
    }
    const auto selected = static_cast<Settings::AppletMode>(SelectedMode());
    const bool in_effect = selected == Settings::values.controller_applet_mode.GetValue();

    // The engine decides the flashes (the overlay shows the same): green when a controller
    // takes a player, red when one leaves; a leaving row stays red until it is dropped
    constexpr qint64 flash_ms = 1500;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int last_player = -1;
    for (const auto& entry : players) {
        if (!entry.leaving) {
            last_player = (std::max)(last_player, entry.player);
        }
    }

    // The saved input profiles, by their files (cheap enough to look at every refresh)
    QStringList profiles;
    {
        const QDir dir{QString::fromStdString(Common::FS::PathToUTF8String(
            Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "input"))};
        for (const QString& file : dir.entryList({QStringLiteral("*.ini")}, QDir::Files,
                                                 QDir::Name)) {
            const QString name = file.chopped(4);
            if (InputProfiles::IsProfileNameValid(name.toStdString())) {
                profiles.push_back(name);
            }
        }
    }

    // Rebuild the rows when they change; the colours only restyle them
    QString signature = QString::number(swap_from) + QLatin1Char(';') +
                        profiles.join(QLatin1Char(',')) + QLatin1Char(';');
    for (const auto& entry : players) {
        signature += QString::number(entry.player) + QLatin1Char(':') +
                     QString::fromStdString(entry.name) + QLatin1Char('/') +
                     QString::fromStdString(entry.profile) +
                     (entry.leaving ? QStringLiteral("x") : QString{}) + QLatin1Char('|');
    }
    if (!handheld_has_player) {
        signature += QStringLiteral("h:") + QString::fromStdString(handheld_name);
    }
    if (signature != players_signature) {
        players_signature = signature;
        for (QFrame* row : player_rows) {
            players_list->removeWidget(row);
            row->hide();
            row->deleteLater();
        }
        player_rows.clear();
        row_players.clear();
        for (const auto& entry : players) {
            const int player = entry.player;
            auto* row = new PlayerRow(player, players_box);
            row->setToolTip(tr("Click to rumble this controller. Hold, then drop on another "
                               "player to swap them."));
            row->on_click = [this](int p) { RequestRumble(p); };
            row->on_drop = [this](int from, int to) { RequestSwap(from, to); };
            auto* line = new QHBoxLayout(row);
            line->setContentsMargins(8, 5, 6, 5);
            line->setSpacing(6);
            auto* grip = new QLabel(QStringLiteral("\u283F"), row);
            grip->setToolTip(tr("Drag onto another player to swap"));
            grip->setEnabled(false);
            line->addWidget(grip);
            // The player's badge, in the overlay's colour for that player
            auto* badge = new QLabel(QStringLiteral("P%1").arg(player + 1), row);
            badge->setAlignment(Qt::AlignCenter);
            badge->setFixedSize(34, 20);
            badge->setStyleSheet(QStringLiteral("QLabel { background: %1; color: white; "
                                                "border-radius: 10px; font-weight: bold; }")
                                     .arg(PlayerColors[static_cast<std::size_t>(player) % 8]
                                              .name()));
            line->addWidget(badge);
            line->addWidget(new QLabel(QString::fromStdString(entry.name), row), 1);
            const bool live = !entry.leaving;

            auto* up = new QToolButton(row);
            up->setArrowType(Qt::UpArrow);
            up->setAutoRaise(true);
            up->setToolTip(tr("Swap with P%1").arg(player));
            up->setEnabled(live && player > 0);
            connect(up, &QToolButton::clicked, this,
                    [this, player] { RequestSwap(player, player - 1); });
            line->addWidget(up);
            auto* down = new QToolButton(row);
            down->setArrowType(Qt::DownArrow);
            down->setAutoRaise(true);
            down->setToolTip(tr("Swap with P%1").arg(player + 2));
            down->setEnabled(live && player < last_player);
            connect(down, &QToolButton::clicked, this,
                    [this, player] { RequestSwap(player, player + 1); });
            line->addWidget(down);


            // Swap and Config stand out: outlined in the highlight colour, filled on hover (and
            // Swap filled while it waits for the second player)
            const QString outlined = OutlinedButtonStyle();
            const QString filled = QStringLiteral(
                "QToolButton { border: 1px solid palette(highlight); border-radius: 6px; "
                "padding: 3px 12px; background: palette(highlight); "
                "color: palette(highlighted-text); font-weight: bold; }");
            auto* swap = new QToolButton(row);
            swap->setCursor(Qt::PointingHandCursor);
            swap->setStyleSheet(swap_from >= 0 ? filled : outlined);
            if (swap_from < 0) {
                swap->setText(tr("Swap"));
            } else if (swap_from == player) {
                swap->setText(tr("Cancel"));
            } else {
                swap->setText(tr("Swap with P%1").arg(swap_from + 1));
            }
            swap->setEnabled(live);
            connect(swap, &QToolButton::clicked, this, [this, player] { OnSwapPressed(player); });
            line->addWidget(swap);

            auto* settings = new QToolButton(row);
            settings->setText(tr("Config"));
            settings->setCursor(Qt::PointingHandCursor);
            settings->setStyleSheet(outlined);
            settings->setToolTip(tr("Open P%1's tab").arg(player + 1));
            settings->setEnabled(static_cast<bool>(open_player_tab));
            connect(settings, &QToolButton::clicked, this, [this, player] {
                if (open_player_tab) {
                    open_player_tab(player);
                }
            });
            line->addWidget(settings);

            // The profile the player's controller carries; picking one puts it on it, and
            // "No profile" takes it off (the controller's mappings from before come back)
            auto* profile = new QComboBox(row);
            profile->addItem(tr("No profile"));
            profile->addItems(profiles);
            // (Not found is -1: "No profile")
            profile->setCurrentIndex(
                static_cast<int>(profiles.indexOf(QString::fromStdString(entry.profile)) + 1));
            profile->setToolTip(tr("Mapping profile for P%1's controller").arg(player + 1));
            profile->setEnabled(live && (!profiles.isEmpty() || !entry.profile.empty()));
            connect(profile, qOverload<int>(&QComboBox::activated), this,
                    [player, profile](int index) {
                        auto& console = Common::GetControllerConsoleState();
                        std::scoped_lock lock{console.mutex};
                        if (index == 0) {
                            console.profile_requests.push_back({player, {}, true});
                        } else {
                            console.profile_requests.push_back(
                                {player, profile->itemText(index).toStdString()});
                        }
                    });
            line->addWidget(profile);
            // The quick way to take the profile off (as "No profile")
            auto* remove = new QToolButton(row);
            remove->setText(QStringLiteral("\u2715"));
            remove->setAutoRaise(true);
            remove->setToolTip(tr("Remove the profile from P%1").arg(player + 1));
            remove->setFixedWidth(remove->sizeHint().height());
            remove->setVisible(!entry.profile.empty());
            remove->setEnabled(live);
            connect(remove, &QToolButton::clicked, this, [player] {
                auto& console = Common::GetControllerConsoleState();
                std::scoped_lock lock{console.mutex};
                console.profile_requests.push_back({player, {}, true});
            });
            line->addWidget(remove);


            players_list->addWidget(row);
            player_rows.push_back(row);
            row_players.push_back(player);
        }
        if (!handheld_name.empty() && !handheld_has_player) {
            auto* row = new QFrame(players_box);
            auto* line = new QHBoxLayout(row);
            line->setContentsMargins(4, 2, 4, 2);
            line->addWidget(new QLabel(QStringLiteral("\u2014"), row));
            line->addWidget(new QLabel(tr("%1 (built-in, not a player while other controllers "
                                          "are connected)")
                                           .arg(QString::fromStdString(handheld_name)),
                                       row),
                            1);
            players_list->addWidget(row);
            player_rows.push_back(row);
            row_players.push_back(-1);
        }
    }

    // Red while leaving; blue while its controller has an input held or was just clicked to
    // rumble (pressing a button shows which controller is which player); green when new
    // Rows look like cards that can be picked up; a fill marks what is happening to them
    const QString card = QStringLiteral("QFrame#playerRow { border: 1px solid palette(mid); "
                                        "border-radius: 8px; background: palette(base); }");
    const auto fill = [](const char* rgba) {
        return QStringLiteral("QFrame#playerRow { border: 1px solid palette(mid); border-radius: "
                              "8px; background-color: rgba(%1); } "
                              "QFrame#playerRow QLabel { color: white; }")
            .arg(QString::fromLatin1(rgba));
    };
    const auto blink_on = [now](qint64 since) {
        const qint64 elapsed = now - since;
        return elapsed < flash_ms && (elapsed / 125) % 2 == 0;
    };
    for (size_t k = 0; k < player_rows.size(); ++k) {
        const int player = row_players[k];
        const auto entry = std::find_if(players.begin(), players.end(),
                                        [&](const auto& row) { return row.player == player; });
        if (player < 0 || entry == players.end()) {
            continue;
        }
        const auto rumbled = rumbled_at.find(player);
        const bool active = entry->active ||
                            (rumbled != rumbled_at.end() && now - rumbled->second < 500);
        const bool blink = entry->flash != 0 && blink_on(entry->flash_since);
        QString style = card;
        if (entry->flash == 2) {
            if (blink) {
                style = fill("210, 60, 60, 190");
            }
        } else if (active && !entry->leaving) {
            style = fill("60, 120, 230, 190");
        } else if (blink) {
            style = fill("60, 180, 90, 190");
        }
        if (player_rows[k]->styleSheet() != style) {
            player_rows[k]->setStyleSheet(style);
        }
    }

    if (!in_effect || (IsAutomatic(selected) && !engine_active)) {
        players_note->setText(tr("Switching..."));
    } else if (last_player < 0) {
        players_note->setText(tr("No controller is a player right now."));
    } else {
        players_note->setText(tr("Press a button on a controller to see its player. Swaps last "
                                 "for this session and do not change your saved controls."));
    }
    players_box->setVisible(in_effect && engine_active);
    ModernizeCombos(this);
}

void ConfigurePlayerAssignment::OnBuiltinPicked(int index) {
    const QString ident = builtin_picker->itemData(index).toString();
    if (ident.isEmpty()) {
        // Detect automatically: forget the stored pad, so the engine detects it afresh
        Settings::values.builtin_pad_identity.SetValue(std::string{});
        Settings::values.builtin_pad_steam_handle.SetValue(std::string{});
        Settings::values.builtin_pad_manual.SetValue(false);
        return;
    }
    // Under Steam the entries are Steam handles: they set the Steam launches' built-in pad and
    // leave the direct launches' identity alone (each way of launching keeps its own)
    const QString prefix = QStringLiteral("steamhandle:");
    if (ident.startsWith(prefix)) {
        Settings::values.builtin_pad_steam_handle.SetValue(ident.mid(prefix.size()).toStdString());
    } else {
        Settings::values.builtin_pad_identity.SetValue(ident.toStdString());
        Settings::values.builtin_pad_manual.SetValue(true);
    }
}

void ConfigurePlayerAssignment::OnIdentifyPressed() {
    auto& state = Common::GetControllerConsoleState();
    std::scoped_lock lock{state.mutex};
    state.identify_armed = true;
    state.identify_armed_at = std::chrono::steady_clock::now();
}

void ConfigurePlayerAssignment::OnSwapPressed(int player) {
    if (swap_from < 0) {
        swap_from = player;
    } else {
        if (swap_from != player) {
            RequestSwap(swap_from, player);
        }
        swap_from = -1;
    }
    RefreshPlayers();
}

void ConfigurePlayerAssignment::RequestSwap(int a, int b) {
    if (a == b || a < 0 || b < 0 || a >= MaxPlayers || b >= MaxPlayers) {
        return;
    }
    auto& state = Common::GetControllerConsoleState();
    std::scoped_lock lock{state.mutex};
    state.swap_requests.emplace_back(a, b);
}

void ConfigurePlayerAssignment::RequestRumble(int player) {
    rumbled_at[player] = QDateTime::currentMSecsSinceEpoch();
    auto& state = Common::GetControllerConsoleState();
    std::scoped_lock lock{state.mutex};
    state.rumble_requests.push_back(player);
}

void ConfigurePlayerAssignment::AddTopWidget(QWidget* widget) {
    content_layout->insertWidget(0, widget);
}

void ConfigurePlayerAssignment::SetOpenPlayerTab(std::function<void(int)> open) {
    open_player_tab = std::move(open);
    players_signature.clear();
    RefreshPlayers();
}

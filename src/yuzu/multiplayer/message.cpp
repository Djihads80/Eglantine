// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <functional>
#include <string>

#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QValidator>

#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/hle/service/acc/profile_manager.h"
#include "yuzu/multiplayer/message.h"
#include "yuzu/multiplayer/validation.h"

namespace NetworkMessage {
const ConnectionError ErrorManager::USERNAME_NOT_VALID(
    QT_TR_NOOP("Username is not valid. Must be 4 to 20 alphanumeric characters."));
const ConnectionError ErrorManager::ROOMNAME_NOT_VALID(
    QT_TR_NOOP("Room name is not valid. Must be 4 to 20 alphanumeric characters."));
const ConnectionError ErrorManager::USERNAME_NOT_VALID_SERVER(
    QT_TR_NOOP("Username is already in use or not valid. Please choose another."));
const ConnectionError ErrorManager::IP_ADDRESS_NOT_VALID(
    QT_TR_NOOP("IP is not a valid IPv4 address."));
const ConnectionError ErrorManager::PORT_NOT_VALID(
    QT_TR_NOOP("Port must be a number between 0 to 65535."));
const ConnectionError ErrorManager::GAME_NOT_SELECTED(QT_TR_NOOP(
    "You must choose a Preferred Game to host a room. If you do not have any games in your game "
    "list yet, add a game folder by clicking on the plus icon in the game list."));
const ConnectionError ErrorManager::NO_INTERNET(
    QT_TR_NOOP("Unable to find an internet connection. Check your internet settings."));
const ConnectionError ErrorManager::UNABLE_TO_CONNECT(
    QT_TR_NOOP("Unable to connect to the host. Verify that the connection settings are correct. If "
               "you still cannot connect, contact the room host and verify that the host is "
               "properly configured with the external port forwarded."));
const ConnectionError ErrorManager::ROOM_IS_FULL(
    QT_TR_NOOP("Unable to connect to the room because it is already full."));
const ConnectionError ErrorManager::COULD_NOT_CREATE_ROOM(
    QT_TR_NOOP("Creating a room failed. Please retry. Restarting Eden might be necessary."));
const ConnectionError ErrorManager::HOST_BANNED(
    QT_TR_NOOP("The host of the room has banned you. Speak with the host to unban you "
               "or try a different room."));
const ConnectionError ErrorManager::WRONG_VERSION(
    QT_TR_NOOP("Version mismatch! Please update to the latest version of Eden. If the problem "
               "persists, contact the room host and ask them to update the server."));
const ConnectionError ErrorManager::WRONG_PASSWORD(QT_TR_NOOP("Incorrect password."));
const ConnectionError ErrorManager::GENERIC_ERROR(QT_TR_NOOP(
    "An unknown error occurred. If this error continues to occur, please open an issue"));
const ConnectionError ErrorManager::LOST_CONNECTION(
    QT_TR_NOOP("Connection to room lost. Try to reconnect."));
const ConnectionError ErrorManager::HOST_KICKED(
    QT_TR_NOOP("You have been kicked by the room host."));
const ConnectionError ErrorManager::IP_COLLISION(
    QT_TR_NOOP("IP address is already in use. Please choose another."));
const ConnectionError ErrorManager::PERMISSION_DENIED(
    QT_TR_NOOP("You do not have enough permission to perform this action."));
const ConnectionError ErrorManager::NO_SUCH_USER(QT_TR_NOOP(
    "The user you are trying to kick/ban could not be found.\nThey may have left the room."));
const ConnectionError ErrorManager::NO_INTERFACE_SELECTED(QT_TR_NOOP(
    "No valid network interface is selected.\nPlease go to Configure -> System -> Network and "
    "make a selection."));

static bool WarnMessage(const std::string& title, const std::string& text) {
    return QMessageBox::Ok == QMessageBox::warning(nullptr, QObject::tr(title.c_str()),
                                                   QObject::tr(text.c_str()),
                                                   QMessageBox::Ok | QMessageBox::Cancel);
}

void ErrorManager::ShowError(const ConnectionError& e) {
    QMessageBox::critical(nullptr, tr("Error"), tr(e.GetString().c_str()));
}

bool WarnGameRunning() {
    return WarnMessage(
        QT_TR_NOOP("Game already running"),
        QT_TR_NOOP("Joining a room when the game is already running is discouraged "
                   "and can cause the room feature not to work correctly.\nProceed anyway?"));
}

bool WarnCloseRoom() {
    return WarnMessage(
        QT_TR_NOOP("Leave Room"),
        QT_TR_NOOP("You are about to close the room. Any network connections will be closed."));
}

bool WarnDisconnect() {
    return WarnMessage(
        QT_TR_NOOP("Disconnect"),
        QT_TR_NOOP("You are about to leave the room. Any network connections will be closed."));
}

namespace {
/// What the user chose when warned about a default profile name or nickname.
enum class DefaultNameAction {
    Continue,    ///< Use the name as it is
    ChangeName,  ///< Fix the name in the settings the button pointed at
    UseNickname, ///< Rename the user profile to the current nickname
    Cancel,      ///< Do not join, host or connect
};

/// Which of the two names a default-name warning is about.
enum class DefaultNameKind {
    ProfileName, ///< The user profile name
    Nickname,    ///< The lobby nickname
};

/// A name the emulator projects ship as a default, and whether this one shipped it as the nickname.
struct DefaultDisplayName {
    const char* name;
    bool shipped_as_nickname;
};

constexpr std::array<DefaultDisplayName, 8> DEFAULT_DISPLAY_NAMES{{
    {"Eglantine", true},
    {"Eden", true},
    {"Lichii", false},
    {"Suyu", false},
    {"Citron", false},
    {"Yuzu", false},
    {"Torzu", false},
    {"Sudachii", false},
}};

const DefaultDisplayName* FindDefaultDisplayName(const QString& name) {
    const QString trimmed_name = name.trimmed();
    for (const auto& default_name : DEFAULT_DISPLAY_NAMES) {
        if (trimmed_name.compare(QLatin1String(default_name.name), Qt::CaseInsensitive) == 0) {
            return &default_name;
        }
    }
    return nullptr;
}

DefaultNameAction WarnDefaultName(QWidget* parent, const QString& name, DefaultNameKind kind,
                                  bool offer_nickname) {
    const bool profile_name = kind == DefaultNameKind::ProfileName;

    QMessageBox message_box(parent);
    message_box.setIcon(QMessageBox::Warning);
    message_box.setWindowTitle(QObject::tr("Default Name Detected"));
    message_box.setText(
        profile_name
            ? QObject::tr("A default user profile name \"%1\" has been detected, to not cause "
                          "confusion and allow other players to recognize you, please change your "
                          "user profile name")
                  .arg(name)
            : QObject::tr("A default nickname \"%1\" has been detected, to not cause confusion and "
                          "allow other players to recognize you, please change your nickname")
                  .arg(name));

    QPushButton* change_name = message_box.addButton(
        profile_name ? QObject::tr("Change Profile Name") : QObject::tr("Change Nickname"),
        QMessageBox::AcceptRole);
    QPushButton* use_nickname = offer_nickname
                                    ? message_box.addButton(QObject::tr("Use My Nickname"),
                                                            QMessageBox::ActionRole)
                                    : nullptr;
    QPushButton* proceed = message_box.addButton(
        QObject::tr("Continue Anyway"), QMessageBox::AcceptRole);
    QPushButton* cancel = message_box.addButton(QObject::tr("Cancel"), QMessageBox::RejectRole);

    message_box.setDefaultButton(change_name);
    message_box.setEscapeButton(cancel);
    message_box.exec();

    if (message_box.clickedButton() == change_name) {
        return DefaultNameAction::ChangeName;
    }
    if (use_nickname != nullptr && message_box.clickedButton() == use_nickname) {
        return DefaultNameAction::UseNickname;
    }
    if (message_box.clickedButton() == proceed) {
        return DefaultNameAction::Continue;
    }
    return DefaultNameAction::Cancel;
}
} // Anonymous namespace

std::string GetProfileUsername(Core::System& system) {
    const auto& current_user =
        system.GetProfileManager().GetUser(Settings::values.current_user.GetValue());
    Service::Account::ProfileBase profile{};

    if (!current_user.has_value()) {
        return "";
    }

    if (!system.GetProfileManager().GetProfileBase(*current_user, profile)) {
        return "";
    }

    return Common::StringFromFixedZeroTerminatedBuffer(
        reinterpret_cast<const char*>(profile.username.data()), profile.username.size());
}

QString GetDefaultDisplayName(const QString& name) {
    const auto* default_name = FindDefaultDisplayName(name);
    return default_name != nullptr ? QString::fromLatin1(default_name->name) : QString();
}

bool IsShippedDefaultNickname(const QString& name) {
    const auto* default_name = FindDefaultDisplayName(name);
    return default_name != nullptr && default_name->shipped_as_nickname;
}

bool RenameCurrentProfile(Core::System& system, const QString& new_name) {
    const auto current_user =
        system.GetProfileManager().GetUser(Settings::values.current_user.GetValue());
    if (!current_user.has_value()) {
        return false;
    }

    return system.GetProfileManager().SetProfileUsername(*current_user, new_name.toStdString());
}

bool ConfirmDisplayName(QWidget* parent, Core::System& system, QLineEdit* nickname,
                        const std::function<void()>& open_profile_settings) {
    const Validation validation;

    while (true) {
        const QString profile_name = QString::fromStdString(GetProfileUsername(system));
        const QString nickname_text = nickname->text();

        const QString default_profile_name = GetDefaultDisplayName(profile_name);
        const QString default_nickname = GetDefaultDisplayName(nickname_text);
        if (default_profile_name.isEmpty() && default_nickname.isEmpty()) {
            return true;
        }

        const bool profile_is_default = !default_profile_name.isEmpty();
        // Only useful when the nickname is a name of the user's own
        const bool offer_nickname = profile_is_default && default_nickname.isEmpty() &&
                                    nickname_text.compare(profile_name, Qt::CaseInsensitive) != 0;
        const QString detected = profile_is_default ? default_profile_name : default_nickname;

        switch (WarnDefaultName(parent, detected,
                                profile_is_default ? DefaultNameKind::ProfileName
                                                   : DefaultNameKind::Nickname,
                                offer_nickname)) {
        case DefaultNameAction::Cancel:
            return false;
        case DefaultNameAction::Continue:
            return true;
        case DefaultNameAction::UseNickname: {
            if (!RenameCurrentProfile(system, nickname_text)) {
                QMessageBox::warning(
                    parent, QObject::tr("Could Not Rename User Profile"),
                    QObject::tr("The user profile could not be renamed to \"%1\". Pick a different "
                                "name, or change it in the user profile settings.")
                        .arg(nickname_text));
            }
            break;
        }
        case DefaultNameAction::ChangeName: {
            if (!profile_is_default) {
                // Only the nickname is a default one, so let the user type a new one here
                nickname->setFocus();
                nickname->selectAll();
                return false;
            }

            open_profile_settings();

            // Carry a profile name picked in that dialog over to the nickname, unless the user
            // chose a nickname of their own
            const QString updated_profile_name = QString::fromStdString(GetProfileUsername(system));
            const bool nickname_is_default =
                !default_nickname.isEmpty() ||
                nickname_text.compare(profile_name, Qt::CaseInsensitive) == 0;
            if (updated_profile_name.compare(profile_name) != 0 && nickname_is_default) {
                QString candidate = updated_profile_name;
                int position = 0;
                if (validation.GetNickname()->validate(candidate, position) ==
                    QValidator::Acceptable) {
                    nickname->setText(updated_profile_name);
                }
            }
            break;
        }
        }
    }
}

} // namespace NetworkMessage

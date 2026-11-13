// SPDX-FileCopyrightText: Copyright 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <utility>
#include <QObject>
#include <QString>

class QLineEdit;
class QWidget;

namespace Core {
class System;
}

namespace NetworkMessage {

class ConnectionError {

public:
    explicit ConnectionError(std::string str) : err(std::move(str)) {}
    const std::string& GetString() const {
        return err;
    }

private:
    std::string err;
};

class ErrorManager : QObject {
    Q_OBJECT
public:
    /// When the nickname is considered invalid by the client
    static const ConnectionError USERNAME_NOT_VALID;
    static const ConnectionError ROOMNAME_NOT_VALID;
    /// When the nickname is considered invalid by the room server
    static const ConnectionError USERNAME_NOT_VALID_SERVER;
    static const ConnectionError IP_ADDRESS_NOT_VALID;
    static const ConnectionError PORT_NOT_VALID;
    static const ConnectionError GAME_NOT_SELECTED;
    static const ConnectionError NO_INTERNET;
    static const ConnectionError UNABLE_TO_CONNECT;
    static const ConnectionError ROOM_IS_FULL;
    static const ConnectionError COULD_NOT_CREATE_ROOM;
    static const ConnectionError HOST_BANNED;
    static const ConnectionError WRONG_VERSION;
    static const ConnectionError WRONG_PASSWORD;
    static const ConnectionError GENERIC_ERROR;
    static const ConnectionError LOST_CONNECTION;
    static const ConnectionError HOST_KICKED;
    static const ConnectionError IP_COLLISION;
    static const ConnectionError PERMISSION_DENIED;
    static const ConnectionError NO_SUCH_USER;
    static const ConnectionError NO_INTERFACE_SELECTED;
    /**
     *  Shows a standard QMessageBox with a error message
     */
    static void ShowError(const ConnectionError& e);
};

/**
 * Show a standard QMessageBox with a warning message about joining a room when
 * the game is already running
 * return true if the user wants to close the network connection
 */
bool WarnGameRunning();

/**
 * Show a standard QMessageBox with a warning message about leaving the room
 * return true if the user wants to close the network connection
 */
bool WarnCloseRoom();

/**
 * Show a standard QMessageBox with a warning message about disconnecting from the room
 * return true if the user wants to disconnect
 */
bool WarnDisconnect();

/**
 * The name the current user profile has, or an empty string when it cannot be read.
 */
std::string GetProfileUsername(Core::System& system);

/**
 * Returns the default name that matches, ignoring case and surrounding spaces, or an empty string
 * when the name is the user's own.
 */
QString GetDefaultDisplayName(const QString& name);

/**
 * Whether the emulator itself shipped this name as its default nickname, meaning a stored value was
 * never picked by the user and can be replaced.
 */
bool IsShippedDefaultNickname(const QString& name);

/**
 * Renames the current user profile. Returns false when the name does not fit the profile field or
 * the profile cannot be saved.
 */
bool RenameCurrentProfile(Core::System& system, const QString& new_name);

/**
 * Warns about a default user profile name or nickname before joining, hosting or connecting to a
 * room, and applies the fix the user picked. Returns false when the user cancelled.
 *
 * \param nickname The lobby nickname field, kept in sync when the profile name is changed.
 * \param open_profile_settings Opens the settings page that edits the user profile name.
 */
bool ConfirmDisplayName(QWidget* parent, Core::System& system, QLineEdit* nickname,
                        const std::function<void()>& open_profile_settings);

} // namespace NetworkMessage

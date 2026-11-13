// SPDX-FileCopyrightText: Copyright 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>
#include <QObject>
#include <QString>

class QWidget;

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

/// What the user chose when warned about a default profile name or nickname.
enum class DefaultNameAction {
    Continue,    ///< Use the name as it is
    ChangeName,  ///< Fix the name in the settings the button pointed at
    UseNickname, ///< Rename the user profile to the current lobby nickname
    Cancel,      ///< Do not join the room
};

/// Which of the two names a default-name warning is about.
enum class DefaultNameKind {
    ProfileName, ///< The user profile name, which is what other players see in the room
    Nickname,    ///< The lobby nickname
};

/**
 * Show a warning about a default name being visible to other players, and offer to change it.
 * \param parent Parent widget of the message box.
 * \param name The default name that was detected.
 * \param kind Whether the profile name or the nickname is the one that needs changing.
 * \param offer_nickname Whether to offer renaming the profile to the lobby nickname.
 * \return The action chosen by the user.
 */
DefaultNameAction WarnDefaultName(QWidget* parent, const QString& name, DefaultNameKind kind,
                                  bool offer_nickname);

} // namespace NetworkMessage

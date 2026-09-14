// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <glaze/core/reflect.hpp>
#include <glaze/glaze.hpp>
#include "common/announce_multiplayer_room.h"
#include "common/logging.h"
#include "web_service/announce_room_json.h"
#include "web_service/web_backend.h"

// clang-format off

// flattening specializations for member/room
// TODO: better solutions?
template <>
struct glz::meta<AnnounceMultiplayerRoom::Member> {
    using T = AnnounceMultiplayerRoom::Member;
    static constexpr auto value = glz::object(
        "username", &T::username,
        "nickname", &T::nickname,
        "avatarUrl", &T::avatarUrl,

        "gameName", [](auto& self) -> auto& { return self.game.name; },
        "gameId", [](auto& self) -> auto& { return self.game.id; }
    );
};

template <>
struct glz::meta<AnnounceMultiplayerRoom::Room> {
    using T = AnnounceMultiplayerRoom::Room;
    static constexpr auto value = glz::object(
        "port", [](auto& self) -> auto& { return self.information.port; },
        "name", [](auto& self) -> auto& { return self.information.name; },
        "description", [](auto& self) -> auto& { return self.information.description; },
        "maxPlayers", [](auto& self) -> auto& { return self.information.member_slots; },

        "preferredGameName", [](auto& self) -> auto& { return self.information.preferred_game.name; },
        "preferredGameId", [](auto& self) -> auto& { return self.information.preferred_game.id; },

        "netVersion", &T::netVersion,
        "hasPassword", &T::hasPassword,
        "players", &T::members
    );
};

struct RoomWrapper {
    AnnounceMultiplayerRoom::RoomList rooms;
};

// clang-format on

namespace WebService {

void RoomJson::SetRoomInformation(const std::string& name, const std::string& description,
                                  const u16 port, const u32 max_player, const u32 net_version,
                                  const bool has_password,
                                  const AnnounceMultiplayerRoom::GameInfo& preferred_game) {
    room.information.name = name;
    room.information.description = description;
    room.information.port = port;
    room.information.member_slots = max_player;
    room.netVersion = net_version;
    room.hasPassword = has_password;
    room.information.preferred_game = preferred_game;
}
void RoomJson::AddPlayer(const AnnounceMultiplayerRoom::Member& member) {
    room.members.push_back(member);
}

WebService::WebResult RoomJson::Update() {
    if (room_id.empty()) {
        LOG_ERROR(WebService, "Room must be registered to be updated");
        return WebService::WebResult{WebService::WebResult::Code::LibError,
                                     "Room is not registered", ""};
    }

    std::string json;
    auto ec = glz::write<glz::opts{.skip_null_members = true}>(
        glz::object("players", std::ref(room.members)), json);

    if (ec) {
        LOG_ERROR(WebService, "JSON write error when updating room members: {}", ec.custom_error_message);
        return WebService::WebResult{WebService::WebResult::Code::LibError,
                                     "Failed to serialize room members", ""};
    }

    return client.PostJson(std::format("/lobby/{}", room_id), json, false);
}

WebService::WebResult RoomJson::Register() {
    std::string json;
    auto ec = glz::write<glz::opts{.skip_null_members = true}>(room, json);

    if (ec) {
        LOG_ERROR(WebService, "JSON write error when updating room: {}", ec.custom_error_message);
        return WebService::WebResult{WebService::WebResult::Code::LibError,
                                     "Failed to serialize room data", ""};
    }

    auto result = client.PostJson("/lobby", json, false);
    if (result.result_code != WebService::WebResult::Code::Success) {
        return result;
    }

    auto reply_ec = glz::read_json(room, result.returned_data);
    if (reply_ec) {
        LOG_ERROR(WebService, "Room JSON parse error:\n{}", glz::format_error(ec, result.returned_data));
        return WebService::WebResult{WebService::WebResult::Code::LibError,
                                     "Failed to parse room response", ""};
    }

    room_id = room.id;
    return WebService::WebResult{WebService::WebResult::Code::Success, "", room.verify_uid};
}

void RoomJson::ClearPlayers() {
    room.members.clear();
}

AnnounceMultiplayerRoom::RoomList RoomJson::GetRoomList() {
    auto reply = client.GetJson("/lobby", true).returned_data;
    if (reply.empty()) {
        return {};
    }

    // glaze does not (yet) support glz::obj for reads?
    RoomWrapper wrapper;
    AnnounceMultiplayerRoom::RoomList room_list{};

    auto ec = glz::read_json(wrapper, reply);
    if (ec) {
        LOG_ERROR(WebService, "Room JSON parse error:\n{}", glz::format_error(ec, reply));
        return {};

    }

    return wrapper.rooms;}

void RoomJson::Delete() {
    if (room_id.empty()) {
        LOG_ERROR(WebService, "Room must be registered to be deleted");
    } else {
        // This jthread won't be destroyed until after the dtor has been ran
        // Once the thread finishes it will stay resident on the vector -- destroyed and freed by dtor()
        // this is still valid while in dtor, so... yeah
        detached_tasks.emplace_back([this](std::stop_token stop_token) {
            client.DeleteJson(fmt::format("/lobby/{}", room_id), "", false);
        });
    }
}

} // namespace WebService

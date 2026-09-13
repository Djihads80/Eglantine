// SPDX-FileCopyrightText: 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <glaze/glaze.hpp>

#include "common/logging.h"
#include "web_service/verify_login.h"
#include "web_service/web_backend.h"
#include "web_service/web_result.h"

namespace WebService {

struct Reply {
    std::optional<std::string> username;
};

bool VerifyLogin(const std::string& host, const std::string& username, const std::string& token) {
    Client client(host, username, token);
    auto reply = client.GetJson("/profile", false).returned_data;
    if (reply.empty()) {
        return false;
    }

    Reply reply_s{};
    auto ec = glz::read_json(reply_s, reply);

    if (ec) {
        LOG_WARNING(WebService, "Failed to parse verification profile:\n{}", glz::format_error(ec, reply));
        return false;
    }

    if (!reply_s.username) return username.empty();

    return reply_s.username == username;
}

} // namespace WebService

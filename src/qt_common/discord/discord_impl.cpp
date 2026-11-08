// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <optional>
#include <string>

#include <QEventLoop>
#include <boost/algorithm/string/replace.hpp>
#include "common/httplib.h"

#include <discord_rpc.h>
#include <fmt/format.h>

#include "common/common_types.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/loader/loader.h"

#include "discord_impl.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace DiscordRPC {

namespace {

// Community-maintained names and covers, keyed by title ID: <TITLEID>.txt and <TITLEID>.png
constexpr char COVERS_HOST[] = "raw.githubusercontent.com";
constexpr char COVERS_PATH[] = "/mushymeow/eglantine-covers/main/";

std::optional<std::string> FetchFromCovers(const std::string& file_name, bool want_body) {
    httplib::SSLClient client(COVERS_HOST);
    client.set_connection_timeout(3);
    client.set_read_timeout(3);
    client.set_follow_location(true);

#ifdef YUZU_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif

    const std::string path = std::string{COVERS_PATH} + file_name;
    const auto res = want_body ? client.Get(path) : client.Head(path);
    if (!res || res->status != 200) {
        return std::nullopt;
    }
    return res->body;
}

} // namespace

DiscordImpl::DiscordImpl(Core::System& system_) : system{system_} {
    DiscordEventHandlers handlers{};
    // The number is the client ID for Eglantine, it's used for images and the
    // application name
    Discord_Initialize("1566479696022081606", &handlers, 1, nullptr);
}

DiscordImpl::~DiscordImpl() {
    Discord_ClearPresence();
    Discord_Shutdown();
}

void DiscordImpl::Pause() {
    Discord_ClearPresence();
}

std::string DiscordImpl::GetGameString(const std::string& title) {
    // Convert to lowercase
    std::string icon_name = Common::ToLower(title);

    // Replace spaces with dashes
    std::replace(icon_name.begin(), icon_name.end(), ' ', '-');
    boost::replace_all(icon_name, "é", "e");

    // Remove non-alphanumeric characters but keep dashes
    std::erase_if(icon_name, [](char c) { return !std::isalnum(c) && c != '-'; });

    // Remove dashes from the start and end of the string
    icon_name.erase(icon_name.begin(), std::find_if(icon_name.begin(), icon_name.end(),
                                                    [](int ch) { return ch != '-'; }));
    icon_name.erase(
        std::find_if(icon_name.rbegin(), icon_name.rend(), [](int ch) { return ch != '-'; }).base(),
        icon_name.end());

    // Remove double dashes
    icon_name.erase(std::unique(icon_name.begin(), icon_name.end(),
                                [](char a, char b) { return a == '-' && b == '-'; }),
                    icon_name.end());

    return icon_name;
}

static constexpr char DEFAULT_DISCORD_TEXT[] = "Eglantine is an emulator for the Nintendo Switch";
static constexpr char DEFAULT_DISCORD_IMAGE[] =
    "https://raw.githubusercontent.com/Djihads80/Eglantine/refs/heads/nx2/dist/Eglantine.png";

void DiscordImpl::UpdateGameStatus(std::string_view game_url, bool has_boxart) {
    const std::string url = std::string{has_boxart ? game_url : DEFAULT_DISCORD_IMAGE};
    s64 start_time = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    DiscordRichPresence presence{};

    presence.largeImageKey = url.c_str();
    presence.largeImageText = game_title.c_str();
    presence.smallImageKey = DEFAULT_DISCORD_IMAGE;
    presence.smallImageText = DEFAULT_DISCORD_TEXT;
    presence.state = game_title.c_str();
    presence.status_display_type = DiscordStatusDisplayType_State;
    const std::string details =
        game_version.empty() ? "Currently in game" : fmt::format("Version {}", game_version);
    presence.details = details.c_str();
    presence.startTimestamp = start_time;
    Discord_UpdatePresence(&presence);
}

void DiscordImpl::Update() {
    if (system.IsPoweredOn()) {
        game_title.clear();
        system.GetAppLoader().ReadTitle(game_title);

        // Read the version from the patched metadata so installed updates are reflected
        const FileSys::PatchManager pm(system.GetApplicationProcessProgramID(),
                                       system.GetFileSystemController(),
                                       system.GetContentProvider());
        const auto metadata = pm.GetControlMetadata();
        game_version = metadata.first != nullptr ? metadata.first->GetVersionString() : "";
        if (game_version.empty()) {
            // Loose dumps without control data can still report a version from their RomFS
            system.GetAppLoader().ReadDisplayVersion(game_version);
        }

        const auto title_id = fmt::format("{:016X}", system.GetApplicationProcessProgramID());
        if (const auto name = FetchFromCovers(title_id + ".txt", true)) {
            if (auto stripped = Common::StripSpaces(*name); !stripped.empty()) {
                game_title = std::move(stripped);
            }
        }
        if (FetchFromCovers(title_id + ".png", false)) {
            UpdateGameStatus(fmt::format("https://{}{}{}.png", COVERS_HOST, COVERS_PATH, title_id),
                             true);
            return;
        }

        // Used to format Icon URL for yuzu website game compatibility page
        std::string icon_name = GetGameString(game_title);
        auto const game_url = fmt::format(
            "https://raw.githubusercontent.com/eden-emulator/boxart/refs/heads/master/img/{}.png",
            icon_name);

        httplib::SSLClient client(game_url);
        client.set_connection_timeout(3);
        client.set_read_timeout(3);
        client.set_follow_location(true);

#ifdef YUZU_BUNDLED_OPENSSL
        client.load_ca_cert_store(kCert, sizeof(kCert));
#endif

        httplib::Request request{
            .method = "HEAD",
            .path = game_url,
        };

        auto res = client.send(request);
        UpdateGameStatus(game_url, res && res->status == 200);

        return;
    }

    s64 start_time = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    DiscordRichPresence presence{};
    presence.largeImageKey = DEFAULT_DISCORD_IMAGE;
    presence.largeImageText = DEFAULT_DISCORD_TEXT;
    presence.status_display_type = DiscordStatusDisplayType_Name;
    presence.details = "Currently not in game";
    presence.startTimestamp = start_time;
    Discord_UpdatePresence(&presence);
}
} // namespace DiscordRPC

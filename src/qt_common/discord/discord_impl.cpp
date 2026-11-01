// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <string>

#include <discord_rpc.h>
#include <fmt/format.h>

#include "common/common_types.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/loader/loader.h"
#include "qt_common/config/uisettings.h"
#include "discord_impl.h"

namespace DiscordRPC {

namespace {
constexpr char CLIENT_ID[] = "1566239570570514524";
constexpr char LARGE_IMAGE[] = "logo";
constexpr char LARGE_IMAGE_TEXT[] = "Eglantine";

s64 NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

DiscordImpl::DiscordImpl(Core::System& system_) : system{system_} {
    DiscordEventHandlers handlers{};
    // The number is the client ID for Eglantine, it's used for images and the
    // application name
    Discord_Initialize(CLIENT_ID, &handlers, 1, nullptr);
}

DiscordImpl::~DiscordImpl() {
    Discord_ClearPresence();
    Discord_Shutdown();
}

void DiscordImpl::Pause() {
    Discord_ClearPresence();
}

void DiscordImpl::Update() {
    const auto mode = UISettings::values.discord_presence.GetValue();
    if (mode == static_cast<u32>(UISettings::DiscordPresence::Off)) {
        game_start_time = 0;
        Discord_ClearPresence();
        return;
    }

    DiscordRichPresence presence{};
    presence.largeImageKey = LARGE_IMAGE;
    presence.largeImageText = LARGE_IMAGE_TEXT;

    if (system.IsPoweredOn()) {
        game_title.clear();
        system.GetAppLoader().ReadTitle(game_title);
        if (game_title.empty()) {
            // Fall back to the same source the Qt window title uses: the control
            // metadata (NACP) application name of the running application.
            if (const auto program_id = system.GetApplicationProcessProgramID();
                program_id != 0) {
                const FileSys::PatchManager pm{program_id, system.GetFileSystemController(),
                                               system.GetContentProvider()};
                if (const auto metadata = pm.GetControlMetadata(); metadata.first != nullptr) {
                    game_title = metadata.first->GetApplicationName();
                }
            }
        }
        if (game_start_time == 0) {
            game_start_time = NowSeconds();
        }
        // "Playing: <title>"; if the loader gave no title, just "Playing".
        if (game_title.empty()) {
            presence.details = "Playing";
        } else {
            // Discord caps the details field at 128 bytes.
            if (game_title.size() > 112) {
                game_title.resize(112);
            }
            details_buffer = fmt::format("Playing: {}", game_title);
            presence.details = details_buffer.c_str();
        }
        presence.startTimestamp = game_start_time;
        Discord_UpdatePresence(&presence);
        return;
    }

    game_start_time = 0;
    if (mode == static_cast<u32>(UISettings::DiscordPresence::On)) {
        // Not emulating but the emulator is open: show Idle.
        presence.details = "Idle";
        Discord_UpdatePresence(&presence);
    } else {
        // Only In-Game: nothing shown outside of a running game.
        Discord_ClearPresence();
    }
}

} // namespace DiscordRPC

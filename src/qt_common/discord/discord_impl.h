// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "discord.h"

#include <string>

namespace Core {
class System;
}

namespace DiscordRPC {

class DiscordImpl : public DiscordInterface {
public:
    DiscordImpl(Core::System& system_);
    ~DiscordImpl() override;

    void Pause() override;
    void Update() override;

private:
    std::string game_title{};
    std::string details_buffer{};
    s64 game_start_time{};
    Core::System& system;
};

} // namespace DiscordRPC

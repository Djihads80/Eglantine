// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include "common/container/unordered_map.h"

#include "video_core/dma_pusher.h"

namespace Tegra {

class GPU;

namespace Control {

struct ChannelState;

class Scheduler {
public:
    void Push(GPU& gpu, s32 channel, CommandList&& entries);

    /// Runs channels whose pending semaphore acquire is now met.
    void ResumePending(GPU& gpu);
    [[nodiscard]] bool HasPendingChannels() const {
        return !pending_channels.empty();
    }

    void DeclareChannel(std::shared_ptr<ChannelState> new_channel);

private:
    void BindChannel(GPU& gpu, s32 bind_id);

    ::Common::unordered_map<s32, std::shared_ptr<ChannelState>> channels;
    std::mutex scheduling_guard;
    std::vector<std::shared_ptr<ChannelState>> pending_channels; ///< Suspended on an acquire
};

} // namespace Control

} // namespace Tegra

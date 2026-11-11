// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Vulkan {

class FragmentBarrierState {
public:
    void Defer(bool render_pass_active) {
        pending = render_pass_active;
    }

    bool Pending() const {
        return pending;
    }

    void Reset() {
        pending = false;
    }

private:
    bool pending{};
};

} // namespace Vulkan

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace Core {
class System;
}

namespace Kernel {
class KernelCore;
class KEvent;
class KProcess;
class KReadableEvent;
} // namespace Kernel

namespace Service::KernelHelpers {

class ServiceContext {
public:
    ServiceContext(Core::System& system_, std::string name_);
    ~ServiceContext();

    Kernel::KEvent* CreateEvent(std::string&& name);

    void CloseEvent(Kernel::KEvent* event);

    /// Name an event was created with (debugger handle listing); empty when unknown.
    static std::string GetEventName(const Kernel::KEvent* event);

    Kernel::KernelCore& kernel;
    Kernel::KProcess* process{};
    bool process_created{false};
};

/// The name a service gave an event (EDEN_NX2_SVC_TRACE shows the handles a thread waits on).
std::string DebugEventName(const Kernel::KReadableEvent* event);

} // namespace Service::KernelHelpers

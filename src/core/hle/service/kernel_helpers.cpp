// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <unordered_map>

#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/service/kernel_helpers.h"

namespace Service::KernelHelpers {

namespace {
// Event names for the debugger's handle listing.
std::mutex event_names_mutex;
std::unordered_map<const Kernel::KEvent*, std::string> event_names;
} // namespace

ServiceContext::ServiceContext(Core::System& system_, std::string name_)
    : kernel(system_.Kernel())
{
    if (process = Kernel::GetCurrentProcessPointer(kernel); process != nullptr) {
        return;
    }

    // Create the process.
    process = Kernel::KProcess::Create(kernel);
    ASSERT(R_SUCCEEDED(process->Initialize(kernel, Kernel::Svc::CreateProcessParameter{}, kernel.GetSystemResourceLimit(), false)));

    // Register the process.
    Kernel::KProcess::Register(kernel, process);
    process_created = true;
}

ServiceContext::~ServiceContext() {
    if (process_created) {
        process->Close(kernel);
        process = nullptr;
    }
}

namespace {
std::mutex debug_names_mutex;
std::unordered_map<const Kernel::KReadableEvent*, std::string> debug_names;
} // Anonymous namespace

std::string DebugEventName(const Kernel::KReadableEvent* event) {
    std::scoped_lock lk{debug_names_mutex};
    const auto it = debug_names.find(event);
    return it != debug_names.end() ? it->second : std::string{"?"};
}

Kernel::KEvent* ServiceContext::CreateEvent(std::string&& name) {
    // Reserve a new event from the process resource limit
    Kernel::KScopedResourceReservation event_reservation(kernel, process,
                                                         Kernel::LimitableResource::EventCountMax);
    if (!event_reservation.Succeeded()) {
        LOG_CRITICAL(Service, "Resource limit reached!");
        return {};
    }

    // Create a new event.
    auto* event = Kernel::KEvent::Create(kernel);
    if (!event) {
        LOG_CRITICAL(Service, "Unable to create event!");
        return {};
    }

    // Initialize the event.
    event->Initialize(kernel, process);

    // Commit the thread reservation.
    event_reservation.Commit();

    // Register the event.
    Kernel::KEvent::Register(kernel, event);

    {
        std::scoped_lock lk{debug_names_mutex};
        debug_names[&event->GetReadableEvent()] = name;
    }
    {
        std::scoped_lock lk{event_names_mutex};
        event_names[event] = std::move(name);
    }
    return event;
}

void ServiceContext::CloseEvent(Kernel::KEvent* event) {
    if (event) {
        {
            std::scoped_lock lk{event_names_mutex};
            event_names.erase(event);
        }
        {
            std::scoped_lock lk{debug_names_mutex};
            debug_names.erase(&event->GetReadableEvent());
        }
        event->GetReadableEvent().Close(kernel);
        event->Close(kernel);
    }
}

std::string ServiceContext::GetEventName(const Kernel::KEvent* event) {
    std::scoped_lock lk{event_names_mutex};
    const auto it = event_names.find(event);
    return it == event_names.end() ? std::string{} : it->second;
}

} // namespace Service::KernelHelpers

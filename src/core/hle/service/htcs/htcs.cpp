// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "core/hle/service/htcs/htcs.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"

namespace Service::HTCS {

class IHtcsManager final : public ServiceFramework<IHtcsManager> {
public:
    explicit IHtcsManager(Core::System& system_) : ServiceFramework{system_, "htcs"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "Socket"},
            {1, nullptr, "Close"},
            {2, nullptr, "Connect"},
            {3, nullptr, "Bind"},
            {4, nullptr, "Listen"},
            {5, nullptr, "Accept"},
            {6, nullptr, "Recv"},
            {7, nullptr, "Send"},
            {8, nullptr, "Shutdown"},
            {9, nullptr, "Fcntl"},
            {10, &IHtcsManager::GetPeerName, "GetPeerNameAny"},
            {11, nullptr, "GetDefaultHostName"},
            {12, nullptr, "CreateSocketOld"},
            {13, &IHtcsManager::CreateSocket, "CreateSocket"},
            {100, &IHtcsManager::RegisterProcessId, "RegisterProcessId"},
            {101, &IHtcsManager::MonitorManager, "MonitorManager"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    void GetPeerName(HLERequestContext& ctx) {
        LOG_WARNING(Service_HTCS, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CreateSocket(HLERequestContext& ctx) {
        // debug because this gets spammed a LOT
        LOG_DEBUG(Service_HTCS, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void RegisterProcessId(HLERequestContext& ctx) {
        LOG_WARNING(Service_HTCS, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void MonitorManager(HLERequestContext& ctx) {
        LOG_WARNING(Service_HTCS, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("htcs", std::make_shared<IHtcsManager>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::HTCS

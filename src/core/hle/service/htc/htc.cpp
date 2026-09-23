// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "core/hle/service/htc/htc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"

namespace Service::HTC {

class IHtcManager final : public ServiceFramework<IHtcManager> {
public:
    explicit IHtcManager(Core::System& system_) : ServiceFramework{system_, "htc"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IHtcManager::GetEnvironmentVariable, "GetEnvironmentVariable"},
            {1, &IHtcManager::GetEnvironmentVariableLength, "GetEnvironmentVariableLength"},
            {2, nullptr, "BindHostConnectionEvent"},
            {3, nullptr, "BindHostDisconnectionEvent"},
            {4, nullptr, "BindHostConnectionEventForSystem"},
            {5, nullptr, "BindHostDisconnectionEventForSystem"},
            {6, nullptr, "GetBridgeIpAddress"}, // 3.0.0+
            {7, nullptr, "GetBridgePort"}, // 3.0.0+
            {8, nullptr, "SetCradleAttached"}, // 3.0.0+
            {9, nullptr, "GetBridgeSubnetMask"}, // 4.0.0+
            {10, nullptr, "GetBridgeMacAddress"}, // 4.0.0+
            {11, nullptr, "GetWorkingDirectoryPath"}, // 6.0.0+
            {12, nullptr, "GetWorkingDirectoryPathSize"}, // 6.0.0+
            {13, nullptr, "RunOnHostStart"}, // 6.0.0+
            {14, nullptr, "RunOnHostResults"}, // 9.0.0+
            {15, nullptr, "SetBridgeIpAddress"}, // 20.0.0+
            {16, nullptr, "SetBridgeSubnetMask"}, // 20.0.0+
            {17, nullptr, "SetBridgePort"}, // 20.0.0+
            {18, nullptr, "GetBridgeSerialNumber"}, // 20.0.0+
            {19, nullptr, "GetBridgeFwVersion"}, // 20.0.0+
            {20, nullptr, "ResetBridgeSettings"}, // 20.0.0+
            {21, nullptr, "BeginUpdateBridge"}, // 19.0.0+
            {22, nullptr, "ContinueUpdateBridge"}, // 19.0.0+
            {23, nullptr, "EndUpdateBridge"}, // 19.0.0+
            {24, nullptr, "GetBridgeType"}, // 19.0.0+
            {25, nullptr, "GetBridgeDefaultGateway"}, // 20.0.0+
            {26, nullptr, "SetBridgeDefaultGateway"}, // 20.0.0+
            {27, nullptr, "GetBridgeDhcp"}, // 20.0.0+
            {28, nullptr, "SetBridgeDhcp"}, // 20.0.0+
            {29, nullptr, "GetBridgeProgramVersion"}, // 21.0.0+
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    void GetEnvironmentVariable(HLERequestContext& ctx) {
        LOG_WARNING(Service_HTC, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(0);
    }

    void GetEnvironmentVariableLength(HLERequestContext& ctx) {
        LOG_WARNING(Service_HTC, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(0);
    }
};

class IServiceManager : public ServiceFramework<IServiceManager> {
public:
    explicit IServiceManager(Core::System& system_) : ServiceFramework{system_, "htc:tenv"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IServiceManager::GetServiceInterface, "GetServiceInterface"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    void GetServiceInterface(HLERequestContext& ctx) {
        LOG_WARNING(Service_HTC, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("htc:tenv", std::make_shared<IServiceManager>(system));
    server_manager->RegisterNamedService("htc", std::make_shared<IHtcManager>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::HTC

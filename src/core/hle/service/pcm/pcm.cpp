// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/pcm/pcm.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"

namespace Service::PCM {

class IManager final : public ServiceFramework<IManager> {
public:
    explicit IManager(Core::System& system_) : ServiceFramework{system_, "pcm"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IManager::IsSupported, "IsSupported"},
            {1, &IManager::ReadCurrentPower, "ReadCurrentPower"},
            {2, &IManager::IsServiceEnabled, "IsServiceEnabled"}, // 4.0.0+
            {3, nullptr, "ReadCurrentVoltage"}, // 4.0.0+
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    void IsSupported(HLERequestContext& ctx) {
        LOG_DEBUG(Service_PCM, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(0);
    }

    void ReadCurrentPower(HLERequestContext& ctx) {
        LOG_DEBUG(Service_PCM, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(0);
    }

    void IsServiceEnabled(HLERequestContext& ctx) {
        LOG_DEBUG(Service_PCM, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(0);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("pcm", std::make_shared<IManager>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::PCM

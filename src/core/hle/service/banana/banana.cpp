// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "core/hle/service/banana/banana.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"

namespace Service::Banana {

class IProfiler final : public ServiceFramework<IProfiler> {
public:
    explicit IProfiler(Core::System& system_) : ServiceFramework{system_, "banana"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetSystemEvent"},
            {1, &IProfiler::StartSignalingEvent, "StartSignalingEvent"},
            {2, nullptr, "StopSignalingEvent"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    void StartSignalingEvent(HLERequestContext& ctx) {
        LOG_WARNING(Service_Banana, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("banana", std::make_shared<IProfiler>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::Banana

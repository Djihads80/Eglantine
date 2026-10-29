// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/audio/audio_in_manager.h"
#include "core/hle/service/audio/audio_out_manager.h"
#include "core/hle/service/audio/audio_renderer_manager.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/service.h"

namespace Service::Audio {

// Switch 2 "aud:u" (nn::audio::CreateUserClientSessionByHipc). The SDK reaches the Switch 1
// audio interfaces through it: command 2 returns an object used exactly like
// IAudioRendererManager (vtable slot 1 = GetWorkBufferSize with a Switch 1 parameter block), so
// it is backed by Eden's audren:u implementation. The nx2: name keeps unknown commands
// auto-stubbed and logged.
class INx2AudioUserClient final : public ServiceFramework<INx2AudioUserClient> {
public:
    explicit INx2AudioUserClient(Core::System& system_)
        : ServiceFramework{system_, "nx2:aud:u"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {1, &INx2AudioUserClient::GetAudioOutManager, "GetAudioOutManager (guessed)"},
            {2, &INx2AudioUserClient::GetAudioRendererManager, "GetAudioRendererManager (guessed)"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    // nn::audio::OpenAudioOut keeps the command 1 object as a singleton and calls its method 3
    // (OpenAudioOutAuto: 48 kHz stereo parameters, device name in/out buffers).
    void GetAudioOutManager(HLERequestContext& ctx) {
        LOG_WARNING(Service_Audio, "(STUBBED) Switch 2 aud:u 1, returning IAudioOutManager");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IAudioOutManager>(ctx, system);
    }

    void GetAudioRendererManager(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 arg = rp.Pop<u32>();
        LOG_WARNING(Service_Audio, "(STUBBED) Switch 2 aud:u 2 arg={}, returning IAudioRendererManager",
                    arg);
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IAudioRendererManager>(ctx, system);
    }
};

} // namespace Service::Audio

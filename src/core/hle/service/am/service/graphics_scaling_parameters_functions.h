// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Service::AM {

// nn::am::service::IGraphicsScalingParametersFunctions (Switch 2), returned by IApplicationProxy
// command 60. nn::oe's GetCurrentGraphicsScale reads command 10 and accepts 10, 20 or 30 (scale
// enum 0, 1, 2). Commands without a handler are auto-stubbed because the name carries the nx2:
// prefix.
class IGraphicsScalingParametersFunctions final
    : public ServiceFramework<IGraphicsScalingParametersFunctions> {
public:
    explicit IGraphicsScalingParametersFunctions(Core::System& system_)
        : ServiceFramework{system_, "nx2:IGraphicsScalingParametersFunctions"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {10, D<&IGraphicsScalingParametersFunctions::GetGraphicsScalingParameters>, "GetGraphicsScalingParameters"},
            {1050, D<&IGraphicsScalingParametersFunctions::SetGraphicsScaleChangedNotificationEnabled>, "SetGraphicsScaleChangedNotificationEnabled"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    // The reply is 12 bytes; only the first word (the scale) is known. The other two used to be
    // left to the message buffer's leftovers.
    Result GetGraphicsScalingParameters(Out<u32> out_scale, Out<u32> out_unknown1,
                                        Out<u32> out_unknown2) {
        LOG_WARNING(Service_AM, "(STUBBED) called, returning 10");
        *out_scale = 10;
        *out_unknown1 = 0;
        *out_unknown2 = 0;
        R_SUCCEED();
    }

    // 1050(bool): every title calls it at boot. The scale never changes here, so there is never a
    // change message to send either way.
    Result SetGraphicsScaleChangedNotificationEnabled(bool enabled) {
        LOG_DEBUG(Service_AM, "called, enabled={}", enabled);
        R_SUCCEED();
    }
};

} // namespace Service::AM

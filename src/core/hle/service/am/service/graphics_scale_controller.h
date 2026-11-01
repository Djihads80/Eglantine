// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/nx2_stub_interface.h"
#include "core/hle/service/service.h"

namespace Service::AM {

// Switch 2 interface returned by IApplicationProxy command 60 (name is a guess). nn::oe's
// GetCurrentGraphicsScale reads command 10 and accepts 10, 20 or 30 (scale enum 0, 1, 2).
// Commands without a handler are auto-stubbed because the name carries the nx2: prefix.
class IGraphicsScaleController final : public ServiceFramework<IGraphicsScaleController> {
public:
    explicit IGraphicsScaleController(Core::System& system_)
        : ServiceFramework{system_, "nx2:IGraphicsScaleController"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {10, D<&IGraphicsScaleController::GetCurrentGraphicsScale>, "GetCurrentGraphicsScale"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    // The reply is 12 bytes; only the first word (the scale) is known. The other two used to be
    // left to the message buffer's leftovers.
    Result GetCurrentGraphicsScale(Out<u32> out_scale, Out<u32> out_unknown1, Out<u32> out_unknown2) {
        LOG_WARNING(Service_AM, "(STUBBED) called, returning 10");
        *out_scale = 10;
        *out_unknown1 = 0;
        *out_unknown2 = 0;
        R_SUCCEED();
    }
};

} // namespace Service::AM

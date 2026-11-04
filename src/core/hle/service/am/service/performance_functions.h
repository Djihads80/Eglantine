// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/settings.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/nx2_stub_interface.h"
#include "core/hle/service/service.h"

namespace Service::AM {

// nn::am::service::IPerformanceFunctions (Switch 2), returned by IApplicationProxy command 40. The
// title's own request is kept and read back, and Eden's CPU clock setting (Boost or Overclock)
// also reads as enabled. The request doesn't change that setting: it scales the guest clock, which
// would jump under a running title. Commands without a handler are auto-stubbed because the name
// carries the nx2: prefix.
class IPerformanceFunctions final : public ServiceFramework<IPerformanceFunctions> {
public:
    explicit IPerformanceFunctions(Core::System& system_)
        : ServiceFramework{system_, "nx2:IPerformanceFunctions"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&IPerformanceFunctions::IsCpuOverclockEnabled>, "IsCpuOverclockEnabled"},
            {1, D<&IPerformanceFunctions::SetCpuOverclockEnabled>, "SetCpuOverclockEnabled"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    Result IsCpuOverclockEnabled(Out<bool> out_is_enabled) {
        *out_is_enabled = m_cpu_overclock_requested ||
                          Settings::values.cpu_clock.GetValue() != Settings::CpuClock::Normal;
        LOG_DEBUG(Service_AM, "called, enabled={}", *out_is_enabled);
        R_SUCCEED();
    }

    Result SetCpuOverclockEnabled(bool enabled) {
        LOG_DEBUG(Service_AM, "called, enabled={}", enabled);
        m_cpu_overclock_requested = enabled;
        R_SUCCEED();
    }

    bool m_cpu_overclock_requested{};
};

} // namespace Service::AM

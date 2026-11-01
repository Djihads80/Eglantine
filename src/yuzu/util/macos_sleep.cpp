// SPDX-License-Identifier: GPL-3.0-or-later

#include <IOKit/pwr_mgt/IOPMLib.h>
#include "common/logging.h"
#include "yuzu/util/macos_sleep.h"

namespace MacOS {

u32 PreventSleep() {
    IOPMAssertionID assertion_id = kIOPMNullAssertionID;
    const auto result =
        IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                    kIOPMAssertionLevelOn, CFSTR("Running a game"), &assertion_id);
    if (result != kIOReturnSuccess) {
        LOG_WARNING(Frontend, "Failed to prevent display sleep: {}", result);
        return kIOPMNullAssertionID;
    }
    return assertion_id;
}

void AllowSleep(u32 assertion_id) {
    IOPMAssertionRelease(assertion_id);
}

} // namespace MacOS

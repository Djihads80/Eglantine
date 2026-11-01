// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace MacOS {

u32 PreventSleep();
void AllowSleep(u32 assertion_id);

} // namespace MacOS

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/block_range_information.h"

#include <algorithm>

#include <boost/icl/interval_set.hpp>
#include "common/common_types.h"
#include "common/container/unordered_set.h"

namespace Dynarmic::Backend {

template<typename P>
void BlockRangeInformation<P>::AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location) {
    if (!boost::icl::is_empty(range)) {
        ranges.push_back({boost::icl::first(range), boost::icl::last(range), location});
    }
}

template<typename P>
void BlockRangeInformation<P>::ClearCache() {
    ranges.clear();
}

template<typename P>
::Common::unordered_set<IR::LocationDescriptor> BlockRangeInformation<P>::InvalidateRanges(const boost::icl::interval_set<P>& invalidated) {
    ::Common::unordered_set<IR::LocationDescriptor> erase_locations;
    std::erase_if(ranges, [&](const Range& range) {
        for (const auto& interval : invalidated) {
            if (range.first <= boost::icl::last(interval) && boost::icl::first(interval) <= range.last) {
                erase_locations.insert(range.location);
                return true;
            }
        }
        return false;
    });
    return erase_locations;
}

template class BlockRangeInformation<u32>;
template class BlockRangeInformation<u64>;

}  // namespace Dynarmic::Backend

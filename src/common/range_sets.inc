// SPDX-FileCopyrightText: 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include <boost/icl/interval.hpp>
#include <boost/icl/interval_base_set.hpp>
#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <boost/icl/split_interval_map.hpp>
#include <boost/pool/pool.hpp>
#include <boost/pool/pool_alloc.hpp>
#include <boost/pool/poolfwd.hpp>

#include "common/range_sets.h"

namespace Common {

namespace {
template <class T>
using RangeSetsAllocator =
    boost::fast_pool_allocator<T, boost::default_user_allocator_new_delete,
                               boost::details::pool::default_mutex, 1024, 2048>;
}

// Sorted, disjoint, non-touching ranges in one array. The buffer cache looks these up tens of
// thousands of times a frame and most lookups miss; sets stay small (Ocarina of Time: ~200 ranges
// at a subtract, 763 at most), so moving a few hundred bytes on an insert costs less than tree
// nodes from a locked pool.
template <typename AddressType>
struct RangeSet<AddressType>::RangeSetImpl {
    struct Range {
        AddressType lower;
        AddressType upper;
    };
    using Iterator = typename std::vector<Range>::iterator;
    using ConstIterator = typename std::vector<Range>::const_iterator;

    /// The first range ending after `address`, the first that could overlap what starts there.
    ConstIterator FirstEndingAfter(AddressType address) const {
        return std::upper_bound(
            m_ranges.begin(), m_ranges.end(), address,
            [](AddressType value, const Range& range) { return value < range.upper; });
    }

    void Add(AddressType base_address, size_t size) {
        const AddressType end_address = base_address + static_cast<AddressType>(size);
        if (end_address <= base_address) {
            return;
        }
        // Ranges that touch the new one join it: the first ending at or after its start...
        auto first = std::lower_bound(
            m_ranges.begin(), m_ranges.end(), base_address,
            [](const Range& range, AddressType value) { return range.upper < value; });
        if (first == m_ranges.end()) {
            m_ranges.push_back(Range{base_address, end_address});
            return;
        }
        // ...up to the first starting after its end.
        const auto last = std::upper_bound(
            first, m_ranges.end(), end_address,
            [](AddressType value, const Range& range) { return value < range.lower; });
        if (first == last) {
            m_ranges.insert(first, Range{base_address, end_address});
            return;
        }
        first->lower = (std::min)(first->lower, base_address);
        first->upper = (std::max)((last - 1)->upper, end_address);
        m_ranges.erase(first + 1, last);
    }

    void Subtract(AddressType base_address, size_t size) {
        const AddressType end_address = base_address + static_cast<AddressType>(size);
        if (end_address <= base_address) {
            return;
        }
        const auto first = m_ranges.begin() + (FirstEndingAfter(base_address) - m_ranges.cbegin());
        const auto last = std::lower_bound(
            first, m_ranges.end(), end_address,
            [](const Range& range, AddressType value) { return range.lower < value; });
        if (first == last) {
            return;
        }
        const AddressType left_lower = first->lower;
        const AddressType right_upper = (last - 1)->upper;
        const bool keep_left = left_lower < base_address;
        const bool keep_right = end_address < right_upper;
        if (keep_left && keep_right && last - first == 1) {
            // Cut out of the middle of one range: it becomes two.
            first->upper = base_address;
            m_ranges.insert(last, Range{end_address, right_upper});
            return;
        }
        Iterator erase_from = first;
        Iterator erase_to = last;
        if (keep_left) {
            first->upper = base_address;
            ++erase_from;
        }
        if (keep_right) {
            --erase_to;
            erase_to->lower = end_address;
        }
        m_ranges.erase(erase_from, erase_to);
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        for (const Range& range : m_ranges) {
            func(range.lower, range.upper);
        }
    }

    template <typename Func>
    void ForEachInRange(AddressType base_addr, size_t size, Func&& func) const {
        const AddressType start_address = base_addr;
        const AddressType end_address = start_address + static_cast<AddressType>(size);
        if (end_address <= start_address) {
            return;
        }
        for (auto it = FirstEndingAfter(start_address);
             it != m_ranges.end() && it->lower < end_address; ++it) {
            func((std::max)(it->lower, start_address), (std::min)(it->upper, end_address));
        }
    }

    std::vector<Range> m_ranges;
};

template <typename AddressType>
struct OverlapRangeSet<AddressType>::OverlapRangeSetImpl {
    using IntervalSet = boost::icl::split_interval_map<
        AddressType, s32, boost::icl::partial_enricher, std::less, boost::icl::inplace_plus,
        boost::icl::inter_section,
        ICL_INTERVAL_INSTANCE(ICL_INTERVAL_DEFAULT, AddressType, std::less), RangeSetsAllocator>;
    using IntervalType = typename IntervalSet::interval_type;

    OverlapRangeSetImpl() = default;
    ~OverlapRangeSetImpl() = default;

    void Add(AddressType base_address, size_t size) {
        AddressType end_address = base_address + static_cast<AddressType>(size);
        IntervalType interval{base_address, end_address};
        m_split_ranges_set += std::make_pair(interval, 1);
    }

    template <bool has_on_delete, typename Func>
    void Subtract(AddressType base_address, size_t size, s32 amount,
                  [[maybe_unused]] Func&& on_delete) {
        if (m_split_ranges_set.empty()) {
            return;
        }
        AddressType end_address = base_address + static_cast<AddressType>(size);
        IntervalType interval{base_address, end_address};
        m_split_ranges_set += std::make_pair(interval, -amount);
        // One pass, erasing the segments that dropped to zero or below in address order (the
        // search restarted after every erase: quadratic in the segments of the range, and the
        // GPU fencing thread does this for each downloaded buffer range while holding the buffer
        // cache's lock). Erasing a node leaves the other iterators valid.
        auto it = m_split_ranges_set.lower_bound(interval);
        const auto end_it = m_split_ranges_set.upper_bound(interval);
        while (it != end_it) {
            if (it->second > 0) {
                ++it;
                continue;
            }
            if constexpr (has_on_delete) {
                if (it->second == 0) {
                    on_delete(it->first.lower(), it->first.upper());
                }
            }
            m_split_ranges_set.erase(it++);
        }
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        if (m_split_ranges_set.empty()) {
            return;
        }
        auto it = m_split_ranges_set.begin();
        auto end_it = m_split_ranges_set.end();
        for (; it != end_it; it++) {
            const AddressType inter_addr_end = it->first.upper();
            const AddressType inter_addr = it->first.lower();
            func(inter_addr, inter_addr_end, it->second);
        }
    }

    template <typename Func>
    void ForEachInRange(AddressType base_address, size_t size, Func&& func) const {
        if (m_split_ranges_set.empty()) {
            return;
        }
        const AddressType start_address = base_address;
        const AddressType end_address = start_address + size;
        const OverlapRangeSetImpl::IntervalType search_interval{start_address, end_address};
        auto it = m_split_ranges_set.lower_bound(search_interval);
        if (it == m_split_ranges_set.end()) {
            return;
        }
        auto end_it = m_split_ranges_set.upper_bound(search_interval);
        for (; it != end_it; it++) {
            auto& inter = it->first;
            AddressType inter_addr_end = inter.upper();
            AddressType inter_addr = inter.lower();
            if (inter_addr_end > end_address) {
                inter_addr_end = end_address;
            }
            if (inter_addr < start_address) {
                inter_addr = start_address;
            }
            func(inter_addr, inter_addr_end, it->second);
        }
    }

    IntervalSet m_split_ranges_set;
};

template <typename AddressType>
RangeSet<AddressType>::RangeSet() {
    m_impl = std::make_unique<RangeSet<AddressType>::RangeSetImpl>();
}

template <typename AddressType>
RangeSet<AddressType>::~RangeSet() = default;

template <typename AddressType>
RangeSet<AddressType>::RangeSet(RangeSet&& other) {
    m_impl = std::make_unique<RangeSet<AddressType>::RangeSetImpl>();
    m_impl->m_ranges = std::move(other.m_impl->m_ranges);
    // A moved-from container is only "valid but unspecified". Callers hand ranges over by
    // moving and then keep adding to the source (BufferCache::AccumulateFlushes).
    other.m_impl->m_ranges.clear();
}

template <typename AddressType>
RangeSet<AddressType>& RangeSet<AddressType>::operator=(RangeSet&& other) {
    if (this != &other) {
        m_impl->m_ranges = std::move(other.m_impl->m_ranges);
        other.m_impl->m_ranges.clear();
    }
    return *this;
}

template <typename AddressType>
void RangeSet<AddressType>::Add(AddressType base_address, size_t size) {
    m_impl->Add(base_address, size);
}

template <typename AddressType>
void RangeSet<AddressType>::Subtract(AddressType base_address, size_t size) {
    m_impl->Subtract(base_address, size);
}

template <typename AddressType>
void RangeSet<AddressType>::Clear() {
    m_impl->m_ranges.clear();
}

template <typename AddressType>
bool RangeSet<AddressType>::Empty() const {
    return m_impl->m_ranges.empty();
}

template <typename AddressType>
bool RangeSet<AddressType>::Intersects(AddressType base_address, size_t size) const {
    const auto& ranges = m_impl->m_ranges;
    if (ranges.empty() || size == 0) {
        return false;
    }
    const AddressType end_address = base_address + static_cast<AddressType>(size);
    const auto it = m_impl->FirstEndingAfter(base_address);
    return it != ranges.end() && it->lower < end_address;
}

template <typename AddressType>
template <typename Func>
void RangeSet<AddressType>::ForEach(Func&& func) const {
    m_impl->ForEach(std::move(func));
}

template <typename AddressType>
template <typename Func>
void RangeSet<AddressType>::ForEachInRange(AddressType base_address, size_t size,
                                           Func&& func) const {
    m_impl->ForEachInRange(base_address, size, std::move(func));
}

template <typename AddressType>
OverlapRangeSet<AddressType>::OverlapRangeSet() {
    m_impl = std::make_unique<OverlapRangeSet<AddressType>::OverlapRangeSetImpl>();
}

template <typename AddressType>
OverlapRangeSet<AddressType>::~OverlapRangeSet() = default;

template <typename AddressType>
OverlapRangeSet<AddressType>::OverlapRangeSet(OverlapRangeSet&& other) {
    m_impl = std::make_unique<OverlapRangeSet<AddressType>::OverlapRangeSetImpl>();
    m_impl->m_split_ranges_set = std::move(other.m_impl->m_split_ranges_set);
    other.m_impl->m_split_ranges_set.clear();
}

template <typename AddressType>
OverlapRangeSet<AddressType>& OverlapRangeSet<AddressType>::operator=(OverlapRangeSet&& other) {
    if (this != &other) {
        m_impl->m_split_ranges_set = std::move(other.m_impl->m_split_ranges_set);
        other.m_impl->m_split_ranges_set.clear();
    }
    return *this;
}

template <typename AddressType>
void OverlapRangeSet<AddressType>::Add(AddressType base_address, size_t size) {
    m_impl->Add(base_address, size);
}

template <typename AddressType>
void OverlapRangeSet<AddressType>::Subtract(AddressType base_address, size_t size) {
    m_impl->template Subtract<false>(base_address, size, 1, [](AddressType, AddressType) {});
}

template <typename AddressType>
template <typename Func>
void OverlapRangeSet<AddressType>::Subtract(AddressType base_address, size_t size,
                                            Func&& on_delete) {
    m_impl->template Subtract<true, Func>(base_address, size, 1, std::move(on_delete));
}

template <typename AddressType>
void OverlapRangeSet<AddressType>::DeleteAll(AddressType base_address, size_t size) {
    m_impl->template Subtract<false>(base_address, size, (std::numeric_limits<s32>::max)(),
                                     [](AddressType, AddressType) {});
}

template <typename AddressType>
void OverlapRangeSet<AddressType>::Clear() {
    m_impl->m_split_ranges_set.clear();
}

template <typename AddressType>
bool OverlapRangeSet<AddressType>::Empty() const {
    return m_impl->m_split_ranges_set.empty();
}

template <typename AddressType>
template <typename Func>
void OverlapRangeSet<AddressType>::ForEach(Func&& func) const {
    m_impl->ForEach(func);
}

template <typename AddressType>
template <typename Func>
void OverlapRangeSet<AddressType>::ForEachInRange(AddressType base_address, size_t size,
                                                  Func&& func) const {
    m_impl->ForEachInRange(base_address, size, std::move(func));
}

} // namespace Common

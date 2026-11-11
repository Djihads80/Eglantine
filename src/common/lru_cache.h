// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <type_traits>
#include <vector>

#include "common/common_types.h"

namespace Common {

template <class Traits>
class LeastRecentlyUsedCache {
    using ObjectType = typename Traits::ObjectType;
    using TickType = typename Traits::TickType;

    static constexpr size_t NO_ITEM = SIZE_MAX;

    struct Item {
        ObjectType obj;
        TickType tick;
        size_t next{NO_ITEM};
        size_t prev{NO_ITEM};
    };

public:
    LeastRecentlyUsedCache() = default;
    ~LeastRecentlyUsedCache() = default;

    size_t Insert(ObjectType obj, TickType tick) {
        const auto new_id = Build();
        auto& item = item_pool[new_id];
        item.obj = obj;
        item.tick = tick;
        Attach(new_id);
        return new_id;
    }

    void Touch(size_t id, TickType tick) {
        auto& item = item_pool[id];
        if (item.tick >= tick) {
            return;
        }
        item.tick = tick;
        if (id == last_item) {
            return;
        }
        Detach(id);
        Attach(id);
    }

    void Free(size_t id) {
        Detach(id);
        auto& item = item_pool[id];
        item.prev = NO_ITEM;
        item.next = NO_ITEM;
        free_items.push_back(id);
    }

    template <typename Func>
    void ForEachItemBelow(TickType tick, Func&& func) {
        static constexpr bool RETURNS_BOOL =
            std::is_same_v<std::invoke_result_t<Func, ObjectType>, bool>;
        size_t iterator = first_item;
        while (iterator != NO_ITEM) {
            if (static_cast<s64>(tick) - static_cast<s64>(item_pool[iterator].tick) < 0) {
                return;
            }
            const size_t next = item_pool[iterator].next;
            const ObjectType obj = item_pool[iterator].obj;
            if constexpr (RETURNS_BOOL) {
                if (func(obj)) {
                    return;
                }
            } else {
                func(obj);
            }
            iterator = next;
        }
    }

private:
    size_t Build() {
        if (free_items.empty()) {
            const size_t item_id = item_pool.size();
            auto& item = item_pool.emplace_back();
            item.next = NO_ITEM;
            item.prev = NO_ITEM;
            return item_id;
        }
        const size_t item_id = free_items.back();
        free_items.pop_back();
        auto& item = item_pool[item_id];
        item.next = NO_ITEM;
        item.prev = NO_ITEM;
        return item_id;
    }

    void Attach(size_t id) {
        auto& item = item_pool[id];
        if (first_item == NO_ITEM) {
            first_item = id;
        }
        if (last_item == NO_ITEM) {
            item.prev = NO_ITEM;
            item.next = NO_ITEM;
            last_item = id;
        } else {
            item.prev = last_item;
            item_pool[last_item].next = id;
            item.next = NO_ITEM;
            last_item = id;
        }
    }

    void Detach(size_t id) {
        auto& item = item_pool[id];
        if (item.prev != NO_ITEM) {
            item_pool[item.prev].next = item.next;
        }
        if (item.next != NO_ITEM) {
            item_pool[item.next].prev = item.prev;
        }
        if (id == first_item) {
            first_item = item.next;
            if (first_item != NO_ITEM) {
                item_pool[first_item].prev = NO_ITEM;
            }
        }
        if (id == last_item) {
            last_item = item.prev;
            if (last_item != NO_ITEM) {
                item_pool[last_item].next = NO_ITEM;
            }
        }
    }

    std::vector<Item> item_pool;
    std::vector<size_t> free_items;
    size_t first_item{NO_ITEM};
    size_t last_item{NO_ITEM};
};

} // namespace Common

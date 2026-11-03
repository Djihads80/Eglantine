// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <random>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "core/hle/kernel/k_priority_queue.h"

namespace {
struct Affinity {
    u64 GetAffinityMask() const {
        return 3;
    }
    void SetAffinityMask(u64) {}
    bool GetAffinity(s32) const {
        return true;
    }
    void SetAffinity(s32, bool) {}
    void SetAll() {}
};

struct Member {
    struct QueueEntry {
        Member* prev{};
        Member* next{};
        void Initialize() {
            prev = next = nullptr;
        }
        void SetPrev(Member* value) {
            prev = value;
        }
        void SetNext(Member* value) {
            next = value;
        }
        Member* GetNext() const {
            return next;
        }
        Member* GetPrev() const {
            return prev;
        }
    };
    std::array<QueueEntry, 2> entries{};
    s32 priority{};
    QueueEntry& GetPriorityQueueEntry(s32 core) {
        return entries[core];
    }
    const QueueEntry& GetPriorityQueueEntry(s32 core) const {
        return entries[core];
    }
    Affinity GetAffinityMask() const {
        return {};
    }
    s32 GetActiveCore() const {
        return 0;
    }
    s32 GetPriority() const {
        return priority;
    }
    bool IsDummyThread() const {
        return false;
    }
};

template <size_t Priorities>
void CheckQueue() {
    using Queue = Kernel::KPriorityQueue<Member, 2, Priorities - 1, 0>;
    typename Queue::KPriorityQueueImpl queue;
    std::array<Member, 160> members{};
    std::array<std::array<bool, 2>, 160> present{};
    std::array<std::array<std::vector<Member*>, Priorities>, 2> model{};
    std::mt19937 random{6152};
    for (size_t i = 0; i < members.size(); ++i) {
        members[i].priority = static_cast<s32>(i % Priorities);
    }
    REQUIRE(queue.GetFront(0) == nullptr);
    for (u32 operation = 0; operation < 3000; ++operation) {
        const size_t index = random() % members.size();
        const s32 core = random() % 2;
        auto& member = members[index];
        auto& expected = model[core][member.priority];
        if (present[index][core]) {
            queue.Remove(member.priority, core, &member);
            std::erase(expected, &member);
        } else if ((random() & 1) != 0) {
            queue.PushFront(member.priority, core, &member);
            expected.insert(expected.begin(), &member);
        } else {
            queue.PushBack(member.priority, core, &member);
            expected.push_back(&member);
        }
        present[index][core] = !present[index][core];
        for (s32 checked_core = 0; checked_core < 2; ++checked_core) {
            Member* actual = queue.GetFront(checked_core);
            for (const auto& priority : model[checked_core]) {
                for (Member* wanted : priority) {
                    REQUIRE(actual == wanted);
                    actual = queue.GetNext(checked_core, actual);
                }
            }
            REQUIRE(actual == nullptr);
        }
    }
}
} // namespace

TEST_CASE("Priority queue bitmap traversal preserves thread order", "[core]") {
    CheckQueue<64>();
    CheckQueue<128>();
}

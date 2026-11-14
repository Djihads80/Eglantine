// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <utility>

#include "common/assert.h"
#include "common/common_types.h"

namespace VideoCommon {

class BankBase {
protected:
    const size_t base_bank_size{};
    size_t bank_size{};
    size_t current_slot{};
    // Queries take their slots' references where they are recorded (the GPU thread) and drop them
    // where their results land (the fencing thread), a report at a time: one counter each, on its
    // own cache line, so neither thread pulls the line from the other at every report.
    alignas(64) std::atomic<size_t> added{};
    alignas(64) std::atomic<size_t> dropped{};
    /// Keeps a derived bank's members off dropped's line (MSVC's layout puts them right after).
    std::array<char, 64 - sizeof(std::atomic<size_t>)> dropped_padding{};

public:
    explicit BankBase(size_t bank_size_) : base_bank_size{bank_size_}, bank_size(bank_size_) {}

    virtual ~BankBase() = default;

    virtual std::pair<bool, size_t> Reserve() {
        if (IsClosed()) {
            return {false, bank_size};
        }
        const size_t result = current_slot++;
        return {true, result};
    }

    virtual void Reset() {
        current_slot = 0;
        added = 0;
        dropped = 0;
        bank_size = base_bank_size;
    }

    size_t Size() const {
        return bank_size;
    }

    void AddReference(size_t how_many = 1) {
        added.fetch_add(how_many, std::memory_order_relaxed);
    }

    void CloseReference(size_t how_many = 1) {
        dropped.fetch_add(how_many, std::memory_order_relaxed);
    }

    /// The references taken and not dropped yet. Called where references are taken, so every
    /// reference dropped was taken before: more dropped than taken is a double free.
    size_t References() const {
        const size_t dropped_now = dropped.load(std::memory_order_acquire);
        const size_t added_now = added.load(std::memory_order_acquire);
        ASSERT(dropped_now <= added_now);
        return added_now - dropped_now;
    }

    void Close() {
        bank_size = current_slot;
    }

    bool IsClosed() const {
        return current_slot >= bank_size;
    }

    bool IsDead() const {
        return IsClosed() && References() == 0;
    }
};

template <typename BankType>
class BankPool {
private:
    std::deque<BankType> bank_pool;
    std::deque<size_t> bank_indices;

public:
    BankPool() = default;
    ~BankPool() = default;

    // Reserve a bank from the pool and return its index
    template <typename Func>
    size_t ReserveBank(Func&& builder) {
        if (!bank_indices.empty() && bank_pool[bank_indices.front()].IsDead()) {
            size_t new_index = bank_indices.front();
            bank_indices.pop_front();
            bank_pool[new_index].Reset();
            bank_indices.push_back(new_index);
            return new_index;
        }
        size_t new_index = bank_pool.size();
        builder(bank_pool, new_index);
        bank_indices.push_back(new_index);
        return new_index;
    }

    // Get a reference to a bank using its index
    BankType& GetBank(size_t index) {
        return bank_pool[index];
    }

    // Get the total number of banks in the pool
    size_t BankCount() const {
        return bank_pool.size();
    }
};

} // namespace VideoCommon

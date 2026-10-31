// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "common/common_types.h"
#include "common/div_ceil.h"

namespace Service {

struct Nx2SparseRegion {
    static constexpr u64 BlockSize = 4ULL << 20;

    enum class Block : u8 { Empty, Backed, Failed, Pending };

    struct PendingBlock {
        u64 index;
        u64 generation;
        u64 size;
    };

    Nx2SparseRegion(u64 size_, u64 generation_)
        : size{size_}, blocks(Common::DivCeil(size, BlockSize)), generation{generation_} {}

    std::optional<PendingBlock> BeginBacking(u64 block) {
        if (block >= blocks.size() || blocks[block] != Block::Empty) {
            return std::nullopt;
        }
        blocks[block] = Block::Pending;
        return PendingBlock{block, generation, (std::min)(BlockSize, size - block * BlockSize)};
    }

    bool IsPending(const PendingBlock& pending) const {
        return generation == pending.generation && pending.index < blocks.size() &&
               blocks[pending.index] == Block::Pending;
    }

    bool FinishBacking(const PendingBlock& pending, bool success) {
        if (!IsPending(pending)) {
            return false;
        }
        blocks[pending.index] = success ? Block::Backed : Block::Failed;
        return true;
    }

    u64 size;
    std::vector<Block> blocks;
    bool warned{};
    u64 generation;
};

} // namespace Service

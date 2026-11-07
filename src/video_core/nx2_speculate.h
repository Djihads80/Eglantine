// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <span>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "video_core/nx2_profile.h"

// EDEN_NX2_CL_SPECULATE=measure: predicts, for each read of a GPU-written command list, whether
// its words will be those the same list's previous read got, before waiting for the GPU's copy,
// and checks the prediction once the words are there (counted, logged every 10 s). Ocarina of
// Time waits ~64 times a second (~255 ms) for these copies, and about half of them return the
// words of the read before.
//
// The prediction comes from content signatures of guest memory as the host GPU sees it, kept
// per 256 bytes of device memory:
// - an upload (CPU data entering a buffer) hashes the bytes uploaded;
// - a compute dispatch gives what it may write the signature of its inputs: its program, grid,
//   uniform buffer bytes, the addresses and signatures of every buffer range it binds (written
//   ones included: their contents before it), and for images their modification ticks and the
//   uploads to images so far; an indirect dispatch's arguments are such a range too;
// - a DMA copy gives its destination the signature of its source, a clear that of its value;
// - every other GPU write (draws' storage writes, image-to-buffer copies) gives a signature
//   never seen before.
// Equal signatures mean equal words if every write is seen and shaders are deterministic: the
// measurement counts the predictions that were wrong. A wrong one can't be rolled back once the
// front has run a list (engine and macro state, uploads, semaphores, recorded Vulkan work), so
// words would only ever be used without waiting when the prediction says so beforehand.
namespace Tegra {

/// 0: off, 1: measure only.
[[nodiscard]] inline int Nx2ClSpeculate() {
    static const int mode = [] {
        const char* const value = std::getenv("EDEN_NX2_CL_SPECULATE");
        return value != nullptr && value[0] == 'm' ? 1 : 0;
    }();
    return mode;
}

[[nodiscard]] inline u64 Nx2SigMix(u64 hash, u64 value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash * 0xff51afd7ed558ccdULL;
}

/// Time spent signing (the measurement's own cost, on the threads that upload and record):
/// what skipping waits would save is the waits minus this.
inline std::atomic<u64> g_nx2_sign_ns{0};

class Nx2SignTimer {
public:
    Nx2SignTimer() : start{VideoCommon::Nx2Profile::Now()} {}
    ~Nx2SignTimer() {
        g_nx2_sign_ns.fetch_add(VideoCommon::Nx2Profile::Now() - start, std::memory_order_relaxed);
    }
    Nx2SignTimer(const Nx2SignTimer&) = delete;
    Nx2SignTimer& operator=(const Nx2SignTimer&) = delete;

private:
    u64 start;
};

/// A signature no content has had before.
inline std::atomic<u64> g_nx2_unique_sig{0};
[[nodiscard]] inline u64 Nx2UniqueSig() {
    return Nx2SigMix(0x756e69717565ULL,
                     g_nx2_unique_sig.fetch_add(1, std::memory_order_relaxed) + 1);
}

/// Bumped by every image upload (TextureCache::RefreshContents): images have no content
/// signature, so a dispatch that samples images depends on all uploads so far.
inline std::atomic<u64> g_nx2_image_upload_generation{0};

// Content signature per 256-byte unit of device memory (0: no tracked write this session). Two
// levels: 2^28 units, 32 KiB blocks (1 MiB of addresses) made on first write.
inline constexpr u64 Nx2SigUnitBits = 8;
inline constexpr u64 Nx2SigBlockBits = 12;
inline std::array<std::atomic<std::atomic<u64>*>,
                  (u64{1} << (36 - Nx2SigUnitBits - Nx2SigBlockBits))>
    g_nx2_content_sig_blocks{};

[[nodiscard]] inline std::atomic<u64>* Nx2SigSlot(u64 unit, bool create) {
    constexpr u64 block_units = u64{1} << Nx2SigBlockBits;
    if (unit >= g_nx2_content_sig_blocks.size() * block_units) {
        return nullptr;
    }
    auto& slot_block = g_nx2_content_sig_blocks[unit >> Nx2SigBlockBits];
    std::atomic<u64>* block = slot_block.load(std::memory_order_acquire);
    if (block == nullptr) {
        if (!create) {
            return nullptr;
        }
        auto* const fresh = new std::atomic<u64>[block_units]{};
        if (slot_block.compare_exchange_strong(block, fresh, std::memory_order_acq_rel)) {
            block = fresh;
        } else {
            delete[] fresh;
        }
    }
    return &block[unit & (block_units - 1)];
}

/// The signature of [device_address, +size) as it is now.
[[nodiscard]] inline u64 Nx2RangeSig(u64 device_address, u64 size) {
    if (size == 0) {
        return 0;
    }
    constexpr u64 block_units = u64{1} << Nx2SigBlockBits;
    u64 hash = Nx2SigMix(device_address & ((u64{1} << Nx2SigUnitBits) - 1), size);
    const u64 last = (device_address + size - 1) >> Nx2SigUnitBits;
    for (u64 unit = device_address >> Nx2SigUnitBits; unit <= last;) {
        if (unit >= g_nx2_content_sig_blocks.size() * block_units) {
            break;
        }
        const std::atomic<u64>* const block =
            g_nx2_content_sig_blocks[unit >> Nx2SigBlockBits].load(std::memory_order_acquire);
        const u64 block_end = (std::min)(last + 1, (unit | (block_units - 1)) + 1);
        if (block == nullptr) {
            hash = Nx2SigMix(hash, block_end - unit); // that many units without a signature
            unit = block_end;
            continue;
        }
        for (; unit < block_end; ++unit) {
            hash = Nx2SigMix(hash, block[unit & (block_units - 1)].load(std::memory_order_relaxed));
        }
    }
    return hash;
}

/// [device_address, +size) now holds what a writer whose signature is `writer` put there. Units
/// it covers in part keep what they held in their signature.
inline void Nx2SetRangeSig(u64 device_address, u64 size, u64 writer) {
    if (size == 0) {
        return;
    }
    constexpr u64 unit_size = u64{1} << Nx2SigUnitBits;
    const u64 end = device_address + size;
    const u64 base = device_address & ~(unit_size - 1);
    for (u64 unit = device_address >> Nx2SigUnitBits; unit <= (end - 1) >> Nx2SigUnitBits; ++unit) {
        std::atomic<u64>* const slot = Nx2SigSlot(unit, true);
        if (slot == nullptr) {
            return;
        }
        const u64 unit_begin = unit << Nx2SigUnitBits;
        const u64 begin = (std::max)(device_address, unit_begin) - unit_begin;
        const u64 stop = (std::min)(end, unit_begin + unit_size) - unit_begin;
        u64 sig = Nx2SigMix(Nx2SigMix(writer, unit_begin - base), (begin << 16) | stop);
        if (begin != 0 || stop != unit_size) {
            sig = Nx2SigMix(sig, slot->load(std::memory_order_relaxed));
        }
        slot->store(sig, std::memory_order_relaxed);
    }
}

/// A write whose result can't be told from what it read (draws' storage writes): a signature
/// never seen before, once per range and GPU tick (a later read sees it differ all the same).
inline void Nx2SetRangeUnique(u64 device_address, u64 size, u64 tick) {
    struct Recent {
        u64 address;
        u64 size;
        u64 tick;
    };
    thread_local std::array<Recent, 64> recent{};
    Recent& slot = recent[((device_address >> 8) ^ (device_address >> 20) ^ size) % recent.size()];
    if (slot.address == device_address && slot.size == size && slot.tick == tick) {
        return;
    }
    slot = {device_address, size, tick};
    Nx2SignTimer timer;
    Nx2SetRangeSig(device_address, size, Nx2UniqueSig());
}

/// Bytes uploaded to [device_address, +bytes.size()): what the host GPU reads there now.
inline void Nx2NoteUploadSig(u64 device_address, std::span<const u8> bytes) {
    // By unit, so a unit's signature depends only on its own bytes.
    Nx2SignTimer timer;
    constexpr u64 unit_size = u64{1} << Nx2SigUnitBits;
    u64 offset = 0;
    while (offset < bytes.size()) {
        const u64 address = device_address + offset;
        const u64 chunk =
            (std::min<u64>)(bytes.size() - offset, unit_size - (address & (unit_size - 1)));
        const u64 hash =
            Common::CityHash64(reinterpret_cast<const char*>(bytes.data() + offset), chunk);
        Nx2SetRangeSig(address, chunk, Nx2SigMix(0x75706c6f6164ULL, hash));
        offset += chunk;
    }
}

/// A new emulation session: no memory has a signature.
inline void Nx2ClearContentSigs() {
    for (auto& slot_block : g_nx2_content_sig_blocks) {
        if (std::atomic<u64>* const block = slot_block.load(std::memory_order_acquire)) {
            for (u64 i = 0; i < (u64{1} << Nx2SigBlockBits); ++i) {
                block[i].store(0, std::memory_order_relaxed);
            }
        }
    }
}

/// Counts of the predictions (EDEN_NX2_CL_SPECULATE=measure), logged every 10 s by
/// Nx2NoteSpeculation's caller thread.
struct Nx2SpeculationStats {
    std::atomic<u64> reads{};          ///< Copy reads with a previous read of the same words
    std::atomic<u64> waits{};          ///< Of reads: the copy was not done (a wait)
    std::atomic<u64> same{};           ///< Of reads: the words were those of the read before
    std::atomic<u64> predicted{};      ///< Of reads: predicted the same beforehand
    std::atomic<u64> predicted_waits{};///< Of predicted: the copy was not done
    std::atomic<u64> right{};          ///< Of predicted: they were the same
    std::atomic<u64> wrong{};          ///< Of predicted: they were not
    std::atomic<u64> waited_ns{};      ///< Wait time of predicted_waits that were right
    std::atomic<u64> untracked{};      ///< Of reads: last written by a dispatch reading memory
                                       ///< the signatures don't follow (never predicted)
    std::atomic<u64> window_start{};
};
inline Nx2SpeculationStats g_nx2_speculation{};

/// Logs and resets the counts when 10 s have passed (nx2_strato.cpp).
void Nx2ReportSpeculation();

} // namespace Tegra

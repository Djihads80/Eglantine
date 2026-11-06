// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon {

/// A FIFO with the subset of std::deque the split front uses, in one block: MSVC's std::deque
/// allocates every element over 8 bytes on its own (Nx2Perf SplitRings). Index 0 is the oldest.
/// A fixed ring (Grows false) holds at most Capacity elements (its users cap themselves); a
/// growing one takes Capacity once reserved, doubles when full and never shrinks.
template <typename T, size_t Capacity, bool Grows = false>
class SplitRing {
    static_assert(Capacity != 0 && (Capacity & (Capacity - 1)) == 0, "a power of two");

public:
    class const_iterator {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        const_iterator() = default;
        reference operator*() const {
            return (*ring)[index];
        }
        pointer operator->() const {
            return &(*ring)[index];
        }
        const_iterator& operator++() {
            ++index;
            return *this;
        }
        const_iterator operator++(int) {
            const const_iterator old = *this;
            ++index;
            return old;
        }
        const_iterator& operator--() {
            --index;
            return *this;
        }
        const_iterator operator--(int) {
            const const_iterator old = *this;
            --index;
            return old;
        }
        bool operator==(const const_iterator&) const = default;

    private:
        friend SplitRing;
        const_iterator(const SplitRing* ring_, size_t index_) : ring{ring_}, index{index_} {}
        const SplitRing* ring{};
        size_t index{};
    };
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    [[nodiscard]] size_t size() const {
        return count;
    }
    [[nodiscard]] bool empty() const {
        return count == 0;
    }
    [[nodiscard]] T& operator[](size_t i) {
        return items[(head + i) & (items.size() - 1)];
    }
    [[nodiscard]] const T& operator[](size_t i) const {
        return items[(head + i) & (items.size() - 1)];
    }
    [[nodiscard]] T& front() {
        return items[head];
    }
    [[nodiscard]] const T& front() const {
        return items[head];
    }
    [[nodiscard]] T& back() {
        return (*this)[count - 1];
    }
    [[nodiscard]] const T& back() const {
        return (*this)[count - 1];
    }
    void push_back(const T& value) {
        if constexpr (Grows) {
            if (count == items.size()) [[unlikely]] {
                Grow((std::max)(Capacity, items.size() * 2));
            }
        } else {
            DEBUG_ASSERT(count < Capacity);
        }
        (*this)[count] = value;
        ++count;
    }
    void pop_front() {
        head = (head + 1) & (items.size() - 1);
        --count;
    }
    void clear() {
        head = 0;
        count = 0;
    }
    /// A growing ring: takes its first Capacity elements now, not on the first push.
    void Reserve() {
        if constexpr (Grows) {
            if (items.size() < Capacity) {
                Grow(Capacity);
            }
        }
    }

    [[nodiscard]] const_iterator begin() const {
        return {this, 0};
    }
    [[nodiscard]] const_iterator end() const {
        return {this, count};
    }
    [[nodiscard]] const_reverse_iterator rbegin() const {
        return const_reverse_iterator{end()};
    }
    [[nodiscard]] const_reverse_iterator rend() const {
        return const_reverse_iterator{begin()};
    }

private:
    void Grow(size_t capacity) {
        std::vector<T> grown(capacity);
        for (size_t i = 0; i < count; ++i) {
            grown[i] = std::move((*this)[i]);
        }
        items = std::move(grown);
        head = 0;
    }

    std::conditional_t<Grows, std::vector<T>, std::array<T, Capacity>> items{};
    size_t head{};
    size_t count{};
};

/// Nx2Perf SplitRings: which containers the split front's bookkeeping uses. Verify
/// (EDEN_NX2_PERF_VERIFY) keeps both, compares them and answers from the std::deque.
enum class SplitRingMode : u8 {
    Off,
    On,
    Verify,
};

inline std::atomic<u64> g_split_ring_mismatches{};

/// SplitRingMode::Verify: the std::deque and the ring answered differently.
inline void ReportSplitRingMismatch(const char* what, u64 deque_value, u64 ring_value) {
    const u64 count = g_split_ring_mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 64) {
        LOG_ERROR(HW_GPU, "Nx2Perf split_rings: {} deque {:#x} ring {:#x} ({} so far)", what,
                  deque_value, ring_value, count);
    }
}

/// Copies `from` over `to`, oldest first.
template <typename From, typename To>
void CopySplitQueue(const From& from, To& to) {
    to.clear();
    for (const auto& item : from) {
        to.push_back(item);
    }
}

/// What the entries the split GPU thread's front queued may write to guest memory, so a read on
/// the front (MME DMA) waits only for the queued work it depends on. Positions are ring positions
/// just past an entry: the back has run the entry once it consumed that far.
class SplitWriteTracker {
public:
    enum class Effect : u8 {
        Unknown, ///< May write guest memory anywhere at once
        None,    ///< Writes no guest memory, or only the ranges noted with Write
        Gpu,     ///< Draws, dispatches and copies with destinations not known to the front
        /// Only draws whose exact programs the back proved write no guest memory
        /// (Nx2Perf MacroReadWriterSet, Tegra::Nx2IsNonWriterDraw)
        Draw,
    };

    /// What a noted write stores: constant buffer data the front queued (its words are known to
    /// the front) or anything else.
    enum class WriteKind : u8 {
        Other,
        ConstantBuffer,
    };

    /// The effect of an entry that does both.
    [[nodiscard]] static constexpr Effect Combine(Effect a, Effect b) {
        if (a == Effect::Unknown || b == Effect::Unknown) {
            return Effect::Unknown;
        }
        if (a == Effect::Gpu || b == Effect::Gpu) {
            return Effect::Gpu;
        }
        return a == Effect::Draw || b == Effect::Draw ? Effect::Draw : Effect::None;
    }

    /// More pending ranges than this collapse into one unknown write.
    static constexpr size_t MaxWrites = 512;

    void Entry(Effect effect, u64 end) {
        if (effect == Effect::Unknown) {
            unknown = end;
        } else if (effect == Effect::Gpu) {
            gpu = end;
            writer = end;
        } else if (effect == Effect::Draw) {
            gpu = end;
            if (use_deques) {
                NoteDrawEnd(draw_epochs, end);
            }
            if (use_rings) {
                NoteDrawEnd(ring_epochs, end);
            }
        }
        entry_epoch = 0;
    }

    /// The next entry's draws were classified with the program epoch `epoch`
    /// (Tegra::g_nx2_code_epoch). Without it a Draw entry never validates (DrawsValid).
    void DrawEpoch(u64 epoch) {
        entry_epoch = entry_epoch == 0 ? epoch : (std::min)(entry_epoch, epoch);
    }

    /// Nx2Perf MacroReadOverlayWriters: a write joins the previous one only when their kinds
    /// match, so each noted range's kind describes exactly its bytes. Set before any Write.
    void SetSeparateKinds(bool separate) {
        separate_kinds = separate;
    }

    /// Nx2Perf SplitRings: the containers that keep the pending writes and draw epochs (the
    /// pending ones move over).
    void SetRings(SplitRingMode mode) {
        const bool deques = mode != SplitRingMode::On;
        const bool rings = mode != SplitRingMode::Off;
        if (rings) {
            ring_epochs.Reserve();
        }
        if (rings && !use_rings) {
            CopySplitQueue(writes, ring_writes);
            CopySplitQueue(draw_epochs, ring_epochs);
        }
        if (deques && !use_deques) {
            CopySplitQueue(ring_writes, writes);
            CopySplitQueue(ring_epochs, draw_epochs);
        }
        if (!deques) {
            writes.clear();
            draw_epochs.clear();
        }
        if (!rings) {
            ring_writes.clear();
            ring_epochs.clear();
        }
        use_deques = deques;
        use_rings = rings;
    }

    /// The entry that ends at `end` writes the guest memory in [begin, end_address). Ends never
    /// decrease. A write next to or over the previous one joins it (conservative: the joined
    /// range retires with the later write; one that mixes kinds is Other, unless kinds are kept
    /// apart: SetSeparateKinds).
    void Write(u64 begin, u64 end_address, u64 end, WriteKind kind = WriteKind::Other) {
        if (begin >= end_address) {
            return;
        }
        if (use_deques) {
            AddWrite(writes, begin, end_address, end, kind);
        }
        if (use_rings) {
            AddWrite(ring_writes, begin, end_address, end, kind);
        }
    }

    /// Drops the writes the back has run (it consumed `consumed` words).
    void Retire(u64 consumed) {
        if (use_deques) {
            RetireFrom(writes, consumed);
            RetireFrom(draw_epochs, consumed);
        }
        if (use_rings) {
            RetireFrom(ring_writes, consumed);
            RetireFrom(ring_epochs, consumed);
            if (use_deques) {
                CheckRings();
            }
        }
    }

    /// The position the back must pass for the noted writes to [begin, end) to be done, or 0.
    [[nodiscard]] u64 LastWriteTo(u64 begin, u64 end) const {
        if (!use_rings) {
            return NewestWrite(writes, begin, end);
        }
        const u64 ring = NewestWrite(ring_writes, begin, end);
        if (!use_deques) {
            return ring;
        }
        return Checked("last write", NewestWrite(writes, begin, end), ring);
    }

    /// LastWriteTo for the noted writes of `kind` only.
    [[nodiscard]] u64 LastWriteTo(u64 begin, u64 end, WriteKind kind) const {
        if (!use_rings) {
            return NewestWriteOfKind(writes, begin, end, kind);
        }
        const u64 ring = NewestWriteOfKind(ring_writes, begin, end, kind);
        if (!use_deques) {
            return ring;
        }
        return Checked("last write of kind", NewestWriteOfKind(writes, begin, end, kind), ring);
    }

    /// The position the back must pass for every unlocalized writer and the noted writes to
    /// [begin, end) to execute. Consumption does not imply completion of deferred fence writes.
    [[nodiscard]] u64 ReadTarget(u64 begin, u64 end) const {
        return (std::max)({unknown, gpu, LastWriteTo(begin, end)});
    }

    /// ReadTarget without the draws the back proved write no memory (Effect::Draw): every
    /// unknown write, every writer (Effect::Gpu) and the noted writes to [begin, end). Valid only
    /// while DrawsValid holds for the draws it skips.
    [[nodiscard]] u64 WriterReadTarget(u64 begin, u64 end) const {
        return (std::max)({unknown, writer, LastWriteTo(begin, end)});
    }

    /// Whether every Effect::Draw entry past `consumed` was classified with `epoch`, so the
    /// program proofs they relied on still hold.
    [[nodiscard]] bool DrawsValid(u64 consumed, u64 epoch) const {
        if (!use_rings) {
            return EpochsValid(draw_epochs, consumed, epoch);
        }
        const bool ring = EpochsValid(ring_epochs, consumed, epoch);
        if (!use_deques) {
            return ring;
        }
        return Checked("draws valid", EpochsValid(draw_epochs, consumed, epoch), ring) != 0;
    }

    /// Whether a noted write past `consumed` and up to `limit` has a range `touches(begin, end)`.
    template <typename Pred>
    [[nodiscard]] bool AnyPendingWrite(u64 consumed, u64 limit, Pred&& touches) const {
        if (!use_rings) {
            return AnyWriteIn(writes, consumed, limit, touches);
        }
        const bool ring = AnyWriteIn(ring_writes, consumed, limit, touches);
        if (!use_deques) {
            return ring;
        }
        return Checked("any pending write", AnyWriteIn(writes, consumed, limit, touches),
                       ring) != 0;
    }

    [[nodiscard]] u64 LastUnknown() const {
        return unknown;
    }
    [[nodiscard]] u64 LastGpu() const {
        return gpu;
    }
    [[nodiscard]] u64 LastWriter() const {
        return writer;
    }
    /// How many noted ranges are pending (at MaxWrites they collapse into an unknown write).
    [[nodiscard]] size_t PendingWrites() const {
        return use_deques ? writes.size() : ring_writes.size();
    }

    void Reset() {
        writes.clear();
        draw_epochs.clear();
        ring_writes.clear();
        ring_epochs.clear();
        unknown = 0;
        gpu = 0;
        writer = 0;
        entry_epoch = 0;
    }

private:
    struct PendingWrite {
        u64 begin;
        u64 end;
        u64 position;
        WriteKind kind;
    };
    struct EpochRecord {
        u64 position; ///< Just past the newest Draw entry classified with `epoch`
        u64 epoch;
    };

    template <typename Epochs>
    void NoteDrawEnd(Epochs& epochs, u64 end) {
        if (epochs.empty() || epochs.back().epoch != entry_epoch) {
            epochs.push_back({end, entry_epoch});
        } else {
            epochs.back().position = end;
        }
    }

    template <typename Writes>
    void AddWrite(Writes& list, u64 begin, u64 end_address, u64 end, WriteKind kind) {
        if (!list.empty()) {
            PendingWrite& last = list.back();
            if (begin <= last.end && last.begin <= end_address &&
                (!separate_kinds || last.kind == kind)) {
                last.begin = (std::min)(last.begin, begin);
                last.end = (std::max)(last.end, end_address);
                last.position = end;
                if (last.kind != kind) {
                    last.kind = WriteKind::Other;
                }
                return;
            }
        }
        if (list.size() >= MaxWrites) {
            unknown = (std::max)(unknown, end);
            list.clear();
            return;
        }
        list.push_back({begin, end_address, end, kind});
    }

    template <typename List>
    static void RetireFrom(List& list, u64 consumed) {
        while (!list.empty() && list.front().position <= consumed) {
            list.pop_front();
        }
    }

    template <typename Writes>
    [[nodiscard]] static u64 NewestWrite(const Writes& list, u64 begin, u64 end) {
        // Positions grow towards the back: the newest overlap is the latest.
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            if (it->begin < end && begin < it->end) {
                return it->position;
            }
        }
        return 0;
    }

    template <typename Writes>
    [[nodiscard]] static u64 NewestWriteOfKind(const Writes& list, u64 begin, u64 end,
                                               WriteKind kind) {
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            if (it->kind == kind && it->begin < end && begin < it->end) {
                return it->position;
            }
        }
        return 0;
    }

    template <typename Epochs>
    [[nodiscard]] static bool EpochsValid(const Epochs& list, u64 consumed, u64 epoch) {
        for (const EpochRecord& record : list) {
            if (record.position > consumed && record.epoch != epoch) {
                return false;
            }
        }
        return true;
    }

    template <typename Writes, typename Pred>
    [[nodiscard]] static bool AnyWriteIn(const Writes& list, u64 consumed, u64 limit,
                                         Pred& touches) {
        for (const PendingWrite& write : list) {
            if (write.position > limit) {
                break;
            }
            if (write.position > consumed && touches(write.begin, write.end)) {
                return true;
            }
        }
        return false;
    }

    /// SplitRingMode::Verify: compares both answers and containers, returns the deque's answer.
    u64 Checked(const char* what, u64 deque_value, u64 ring_value) const {
        if (deque_value != ring_value) {
            ReportSplitRingMismatch(what, deque_value, ring_value);
        }
        CheckRings();
        return deque_value;
    }

    void CheckRings() const {
        const auto check = [](const char* what, u64 deque_value, u64 ring_value) {
            if (deque_value != ring_value) {
                ReportSplitRingMismatch(what, deque_value, ring_value);
            }
        };
        check("writes size", writes.size(), ring_writes.size());
        if (!writes.empty() && writes.size() == ring_writes.size()) {
            const PendingWrite& front = writes.front();
            const PendingWrite& back = writes.back();
            check("writes front position", front.position, ring_writes.front().position);
            check("writes front begin", front.begin, ring_writes.front().begin);
            check("writes back position", back.position, ring_writes.back().position);
            check("writes back begin", back.begin, ring_writes.back().begin);
            check("writes back end", back.end, ring_writes.back().end);
            check("writes back kind", static_cast<u64>(back.kind),
                  static_cast<u64>(ring_writes.back().kind));
        }
        check("draw epochs size", draw_epochs.size(), ring_epochs.size());
        if (!draw_epochs.empty() && draw_epochs.size() == ring_epochs.size()) {
            check("draw epochs front position", draw_epochs.front().position,
                  ring_epochs.front().position);
            check("draw epochs back position", draw_epochs.back().position,
                  ring_epochs.back().position);
            check("draw epochs back epoch", draw_epochs.back().epoch, ring_epochs.back().epoch);
        }
    }

    std::deque<PendingWrite> writes;
    std::deque<EpochRecord> draw_epochs;
    /// Nx2Perf SplitRings: the same, without an allocation per element
    SplitRing<PendingWrite, MaxWrites> ring_writes;
    SplitRing<EpochRecord, 1024, true> ring_epochs;
    u64 unknown{};
    u64 gpu{};    ///< Past the newest entry that draws, dispatches or copies (Gpu or Draw)
    u64 writer{}; ///< Past the newest Gpu entry: the ones that can write guest memory
    u64 entry_epoch{};
    bool separate_kinds{};
    bool use_deques{true};
    bool use_rings{};
};

/// Nx2Perf MacroReadCbOverlay: the constant buffer words the split front queued, oldest first,
/// kept by device address until the back ran their entries.
class SplitConstBufferWords {
public:
    struct Word {
        u64 device;
        u64 position;
        u32 value;
    };
    static constexpr size_t MaxWords = 4096;

    /// Nx2Perf SplitRings, as SplitWriteTracker::SetRings.
    void SetRings(SplitRingMode mode) {
        const bool deques = mode != SplitRingMode::On;
        const bool rings = mode != SplitRingMode::Off;
        if (rings && !use_ring) {
            CopySplitQueue(words, ring);
        }
        if (deques && !use_deque) {
            CopySplitQueue(ring, words);
        }
        if (!deques) {
            words.clear();
        }
        if (!rings) {
            ring.clear();
        }
        use_deque = deques;
        use_ring = rings;
    }

    [[nodiscard]] size_t Size() const {
        return use_deque ? words.size() : ring.size();
    }
    /// The caller keeps Size() at most MaxWords.
    void Push(const Word& word) {
        if (use_deque) {
            words.push_back(word);
        }
        if (use_ring) {
            ring.push_back(word);
        }
    }
    /// Drops the words of the entries the back ran.
    void Retire(u64 consumed) {
        if (use_deque) {
            RetireFrom(words, consumed);
        }
        if (use_ring) {
            RetireFrom(ring, consumed);
            if (use_deque) {
                Check();
            }
        }
    }
    /// Copies the words queued after `consumed` over the bytes `out` holds for the device range
    /// [begin, begin + size), oldest first: the newest wins.
    void Overlay(u64 consumed, u64 begin, u64 size, u8* out) const {
        if (!use_ring) {
            OverlayFrom(words, consumed, begin, size, out);
            return;
        }
        if (!use_deque) {
            OverlayFrom(ring, consumed, begin, size, out);
            return;
        }
        std::vector<u8> ring_out(out, out + size);
        OverlayFrom(words, consumed, begin, size, out);
        OverlayFrom(ring, consumed, begin, size, ring_out.data());
        for (u64 i = 0; i < size; ++i) {
            if (out[i] != ring_out[i]) {
                ReportSplitRingMismatch("cb overlay byte", out[i], ring_out[i]);
                break;
            }
        }
        Check();
    }
    void Clear() {
        words.clear();
        ring.clear();
    }

private:
    template <typename List>
    static void RetireFrom(List& list, u64 consumed) {
        while (!list.empty() && list.front().position <= consumed) {
            list.pop_front();
        }
    }

    template <typename List>
    static void OverlayFrom(const List& list, u64 consumed, u64 begin, u64 size, u8* out) {
        const u64 end = begin + size;
        for (const Word& word : list) {
            if (word.position <= consumed) {
                continue;
            }
            const u64 low = (std::max)(word.device, begin);
            const u64 high = (std::min)(word.device + sizeof(u32), end);
            if (low < high) {
                std::memcpy(out + (low - begin),
                            reinterpret_cast<const u8*>(&word.value) + (low - word.device),
                            high - low);
            }
        }
    }

    void Check() const {
        if (words.size() != ring.size()) {
            ReportSplitRingMismatch("cb words size", words.size(), ring.size());
            return;
        }
        if (words.empty()) {
            return;
        }
        if (words.front().position != ring.front().position ||
            words.front().device != ring.front().device) {
            ReportSplitRingMismatch("cb words front position", words.front().position,
                                    ring.front().position);
        }
        if (words.back().position != ring.back().position ||
            words.back().device != ring.back().device || words.back().value != ring.back().value) {
            ReportSplitRingMismatch("cb words back position", words.back().position,
                                    ring.back().position);
        }
    }

    std::deque<Word> words;
    /// Nx2Perf SplitRings: the same, without an allocation per word
    SplitRing<Word, MaxWords> ring;
    bool use_deque{true};
    bool use_ring{};
};

} // namespace VideoCommon

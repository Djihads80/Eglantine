// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_perf.h"

// Nx2Perf inflight_write_wait / inflight_read_exact: with frames_ahead, vi2 replies to present N
// after frame N-1's fence and opens an epoch for fence N. Core memory write-protects the pages the
// back may still read for frame N (the war_guard_probe arm); the guest's first write of one, or of
// a tracker-protected page, waits on a guest core until the back passes fence N and closes the
// epoch. inflight_read_exact: while the epoch is open, a guest read the buffer cache serves from a
// preflush block whose last GPU writer is in flight waits for fence N's host value. Shared by core
// memory (traps, waits), the buffer cache (writer stamps, preflush reads) and vi2 (opening, log).
namespace VideoCommon::Nx2Inflight {

/// inflight_wait_v2: both waits, escaping only on a release page or a lack of progress.
inline bool V2() {
    static const bool on = Nx2Perf::On(Nx2Perf::Opt::InflightWaitV2);
    return on;
}

inline bool WriteWait() {
    static const bool on = (Nx2Perf::On(Nx2Perf::Opt::FramesAhead) &&
                            Nx2Perf::On(Nx2Perf::Opt::InflightWriteWait)) ||
                           V2();
    return on;
}

inline bool ReadExact() {
    static const bool on = (Nx2Perf::On(Nx2Perf::Opt::FramesAhead) &&
                            Nx2Perf::On(Nx2Perf::Opt::InflightReadExact)) ||
                           V2();
    return on;
}

/// Whether epochs can open at all; false keeps every trap and read path as it was.
inline bool Any() {
    static const bool on = WriteWait() || ReadExact();
    return on;
}

struct Epoch {
    std::atomic<u64> open_seq{0}; ///< Sequence of the open epoch, 0 while none is open
    std::atomic<u64> seq{0};      ///< Epochs opened so far
    std::atomic<u32> fence_id{~0U};
    std::atomic<u32> fence_value{0};
    std::atomic<u32> prev_id{~0U};
    std::atomic<u32> prev_value{0};
    std::atomic<bool> guarded{false}; ///< Pages may still be write-protected by an epoch's arm
    std::atomic<u64> fence_reached_seq{0}; ///< Last epoch whose fence the back passed
    /// Back: one stamp per GPU write of a preflush block (BufferCache::QueueGpuWrite).
    std::atomic<u64> write_stamp{0};
    std::atomic<u64> passed_stamp{0}; ///< write_stamp when the back passed passed_seq's fence
    std::atomic<u64> passed_seq{0};
    /// Blocks written with a stamp above this are frame N's writers; 0 when unknown (every
    /// written block counts as in flight).
    std::atomic<u64> prev_fence_stamp{0};

    // Window counters, read and cleared by vi2's per-second log.
    std::atomic<u64> opened{}, closed_by_write{}, closed_by_fence{}, closed_by_present{};
    std::atomic<u64> escapes{}, host_writes{}, write_waits{}, write_wait_ns{}, max_wait_ns{};
    std::atomic<u64> read_waits{}, read_wait_ns{}, read_escapes{}, preflush_reads{};
    std::atomic<u64> failed_opens{};
    // inflight_wait_v2: escapes (writes and reads) by reason; Verify: waits the 3 ms rule would
    // have given up on.
    std::atomic<u64> escape_release_page{}, escape_no_progress{}, escape_timeout{};
    std::atomic<u64> old_rule_differences{};
    // Verify: (a) bank pages the guest changed before the back passed the epoch's fence, (b)
    // preflush-served reads that differ from the bytes after their in-flight writer landed.
    std::atomic<u64> bank_violations{}, read_compared{}, read_differs{}, read_compare_dropped{};
    std::atomic<u64> escape_logs{}, verify_logs{};
};

inline Epoch& Get() {
    static Epoch epoch;
    return epoch;
}

/// inflight_wait_v2: the device address of each suspended channel's semaphore acquire (0 =
/// free slot). Puller sets a slot when it suspends a channel and clears it when it resumes.
struct PendingAcquireSlot {
    std::atomic<const void*> owner{nullptr};
    std::atomic<u64> device_addr{0};
};
inline constexpr size_t NumPendingAcquires = 16;

inline std::array<PendingAcquireSlot, NumPendingAcquires>& PendingAcquirePages() {
    static std::array<PendingAcquireSlot, NumPendingAcquires> slots;
    return slots;
}

inline std::atomic<u64>& PendingAcquireOverflow() {
    static std::atomic<u64> overflow{0};
    return overflow;
}

inline void SetPendingAcquire(const void* owner, u64 device_addr) {
    auto& slots = PendingAcquirePages();
    PendingAcquireSlot* free_slot = nullptr;
    for (PendingAcquireSlot& slot : slots) {
        const void* current = slot.owner.load(std::memory_order_acquire);
        if (current == owner) {
            slot.device_addr.store(device_addr, std::memory_order_release);
            return;
        }
        if (current == nullptr && free_slot == nullptr) {
            free_slot = &slot;
        }
    }
    for (PendingAcquireSlot* slot = free_slot; slot != nullptr && slot != slots.data() + slots.size();
         ++slot) {
        const void* expected = nullptr;
        if (slot->owner.compare_exchange_strong(expected, owner, std::memory_order_acq_rel)) {
            slot->device_addr.store(device_addr, std::memory_order_release);
            return;
        }
    }
    PendingAcquireOverflow().fetch_add(1, std::memory_order_relaxed);
}

inline void ClearPendingAcquire(const void* owner) {
    for (PendingAcquireSlot& slot : PendingAcquirePages()) {
        if (slot.owner.load(std::memory_order_acquire) == owner) {
            slot.device_addr.store(0, std::memory_order_release);
            slot.owner.store(nullptr, std::memory_order_release);
            return;
        }
    }
}

/// Buffer cache -> core memory, on the reading thread: what the last OnCPURead served.
struct ReadNote {
    bool preflush_served = false; ///< Served from a preflush mark while an epoch was open
    bool in_flight = false;       ///< A block of it was written after the previous fence
};
inline thread_local ReadNote read_note{};

/// Verify (b): reads compared with their bytes after fence N lands, a fixed pool.
struct ReadCompare {
    static constexpr size_t MaxBytes = 64;
    std::atomic<u32> busy{0};
    u64 address = 0;
    u32 size = 0;
    const u8* host = nullptr;
    std::array<u8, MaxBytes> bytes{};
};
inline constexpr size_t NumReadCompares = 64;

inline std::array<ReadCompare, NumReadCompares>& ReadCompares() {
    static std::array<ReadCompare, NumReadCompares> pool;
    return pool;
}

} // namespace VideoCommon::Nx2Inflight

// Nx2Perf war_guard_probe (measurement only): how long after vi2's Ocarina reply the guest first
// touches memory the back may still read for the frame it just presented (t_w), and how many
// native uniform bank pages the guest rewrites before the next frame's first bind of them. No
// access is blocked or reordered: core memory write-protects the pages the tracker left writable
// (CPU-written since their upload) after the reply and gives each one back its permission on its
// first write. Shared by core memory (traps), the buffer cache (bank pages) and vi2 (arming, log).
namespace VideoCommon::Nx2WarProbe {

inline bool On() {
    static const bool on = Nx2Perf::On(Nx2Perf::Opt::WarGuardProbe);
    return on;
}

enum class Category : u32 {
    Cached,      ///< Write to a page the tracker protects (HandleRasterizerWrite)
    Unprotected, ///< First write to a CPU-written page the probe protected after the reply
    Bank,        ///< Either kind of write to a native uniform bank page
    Read,        ///< Read trap of a GPU-written page (HandleRasterizerDownload)
    Count,
};

inline constexpr size_t NumCategories = static_cast<size_t>(Category::Count);
inline constexpr size_t PageBits = 12;
inline constexpr size_t PageSize = size_t{1} << PageBits;
inline constexpr size_t MaxBankPages = 4096;
inline constexpr size_t SnapshotPages = 1024;

struct BankPage {
    u64 device_page; ///< Device address >> PageBits
    u64 cpu_page;    ///< Guest virtual address >> PageBits, 0 without a CPU mapping
    const u8* host;  ///< Backing memory of the page, nullptr when unmapped
};

struct State {
    // Buffer cache (back thread, TickFrame) -> vi2: bank pages bound in the last two frames,
    // sorted by device page.
    std::mutex export_mutex;
    std::array<BankPage, MaxBankPages> exported{};
    size_t exported_count = 0;
    u64 export_overflow = 0;

    // vi2 (after its wait) -> back (first bind of a bank after it): the bank pages' guest bytes.
    std::mutex snapshot_mutex;
    std::unique_ptr<u8[]> snapshot_bytes;
    std::array<u64, SnapshotPages> snapshot_device_page{};
    std::array<u8, SnapshotPages> snapshot_pending{};
    size_t snapshot_count = 0;
    /// inflight_write_wait: the epoch opened after this snapshot, 0 when none did.
    u64 snapshot_epoch = 0;

    // The armed frame: vi2 bumps frame and clears reply_ns when it arms, sets reply_ns when it
    // replies; the first guest trap after that claims the frame.
    std::atomic<u64> frame{0};
    std::atomic<s64> reply_ns{0};
    std::atomic<u64> first_claim{0};
    std::atomic<u64> first_done{0};
    s64 first_ns = 0;
    Category first_category = Category::Cached;

    // Window counters, read and cleared by vi2's log.
    std::array<std::atomic<u64>, NumCategories> traps{};
    std::atomic<u64> host_traps{};      ///< Traps from threads that are not guest cores
    std::atomic<u64> pre_reply_traps{}; ///< Guest traps between the arm and the reply
    std::atomic<u64> bank_compared{};
    std::atomic<u64> bank_changed{};
    std::atomic<u64> verify_differences{};
};

inline State& Get() {
    static State state;
    return state;
}

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// A trapped access of a guarded page; guest_core is false for host (service) threads.
inline void NoteTrap(Category category, bool guest_core) {
    State& state = Get();
    if (!guest_core) {
        state.host_traps.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    state.traps[static_cast<size_t>(category)].fetch_add(1, std::memory_order_relaxed);
    const u64 frame = state.frame.load(std::memory_order_acquire);
    if (frame == 0 || state.first_done.load(std::memory_order_acquire) == frame) {
        return;
    }
    const s64 reply = state.reply_ns.load(std::memory_order_acquire);
    if (reply == 0) {
        state.pre_reply_traps.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    u64 claimed = state.first_claim.load(std::memory_order_relaxed);
    if (claimed == frame ||
        !state.first_claim.compare_exchange_strong(claimed, frame, std::memory_order_acq_rel)) {
        return;
    }
    state.first_ns = NowNs() - reply;
    state.first_category = category;
    state.first_done.store(frame, std::memory_order_release);
}

/// The back's first bind of a bank after vi2's snapshot: counts the bank pages whose guest bytes
/// changed since then (direct evidence that the guest rewrites data a frame still in flight
/// would read). get_pointer(device address) returns the page's backing memory or nullptr.
template <typename GetPointer>
void CompareBank(u64 device_addr, u64 size, GetPointer&& get_pointer) {
    State& state = Get();
    std::scoped_lock lk{state.snapshot_mutex};
    if (state.snapshot_count == 0 || size == 0) {
        return;
    }
    const u64 first = device_addr >> PageBits;
    const u64 last = (device_addr + (std::min<u64>)(size, 0x10000) - 1) >> PageBits;
    const u64* const begin = state.snapshot_device_page.data();
    const u64* const end = begin + state.snapshot_count;
    for (const u64* it = std::lower_bound(begin, end, first); it != end && *it <= last; ++it) {
        const size_t index = static_cast<size_t>(it - begin);
        if (state.snapshot_pending[index] == 0) {
            continue;
        }
        state.snapshot_pending[index] = 0;
        const u8* const now = get_pointer(*it << PageBits);
        if (now == nullptr) {
            continue;
        }
        state.bank_compared.fetch_add(1, std::memory_order_relaxed);
        if (std::memcmp(now, state.snapshot_bytes.get() + index * PageSize, PageSize) != 0) {
            state.bank_changed.fetch_add(1, std::memory_order_relaxed);
            // inflight_write_wait: the back binds the page before passing the epoch's fence, so
            // this is frame N's read of a page the guest rewrote with the epoch open.
            auto& epoch = Nx2Inflight::Get();
            if (state.snapshot_epoch != 0 &&
                epoch.fence_reached_seq.load(std::memory_order_acquire) < state.snapshot_epoch) {
                const u64 count =
                    epoch.bank_violations.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count <= 64) {
                    LOG_ERROR(HW_GPU,
                              "Nx2Perf inflight_write_wait: bank page {:#x} changed before the "
                              "back passed epoch {}'s fence ({} so far)",
                              *it << PageBits, state.snapshot_epoch, count);
                }
            }
        }
    }
}

} // namespace VideoCommon::Nx2WarProbe

// Nx2Perf run_ahead_ledger (measurement only): the device pages the split back reads for frame N
// (by category), which of them the guest changes between vi2's reply for present N and the entry
// of present N+1, and, for guest reads of preflushed blocks written in frame N, how long after the
// reply the read came (a) and how long before the fence the back recorded the writer (b). Nothing
// is blocked, reordered or read differently; vi2 hashes the pages before its reply.
namespace VideoCommon::Nx2Ledger {

inline bool On() {
    static const bool on = Nx2Perf::On(Nx2Perf::Opt::RunAheadLedger);
    return on;
}

/// run_ahead_cover (measurement only, needs the ledger): K-frame unions of the read pages, the
/// tracker state of rewritten pages and the simulated run-ahead RAW stall per wait rule.
inline bool CoverOn() {
    static const bool on = On() && Nx2Perf::On(Nx2Perf::Opt::RunAheadCover);
    return on;
}

/// run_ahead_cover: a guest page's state at vi2's reply (core memory's tracker view).
enum class TrackerClass : u8 {
    Tracked,   ///< The tracker protects it: a write traps anyway
    ArmSet,    ///< CPU-writable, in the war_guard arm set (tracker-registered, CPU-written)
    Untracked, ///< Writable and outside the arm set: no trap design covers it
    NoCpu,     ///< No guest mapping behind the device page
    Count,
};
inline constexpr size_t NumTrackerClasses = static_cast<size_t>(TrackerClass::Count);

enum class Category : u8 {
    Other, ///< Outside every scope: shader code, QMD, queries, ...
    UniformFast,
    BankCompare,
    UniformCached,
    Vertex,
    Index,
    Storage,
    Indirect,
    TextureUpload,
    Descriptor,
    Count,
};
inline constexpr size_t NumCategories = static_cast<size_t>(Category::Count);
inline constexpr std::array<std::string_view, NumCategories> category_names{
    "other", "ufast", "bank", "ucached", "vertex", "index", "storage", "indirect", "texture", "desc"};

/// The split back with the switch on (BackLoop): its device memory reads are recorded.
inline thread_local bool g_is_back = false;
/// What the reading code is doing (Category), Other outside every LedgerScope.
inline thread_local u8 g_ledger_category = 0;

class LedgerScope {
public:
    explicit LedgerScope(Category category) : previous{g_ledger_category} {
        g_ledger_category = static_cast<u8>(category);
    }
    ~LedgerScope() {
        g_ledger_category = previous;
    }
    LedgerScope(const LedgerScope&) = delete;
    LedgerScope& operator=(const LedgerScope&) = delete;

private:
    u8 previous;
};

inline constexpr u64 PageBits = 12;
inline constexpr u64 PageSize = u64{1} << PageBits;
inline constexpr size_t SetSize = 65536;
inline constexpr size_t SetLimit = SetSize / 4 * 3; ///< New pages past this count as overflow
inline constexpr u64 EmptyPage = ~u64{0};

struct Entry {
    u64 device_page;
    u32 bytes; ///< Bytes read with a known size, saturating
    u16 category_mask;
    u16 padding;
};
static_assert(sizeof(Entry) == 0x10);

/// One frame's pages: open addressing with linear probing, written by the back only.
struct PageSet {
    std::array<Entry, SetSize> entries;
    std::array<u32, SetSize> used; ///< Occupied slots in insertion order
    u32 used_count = 0;
    u32 last_slot = 0;
    u64 last_page = EmptyPage;
    u64 overflow = 0;
    u64 pointer_notes = 0; ///< GetPointer translations: the page is known, the size is not
    std::array<u64, NumCategories> bytes{};

    PageSet() {
        entries.fill(Entry{EmptyPage, 0, 0, 0});
    }

    static size_t Slot(u64 page) {
        static_assert(SetSize == 65536);
        return static_cast<size_t>((page * 0x9E3779B97F4A7C15ULL) >> 48);
    }

    void Clear() {
        for (u32 i = 0; i < used_count; ++i) {
            entries[used[i]] = Entry{EmptyPage, 0, 0, 0};
        }
        used_count = 0;
        last_page = EmptyPage;
        overflow = 0;
        pointer_notes = 0;
        bytes.fill(0);
    }

    void Insert(u64 page, u32 size, u16 category_bit) {
        const auto update = [&](Entry& entry) {
            entry.bytes = static_cast<u32>((std::min<u64>)(u64{entry.bytes} + size, ~u32{0}));
            entry.category_mask |= category_bit;
        };
        if (page == last_page) {
            update(entries[last_slot]);
            return;
        }
        for (size_t slot = Slot(page);; slot = (slot + 1) & (SetSize - 1)) {
            Entry& entry = entries[slot];
            if (entry.device_page == page) {
                update(entry);
            } else if (entry.device_page == EmptyPage) {
                if (used_count >= SetLimit) {
                    ++overflow;
                    return;
                }
                entry = Entry{page, size, category_bit, 0};
                used[used_count++] = static_cast<u32>(slot);
            } else {
                continue;
            }
            last_page = page;
            last_slot = static_cast<u32>(slot);
            return;
        }
    }

    /// The slot holding page, or SetSize.
    size_t Find(u64 page) const {
        size_t slot = Slot(page);
        for (size_t probes = 0; probes < SetSize; ++probes) {
            if (entries[slot].device_page == page) {
                return slot;
            }
            if (entries[slot].device_page == EmptyPage) {
                return SetSize;
            }
            slot = (slot + 1) & (SetSize - 1);
        }
        return SetSize;
    }
};

inline constexpr u64 WriterBlockBits = 16; ///< The buffer cache's 64 KiB preflush blocks
inline constexpr size_t NumWriterSlots = 16384;
inline constexpr size_t NumRawSamples = 4096;
inline constexpr u64 ArmedBit = u64{1} << 63;

struct WriterSlot {
    std::atomic<u64> block{~u64{0}};
    std::atomic<s64> ns{0};
    std::atomic<u64> stamp{0}; ///< run_ahead_cover: CoverState::write_stamp of the write
};

struct State {
    // Back only.
    std::array<std::unique_ptr<PageSet>, 2> sets;
    u32 write_index = 0;

    // Back -> vi2 once per frame, under the probe-only mutex.
    std::mutex mutex;
    PageSet* published = nullptr;
    u32 published_id = ~0U;
    u32 published_value = 0;
    u64 published_seq = 0;
    u64 unconsumed = 0; ///< Sets replaced before vi2 took them

    /// vi2 -> back: ArmedBit | id << 32 | value of the present fence to swap at. The back
    /// leaves id << 32 | value (unarmed) of the increment it swapped at.
    std::atomic<u64> target{0};
    std::atomic<u64> fence_seq{0};
    std::atomic<s64> fence_ns{0};
    std::atomic<s64> prev_fence_ns{0};

    // RAW: the back stamps preflushed blocks' GPU writers; guest reads of them sample a and b.
    std::array<WriterSlot, NumWriterSlots> writers{};
    std::atomic<u64> reply_seq{0};
    std::atomic<s64> reply_ns{0};
    std::array<std::atomic<s64>, NumRawSamples> raw_a{};
    std::array<std::atomic<s64>, NumRawSamples> raw_b{};
    std::atomic<u64> raw_count{}, raw_pre_reply{}, raw_unfenced{}, raw_older{}, raw_untracked{};

    std::atomic<u64> verify_differences{};
};

inline State& Get() {
    static State state;
    return state;
}

inline s64 NowNs() {
    return Nx2WarProbe::NowNs();
}

inline void NoteVerifyDifference(std::string_view what, u64 page) {
    const u64 count = Get().verify_differences.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 64) {
        LOG_ERROR(HW_GPU, "Nx2Perf run_ahead_ledger: {} (page {:#x}, {} so far)", what,
                  page << PageBits, count);
    }
}

inline constexpr size_t NumCoverCommits = 4096;
inline constexpr size_t NumCoverSlots = 1024;
inline constexpr size_t NumCoverFences = 256;

/// A buffer cache commit that carried stamped writes; number is its index + 1 (0 while unset).
struct CoverCommit {
    std::atomic<u64> number{0};
    std::atomic<u64> stamp{0};   ///< Highest write stamp it downloads
    std::atomic<s64> land_ns{0}; ///< When its bytes reached guest memory, 0 not yet
};

/// A present fence the ledger swapped at (seq) and when its host value rose.
struct CoverFence {
    std::atomic<u64> seq{0};
    std::atomic<u64> key{0}; ///< id << 32 | value
    std::atomic<s64> host_ns{0};
};

struct CoverState {
    std::atomic<u64> write_stamp{0}; ///< Back: one per preflushed GPU write (NoteWriter)
    // Under the buffer cache mutex (commits and pops), read by vi2.
    std::array<CoverCommit, NumCoverCommits> commits{};
    std::atomic<u64> commit_count{0};
    u64 committed_stamp = 0;
    /// Per async download slot with bytes, its commit number (0 none), FIFO like async_buffers.
    std::array<u64, NumCoverSlots> slot_commit{};
    u64 slot_push = 0;
    u64 slot_pop = 0;
    u64 slot_dropped = 0; ///< Oldest slots dropped on a full FIFO; their pops land nothing
    std::array<CoverFence, NumCoverFences> fences{};
    std::atomic<u64> fence_armed{0}; ///< Last seq armed in fences
    // RAW samples at State::raw_a's index: the writer's stamp, frame N's seq, fence N-1's time.
    std::array<std::atomic<u64>, NumRawSamples> raw_stamp{};
    std::array<std::atomic<u64>, NumRawSamples> raw_seq{};
    std::array<std::atomic<s64>, NumRawSamples> raw_prev_fence{};
    std::atomic<u64> verify_differences{};
};

inline CoverState& CoverGet() {
    static CoverState cover;
    return cover;
}

inline void NoteCoverDifference(std::string_view what, u64 a, u64 b) {
    const u64 count =
        CoverGet().verify_differences.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 64) {
        LOG_ERROR(HW_GPU, "Nx2Perf run_ahead_cover: {} ({} vs {}, {} so far)", what, a, b, count);
    }
}

/// CommitAsyncFlushesHigh (buffer cache mutex): records a commit when writes were stamped since
/// the last one. Returns its number, 0 when none.
inline u64 CoverCommitBegin() {
    CoverState& cover = CoverGet();
    const u64 stamp = cover.write_stamp.load(std::memory_order_acquire);
    if (stamp == cover.committed_stamp) {
        return 0;
    }
    cover.committed_stamp = stamp;
    const u64 number = cover.commit_count.load(std::memory_order_relaxed) + 1;
    CoverCommit& commit = cover.commits[(number - 1) % NumCoverCommits];
    commit.number.store(0, std::memory_order_relaxed);
    commit.stamp.store(stamp, std::memory_order_relaxed);
    commit.land_ns.store(0, std::memory_order_relaxed);
    commit.number.store(number, std::memory_order_release);
    cover.commit_count.store(number, std::memory_order_release);
    return number;
}

/// Commit number's bytes are in guest memory now.
inline void CoverLand(u64 number) {
    if (number == 0) {
        return;
    }
    CoverCommit& commit = CoverGet().commits[(number - 1) % NumCoverCommits];
    if (commit.number.load(std::memory_order_acquire) == number) {
        commit.land_ns.store(NowNs(), std::memory_order_release);
    }
}

/// End of CommitAsyncFlushesHigh: without downloads the commit lands now, else at its pop.
inline void CoverCommitEnd(u64 number, bool downloads) {
    if (!downloads) {
        CoverLand(number);
        return;
    }
    CoverState& cover = CoverGet();
    if (cover.slot_push - cover.slot_pop == NumCoverSlots) {
        NoteCoverDifference("download slot FIFO full, a landing is lost", cover.slot_push,
                            cover.slot_pop);
        ++cover.slot_pop;
        ++cover.slot_dropped;
    }
    cover.slot_commit[cover.slot_push++ % NumCoverSlots] = number;
}

/// PopAsyncBuffers (buffer cache mutex): a slot with bytes was written to guest memory.
inline void CoverPopped() {
    CoverState& cover = CoverGet();
    if (cover.slot_dropped != 0) {
        // The dropped slots were the oldest: keep the rest paired with their pops.
        --cover.slot_dropped;
        return;
    }
    if (cover.slot_pop == cover.slot_push) {
        NoteCoverDifference("download slot popped with none queued", cover.slot_pop,
                            cover.slot_push);
        return;
    }
    CoverLand(cover.slot_commit[cover.slot_pop++ % NumCoverSlots]);
}

/// Fence manager: syncpoint id's host value rose to value.
inline void AfterHostIncrement(u32 id, u32 value) {
    CoverState& cover = CoverGet();
    const u64 last = cover.fence_armed.load(std::memory_order_acquire);
    for (u64 seq = last; seq != 0 && seq + 8 > last; --seq) {
        CoverFence& fence = cover.fences[seq % NumCoverFences];
        if (fence.seq.load(std::memory_order_acquire) != seq ||
            fence.host_ns.load(std::memory_order_relaxed) != 0) {
            continue;
        }
        const u64 key = fence.key.load(std::memory_order_relaxed);
        if ((key >> 32) != id || static_cast<s32>(value - static_cast<u32>(key)) < 0) {
            continue;
        }
        s64 expected = 0;
        fence.host_ns.compare_exchange_strong(expected, NowNs(), std::memory_order_acq_rel);
    }
}

/// BackLoop: the back records its reads from here on (switch on only).
inline void StartBackThread() {
    if (!On()) {
        return;
    }
    State& state = Get();
    // Allocated once: a restarted back must not free a set vi2 may still take as published.
    if (!state.sets[0]) {
        state.sets[0] = std::make_unique<PageSet>();
        state.sets[1] = std::make_unique<PageSet>();
    }
    g_is_back = true;
}

inline void NoteSlow(u64 device_addr, u64 size) {
    State& state = Get();
    PageSet& set = *state.sets[state.write_index];
    const u8 category = g_ledger_category;
    const u16 bit = static_cast<u16>(1U << category);
    if (size == 0) {
        ++set.pointer_notes;
        set.Insert(device_addr >> PageBits, 0, bit);
        return;
    }
    set.bytes[category] += size;
    const u64 end = device_addr + size;
    for (u64 page = device_addr >> PageBits; page <= (end - 1) >> PageBits; ++page) {
        const u64 begin = (std::max)(device_addr, page << PageBits);
        const u64 stop = (std::min)(end, (page + 1) << PageBits);
        set.Insert(page, static_cast<u32>(stop - begin), bit);
    }
}

/// Back: device memory [device_addr, device_addr + size) is read; size 0 when unknown (GetPointer).
inline void Note(u64 device_addr, u64 size) {
    if (g_is_back) [[unlikely]] {
        NoteSlow(device_addr, size);
    }
}

/// Back, before it raises syncpoint id's guest value to value: at the armed present fence the
/// frame's set goes to vi2 and the back records into the other one.
inline void BeforeBackIncrement(u32 id, u32 value) {
    State& state = Get();
    u64 target = state.target.load(std::memory_order_acquire);
    if ((target & ArmedBit) == 0 || ((target >> 32) & 0x7fffffff) != id ||
        static_cast<s32>(value - static_cast<u32>(target)) < 0) {
        return;
    }
    if (!state.target.compare_exchange_strong(target, (u64{id} << 32) | value,
                                              std::memory_order_acq_rel)) {
        return;
    }
    PageSet* const filled = state.sets[state.write_index].get();
    if (Nx2Perf::Verify()) {
        for (u32 i = 0; i < filled->used_count; ++i) {
            const u64 page = filled->entries[filled->used[i]].device_page;
            if (filled->Find(page) != filled->used[i]) {
                NoteVerifyDifference("a recorded page is not found in its set", page);
            }
        }
    }
    state.prev_fence_ns.store(state.fence_ns.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
    state.fence_ns.store(NowNs(), std::memory_order_relaxed);
    const u64 seq = state.fence_seq.load(std::memory_order_relaxed) + 1;
    if (CoverOn()) {
        CoverState& cover = CoverGet();
        CoverFence& fence = cover.fences[seq % NumCoverFences];
        fence.seq.store(0, std::memory_order_relaxed);
        fence.key.store((u64{id} << 32) | value, std::memory_order_relaxed);
        fence.host_ns.store(0, std::memory_order_relaxed);
        fence.seq.store(seq, std::memory_order_release);
        cover.fence_armed.store(seq, std::memory_order_release);
    }
    {
        std::scoped_lock lock{state.mutex};
        state.unconsumed += state.published != nullptr ? 1 : 0;
        state.published = filled;
        state.published_id = id;
        state.published_value = value;
        state.published_seq = seq;
    }
    state.fence_seq.store(seq, std::memory_order_release);
    state.write_index ^= 1;
    state.sets[state.write_index]->Clear();
}

inline size_t WriterSlotOf(u64 block) {
    static_assert(NumWriterSlots == 16384);
    return static_cast<size_t>((block * 0x9E3779B97F4A7C15ULL) >> 50);
}

/// Back: a GPU write of the preflushed range [begin, end) is recorded now.
inline void NoteWriter(u64 begin, u64 end) {
    State& state = Get();
    const s64 now = NowNs();
    // run_ahead_cover: one stamp per write, the order the buffer cache commits them in.
    const u64 stamp =
        CoverOn() ? CoverGet().write_stamp.fetch_add(1, std::memory_order_acq_rel) + 1 : 0;
    for (u64 block = begin >> WriterBlockBits; block <= (end - 1) >> WriterBlockBits; ++block) {
        WriterSlot& slot = state.writers[WriterSlotOf(block)];
        slot.block.store(~u64{0}, std::memory_order_relaxed);
        slot.ns.store(now, std::memory_order_relaxed);
        slot.stamp.store(stamp, std::memory_order_relaxed);
        slot.block.store(block, std::memory_order_release);
    }
}

/// A guest read of [begin, end) served from preflushed data.
inline void NoteGuestRead(u64 begin, u64 end) {
    if (g_is_back || end <= begin) {
        return;
    }
    State& state = Get();
    s64 writer = 0;
    u64 writer_stamp = 0;
    bool found = false;
    for (u64 block = begin >> WriterBlockBits; block <= (end - 1) >> WriterBlockBits; ++block) {
        const WriterSlot& slot = state.writers[WriterSlotOf(block)];
        if (slot.block.load(std::memory_order_acquire) != block) {
            continue;
        }
        const s64 ns = slot.ns.load(std::memory_order_relaxed);
        const u64 stamp = slot.stamp.load(std::memory_order_relaxed);
        if (slot.block.load(std::memory_order_relaxed) == block) {
            writer = (std::max)(writer, ns);
            writer_stamp = (std::max)(writer_stamp, stamp);
            found = true;
        }
    }
    if (!found) {
        state.raw_untracked.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 seq = state.fence_seq.load(std::memory_order_acquire);
    const s64 fence = state.fence_ns.load(std::memory_order_relaxed);
    const s64 prev_fence = state.prev_fence_ns.load(std::memory_order_relaxed);
    if (writer > fence) {
        state.raw_unfenced.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (writer <= prev_fence) {
        state.raw_older.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (state.reply_seq.load(std::memory_order_acquire) != seq) {
        state.raw_pre_reply.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const s64 a = NowNs() - state.reply_ns.load(std::memory_order_relaxed);
    const size_t index = state.raw_count.fetch_add(1, std::memory_order_relaxed) % NumRawSamples;
    state.raw_a[index].store(a, std::memory_order_relaxed);
    state.raw_b[index].store(fence - writer, std::memory_order_relaxed);
    if (CoverOn()) {
        CoverState& cover = CoverGet();
        cover.raw_stamp[index].store(writer_stamp, std::memory_order_relaxed);
        cover.raw_seq[index].store(seq, std::memory_order_relaxed);
        cover.raw_prev_fence[index].store(prev_fence, std::memory_order_relaxed);
    }
}

/// A 64-bit hash of one 4 KiB page (xxh64-style rounds over eight lanes).
inline u64 HashPage(const u8* data) {
    constexpr u64 P1 = 0x9E3779B185EBCA87ULL;
    constexpr u64 P2 = 0xC2B2AE3D27D4EB4FULL;
    std::array<u64, 8> acc{P1, P2, P1 + P2, P2 - P1, 1, 2, 3, 4};
    for (size_t offset = 0; offset < PageSize; offset += 64) {
        for (size_t lane = 0; lane < 8; ++lane) {
            u64 word;
            std::memcpy(&word, data + offset + lane * 8, sizeof(word));
            acc[lane] = std::rotl(acc[lane] + word * P2, 31) * P1;
        }
    }
    u64 hash = P1;
    for (size_t lane = 0; lane < 8; ++lane) {
        hash = (hash ^ std::rotl(acc[lane], static_cast<int>(lane * 8 + 1))) * P2;
    }
    return hash ^ (hash >> 29);
}

/// vi2's side: takes frame N's set at its reply, compares at the entry of present N+1, logs.
class Vi2Window {
public:
    static constexpr size_t MaxHashed = 8192;
    static constexpr size_t CopyPages = 1024;
    static constexpr size_t Chunk = 64;

    Vi2Window() : copy{std::make_unique<u8[]>(CopyPages * PageSize)} {
        if (CoverOn()) {
            cover = std::make_unique<Cover>();
        }
    }

    /// Present of fence (id, value), before its wait: compares the pages taken at the last
    /// reply, then arms the back's swap for this fence. reached: the fence already passed.
    template <typename GetPointer, typename GpuWritten>
    void Enter(u32 id, u32 value, bool reached, GetPointer&& get_pointer,
               GpuWritten&& gpu_written) {
        if (valid) {
            Compare(get_pointer, gpu_written);
        }
        State& state = Get();
        const u64 want = ArmedBit | (u64{id} << 32) | value;
        u64 target = state.target.load(std::memory_order_acquire);
        while (target != want) {
            if ((target & ArmedBit) == 0) {
                // The back swapped for this fence already (it ran ahead of the present).
                const bool swapped = target != 0 && ((target >> 32) & 0x7fffffff) == id &&
                                     static_cast<s32>(static_cast<u32>(target) - value) >= 0;
                if (swapped || reached) {
                    break;
                }
            }
            if (state.target.compare_exchange_weak(target, want, std::memory_order_acq_rel)) {
                break;
            }
        }
    }

    /// After the wait for (id, value), before the reply: frame N's set and its pages' hashes.
    template <typename GetPointer>
    void Take(u32 id, u32 value, GetPointer&& get_pointer) {
        State& state = Get();
        const s64 start = NowNs();
        valid = false;
        if (cover) {
            cover->classified = false;
        }
        if (id == last_id && value - last_value - 1 < 64) {
            // The predicted next fence, for a back that passes it before the next present
            // enters. The guest waits here, so the back cannot reach it meanwhile.
            state.target.store(ArmedBit | (u64{id} << 32) | (value + (value - last_value)),
                               std::memory_order_release);
        }
        last_id = id;
        last_value = value;
        {
            std::scoped_lock lock{state.mutex};
            sum.unconsumed = state.unconsumed;
            if (state.published == nullptr || state.published_id != id ||
                state.published_value != value) {
                ++sum.unmatched;
                return;
            }
            const PageSet& set = *state.published;
            count = (std::min<size_t>)(set.used_count, MaxHashed);
            sum.capped += set.used_count - count;
            sum.pages += set.used_count;
            sum.overflow += set.overflow;
            sum.pointer_notes += set.pointer_notes;
            for (size_t c = 0; c < NumCategories; ++c) {
                sum.bytes[c] += set.bytes[c];
            }
            for (u32 i = 0; i < set.used_count; ++i) {
                const Entry& entry = set.entries[set.used[i]];
                AddByCategory(sum.category_pages, entry.category_mask, 1);
                sum.pointer_only += entry.bytes == 0 ? 1 : 0;
                if (i < count) {
                    pages[i] = entry.device_page;
                    masks[i] = entry.category_mask;
                }
            }
            frame_seq = state.published_seq;
            state.published = nullptr;
        }
        for (size_t i = 0; i < count; ++i) {
            const u8* const host = get_pointer(pages[i] << PageBits);
            if (host == nullptr) {
                hashes[i] = 0;
                continue;
            }
            if (i < CopyPages) {
                u8* const saved = copy.get() + i * PageSize;
                std::memcpy(saved, host, PageSize);
                hashes[i] = HashPage(saved);
                // Verify: the saved bytes are still the page's bytes (no write raced the copy).
                if (Nx2Perf::Verify() && HashPage(host) != hashes[i]) {
                    NoteVerifyDifference("page changed while vi2 saved it", pages[i]);
                }
            } else {
                hashes[i] = HashPage(host);
            }
        }
        valid = true;
        ++sum.frames;
        sum.hash_ns += NowNs() - start;
    }

    /// Right before the reply: the guest may read frame N's results from here on.
    void Reply() {
        if (!valid) {
            return;
        }
        State& state = Get();
        state.reply_ns.store(NowNs(), std::memory_order_relaxed);
        state.reply_seq.store(frame_seq, std::memory_order_release);
    }

    /// run_ahead_cover, right before the reply: the tracker state of each taken page.
    /// cpu_of(device address) is its guest address (0 none); classify(cpu pages, classes) fills
    /// TrackerClass values and returns whether core memory keeps the arm set.
    template <typename CpuOf, typename Classify>
    void CoverClassify(CpuOf&& cpu_of, Classify&& classify) {
        if (!cover || !valid) {
            return;
        }
        Cover& c = *cover;
        const s64 start = NowNs();
        for (size_t i = 0; i < count; ++i) {
            c.cpu_pages[i] = static_cast<u64>(cpu_of(pages[i] << PageBits)) >> PageBits;
            c.classes[i] = static_cast<u8>(TrackerClass::Untracked);
        }
        c.arm_set_known = classify(std::span<const u64>(c.cpu_pages.data(), count),
                                   std::span<u8>(c.classes.data(), count));
        for (size_t i = 0; i < count; ++i) {
            if (c.cpu_pages[i] == 0) {
                c.classes[i] = static_cast<u8>(TrackerClass::NoCpu);
            }
        }
        c.classified = true;
        c.sum.ns += NowNs() - start;
    }

    /// Once a second with the ledger line: the run_ahead_cover line, empty when off.
    std::string CoverReport() {
        if (!cover) {
            return {};
        }
        Cover& c = *cover;
        CoverSums& s = c.sum;
        State& state = Get();
        CoverState& shared = CoverGet();
        // RAW samples one report behind, so their writers have landed.
        const u64 total = state.raw_count.load(std::memory_order_relaxed);
        u64 from = c.raw_from;
        const u64 to = c.raw_to;
        if (total > NumRawSamples && from < total - NumRawSamples) {
            from = total - NumRawSamples;
        }
        size_t samples = 0, land_n = 0, fence_n = 0;
        for (u64 n = from; n < to; ++n) {
            const size_t index = static_cast<size_t>(n % NumRawSamples);
            ++samples;
            const s64 a = state.raw_a[index].load(std::memory_order_relaxed);
            const s64 base = shared.raw_prev_fence[index].load(std::memory_order_relaxed) + a;
            if (const s64 land = LandNs(shared.raw_stamp[index].load(std::memory_order_relaxed));
                land != 0) {
                c.land_stall[land_n++] = (std::max<s64>)(0, land - base);
            }
            if (const s64 host = FenceHostNs(shared.raw_seq[index].load(std::memory_order_relaxed));
                host != 0) {
                c.fence_stall[fence_n++] = (std::max<s64>)(0, host - base);
            }
        }
        c.raw_from = (std::max)(to, from);
        c.raw_to = total;
        const u64 commits = shared.commit_count.load(std::memory_order_relaxed);
        const u64 new_commits = commits - c.commits_reported;
        c.commits_reported = commits;

        const auto pct = [](std::array<s64, NumRawSamples>& values, size_t n,
                            size_t percent) -> double {
            if (n == 0) {
                return 0.0;
            }
            const size_t index = (percent * n + 99) / 100 - 1;
            std::nth_element(values.begin(), values.begin() + index, values.begin() + n);
            return static_cast<double>(values[index]) / 1e6;
        };
        const double frames = s.frames == 0 ? 1.0 : static_cast<double>(s.frames);
        const auto per_k = [&s](const std::array<u64, NumK>& values) {
            std::string out;
            for (size_t k = 0; k < NumK; ++k) {
                const double n = s.k_frames[k] == 0 ? 1.0 : static_cast<double>(s.k_frames[k]);
                out += fmt::format("{}{:.1f}", k == 0 ? "" : "/", static_cast<double>(values[k]) / n);
            }
            return out;
        };
        std::string classes;
        for (size_t g = 0; g <= NumGroups; ++g) {
            classes += fmt::format(" {}=", g == NumGroups ? std::string_view{"all"} : group_names[g]);
            for (size_t t = 0; t < NumTrackerClasses; ++t) {
                u64 value = 0;
                if (g == NumGroups) {
                    for (size_t h = 0; h < NumGroups; ++h) {
                        value += s.classes[t][h];
                    }
                } else {
                    value = s.classes[t][g];
                }
                classes += fmt::format("{}{:.2f}", t == 0 ? "" : "/",
                                       static_cast<double>(value) / frames);
            }
        }
        std::string line = fmt::format(
            "Nx2Perf run_ahead_cover: frames={} K=1/2/4/8/16 misses={} changed_misses={} "
            "gpu_changed_misses={} union_pages={} k_frames={}/{}/{}/{}/{} changed={:.2f} "
            "gpu_changed={:.2f} unclassified={} arm_set_known={} "
            "changed_class(tracked/armset/untracked/nocpu per frame):{} raw_samples={} "
            "resolved_land={} resolved_fence={} land_stall_ms p50={:.2f} p90={:.2f} "
            "fenceN_stall_ms p50={:.2f} p90={:.2f} preflush_commits_per_frame={:.2f} "
            "cover_ms={:.3f} verify_differences={}",
            s.frames, per_k(s.misses), per_k(s.changed_misses), per_k(s.gpu_changed_misses),
            per_k(s.union_pages), s.k_frames[0], s.k_frames[1], s.k_frames[2], s.k_frames[3],
            s.k_frames[4], static_cast<double>(s.changed) / frames,
            static_cast<double>(s.gpu_changed) / frames, s.unclassified, c.arm_set_known ? 1 : 0,
            classes, samples, land_n, fence_n, pct(c.land_stall, land_n, 50),
            pct(c.land_stall, land_n, 90), pct(c.fence_stall, fence_n, 50),
            pct(c.fence_stall, fence_n, 90), static_cast<double>(new_commits) / frames,
            static_cast<double>(s.ns) / 1e6 / frames,
            shared.verify_differences.load(std::memory_order_relaxed));
        s = CoverSums{};
        return line;
    }

    /// Once a second, the Nx2Ledger line; empty otherwise.
    std::string Report() {
        const s64 now = NowNs();
        if (last_report == 0) {
            last_report = now;
            return {};
        }
        if (now - last_report < 1'000'000'000) {
            return {};
        }
        last_report = now;
        State& state = Get();
        const u64 raw_total = state.raw_count.load(std::memory_order_relaxed);
        const u64 raw_reads = raw_total - raw_reported;
        const size_t raw_new = static_cast<size_t>((std::min<u64>)(raw_reads, NumRawSamples));
        for (size_t i = 0; i < raw_new; ++i) {
            const size_t index = static_cast<size_t>((raw_total - raw_new + i) % NumRawSamples);
            raw_a[i] = state.raw_a[index].load(std::memory_order_relaxed);
            raw_b[i] = state.raw_b[index].load(std::memory_order_relaxed);
            raw_wait[i] = raw_b[i] - raw_a[i];
        }
        raw_reported = raw_total;
        const double frames = sum.frames == 0 ? 1.0 : static_cast<double>(sum.frames);
        const auto per_frame = [frames](u64 value) { return static_cast<double>(value) / frames; };
        std::string categories;
        for (size_t c = 0; c < NumCategories; ++c) {
            if (sum.category_pages[c] == 0 && sum.bytes[c] == 0) {
                continue;
            }
            categories += fmt::format(" {}={:.0f}p/{:.0f}K/{:.1f}c/{:.1f}cK", category_names[c],
                                      per_frame(sum.category_pages[c]),
                                      per_frame(sum.bytes[c]) / 1024.0,
                                      per_frame(sum.changed_pages[c]),
                                      per_frame(sum.changed_bytes[c]) / 1024.0);
        }
        const auto pct = [](std::array<s64, NumRawSamples>& values, size_t n,
                            size_t percent) -> double {
            if (n == 0) {
                return 0.0;
            }
            const size_t index = (percent * n + 99) / 100 - 1;
            std::nth_element(values.begin(), values.begin() + index, values.begin() + n);
            return static_cast<double>(values[index]) / 1e6;
        };
        const auto take = [](std::atomic<u64>& counter) {
            return counter.exchange(0, std::memory_order_relaxed);
        };
        u64 total_bytes = 0;
        for (const u64 bytes : sum.bytes) {
            total_bytes += bytes;
        }
        std::string line = fmt::format(
            "Nx2Ledger: frames={} pages={:.0f} read={:.0f}K ptr_notes={:.0f} ptr_only={:.0f} "
            "changed={:.1f} "
            "changed_copied={:.1f} gpu_written={:.1f} misses={:.1f} changed_misses={:.1f} "
            "overflow={:.1f} capped={:.1f} unmatched={} unconsumed={} hash_ms={:.3f} "
            "compare_ms={:.3f} cat(pages/KiB read/changed pages/changed KiB, first {} pages):{} "
            "raw_reads={:.1f} raw_a_ms p50={:.2f} p90={:.2f} raw_b_ms p50={:.2f} p90={:.2f} "
            "raw_b_minus_a_ms p50={:.2f} p90={:.2f} raw_pre_reply={} raw_unfenced={} "
            "raw_older={} raw_untracked={} verify_differences={}",
            sum.frames, per_frame(sum.pages), per_frame(total_bytes) / 1024.0,
            per_frame(sum.pointer_notes), per_frame(sum.pointer_only), per_frame(sum.changed),
            per_frame(sum.changed_copied),
            per_frame(sum.gpu_written), per_frame(sum.misses), per_frame(sum.changed_misses),
            per_frame(sum.overflow), per_frame(sum.capped), sum.unmatched,
            sum.unconsumed - unconsumed_reported,
            static_cast<double>(sum.hash_ns) / 1e6 / frames,
            static_cast<double>(sum.compare_ns) / 1e6 / frames, CopyPages, categories,
            per_frame(raw_reads), pct(raw_a, raw_new, 50), pct(raw_a, raw_new, 90),
            pct(raw_b, raw_new, 50), pct(raw_b, raw_new, 90), pct(raw_wait, raw_new, 50),
            pct(raw_wait, raw_new, 90), take(state.raw_pre_reply), take(state.raw_unfenced),
            take(state.raw_older), take(state.raw_untracked),
            state.verify_differences.load(std::memory_order_relaxed));
        unconsumed_reported = sum.unconsumed;
        sum = Sums{};
        sum.unconsumed = unconsumed_reported;
        return line;
    }

private:
    struct Sums {
        u64 frames = 0;
        u64 pages = 0;
        u64 pointer_notes = 0;
        u64 pointer_only = 0; ///< Pages read only through GetPointer: no size known
        u64 overflow = 0;
        u64 capped = 0;
        u64 unmatched = 0;
        u64 unconsumed = 0; ///< The State's running total
        u64 changed = 0;
        u64 changed_copied = 0;
        u64 gpu_written = 0;
        u64 misses = 0;
        u64 changed_misses = 0;
        s64 hash_ns = 0;
        s64 compare_ns = 0;
        std::array<u64, NumCategories> category_pages{};
        std::array<u64, NumCategories> bytes{};
        std::array<u64, NumCategories> changed_pages{};
        std::array<u64, NumCategories> changed_bytes{};
    };

    // run_ahead_cover state, allocated once with the window.
    static constexpr size_t CoverRing = 16;
    static constexpr size_t NumK = 5;
    static constexpr std::array<size_t, NumK> cover_k{1, 2, 4, 8, 16};
    static constexpr size_t NumGroups = 6;
    static constexpr std::array<std::string_view, NumGroups> group_names{
        "bank", "ufast", "ucached", "storage", "desc", "other"};

    struct CoverSums {
        u64 frames = 0;
        std::array<u64, NumK> k_frames{};
        std::array<u64, NumK> misses{};
        std::array<u64, NumK> changed_misses{};
        std::array<u64, NumK> gpu_changed_misses{};
        std::array<u64, NumK> union_pages{};
        u64 changed = 0;
        u64 gpu_changed = 0;
        u64 unclassified = 0; ///< Changed pages of a frame vi2 did not classify
        std::array<std::array<u64, NumGroups>, NumTrackerClasses> classes{};
        s64 ns = 0;
    };

    struct Cover {
        std::array<std::array<u64, MaxHashed>, CoverRing> ring{}; ///< Sorted pages per frame
        std::array<size_t, CoverRing> ring_size{};
        size_t ring_head = 0; ///< Next slot to write
        size_t ring_count = 0;
        std::array<u64, MaxHashed * CoverRing> merge_a{};
        std::array<u64, MaxHashed * CoverRing> merge_b{};
        std::array<u64, MaxHashed> current{}; ///< page << 2 | flags, sorted
        std::array<u8, MaxHashed> flags{};    ///< 1 changed (not GPU-written), 2 GPU-written
        std::array<u8, MaxHashed> classes{};
        std::array<u64, MaxHashed> cpu_pages{};
        bool classified = false;
        bool arm_set_known = false;
        u64 raw_from = 0;
        u64 raw_to = 0;
        u64 commits_reported = 0;
        std::array<s64, NumRawSamples> land_stall{};
        std::array<s64, NumRawSamples> fence_stall{};
        CoverSums sum{};
    };

    static size_t GroupOf(u16 mask) {
        const auto has = [mask](Category category) {
            return ((mask >> static_cast<size_t>(category)) & 1) != 0;
        };
        return has(Category::BankCompare)     ? 0
               : has(Category::UniformFast)   ? 1
               : has(Category::UniformCached) ? 2
               : has(Category::Storage)       ? 3
               : has(Category::Descriptor)    ? 4
                                              : 5;
    }

    /// When the first commit holding write stamp `stamp` landed; 0 unknown or not landed.
    static s64 LandNs(u64 stamp) {
        if (stamp == 0) {
            return 0;
        }
        CoverState& shared = CoverGet();
        const u64 count = shared.commit_count.load(std::memory_order_acquire);
        const u64 oldest = count > NumCoverCommits ? count - NumCoverCommits + 1 : 1;
        u64 lo = oldest;
        u64 hi = count + 1;
        while (lo < hi) {
            const u64 mid = lo + (hi - lo) / 2;
            if (shared.commits[(mid - 1) % NumCoverCommits].stamp.load(
                    std::memory_order_relaxed) >= stamp) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        if (lo > count || (lo == oldest && oldest > 1)) {
            return 0;
        }
        const CoverCommit& commit = shared.commits[(lo - 1) % NumCoverCommits];
        const s64 land = commit.land_ns.load(std::memory_order_acquire);
        return commit.number.load(std::memory_order_acquire) == lo ? land : 0;
    }

    /// When fence seq's host value rose; 0 unknown.
    static s64 FenceHostNs(u64 seq) {
        const CoverFence& fence = CoverGet().fences[seq % NumCoverFences];
        const s64 host = fence.host_ns.load(std::memory_order_acquire);
        return fence.seq.load(std::memory_order_acquire) == seq ? host : 0;
    }

    /// Compare's tail: K-frame unions, classes of the changed pages, then this frame's set
    /// enters the ring. old_*: the ledger's own K=1 answer (old_valid: it had one).
    void CoverCompare(bool old_valid, u64 old_misses, u64 old_changed_misses) {
        Cover& c = *cover;
        CoverSums& s = c.sum;
        const s64 start = NowNs();
        for (size_t i = 0; i < count; ++i) {
            const u8 f = c.flags[i];
            s.gpu_changed += (f >> 1) & 1;
            if ((f & 1) == 0) {
                continue;
            }
            ++s.changed;
            if (!c.classified) {
                ++s.unclassified;
            } else {
                ++s.classes[c.classes[i]][GroupOf(masks[i])];
            }
        }
        for (size_t i = 0; i < count; ++i) {
            c.current[i] = (pages[i] << 2) | c.flags[i];
        }
        std::sort(c.current.begin(), c.current.begin() + count);
        const u64* union_data = nullptr;
        size_t union_size = 0;
        u64* out = c.merge_a.data();
        u64* spare = c.merge_b.data();
        size_t k = 0;
        for (size_t j = 1; j <= c.ring_count && k < NumK; ++j) {
            const size_t slot = (c.ring_head + CoverRing - j) % CoverRing;
            const u64* const set = c.ring[slot].data();
            const size_t set_size = c.ring_size[slot];
            if (j == 1) {
                union_data = set;
                union_size = set_size;
            } else {
                u64* const end =
                    std::set_union(union_data, union_data + union_size, set, set + set_size, out);
                union_size = static_cast<size_t>(end - out);
                union_data = out;
                std::swap(out, spare);
            }
            if (j != cover_k[k]) {
                continue;
            }
            u64 misses = 0, changed = 0, gpu = 0;
            size_t u = 0;
            for (size_t i = 0; i < count; ++i) {
                const u64 page = c.current[i] >> 2;
                while (u < union_size && union_data[u] < page) {
                    ++u;
                }
                if (u == union_size || union_data[u] != page) {
                    ++misses;
                    changed += c.current[i] & 1;
                    gpu += (c.current[i] >> 1) & 1;
                }
            }
            if (k == 0 && Nx2Perf::Verify()) {
                if (!old_valid) {
                    NoteCoverDifference("K=1 union without a ledger previous frame", misses, 0);
                } else if (misses != old_misses || changed != old_changed_misses) {
                    NoteCoverDifference("K=1 misses differ from the ledger's", misses,
                                        old_misses);
                    misses = old_misses;
                    changed = old_changed_misses;
                }
            }
            s.misses[k] += misses;
            s.changed_misses[k] += changed;
            s.gpu_changed_misses[k] += gpu;
            s.union_pages[k] += union_size;
            ++s.k_frames[k];
            ++k;
        }
        if (k == 0 && old_valid && Nx2Perf::Verify()) {
            NoteCoverDifference("the ledger had a previous frame, the ring none", old_misses, 0);
        }
        auto& next = c.ring[c.ring_head];
        for (size_t i = 0; i < count; ++i) {
            next[i] = c.current[i] >> 2;
        }
        c.ring_size[c.ring_head] = count;
        c.ring_head = (c.ring_head + 1) % CoverRing;
        c.ring_count = (std::min)(c.ring_count + 1, CoverRing);
        ++s.frames;
        s.ns += NowNs() - start;
    }

    static void AddByCategory(std::array<u64, NumCategories>& counts, u16 mask, u64 amount) {
        for (size_t c = 0; c < NumCategories; ++c) {
            if ((mask >> c) & 1) {
                counts[c] += amount;
            }
        }
    }

    /// Entry of present N+1: frame N's pages the guest changed since the reply.
    template <typename GetPointer, typename GpuWritten>
    void Compare(GetPointer& get_pointer, GpuWritten& gpu_written) {
        const s64 start = NowNs();
        valid = false;
        for (size_t i = 0; i < count; ++i) {
            bool changed = false;
            u64 changed_bytes = 0;
            if (const u8* const host = get_pointer(pages[i] << PageBits); host != nullptr) {
                if (i < CopyPages) {
                    const u8* const saved = copy.get() + i * PageSize;
                    for (size_t offset = 0; offset < PageSize; offset += Chunk) {
                        if (std::memcmp(saved + offset, host + offset, Chunk) != 0) {
                            changed_bytes += Chunk;
                        }
                    }
                    changed = changed_bytes != 0;
                } else {
                    changed = HashPage(host) != hashes[i];
                }
            }
            bool gpu = false;
            if (changed && gpu_written(pages[i] << PageBits)) {
                ++sum.gpu_written;
                changed = false;
                gpu = true;
            }
            if (changed) {
                ++sum.changed;
                sum.changed_copied += i < CopyPages ? 1 : 0;
                AddByCategory(sum.changed_pages, masks[i], 1);
                AddByCategory(sum.changed_bytes, masks[i], changed_bytes);
            }
            if (cover) [[unlikely]] {
                cover->flags[i] = static_cast<u8>((changed ? 1 : 0) | (gpu ? 2 : 0));
            }
            sorted[i] = (pages[i] << 1) | (changed ? 1 : 0);
        }
        std::sort(sorted.begin(), sorted.begin() + count);
        const u64 misses_before = sum.misses;
        const u64 changed_misses_before = sum.changed_misses;
        const bool had_previous = have_previous;
        if (have_previous) {
            // Pages of frame N that frame N-1 did not read: an arming set from the last frame
            // would not have covered them.
            for (size_t i = 0; i < count; ++i) {
                if (!std::binary_search(previous.begin(), previous.begin() + previous_count,
                                        sorted[i] >> 1)) {
                    ++sum.misses;
                    sum.changed_misses += sorted[i] & 1;
                }
            }
        }
        for (size_t i = 0; i < count; ++i) {
            previous[i] = sorted[i] >> 1;
        }
        previous_count = count;
        have_previous = true;
        sum.compare_ns += NowNs() - start;
        if (cover) [[unlikely]] {
            CoverCompare(had_previous, sum.misses - misses_before,
                         sum.changed_misses - changed_misses_before);
        }
    }

    std::array<u64, MaxHashed> pages{};
    std::array<u16, MaxHashed> masks{};
    std::array<u64, MaxHashed> hashes{};
    std::array<u64, MaxHashed> sorted{};
    std::array<u64, MaxHashed> previous{};
    std::unique_ptr<u8[]> copy;
    size_t count = 0;
    size_t previous_count = 0;
    bool have_previous = false;
    bool valid = false;
    u64 frame_seq = 0;
    u32 last_id = ~0U;
    u32 last_value = 0;
    s64 last_report = 0;
    u64 raw_reported = 0;
    u64 unconsumed_reported = 0;
    std::array<s64, NumRawSamples> raw_a{};
    std::array<s64, NumRawSamples> raw_b{};
    std::array<s64, NumRawSamples> raw_wait{};
    Sums sum{};
    std::unique_ptr<Cover> cover; ///< run_ahead_cover only
};

} // namespace VideoCommon::Nx2Ledger

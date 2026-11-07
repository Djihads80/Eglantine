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
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_perf.h"

#ifdef _MSC_VER
#include <intrin.h>
#define NX2_RETURN_ADDRESS() _ReturnAddress()
#else
#define NX2_RETURN_ADDRESS() __builtin_return_address(0)
#endif

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

// Nx2Perf run_ahead_raw_probe (Nx2Raw at the end of this file): hooks the ledger and the cow
// probe call when it is on.
namespace VideoCommon::Nx2Raw {
// A reply page's state at the reply (ClassifyReply), then a gap's at the epoch's close.
inline constexpr u8 InfoTypeMask = 3; ///< 0 Memory, 1 RasterizerCached, 2 DebugMemory, 3 none
inline constexpr u8 InfoArmed = 4;    ///< In the cow arm set
inline constexpr u8 InfoMulti = 8;    ///< The physical page has several device mappings
inline constexpr u8 GapNewMap = 16;   ///< A device mapping made after the arm covers it
inline constexpr u8 GapHostWritten = 32; ///< Written through device memory hooks this epoch
inline constexpr u8 InfoValid = 64;

inline bool On();
inline void GuestRead(u64 begin, u64 end, u64 addr, u64 size);
inline void Writer(u64 begin, u64 end, u64 stamp);
inline void Landed();
inline void BackIncrement(u32 id, u32 value);
inline void Begin(u32 id, u32 value, u32 gen);
inline void Open();
inline void NoteGap(u64 phys, u64 device_page, u8 reply_info, u32 gen);
inline void FrameClosed();
inline std::string Report(size_t frames, u64 unarmed_changed,
                          std::span<const std::pair<u64, uintptr_t>> sites);
} // namespace VideoCommon::Nx2Raw

// Nx2Perf run_ahead_snapshot_probe (Nx2Snap at the end of this file): hooks the cow probe calls
// when it is on.
namespace VideoCommon::Nx2Snap {
inline bool On();
/// The front is in Maxwell3D::FrontDmaRead (macro DMA reads), not a list fetch.
inline thread_local bool t_front_macro = false;
/// The call site of the read Nx2Cow::ReadSlow is classifying.
inline thread_local const void* t_site = nullptr;
inline void NotePreImage(u32 pool_slot, bool tracked);
inline void Hazard(u64 phys, u32 pool_slot, const u8* pre, u64 bytes, size_t role,
                   size_t category, u32 touch_bits, u32 gen, u64 device_addr, u64 len);
/// run_ahead_overlay_budget (with the snapshot probe).
inline bool BudgetOn();
/// The read Nx2Cow::ReadSlow is classifying is a GetSpan over several pages.
inline thread_local bool t_span = false;
inline void HostWritePages(u64 device_addr, u64 size);

/// run_ahead_own_write_probe (with the overlay budget).
inline bool OwnOn();
/// Set once the own-write tables exist; every hook site tests it first.
inline std::atomic<bool> g_own{false};
/// The calling thread writes guest memory as the emulated GPU (fencing thread, split back,
/// OwnScope sites).
inline thread_local bool t_gpu_own = false;
inline void OwnHostWrite(u64 device_addr, u64 size, const void* src, const void* site);
inline void OwnPointerWrite(const void* host, u64 size, const void* src);

/// A host write site that stores GPU results (downloads): its writes count as GPU-own.
class OwnScope {
public:
    OwnScope() : on{g_own.load(std::memory_order_relaxed)} {
        if (on) [[unlikely]] {
            previous = t_gpu_own;
            t_gpu_own = true;
        }
    }
    ~OwnScope() {
        if (on) [[unlikely]] {
            t_gpu_own = previous;
        }
    }
    OwnScope(const OwnScope&) = delete;
    OwnScope& operator=(const OwnScope&) = delete;

private:
    bool on;
    bool previous = false;
};

/// A raw store of GPU results into guest memory (host pointer) that bypasses Nx2Cow::HostWrite.
inline void NoteOwnPointer(const void* host, u64 size, const void* src) {
    if (g_own.load(std::memory_order_relaxed)) [[unlikely]] {
        OwnPointerWrite(host, size, src);
    }
}
} // namespace VideoCommon::Nx2Snap

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

/// Write stamps and commit landings: run_ahead_cover or run_ahead_raw_probe.
inline bool LandOn() {
    static const bool on = CoverOn() || Nx2Raw::On();
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

/// Read hooks this thread runs: NoteLedger (the back, run_ahead_ledger) and NoteCow (the front or
/// back, run_ahead_cow_probe). 0 keeps every Note at one not-taken branch.
inline constexpr u8 NoteLedger = 1;
inline constexpr u8 NoteCow = 2;
inline thread_local u8 g_note = 0;

enum class NoteKind : u8 {
    Sized,   ///< [address, address + size) is read
    Pointer, ///< GetPointer: the page is known, the size is not
    Span,    ///< GetSpan: the caller reads or writes through the span
};

/// run_ahead_cow_probe (Nx2Cow below).
inline void NoteCowRead(u64 device_addr, u64 size, NoteKind kind, const void* site);
inline void NoteCowStartBack();

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
        if (Nx2Raw::On()) [[unlikely]] {
            Nx2Raw::Landed();
        }
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
    g_note |= NoteLedger;
    NoteCowStartBack();
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

inline void NoteAny(u64 device_addr, u64 size, NoteKind kind, const void* site) {
    if ((g_note & NoteLedger) != 0) {
        NoteSlow(device_addr, size);
    }
    if ((g_note & NoteCow) != 0) {
        if (site == nullptr && Nx2Snap::On()) [[unlikely]] {
            site = NX2_RETURN_ADDRESS();
        }
        NoteCowRead(device_addr, size, kind, site);
    }
}

/// Back: device memory [device_addr, device_addr + size) is read; size 0 when unknown (GetPointer).
inline void Note(u64 device_addr, u64 size, NoteKind kind = NoteKind::Sized) {
    if (g_note != 0) [[unlikely]] {
        NoteAny(device_addr, size, kind, nullptr);
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
    // run_ahead_cover / run_ahead_raw_probe: one stamp per write, the order the buffer cache
    // commits them in.
    const u64 stamp =
        LandOn() ? CoverGet().write_stamp.fetch_add(1, std::memory_order_acq_rel) + 1 : 0;
    for (u64 block = begin >> WriterBlockBits; block <= (end - 1) >> WriterBlockBits; ++block) {
        WriterSlot& slot = state.writers[WriterSlotOf(block)];
        slot.block.store(~u64{0}, std::memory_order_relaxed);
        slot.ns.store(now, std::memory_order_relaxed);
        slot.stamp.store(stamp, std::memory_order_relaxed);
        slot.block.store(block, std::memory_order_release);
    }
    if (Nx2Raw::On()) [[unlikely]] {
        Nx2Raw::Writer(begin, end, stamp);
    }
}

/// The newest write stamp recorded for any block of [begin, end); false when none is.
inline bool WriterStamp(u64 begin, u64 end, u64& stamp) {
    State& state = Get();
    bool found = false;
    stamp = 0;
    for (u64 block = begin >> WriterBlockBits; block <= (end - 1) >> WriterBlockBits; ++block) {
        const WriterSlot& slot = state.writers[WriterSlotOf(block)];
        if (slot.block.load(std::memory_order_acquire) != block) {
            continue;
        }
        const u64 value = slot.stamp.load(std::memory_order_relaxed);
        if (slot.block.load(std::memory_order_relaxed) == block) {
            stamp = (std::max)(stamp, value);
            found = true;
        }
    }
    return found;
}

/// A guest read of [begin, end) served from preflushed data; [addr, addr + size) is the access
/// itself (run_ahead_raw_probe), size 0 when unknown.
inline void NoteGuestRead(u64 begin, u64 end, u64 addr = 0, u64 size = 0) {
    if (g_is_back || end <= begin) {
        return;
    }
    if (Nx2Raw::On()) [[unlikely]] {
        Nx2Raw::GuestRead(begin, end, addr, size);
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

/// When the first commit holding write stamp `stamp` landed; 0 unknown or not landed.
inline s64 CommitLandNs(u64 stamp) {
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
        if (shared.commits[(mid - 1) % NumCoverCommits].stamp.load(std::memory_order_relaxed) >=
            stamp) {
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

    /// run_ahead_snapshot_probe: the device pages Take kept for this present, empty without.
    std::span<const u64> TakenPages() const {
        return valid ? std::span<const u64>(pages.data(), count) : std::span<const u64>{};
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

    static s64 LandNs(u64 stamp) {
        return CommitLandNs(stamp);
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

// Nx2Perf run_ahead_cow_probe (measurement only, needs run_ahead_ledger; meaningful with
// frames_ahead and present_one_ahead on). At vi2's Ocarina reply core memory write-protects every
// device-mapped guest page of type Memory (later epochs re-arm only the pages given back and new
// device mappings). The first guest write of each one, or of a tracker-protected page, copies the
// page into a fixed pool before the write goes on. Until each passes the fence's increment, the
// front and back record their reads: pages, bytes, pre-imaged hits, bytes that differ from the
// pre-image (hazard), unsized reads by call site and host writes into pre-imaged pages. Nothing
// waits and no read is redirected. Shared by core memory (arm, traps), device memory (reads,
// writes), the fence manager and engines (boundaries) and vi2 (epochs, log).
namespace VideoCommon::Nx2Cow {

inline bool On() {
    static const bool on = Nx2Ledger::On() && Nx2Perf::On(Nx2Perf::Opt::RunAheadCowProbe);
    return on;
}

/// Set once the probe's state exists; host write hooks test it.
inline std::atomic<bool> g_enabled{false};

inline constexpr u64 PageBits = 12;
inline constexpr u64 PageSize = u64{1} << PageBits;
inline constexpr u32 PoolSlots = 4096; ///< 16 MiB of pre-images per epoch
inline constexpr u32 SlotBits = 13;    ///< Index entry: generation << SlotBits | slot + 1
inline constexpr u32 SlotMask = (1U << SlotBits) - 1;
inline constexpr u32 SlotOverflow = SlotMask; ///< Trapped with the pool full: no bytes kept
inline constexpr u32 GenMask = (1U << (32 - SlotBits)) - 1;
inline constexpr size_t NumPhysPages = size_t{1} << 23; ///< 35-bit physical layout
inline constexpr u32 SetSlots = 65536;
inline constexpr u32 SetLimit = SetSlots / 4 * 3;
inline constexpr u32 MaxReplyHashes = 16384;
inline constexpr size_t NumSites = 32;
inline constexpr u32 NumIds = 192;
inline constexpr u32 WindowFrames = 600;
static_assert(PoolSlots < SlotOverflow);

enum class Role : u8 { None, Front, Back };
inline constexpr size_t NumRoles = 2;
inline constexpr size_t NumCats = Nx2Ledger::NumCategories + 1;
inline constexpr size_t FrontCategory = Nx2Ledger::NumCategories; ///< The front: macro/list
static_assert(NumCats <= 16);

enum class TrapClass : u8 { Tracked, Untracked, Bank };
inline constexpr size_t NumTrapClasses = 3;

// Touch set bits: categories read in the low 16, then who touched the page.
inline constexpr u32 BitFront = 1U << 16;
inline constexpr u32 BitBack = 1U << 17;
inline constexpr u32 BitHostWritten = 1U << 18;
inline constexpr u32 BitChecked = 1U << 19;

enum Counter : size_t {
    TrapsGuest = 0, ///< + TrapClass
    TrapsService = TrapsGuest + NumTrapClasses,
    TrapNsGuest = TrapsService + NumTrapClasses,
    TrapNsService,
    TrapsClosed, ///< Given back while no epoch was arming or open: no pre-image
    PoolOverflow,
    ClaimLost,
    UnsizedReads, ///< + role
    UnsizedHits = UnsizedReads + NumRoles,
    SpanHits = UnsizedHits + NumRoles,
    OverflowHits = SpanHits + NumRoles,
    WriteThrough = OverflowHits + NumRoles, ///< + front, back, other thread
    WriteThroughBytes = WriteThrough + 3,
    UnarmedChanged,
    UnarmedChecked,
    Unhashed,
    HostChanged,
    PreimageStale,
    CheckNs,
    TouchOverflow,
    SiteOverflow,
    FrontPassedAtOpen,
    BackPassedAtOpen,
    FrontBeforeAtClose,
    BackBeforeAtClose,
    NumCounters,
};

enum CatField : size_t { Calls, Pages, Bytes, HitPages, HazardBytes, NumCatFields };
inline constexpr size_t NumCatCounters = NumRoles * NumCats * NumCatFields;

inline constexpr size_t CatIndex(size_t role, size_t category, CatField field) {
    return (role * NumCats + category) * NumCatFields + field;
}

/// Pages one epoch touched, keyed by physical page: open addressing, lock-free inserts.
struct TouchSet {
    std::array<std::atomic<u32>, SetSlots> key{}; ///< phys + 1, 0 when empty
    std::array<std::atomic<u32>, SetSlots> bits{};
    std::array<std::atomic<u32>, SetSlots> used{};
    std::atomic<u32> used_count{0};
    std::array<std::atomic<u32>, SetSlots> device{}; ///< run_ahead_raw_probe: a device page read

    static u32 Home(u32 key_value) {
        static_assert(SetSlots == 65536);
        return (key_value * 0x9E3779B1U) >> 16;
    }

    /// The page's slot, inserting it; SetSlots when the set is full.
    u32 Insert(u64 phys) {
        const u32 want = static_cast<u32>(phys) + 1;
        u32 slot = Home(want);
        for (u32 probes = 0; probes < SetSlots; ++probes, slot = (slot + 1) & (SetSlots - 1)) {
            u32 current = key[slot].load(std::memory_order_acquire);
            if (current == want) {
                return slot;
            }
            if (current != 0) {
                continue;
            }
            if (used_count.load(std::memory_order_relaxed) >= SetLimit) {
                return SetSlots;
            }
            if (key[slot].compare_exchange_strong(current, want, std::memory_order_acq_rel)) {
                const u32 n = used_count.fetch_add(1, std::memory_order_acq_rel);
                used[n].store(slot, std::memory_order_release);
                return slot;
            }
            if (current == want) {
                return slot;
            }
        }
        return SetSlots;
    }

    u32 Find(u64 phys) const {
        const u32 want = static_cast<u32>(phys) + 1;
        u32 slot = Home(want);
        for (u32 probes = 0; probes < SetSlots; ++probes, slot = (slot + 1) & (SetSlots - 1)) {
            const u32 current = key[slot].load(std::memory_order_acquire);
            if (current == want) {
                return slot;
            }
            if (current == 0) {
                return SetSlots;
            }
        }
        return SetSlots;
    }

    /// vi2, with no reader of this set left (two epochs old).
    void Clear() {
        const u32 n = (std::min)(used_count.load(std::memory_order_acquire), SetSlots);
        for (u32 i = 0; i < n; ++i) {
            const u32 slot = used[i].load(std::memory_order_relaxed);
            key[slot].store(0, std::memory_order_relaxed);
            bits[slot].store(0, std::memory_order_relaxed);
        }
        used_count.store(0, std::memory_order_release);
    }
};

/// Hashes at the reply of the pages the previous epoch read; built by vi2 before readers open.
struct ReplySet {
    std::array<std::atomic<u32>, SetSlots> key{};
    std::array<std::atomic<u64>, SetSlots> hash{};
    std::array<u32, SetSlots> used{};
    u32 used_count = 0;
    // run_ahead_raw_probe, written by vi2 before the readers open: the device page the previous
    // epoch read the page through and the page's state at the reply (Nx2Raw::Info*).
    std::array<u32, SetSlots> device{};
    std::array<u8, SetSlots> info{};

    void Clear() {
        for (u32 i = 0; i < used_count; ++i) {
            key[used[i]].store(0, std::memory_order_relaxed);
        }
        used_count = 0;
    }

    void Insert(u64 phys, u64 value, u32 device_page = 0) {
        const u32 want = static_cast<u32>(phys) + 1;
        for (u32 slot = TouchSet::Home(want);; slot = (slot + 1) & (SetSlots - 1)) {
            const u32 current = key[slot].load(std::memory_order_relaxed);
            if (current == want || current == 0) {
                device[slot] = device_page;
                info[slot] = 0;
            }
            if (current == want) {
                hash[slot].store(value, std::memory_order_relaxed);
                return;
            }
            if (current == 0) {
                hash[slot].store(value, std::memory_order_relaxed);
                key[slot].store(want, std::memory_order_release);
                used[used_count++] = slot;
                return;
            }
        }
    }

    bool Find(u64 phys, u64& value, u32* found_slot = nullptr) const {
        const u32 want = static_cast<u32>(phys) + 1;
        u32 slot = TouchSet::Home(want);
        for (u32 probes = 0; probes < SetSlots; ++probes, slot = (slot + 1) & (SetSlots - 1)) {
            const u32 current = key[slot].load(std::memory_order_acquire);
            if (current == want) {
                value = hash[slot].load(std::memory_order_relaxed);
                if (found_slot != nullptr) {
                    *found_slot = slot;
                }
                return true;
            }
            if (current == 0) {
                return false;
            }
        }
        return false;
    }
};

/// Device memory's host pointer of a device address, without a ledger note.
using DevicePointerFn = const u8* (*)(const void* device, u64 device_addr);

struct State {
    std::mutex init_mutex;
    std::atomic<bool> ready{false};

    // Epoch: vi2 arms with pool_active set, then opens the readers; both end at the close.
    std::atomic<bool> pool_active{false};
    std::atomic<bool> open{false};
    std::atomic<u32> gen{0};
    std::atomic<u64> pool{0}; ///< gen << 32 | slots taken
    std::atomic<u32> fence_id{~0U};
    std::atomic<u32> fence_value{0};

    // Boundaries: the n-th front parse of an id's increment is the back's n-th increment of it.
    std::array<std::atomic<u32>, NumIds> front_count{};
    std::array<std::atomic<u32>, NumIds> back_count{};
    std::array<std::atomic<u32>, NumIds> back_value{};

    // Pre-images, allocated once.
    std::unique_ptr<u8[]> bytes;
    std::unique_ptr<std::atomic<u32>[]> index; ///< Per physical page
    std::array<std::atomic<u64>, PoolSlots> slot_phys{}; ///< gen << 32 | phys + 1
    std::array<std::unique_ptr<TouchSet>, 2> touch;      ///< By gen & 1
    std::unique_ptr<ReplySet> reply;

    std::atomic<const void*> device{nullptr};
    std::atomic<DevicePointerFn> device_pointer{nullptr};
    std::atomic<uintptr_t> physical_base{0};

    std::array<std::atomic<u64>, NumCounters> c{};
    std::array<std::atomic<u64>, NumCatCounters> cat{};
    std::array<std::atomic<uintptr_t>, NumSites> site_addr{};
    std::array<std::atomic<u64>, NumSites> site_hits{};
    std::atomic<u64> verify_differences{};
};

inline State& Get() {
    static State state;
    return state;
}

inline thread_local Role t_role = Role::None;
inline thread_local u32 t_seen_gen = 0;
inline thread_local bool t_before = false;
inline thread_local bool t_skip_pointer = false;
inline thread_local const void* t_site = nullptr;

inline s64 NowNs() {
    return Nx2WarProbe::NowNs();
}

inline void Add(Counter counter, u64 amount = 1) {
    Get().c[counter].fetch_add(amount, std::memory_order_relaxed);
}

inline void AddCat(size_t role, size_t category, CatField field, u64 amount) {
    Get().cat[CatIndex(role, category, field)].fetch_add(amount, std::memory_order_relaxed);
}

template <typename... Args>
void VerifyDifference(fmt::format_string<Args...> format, Args&&... args) {
    const u64 count = Get().verify_differences.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 64) {
        LOG_ERROR(HW_GPU, "Nx2Perf run_ahead_cow_probe: {} ({} so far)",
                  fmt::format(format, std::forward<Args>(args)...), count);
    }
}

/// Allocates the probe's state once (switch on only).
inline void EnsureInit() {
    State& s = Get();
    if (s.ready.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{s.init_mutex};
    if (s.ready.load(std::memory_order_relaxed)) {
        return;
    }
    s.bytes = std::make_unique<u8[]>(size_t{PoolSlots} * PageSize);
    s.index = std::make_unique<std::atomic<u32>[]>(NumPhysPages);
    s.touch[0] = std::make_unique<TouchSet>();
    s.touch[1] = std::make_unique<TouchSet>();
    s.reply = std::make_unique<ReplySet>();
    s.ready.store(true, std::memory_order_release);
    g_enabled.store(true, std::memory_order_release);
}

/// Device memory (the GPU's manager) at bind time.
inline void RegisterDevice(const void* device, DevicePointerFn fn, uintptr_t physical_base) {
    if (!On()) {
        return;
    }
    EnsureInit();
    State& s = Get();
    s.physical_base.store(physical_base, std::memory_order_relaxed);
    s.device.store(device, std::memory_order_relaxed);
    s.device_pointer.store(fn, std::memory_order_release);
}

inline const u8* DevicePointer(u64 device_addr) {
    State& s = Get();
    const DevicePointerFn fn = s.device_pointer.load(std::memory_order_acquire);
    return fn == nullptr ? nullptr : fn(s.device.load(std::memory_order_relaxed), device_addr);
}

inline u64 PhysOf(const u8* host) {
    return (reinterpret_cast<uintptr_t>(host) -
            Get().physical_base.load(std::memory_order_relaxed)) >>
           PageBits;
}

/// Whether role has passed the open epoch's fence increment.
inline bool Passed(Role role) {
    State& s = Get();
    const u32 id = s.fence_id.load(std::memory_order_acquire);
    if (id >= NumIds) {
        return true;
    }
    const u32 value = s.fence_value.load(std::memory_order_relaxed);
    u32 reached = s.back_value[id].load(std::memory_order_acquire);
    if (role == Role::Front) {
        reached += s.front_count[id].load(std::memory_order_acquire) -
                   s.back_count[id].load(std::memory_order_acquire);
    }
    return static_cast<s32>(reached - value) >= 0;
}

/// The calling GPU thread reads for the open epoch's frame (before its boundary).
inline bool BeforeBoundary(u32 gen) {
    if (t_seen_gen != gen) {
        t_seen_gen = gen;
        t_before = !Passed(t_role);
    }
    return t_before;
}

/// Front (Maxwell3D sync_info, puller fence action): an increment of id is parsed.
inline void FrontIncrement(u32 id) {
    if (id >= NumIds) {
        return;
    }
    if (t_role == Role::None) {
        EnsureInit();
        t_role = Role::Front;
        Nx2Ledger::g_note |= Nx2Ledger::NoteCow;
    }
    State& s = Get();
    s.front_count[id].fetch_add(1, std::memory_order_acq_rel);
    if (t_role == Role::Front && t_before && Passed(Role::Front)) {
        t_before = false;
    }
}

/// Back (fence manager), before it raises id's guest value to value.
inline void BackIncrement(u32 id, u32 value) {
    if (id >= NumIds) {
        return;
    }
    if (Nx2Raw::On()) [[unlikely]] {
        Nx2Raw::BackIncrement(id, value);
    }
    State& s = Get();
    // Count first: a front reading in between sees itself behind, never ahead.
    s.back_count[id].fetch_add(1, std::memory_order_acq_rel);
    s.back_value[id].store(value, std::memory_order_release);
    if (t_before && Passed(Role::Back)) {
        t_before = false;
    }
}

/// The page's pre-image entry in this generation: slot + 1, SlotOverflow, or 0.
inline u32 PreImageEntry(u64 phys, u32 gen) {
    State& s = Get();
    if (phys >= NumPhysPages) {
        return 0;
    }
    const u32 tag = (gen & GenMask) << SlotBits;
    const u32 entry = s.index[phys].load(std::memory_order_acquire);
    u32 fast = (entry & ~SlotMask) == tag ? (entry & SlotMask) : 0;
    if (Nx2Perf::Verify() && fast != SlotOverflow) {
        // The old answer: a scan of the slots taken this generation.
        u32 old = 0;
        const u64 pool = s.pool.load(std::memory_order_acquire);
        if ((pool >> 32) == gen) {
            const u32 taken = (std::min)(static_cast<u32>(pool), PoolSlots);
            const u64 want = (u64{gen} << 32) | (phys + 1);
            for (u32 i = 0; i < taken; ++i) {
                if (s.slot_phys[i].load(std::memory_order_acquire) == want) {
                    old = i + 1;
                    break;
                }
            }
        }
        if (old != fast) {
            VerifyDifference("pre-image index says slot {} for page {:#x}, the pool scan {}",
                             fast, phys << PageBits, old);
            fast = old;
        }
    }
    return fast;
}

inline bool IsBankPage(u64 phys, u32 gen) {
    State& s = Get();
    constexpr u32 bank = 1U << static_cast<u32>(Nx2Ledger::Category::BankCompare);
    for (const u32 parity : {gen & 1, (gen - 1) & 1}) {
        const TouchSet& set = *s.touch[parity];
        if (const u32 slot = set.Find(phys);
            slot != SetSlots && (set.bits[slot].load(std::memory_order_relaxed) & bank) != 0) {
            return true;
        }
    }
    return false;
}

/// Core memory, on a page's first write since the arm and before it lands: copies the page.
/// Returns whether this write was the page's first in the epoch (a trap the CoW would take).
inline bool PreImage(u64 phys, const u8* host, TrapClass cls, bool guest_core) {
    State& s = Get();
    if (!s.ready.load(std::memory_order_acquire) ||
        !s.pool_active.load(std::memory_order_acquire)) {
        Add(TrapsClosed);
        return false;
    }
    if (phys >= NumPhysPages) {
        return false;
    }
    const u32 gen = s.gen.load(std::memory_order_acquire);
    const u32 tag = (gen & GenMask) << SlotBits;
    std::atomic<u32>& entry = s.index[phys];
    u32 current = entry.load(std::memory_order_acquire);
    if ((current & ~SlotMask) == tag && (current & SlotMask) != 0) {
        return false;
    }
    u64 pool = s.pool.load(std::memory_order_acquire);
    u32 value = SlotOverflow;
    for (;;) {
        if ((pool >> 32) != gen) {
            return false;
        }
        if (static_cast<u32>(pool) >= PoolSlots) {
            Add(PoolOverflow);
            break;
        }
        if (s.pool.compare_exchange_weak(pool, pool + 1, std::memory_order_acq_rel)) {
            const u32 slot = static_cast<u32>(pool);
            if (Nx2Snap::On()) [[unlikely]] {
                Nx2Snap::NotePreImage(slot, cls == TrapClass::Tracked);
            }
            std::memcpy(s.bytes.get() + size_t{slot} * PageSize, host, PageSize);
            s.slot_phys[slot].store((u64{gen} << 32) | (phys + 1), std::memory_order_release);
            value = slot + 1;
            break;
        }
    }
    if (!entry.compare_exchange_strong(current, tag | value, std::memory_order_acq_rel)) {
        if (value != SlotOverflow) {
            s.slot_phys[value - 1].store(0, std::memory_order_release);
            Add(ClaimLost);
        }
        return false;
    }
    if (cls != TrapClass::Bank && IsBankPage(phys, gen)) {
        cls = TrapClass::Bank;
    }
    Add(static_cast<Counter>((guest_core ? TrapsGuest : TrapsService) + static_cast<size_t>(cls)));
    return true;
}

/// Core memory: a trap's cost (pre-image and giving the page back).
inline void AddTrapNs(s64 ns, bool guest_core) {
    Add(guest_core ? TrapNsGuest : TrapNsService, static_cast<u64>((std::max<s64>)(ns, 0)));
}

/// Whether pre-images are being taken (an epoch arms or is open).
inline bool PoolActive() {
    return Get().pool_active.load(std::memory_order_acquire);
}

/// Bytes of [a, a + len) and [b, b + len) that differ.
inline u64 DiffBytes(const u8* a, const u8* b, u64 len) {
    u64 differ = 0;
    for (u64 offset = 0; offset < len; offset += 64) {
        const u64 n = (std::min<u64>)(64, len - offset);
        if (std::memcmp(a + offset, b + offset, n) == 0) {
            continue;
        }
        for (u64 i = 0; i < n; ++i) {
            differ += a[offset + i] != b[offset + i] ? 1 : 0;
        }
    }
    if (Nx2Perf::Verify()) {
        u64 old = 0;
        for (u64 i = 0; i < len; ++i) {
            old += a[i] != b[i] ? 1 : 0;
        }
        if (old != differ) {
            VerifyDifference("hazard bytes {} by chunks, {} by bytes", differ, old);
            differ = old;
        }
    }
    return differ;
}

/// First check of a page this epoch: its bytes (the pre-image when it has one) against its hash
/// at the reply.
inline void CheckPage(u64 phys, const u8* bytes, bool preimaged, u32 bits, u64 device_page,
                      u32 gen) {
    State& s = Get();
    const s64 start = NowNs();
    u64 reply_hash = 0;
    u32 reply_slot = 0;
    if (!s.reply->Find(phys, reply_hash, &reply_slot)) {
        Add(Unhashed);
    } else {
        Add(UnarmedChecked);
        if (Nx2Ledger::HashPage(bytes) != reply_hash) {
            const Counter changed = preimaged                      ? PreimageStale
                                    : (bits & BitHostWritten) != 0 ? HostChanged
                                                                   : UnarmedChanged;
            Add(changed);
            if (changed == UnarmedChanged && Nx2Raw::On()) [[unlikely]] {
                Nx2Raw::NoteGap(phys, device_page, s.reply->info[reply_slot], gen);
            }
        }
    }
    Add(CheckNs, static_cast<u64>((std::max<s64>)(NowNs() - start, 0)));
}

/// One page of a read before the boundary; len 0 for an unsized read. Returns whether the page
/// has a pre-image.
inline bool ReadPage(size_t role, size_t category, u64 phys, const u8* host, u64 offset, u64 len,
                     u32 gen, u64 device_page) {
    State& s = Get();
    TouchSet& set = *s.touch[gen & 1];
    const u32 role_bit = role == 0 ? BitFront : BitBack;
    const u32 slot = set.Insert(phys);
    u32 old = 0;
    bool check = false;
    if (slot != SetSlots) {
        if (Nx2Raw::On()) [[unlikely]] {
            set.device[slot].store(static_cast<u32>(device_page), std::memory_order_relaxed);
        }
        old = set.bits[slot].fetch_or(role_bit | (1U << category) | BitChecked,
                                      std::memory_order_acq_rel);
        check = (old & BitChecked) == 0;
        if ((old & role_bit) == 0) {
            AddCat(role, category, Pages, 1);
        }
    } else {
        Add(TouchOverflow);
    }
    const u32 entry = PreImageEntry(phys, gen);
    if (entry == SlotOverflow) {
        Add(static_cast<Counter>(OverflowHits + role));
        return true;
    }
    if (entry == 0) {
        if (check) {
            CheckPage(phys, host, false, old, device_page, gen);
        }
        return false;
    }
    const u8* const pre = s.bytes.get() + size_t{entry - 1} * PageSize;
    AddCat(role, category, HitPages, 1);
    if (len != 0) {
        const u64 hazard = DiffBytes(pre + offset, host + offset, len);
        AddCat(role, category, HazardBytes, hazard);
        if (hazard != 0 && Nx2Snap::On()) [[unlikely]] {
            Nx2Snap::Hazard(phys, entry - 1, pre, hazard, role, category, old, gen,
                            (device_page << PageBits) + offset, len);
        }
    }
    if (check) {
        CheckPage(phys, pre, true, old, device_page, gen);
    }
    return true;
}

inline void NoteSite(const void* site) {
    State& s = Get();
    const uintptr_t address = reinterpret_cast<uintptr_t>(site);
    size_t i = static_cast<size_t>((u64{address} * 0x9E3779B97F4A7C15ULL) >> 59);
    for (size_t probes = 0; probes < NumSites; ++probes, i = (i + 1) % NumSites) {
        uintptr_t current = s.site_addr[i].load(std::memory_order_acquire);
        if (current == 0 &&
            s.site_addr[i].compare_exchange_strong(current, address, std::memory_order_acq_rel)) {
            current = address;
        }
        if (current == address) {
            s.site_hits[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    Add(SiteOverflow);
}

inline void ReadSlow(u64 device_addr, u64 size, Nx2Ledger::NoteKind kind, const void* site,
                     u32 gen) {
    const size_t role = t_role == Role::Front ? 0 : 1;
    const size_t category =
        role == 0 ? FrontCategory
                  : (std::min<size_t>)(Nx2Ledger::g_ledger_category, Nx2Ledger::NumCategories - 1);
    if (Nx2Snap::On()) [[unlikely]] {
        Nx2Snap::t_site = site;
        if (Nx2Snap::BudgetOn()) [[unlikely]] {
            Nx2Snap::t_span = kind == Nx2Ledger::NoteKind::Span &&
                              (device_addr & (PageSize - 1)) + size > PageSize;
        }
    }
    if (kind == Nx2Ledger::NoteKind::Pointer) {
        Add(static_cast<Counter>(UnsizedReads + role));
        const u8* const host = DevicePointer(device_addr & ~(PageSize - 1));
        if (host != nullptr &&
            ReadPage(role, category, PhysOf(host), host, 0, 0, gen, device_addr >> PageBits)) {
            Add(static_cast<Counter>(UnsizedHits + role));
            NoteSite(site);
        }
        return;
    }
    if (size == 0) {
        return;
    }
    AddCat(role, category, Calls, 1);
    AddCat(role, category, Bytes, size);
    bool span_hit = false;
    const u64 end = device_addr + size;
    for (u64 page = device_addr >> PageBits; page <= (end - 1) >> PageBits; ++page) {
        const u8* const host = DevicePointer(page << PageBits);
        if (host == nullptr) {
            continue;
        }
        const u64 begin = (std::max)(device_addr, page << PageBits) - (page << PageBits);
        const u64 stop = (std::min)(end, (page + 1) << PageBits) - (page << PageBits);
        span_hit |= ReadPage(role, category, PhysOf(host), host, begin, stop - begin, gen, page);
    }
    if (span_hit && kind == Nx2Ledger::NoteKind::Span) {
        Add(static_cast<Counter>(SpanHits + role));
    }
}

/// Device memory, before a host write of [device_addr, device_addr + size) lands. src: the bytes
/// it writes (or nullptr), site: its caller (run_ahead_own_write_probe).
inline void HostWriteSlow(u64 device_addr, u64 size, const void* src = nullptr,
                          const void* site = nullptr) {
    State& s = Get();
    if (Nx2Snap::BudgetOn()) [[unlikely]] {
        Nx2Snap::HostWritePages(device_addr, size);
    }
    if (Nx2Snap::g_own.load(std::memory_order_relaxed)) [[unlikely]] {
        Nx2Snap::OwnHostWrite(device_addr, size, src,
                              site != nullptr ? site : NX2_RETURN_ADDRESS());
    }
    if (size == 0 || !s.open.load(std::memory_order_acquire)) {
        return;
    }
    const u32 gen = s.gen.load(std::memory_order_acquire);
    size_t who = 2; // A thread without a boundary (fencing, services)
    bool count = true;
    if (t_role != Role::None) {
        who = t_role == Role::Front ? 0 : 1;
        count = BeforeBoundary(gen);
    }
    TouchSet& set = *s.touch[gen & 1];
    const u64 end = device_addr + size;
    for (u64 page = device_addr >> PageBits; page <= (end - 1) >> PageBits; ++page) {
        const u8* const host = DevicePointer(page << PageBits);
        if (host == nullptr) {
            continue;
        }
        const u64 phys = PhysOf(host);
        if (const u32 slot = set.Insert(phys); slot != SetSlots) {
            set.bits[slot].fetch_or(BitHostWritten, std::memory_order_acq_rel);
        }
        if (count && PreImageEntry(phys, gen) != 0) {
            const u64 len = (std::min)(end, (page + 1) << PageBits) -
                            (std::max)(device_addr, page << PageBits);
            Add(static_cast<Counter>(WriteThrough + who));
            Add(WriteThroughBytes, len);
        }
    }
}

/// Device memory and the GPU's memory manager: a host write of src's bytes is about to land.
inline void HostWrite(u64 device_addr, u64 size, const void* src = nullptr) {
    if (g_enabled.load(std::memory_order_relaxed)) [[unlikely]] {
        HostWriteSlow(device_addr, size, src);
    }
}

/// The next unsized note on this thread starts an access noted with its size.
inline void SkipNextPointer() {
    if ((Nx2Ledger::g_note & Nx2Ledger::NoteCow) != 0) [[unlikely]] {
        t_skip_pointer = true;
    }
}

/// The next unsized note on this thread is reported at site (a wrapper's caller).
inline void SetSite(const void* site) {
    if ((Nx2Ledger::g_note & Nx2Ledger::NoteCow) != 0) [[unlikely]] {
        t_site = site;
    }
}

/// vi2, at the reply: a new epoch for fence (id, value); pre-images are taken from here on.
inline void Begin(u32 id, u32 value) {
    EnsureInit();
    State& s = Get();
    const u32 gen = s.gen.load(std::memory_order_relaxed) + 1;
    s.touch[gen & 1]->Clear();
    s.fence_id.store(id, std::memory_order_relaxed);
    s.fence_value.store(value, std::memory_order_relaxed);
    s.pool.store(u64{gen} << 32, std::memory_order_release);
    s.gen.store(gen, std::memory_order_release);
    s.pool_active.store(true, std::memory_order_release);
    if (Passed(Role::Front)) {
        Add(FrontPassedAtOpen);
    }
    if (Passed(Role::Back)) {
        Add(BackPassedAtOpen);
    }
    if (Nx2Raw::On()) [[unlikely]] {
        Nx2Raw::Begin(id, value, gen);
    }
}

/// vi2, after the arm: hashes the pages the previous epoch read. Returns how many.
inline u32 HashReply() {
    State& s = Get();
    ReplySet& reply = *s.reply;
    reply.Clear();
    const uintptr_t base = s.physical_base.load(std::memory_order_relaxed);
    const u32 gen = s.gen.load(std::memory_order_relaxed);
    if (base == 0 || gen < 2) {
        return 0;
    }
    const TouchSet& previous = *s.touch[(gen - 1) & 1];
    const u32 n = (std::min)(previous.used_count.load(std::memory_order_acquire), SetSlots);
    const bool raw = Nx2Raw::On();
    u32 hashed = 0;
    for (u32 i = 0; i < n && hashed < MaxReplyHashes; ++i) {
        const u32 slot = previous.used[i].load(std::memory_order_acquire);
        if ((previous.bits[slot].load(std::memory_order_relaxed) & (BitFront | BitBack)) == 0) {
            continue;
        }
        const u32 key = previous.key[slot].load(std::memory_order_relaxed);
        if (key == 0) {
            continue;
        }
        const u64 phys = key - 1;
        reply.Insert(phys,
                     Nx2Ledger::HashPage(reinterpret_cast<const u8*>(base) + (phys << PageBits)),
                     raw ? previous.device[slot].load(std::memory_order_relaxed) : 0);
        ++hashed;
    }
    return hashed;
}

/// run_ahead_raw_probe, vi2 after HashReply: each reply page's state at the reply.
/// classify(device pages, cpu page scratch, info) fills Nx2Raw::Info* bits.
template <typename Classify>
void ClassifyReply(std::span<u32> device_scratch, std::span<u64> cpu_scratch,
                   std::span<u8> info_scratch, Classify&& classify) {
    ReplySet& reply = *Get().reply;
    const size_t n = (std::min)({size_t{reply.used_count}, device_scratch.size(),
                                 cpu_scratch.size(), info_scratch.size()});
    for (size_t i = 0; i < n; ++i) {
        device_scratch[i] = reply.device[reply.used[i]];
        info_scratch[i] = 0;
    }
    classify(std::span<const u32>(device_scratch.data(), n), cpu_scratch.first(n),
             info_scratch.first(n));
    for (size_t i = 0; i < n; ++i) {
        reply.info[reply.used[i]] = static_cast<u8>(info_scratch[i] | Nx2Raw::InfoValid);
    }
}

/// vi2, right before the reply: the front and back record their reads from here on.
inline void OpenReaders() {
    if (Nx2Raw::On()) [[unlikely]] {
        Nx2Raw::Open();
    }
    Get().open.store(true, std::memory_order_release);
}

/// vi2, at the next present after its wait: the epoch ends.
inline void Close() {
    State& s = Get();
    if (!s.pool_active.load(std::memory_order_acquire)) {
        return;
    }
    if (!Passed(Role::Front)) {
        Add(FrontBeforeAtClose);
    }
    if (!Passed(Role::Back)) {
        Add(BackBeforeAtClose);
    }
    s.open.store(false, std::memory_order_release);
    s.pool_active.store(false, std::memory_order_release);
}

/// One arm by core memory and vi2's hashing, for the window.
struct ArmSample {
    u64 armed = 0;         ///< Pages write-protected now
    u64 protect_calls = 0; ///< Host protect calls (runs)
    u64 armed_total = 0;   ///< Pages armed after it
    u64 new_mapped = 0;    ///< Pages of device mappings made since the last arm
    u64 full = 0;          ///< 1 when every device-mapped page was collected
    u64 overflow = 0;      ///< Pages the fixed lists could not hold
    s64 arm_ns = 0;
    s64 hash_ns = 0;
    u64 hashed = 0;
};

/// vi2's side: per-frame samples and the line once per WindowFrames presents.
class Vi2Window {
public:
    Vi2Window() {
        LOG_INFO(HW_GPU,
                 "Nx2RunAheadCow: probe on (measurement only; meaningful with EDEN_NX2_PERF_ON="
                 "frames_ahead,present_one_ahead,run_ahead_ledger,run_ahead_cow_probe; flashes "
                 "there are expected, not a result). Decision rules: build run_ahead_cow next "
                 "only if traps x ns/trap <= ~8 ms/frame on guest threads, the pre-imaged pages "
                 "fit 4096/frame (pool_overflow 0), unarmed_changed == 0 and the unsized hazards "
                 "come from <= ~5 call sites. If hazard_bytes is ~0 outside bank and unsized "
                 "reads while flashes persist, the flash is RAW (the guest reading GPU results): "
                 "pursue raw_landing_wait instead. trap_ns excludes the host fault dispatch; "
                 "unarmed_changed is checked once per page and epoch, at its first read, against "
                 "its hash at the reply (pages the previous epoch read; others count unhashed); "
                 "unsized sites are return addresses (ref={:#x} is Nx2Cow::Get)",
                 reinterpret_cast<uintptr_t>(&Get));
        if (Nx2Raw::On()) {
            LOG_INFO(
                HW_GPU,
                "Nx2RunAheadRaw: probe on (measurement only; meaningful with EDEN_NX2_PERF_ON="
                "frames_ahead,run_ahead_ledger,run_ahead_cow_probe,run_ahead_raw_probe; flashes "
                "there are expected, not a result). Decision rules: build run_ahead_exact only if "
                "the unarmed_changed pages (gap classes) fall into at most ~3 fixable classes. RAW "
                "is exact with guest_landed_syncpoints if 'stale and not covered' is 0. If the "
                "stale reads are mostly no_writer_yet, exactness needs fence-N waits and the "
                "lever is dead. If stale is ~0, no RAW wait is needed. Reads are guest reads of "
                "preflushed blocks with a frame N writer (written after fence N-1's back "
                "increment); stale: the read bytes' hash differs once that writer landed; "
                "covered: the guest's last syncpoint wait was raised by the back after the "
                "writer (tl = on the reading thread, any = on any thread)");
        }
    }

    void NoteArm(const ArmSample& sample) {
        arm = sample;
        have_arm = true;
        sum.armed += sample.armed;
        sum.protect_calls += sample.protect_calls;
        sum.new_mapped += sample.new_mapped;
        sum.full += sample.full;
        sum.overflow += sample.overflow;
        sum.hash_ns += sample.hash_ns;
        sum.hashed += sample.hashed;
        armed_total = sample.armed_total;
    }

    /// At the close: one frame. Returns the report line every WindowFrames frames.
    std::string Close() {
        State& s = Get();
        const Snapshot now = Take();
        if (have_frame_base && have_arm) {
            const auto delta = [&](size_t counter) {
                return now.c[counter] - frame_base.c[counter];
            };
            const size_t f = frames;
            metrics[TrapTrackedM][f] =
                static_cast<float>(delta(TrapsGuest) + delta(TrapsService));
            metrics[TrapUntrackedM][f] =
                static_cast<float>(delta(TrapsGuest + 1) + delta(TrapsService + 1));
            metrics[TrapBankM][f] =
                static_cast<float>(delta(TrapsGuest + 2) + delta(TrapsService + 2));
            metrics[TrapMsGuestM][f] = static_cast<float>(delta(TrapNsGuest)) / 1e6f;
            metrics[ArmMsM][f] = static_cast<float>(arm.arm_ns) / 1e6f;
            metrics[PreimagedM][f] = static_cast<float>((std::min<u64>)(
                static_cast<u32>(s.pool.load(std::memory_order_acquire)), PoolSlots));
            u64 front_bytes = 0;
            u64 back_bytes = 0;
            u64 hazard = 0;
            for (size_t c = 0; c < NumCats; ++c) {
                front_bytes +=
                    now.cat[CatIndex(0, c, Bytes)] - frame_base.cat[CatIndex(0, c, Bytes)];
                back_bytes +=
                    now.cat[CatIndex(1, c, Bytes)] - frame_base.cat[CatIndex(1, c, Bytes)];
                for (size_t r = 0; r < NumRoles; ++r) {
                    hazard += now.cat[CatIndex(r, c, HazardBytes)] -
                              frame_base.cat[CatIndex(r, c, HazardBytes)];
                }
            }
            metrics[FrontKibM][f] = static_cast<float>(front_bytes) / 1024.0f;
            metrics[BackKibM][f] = static_cast<float>(back_bytes) / 1024.0f;
            metrics[HazardM][f] = static_cast<float>(hazard);
            metrics[UnarmedM][f] = static_cast<float>(delta(UnarmedChanged));
            ++frames;
        }
        frame_base = now;
        have_frame_base = true;
        have_arm = false;
        if (!have_window_base) {
            window_base = now;
            have_window_base = true;
        }
        if (Nx2Perf::Verify()) {
            const s64 t = NowNs();
            const u64 count = s.verify_differences.load(std::memory_order_relaxed);
            if (count != verify_logged && t - verify_log_ns >= 1'000'000'000) {
                LOG_INFO(HW_GPU, "Nx2Perf run_ahead_cow_probe: {} verify differences so far",
                         count);
                verify_logged = count;
                verify_log_ns = t;
            }
        }
        if (Nx2Raw::On()) [[unlikely]] {
            Nx2Raw::FrameClosed();
        }
        if (frames < WindowFrames) {
            return {};
        }
        std::string line = Report(now);
        if (Nx2Raw::On()) [[unlikely]] {
            raw_line = RawReport(now);
        }
        frames = 0;
        window_base = now;
        sum = Sums{};
        return line;
    }

    /// run_ahead_raw_probe: the Nx2RunAheadRaw line of the window Close just ended, once.
    std::string TakeRawLine() {
        return std::exchange(raw_line, std::string{});
    }

private:
    enum Metric : size_t {
        TrapTrackedM,
        TrapUntrackedM,
        TrapBankM,
        TrapMsGuestM,
        ArmMsM,
        PreimagedM,
        FrontKibM,
        BackKibM,
        HazardM,
        UnarmedM,
        NumMetrics,
    };

    struct Snapshot {
        std::array<u64, NumCounters> c{};
        std::array<u64, NumCatCounters> cat{};
        std::array<u64, NumSites> site_hits{};
    };

    struct Sums {
        u64 armed = 0;
        u64 protect_calls = 0;
        u64 new_mapped = 0;
        u64 full = 0;
        u64 overflow = 0;
        u64 hashed = 0;
        s64 hash_ns = 0;
    };

    static Snapshot Take() {
        State& s = Get();
        Snapshot snapshot;
        for (size_t i = 0; i < NumCounters; ++i) {
            snapshot.c[i] = s.c[i].load(std::memory_order_relaxed);
        }
        for (size_t i = 0; i < NumCatCounters; ++i) {
            snapshot.cat[i] = s.cat[i].load(std::memory_order_relaxed);
        }
        for (size_t i = 0; i < NumSites; ++i) {
            snapshot.site_hits[i] = s.site_hits[i].load(std::memory_order_relaxed);
        }
        return snapshot;
    }

    std::string Percentiles(Metric metric) {
        std::copy_n(metrics[metric].begin(), frames, sorted.begin());
        const auto at = [&](size_t percent) {
            const size_t index = (std::min<size_t>)(frames - 1, frames * percent / 100);
            std::nth_element(sorted.begin(), sorted.begin() + index, sorted.begin() + frames);
            return sorted[index];
        };
        const float p50 = at(50);
        const float p99 = at(99);
        return fmt::format("{:.1f}/{:.1f}", p50, p99);
    }

    static std::string_view CategoryName(size_t c) {
        return c == FrontCategory ? std::string_view{"macro_list"}
                                  : Nx2Ledger::category_names[c];
    }

    std::string Report(const Snapshot& now) {
        State& s = Get();
        const double n = static_cast<double>(frames);
        const auto delta = [&](size_t counter) { return now.c[counter] - window_base.c[counter]; };
        const auto per = [&](u64 value) { return static_cast<double>(value) / n; };
        const auto cat_delta = [&](size_t role, size_t c, CatField field) {
            return now.cat[CatIndex(role, c, field)] - window_base.cat[CatIndex(role, c, field)];
        };
        // Reads by thread and category: calls/pages/KiB/hit pages/hazard bytes per frame.
        std::string reads;
        std::array<u64, NumCats> hazard_by_cat{};
        for (size_t role = 0; role < NumRoles; ++role) {
            reads += role == 0 ? " front:" : " back:";
            for (size_t c = 0; c < NumCats; ++c) {
                const u64 calls = cat_delta(role, c, Calls);
                const u64 pages = cat_delta(role, c, Pages);
                const u64 hazard = cat_delta(role, c, HazardBytes);
                hazard_by_cat[c] += hazard;
                if (calls == 0 && pages == 0) {
                    continue;
                }
                reads += fmt::format(" {}={:.1f}/{:.1f}/{:.1f}/{:.2f}/{:.1f}", CategoryName(c),
                                     per(calls), per(pages),
                                     per(cat_delta(role, c, Bytes)) / 1024.0,
                                     per(cat_delta(role, c, HitPages)), per(hazard));
            }
        }
        std::string hazards;
        for (size_t c = 0; c < NumCats; ++c) {
            if (hazard_by_cat[c] != 0) {
                hazards += fmt::format(" {}={:.1f}", CategoryName(c), per(hazard_by_cat[c]));
            }
        }
        // Front capture: back bytes whose ranges the front has at parse vs derived lazily.
        using Nx2Ledger::Category;
        const auto back_kib = [&](std::initializer_list<Category> categories) {
            u64 total = 0;
            for (const Category c : categories) {
                total += cat_delta(1, static_cast<size_t>(c), Bytes);
            }
            return per(total) / 1024.0;
        };
        const double known = back_kib({Category::UniformFast, Category::UniformCached,
                                       Category::Vertex, Category::Index, Category::Indirect});
        const double lazy = back_kib({Category::Storage, Category::Descriptor,
                                      Category::TextureUpload, Category::BankCompare});
        const double other = back_kib({Category::Other});
        // Unsized-hit call sites this window, most hits first.
        std::array<std::pair<u64, size_t>, NumSites> sites{};
        for (size_t i = 0; i < NumSites; ++i) {
            sites[i] = {now.site_hits[i] - window_base.site_hits[i], i};
        }
        std::sort(sites.begin(), sites.end(), std::greater<>{});
        std::string top;
        size_t site_count = 0;
        for (const auto& [hits, i] : sites) {
            if (hits == 0) {
                continue;
            }
            if (++site_count <= 8) {
                top += fmt::format(" {:#x}:{}", s.site_addr[i].load(std::memory_order_relaxed),
                                   hits);
            }
        }
        const u64 traps_guest = delta(TrapsGuest) + delta(TrapsGuest + 1) + delta(TrapsGuest + 2);
        const double ns_per_trap = traps_guest == 0
                                       ? 0.0
                                       : static_cast<double>(delta(TrapNsGuest)) /
                                             static_cast<double>(traps_guest);
        return fmt::format(
            "Nx2RunAheadCow: frames={} traps/frame p50/p99 tracked={} untracked={} bank={} "
            "service={:.1f}/frame (ns {:.0f}) guest_trap_ms p50/p99={} ns_per_trap={:.0f} "
            "closed_traps={:.1f}/frame arm_ms p50/p99={} arm_calls={:.1f}/frame "
            "armed={:.1f}/frame armed_total={} new_mapped={:.1f}/frame full_arms={} "
            "arm_overflow={} preimaged p50/p99={} pool_overflow={} claim_lost={} "
            "reads(calls/pages/KiB/hit pages/hazard B per frame):{} KiB p50/p99 front={} "
            "back={} hazard_bytes p50/p99={} by_cat:{} unsized(reads/hits per frame) "
            "front={:.1f}/{:.2f} back={:.1f}/{:.2f} sites={}{} site_overflow={} "
            "overflow_hits={}/{} getspan_hits front={} back={} host_write_through "
            "front/back/other={}/{}/{} ({:.1f} KiB/frame) unarmed_changed={} p50/p99={} "
            "(checked {}, unhashed {}, host_changed {}, preimage_stale {}) check_ms={:.3f}/frame "
            "reply_hashed={:.0f}/frame reply_hash_ms={:.3f}/frame capture(back KiB/frame "
            "known/lazy/other)={:.1f}/{:.1f}/{:.1f} boundary(front/back passed at open {}/{}, "
            "before at close {}/{}) touch_overflow={} verify_differences={}",
            frames, Percentiles(TrapTrackedM), Percentiles(TrapUntrackedM),
            Percentiles(TrapBankM),
            per(delta(TrapsService) + delta(TrapsService + 1) + delta(TrapsService + 2)),
            per(delta(TrapNsService)), Percentiles(TrapMsGuestM), ns_per_trap,
            per(delta(TrapsClosed)), Percentiles(ArmMsM), per(sum.protect_calls), per(sum.armed),
            armed_total, per(sum.new_mapped), sum.full, sum.overflow, Percentiles(PreimagedM),
            delta(PoolOverflow), delta(ClaimLost), reads, Percentiles(FrontKibM),
            Percentiles(BackKibM), Percentiles(HazardM), hazards, per(delta(UnsizedReads)),
            per(delta(UnsizedHits)), per(delta(UnsizedReads + 1)), per(delta(UnsizedHits + 1)),
            site_count, top, delta(SiteOverflow), delta(OverflowHits), delta(OverflowHits + 1),
            delta(SpanHits), delta(SpanHits + 1), delta(WriteThrough), delta(WriteThrough + 1),
            delta(WriteThrough + 2), per(delta(WriteThroughBytes)) / 1024.0,
            delta(UnarmedChanged), Percentiles(UnarmedM), delta(UnarmedChecked), delta(Unhashed),
            delta(HostChanged), delta(PreimageStale),
            static_cast<double>(delta(CheckNs)) / 1e6 / n, per(sum.hashed),
            static_cast<double>(sum.hash_ns) / 1e6 / n, known, lazy, other,
            delta(FrontPassedAtOpen), delta(BackPassedAtOpen), delta(FrontBeforeAtClose),
            delta(BackBeforeAtClose), delta(TouchOverflow),
            s.verify_differences.load(std::memory_order_relaxed));
    }

    /// run_ahead_raw_probe: the window's line, with the unsized sites by hits.
    std::string RawReport(const Snapshot& now) {
        State& s = Get();
        std::array<std::pair<u64, uintptr_t>, NumSites> sites{};
        for (size_t i = 0; i < NumSites; ++i) {
            sites[i] = {now.site_hits[i] - window_base.site_hits[i],
                        s.site_addr[i].load(std::memory_order_relaxed)};
        }
        std::sort(sites.begin(), sites.end(), std::greater<>{});
        return Nx2Raw::Report(frames, now.c[UnarmedChanged] - window_base.c[UnarmedChanged],
                              sites);
    }

    std::array<std::array<float, WindowFrames>, NumMetrics> metrics{};
    std::array<float, WindowFrames> sorted{};
    size_t frames = 0;
    Snapshot frame_base{};
    Snapshot window_base{};
    bool have_frame_base = false;
    bool have_window_base = false;
    bool have_arm = false;
    ArmSample arm{};
    Sums sum{};
    u64 armed_total = 0;
    u64 verify_logged = 0;
    s64 verify_log_ns = 0;
    std::string raw_line;
};

} // namespace VideoCommon::Nx2Cow

namespace VideoCommon::Nx2Ledger {

inline void NoteCowRead(u64 device_addr, u64 size, NoteKind kind, const void* site) {
    if (kind == NoteKind::Pointer) {
        if (Nx2Cow::t_site != nullptr) {
            site = Nx2Cow::t_site;
            Nx2Cow::t_site = nullptr;
        }
        if (Nx2Cow::t_skip_pointer) {
            Nx2Cow::t_skip_pointer = false;
            return;
        }
    } else {
        // A skip or site whose GetPointer returned early (unmapped) ends here.
        Nx2Cow::t_skip_pointer = false;
        Nx2Cow::t_site = nullptr;
    }
    Nx2Cow::State& s = Nx2Cow::Get();
    if (!s.open.load(std::memory_order_acquire)) {
        return;
    }
    const u32 gen = s.gen.load(std::memory_order_acquire);
    if (!Nx2Cow::BeforeBoundary(gen)) {
        return;
    }
    Nx2Cow::ReadSlow(device_addr, size, kind, site, gen);
}

inline void NoteCowStartBack() {
    if (!Nx2Cow::On()) {
        return;
    }
    Nx2Cow::EnsureInit();
    Nx2Cow::t_role = Nx2Cow::Role::Back;
    g_note |= NoteCow;
}

} // namespace VideoCommon::Nx2Ledger

// Nx2Perf run_ahead_raw_probe (measurement only, needs run_ahead_cow_probe; meaningful with
// frames_ahead on). (a) Guest reads of preflushed blocks after vi2's reply for present N whose
// writer is frame N's (recorded by the back after fence N-1's increment): whether the back had
// recorded it at the read, whether the guest's last syncpoint wait covers it (the back raised
// that syncpoint after the writer) and whether the read bytes differ once it landed (stale).
// (b) Each unarmed_changed page's state at the reply and at the epoch's close. (c) Unsized read
// sites as RVAs. Nothing waits and no read is redirected: the probe only hashes the bytes read,
// at the read and at the writer's landing. Its mutex is taken only with the switch on.
namespace VideoCommon::Nx2Raw {

inline bool On() {
    static const bool on = Nx2Cow::On() && Nx2Perf::On(Nx2Perf::Opt::RunAheadRawProbe);
    return on;
}

inline constexpr size_t NumIncs = 1024;
inline constexpr size_t NumPending = 256;
inline constexpr size_t VerifyBytes = 256;
inline constexpr u64 VerifyEvery = 64;
inline constexpr size_t NumSamples = 4096;
inline constexpr u32 MaxGaps = 64;
inline constexpr u64 MaxHashBytes = 64 * 1024;
inline constexpr size_t NumClasses = 8;
inline constexpr size_t NumGapCodes = 128;
inline constexpr u64 KeyValid = u64{1} << 63;

/// A back increment of a syncpoint and the write stamp when it came.
struct Inc {
    std::atomic<u64> seq{0};
    std::atomic<u64> key{0}; ///< id << 32 | value
    std::atomic<u64> stamp{0};
};

enum class Slot : u8 { Free, NoWriter, Writer };

/// A guest read waiting for frame N's writer of its blocks to be recorded or to land.
struct Pending {
    Slot state = Slot::Free;
    bool writer_known = false; ///< Frame N's writer was recorded at the read
    bool verify = false;       ///< bytes holds the read bytes (Verify, 1 in VerifyEvery)
    u32 gen = 0;
    u32 size = 0;
    u64 first_block = 0;
    u64 last_block = 0;
    u64 addr = 0;
    s64 read_ns = 0;
    u64 hash = 0;
    u64 writer_stamp = 0;
    u64 tl_key = 0;  ///< The reading thread's last guest syncpoint wait (Key), 0 none
    u64 any_key = 0; ///< Any thread's last one
    std::array<u8, VerifyBytes> bytes{};
};

/// An unarmed_changed page (Nx2Cow::CheckPage), classified at the epoch's close.
struct Gap {
    std::atomic<u32> gen{0}; ///< 0 while empty
    u64 phys = 0;
    u32 device_page = 0;
    u8 info = 0;
    bool classified = false; ///< vi2 only
};

enum Counter : size_t {
    Reads,
    Untracked,
    NoEpoch,
    PrevUnknown,
    AfterFence,
    NoFrameWriter,
    Recorded,
    LandedBefore,
    PendingOverflow,
    Unlanded,
    Truncated,
    TlWaitReads,
    Resolved,
    StaleBytes,
    StaleUncoveredAny,
    GapsDropped,
    Class0, ///< + (no writer yet ? 4 : 0) | (uncovered ? 2 : 0) | (stale ? 1 : 0)
    NumCounters = Class0 + NumClasses,
};

struct State {
    std::mutex mutex; ///< Pending reads and samples (switch on only)
    // The back's increments, written by the back only.
    std::array<Inc, NumIncs> incs{};
    std::atomic<u64> inc_count{0};
    // The epoch, set by vi2 at Begin before the readers open.
    std::atomic<u32> gen{0};
    std::atomic<u64> fence_key{0}; ///< Fence N (Key)
    std::atomic<u64> prev_stamp{0};
    std::atomic<bool> prev_known{false};
    std::atomic<s64> reply_ns{0};
    std::atomic<u64> global_wait_key{0};
    // Under mutex.
    std::array<Pending, NumPending> pending{};
    u32 active = 0;
    u32 writers = 0; ///< Pending reads with a writer stamp
    u64 verify_seq = 0;
    std::array<s64, NumSamples> land_after_read{};
    size_t land_n = 0;
    std::array<s64, NumSamples> read_after_reply{};
    size_t reply_n = 0;
    // Window counters, taken by Report.
    std::array<std::atomic<u64>, NumCounters> c{};
    std::array<Gap, MaxGaps> gaps{};
    std::atomic<u32> gap_count{0};
    // vi2 only.
    std::array<u64, NumGapCodes> gap_codes{};
    u64 gap_unclassified = 0;
    u64 verify_logged = 0;
    s64 verify_log_ns = 0;
    std::array<u32, Nx2Cow::MaxReplyHashes> reply_device{};
    std::array<u64, Nx2Cow::MaxReplyHashes> reply_cpu{};
    std::array<u8, Nx2Cow::MaxReplyHashes> reply_info{};
    std::atomic<uintptr_t> module_base{0};
    std::atomic<u64> verify_differences{0};
};

inline State& Get() {
    static State state;
    return state;
}

inline thread_local u64 t_wait_key = 0;

inline u64 Key(u32 id, u32 value) {
    return KeyValid | (u64{id} << 32) | value;
}

inline s64 NowNs() {
    return Nx2WarProbe::NowNs();
}

inline void Add(Counter counter, u64 amount = 1) {
    Get().c[counter].fetch_add(amount, std::memory_order_relaxed);
}

template <typename... Args>
void VerifyDifference(fmt::format_string<Args...> format, Args&&... args) {
    const u64 count = Get().verify_differences.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 64) {
        LOG_ERROR(HW_GPU, "Nx2Perf run_ahead_raw_probe: {} ({} so far)",
                  fmt::format(format, std::forward<Args>(args)...), count);
    }
}

/// The executable's base, for unsized sites as RVAs (core memory sets it on Windows).
inline void SetModuleBase(uintptr_t base) {
    Get().module_base.store(base, std::memory_order_relaxed);
}

/// nvhost_ctrl: a guest syncpoint wait for (id, value) returned satisfied on this thread.
inline void NoteGuestWait(u32 id, u32 value) {
    if (!On()) {
        return;
    }
    t_wait_key = Key(id, value);
    Get().global_wait_key.store(Key(id, value), std::memory_order_relaxed);
}

/// nvhost_ctrl: an event wait for (id, value) was signalled (on the host1x thread).
inline void NoteGuestWaitSignalled(u32 id, u32 value) {
    if (!On()) {
        return;
    }
    Get().global_wait_key.store(Key(id, value), std::memory_order_relaxed);
}

/// The write stamp when the back raised key's syncpoint to key's value (the first increment that
/// reached it); false when that increment is not in the ring (not yet, or too old).
inline bool IncStamp(u64 key, u64& stamp) {
    if ((key & KeyValid) == 0) {
        return false;
    }
    State& s = Get();
    const u32 id = static_cast<u32>(key >> 32) & 0x7fffffff;
    const u32 value = static_cast<u32>(key);
    const u64 n = s.inc_count.load(std::memory_order_acquire);
    bool found = false;
    for (u64 i = n; i > 0 && n - i < NumIncs; --i) {
        const Inc& inc = s.incs[(i - 1) % NumIncs];
        if (inc.seq.load(std::memory_order_acquire) != i) {
            continue;
        }
        const u64 k = inc.key.load(std::memory_order_relaxed);
        const u64 st = inc.stamp.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (inc.seq.load(std::memory_order_relaxed) != i || static_cast<u32>(k >> 32) != id) {
            continue;
        }
        if (static_cast<s32>(static_cast<u32>(k) - value) < 0) {
            break; // Older increments are below the value too
        }
        stamp = st;
        found = true;
    }
    return found;
}

/// The writer with stamp writer_stamp was recorded before the back raised key's syncpoint.
inline bool Covered(u64 key, u64 writer_stamp) {
    u64 stamp = 0;
    return IncStamp(key, stamp) && stamp >= writer_stamp;
}

inline void BackIncrement(u32 id, u32 value) {
    State& s = Get();
    const u64 stamp = Nx2Ledger::CoverGet().write_stamp.load(std::memory_order_acquire);
    const u64 n = s.inc_count.load(std::memory_order_relaxed) + 1;
    Inc& inc = s.incs[(n - 1) % NumIncs];
    inc.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    inc.key.store((u64{id} << 32) | value, std::memory_order_relaxed);
    inc.stamp.store(stamp, std::memory_order_relaxed);
    inc.seq.store(n, std::memory_order_release);
    s.inc_count.store(n, std::memory_order_release);
}

inline u64 HashBytes(const u8* data, u64 size, u64 hash) {
    constexpr u64 P1 = 0x9E3779B185EBCA87ULL;
    constexpr u64 P2 = 0xC2B2AE3D27D4EB4FULL;
    u64 i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 word;
        std::memcpy(&word, data + i, sizeof(word));
        hash = std::rotl(hash ^ (word * P2), 31) * P1;
    }
    for (; i < size; ++i) {
        hash = std::rotl(hash ^ (data[i] * P2), 11) * P1;
    }
    return hash ^ (hash >> 29);
}

/// Calls fn(host or nullptr, length) for the pages of [addr, addr + size) in order.
template <typename Fn>
void ForEachPiece(u64 addr, u64 size, Fn&& fn) {
    const u64 end = addr + size;
    for (u64 cur = addr; cur < end;) {
        const u64 piece = (std::min)(end, ((cur >> Nx2Cow::PageBits) + 1) << Nx2Cow::PageBits) - cur;
        fn(Nx2Cow::DevicePointer(cur), piece);
        cur += piece;
    }
}

/// A hash of the device memory bytes [addr, addr + size), the first MaxHashBytes of them.
inline u64 HashRange(u64 addr, u64 size, bool& truncated) {
    truncated = size > MaxHashBytes;
    u64 hash = 0x27D4EB2F165667C5ULL ^ size;
    ForEachPiece(addr, (std::min)(size, MaxHashBytes), [&](const u8* host, u64 length) {
        hash = host != nullptr ? HashBytes(host, length, hash) : std::rotl(hash, 7) ^ length;
    });
    return hash;
}

/// Copies [addr, addr + size) (size <= VerifyBytes), unmapped bytes as 0. Returns HashRange of
/// the copied bytes, so the hash and the copy are one snapshot.
inline u64 CopyRange(u64 addr, u64 size, u8* out) {
    u64 hash = 0x27D4EB2F165667C5ULL ^ size;
    ForEachPiece(addr, size, [&](const u8* host, u64 length) {
        if (host != nullptr) {
            std::memcpy(out, host, length);
            hash = HashBytes(out, length, hash);
        } else {
            std::memset(out, 0, length);
            hash = std::rotl(hash, 7) ^ length;
        }
        out += length;
    });
    return hash;
}

inline void Account(bool writer_known, bool covered, bool covered_any, bool stale, u64 size) {
    Add(static_cast<Counter>(Class0 + (writer_known ? 0 : 4) + (covered ? 0 : 2) + (stale ? 1 : 0)));
    Add(Resolved);
    if (stale) {
        Add(StaleBytes, size);
        if (!covered_any) {
            Add(StaleUncoveredAny);
        }
    }
}

/// Caller holds the mutex.
inline void Free(Pending& p) {
    State& s = Get();
    if (p.state == Slot::Writer) {
        --s.writers;
    }
    if (p.state != Slot::Free) {
        --s.active;
    }
    p.state = Slot::Free;
}

/// Caller holds the mutex: p's writer landed, compare the read bytes with them.
inline void Resolve(Pending& p, s64 now) {
    State& s = Get();
    bool truncated = false;
    bool stale = false;
    if (!p.verify) {
        stale = HashRange(p.addr, p.size, truncated) != p.hash;
    } else {
        std::array<u8, VerifyBytes> current{};
        stale = CopyRange(p.addr, p.size, current.data()) != p.hash;
        const bool byte_stale = std::memcmp(current.data(), p.bytes.data(), p.size) != 0;
        if (byte_stale != stale) {
            VerifyDifference("read of {:#x}+{} is {} by its hash, {} by a byte compare", p.addr,
                             p.size, stale ? "stale" : "fresh", byte_stale ? "stale" : "fresh");
            stale = byte_stale;
        }
    }
    if (s.land_n < NumSamples) {
        s.land_after_read[s.land_n++] = now - p.read_ns;
    }
    Account(p.writer_known, Covered(p.tl_key, p.writer_stamp),
            Covered(p.any_key, p.writer_stamp), stale, p.size);
    Free(p);
}

/// A guest read served from preflushed data: [begin, end) its pages, [addr, addr + size) the
/// access (size 0: the pages).
inline void GuestRead(u64 begin, u64 end, u64 addr, u64 size) {
    State& s = Get();
    Add(Reads);
    if (size == 0) {
        addr = begin;
        size = end - begin;
    }
    u64 writer = 0;
    if (!Nx2Ledger::WriterStamp(begin, end, writer) || writer == 0) {
        Add(Untracked);
        return;
    }
    const u32 gen = s.gen.load(std::memory_order_acquire);
    if (gen == 0) {
        Add(NoEpoch);
        return;
    }
    if (!s.prev_known.load(std::memory_order_relaxed)) {
        Add(PrevUnknown);
        return;
    }
    // Frame N's writers came after fence N-1's back increment and up to fence N's.
    u64 fence_stamp = 0;
    const bool passed = IncStamp(s.fence_key.load(std::memory_order_relaxed), fence_stamp);
    if (passed && writer > fence_stamp) {
        Add(AfterFence);
        return;
    }
    const bool writer_known = writer > s.prev_stamp.load(std::memory_order_relaxed);
    if (!writer_known && passed) {
        Add(NoFrameWriter);
        return;
    }
    const s64 now = NowNs();
    const u64 tl_key = t_wait_key;
    const u64 any_key = s.global_wait_key.load(std::memory_order_relaxed);
    if (tl_key != 0) {
        Add(TlWaitReads);
    }
    bool truncated = false;
    const u64 hash = HashRange(addr, size, truncated);
    if (truncated) {
        Add(Truncated);
    }
    std::scoped_lock lock{s.mutex};
    if (s.reply_n < NumSamples) {
        s.read_after_reply[s.reply_n++] = now - s.reply_ns.load(std::memory_order_relaxed);
    }
    if (writer_known && Nx2Ledger::CommitLandNs(writer) != 0) {
        // Landed before the read: the read has its bytes.
        Add(LandedBefore);
        Account(true, Covered(tl_key, writer), Covered(any_key, writer), false, size);
        return;
    }
    if (s.active == NumPending) {
        Add(PendingOverflow);
        return;
    }
    Pending* p = s.pending.data();
    while (p->state != Slot::Free) {
        ++p;
    }
    p->state = writer_known ? Slot::Writer : Slot::NoWriter;
    p->writer_known = writer_known;
    p->gen = gen;
    p->size = static_cast<u32>((std::min<u64>)(size, ~u32{0}));
    p->first_block = begin >> Nx2Ledger::WriterBlockBits;
    p->last_block = (end - 1) >> Nx2Ledger::WriterBlockBits;
    p->addr = addr;
    p->read_ns = now;
    p->hash = hash;
    p->writer_stamp = writer_known ? writer : 0;
    p->tl_key = tl_key;
    p->any_key = any_key;
    p->verify = Nx2Perf::Verify() && size <= VerifyBytes && (s.verify_seq++ % VerifyEvery) == 0;
    if (p->verify) {
        // The hash and the bytes from one copy: a landing since the hash above would otherwise
        // show up as a verify difference.
        p->hash = CopyRange(addr, size, p->bytes.data());
    }
    ++s.active;
    s.writers += writer_known ? 1 : 0;
    Add(Recorded);
}

/// The back recorded a GPU write of [begin, end) with stamp: frame N's while it has not raised
/// the epoch's fence.
inline void Writer(u64 begin, u64 end, u64 stamp) {
    State& s = Get();
    if (stamp == 0 || end <= begin) {
        return;
    }
    std::scoped_lock lock{s.mutex};
    if (s.active == 0) {
        return;
    }
    // This thread raises the fences: once it raised fence N, its writes are a later frame's.
    u64 fence_stamp = 0;
    if (IncStamp(s.fence_key.load(std::memory_order_relaxed), fence_stamp)) {
        return;
    }
    const u32 gen = s.gen.load(std::memory_order_relaxed);
    const u64 first = begin >> Nx2Ledger::WriterBlockBits;
    const u64 last = (end - 1) >> Nx2Ledger::WriterBlockBits;
    for (Pending& p : s.pending) {
        if (p.state == Slot::Free || p.gen != gen || p.last_block < first ||
            p.first_block > last) {
            continue;
        }
        if (p.state == Slot::NoWriter) {
            p.state = Slot::Writer;
            ++s.writers;
        }
        p.writer_stamp = (std::max)(p.writer_stamp, stamp);
    }
}

/// A commit's bytes reached guest memory (CoverLand).
inline void Landed() {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    if (s.writers == 0) {
        return;
    }
    const s64 now = NowNs();
    for (Pending& p : s.pending) {
        if (p.state == Slot::Writer && Nx2Ledger::CommitLandNs(p.writer_stamp) != 0) {
            Resolve(p, now);
        }
    }
}

/// vi2 (Nx2Cow::Begin): a new epoch for fence (id, value); the back passed the previous one.
inline void Begin(u32 id, u32 value, u32 gen) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    for (Pending& p : s.pending) {
        if (p.state == Slot::NoWriter && p.gen < gen) {
            Add(NoFrameWriter);
            Free(p);
        } else if (p.state == Slot::Writer && p.gen + 1 < gen) {
            Add(Unlanded);
            Free(p);
        }
    }
    u64 stamp = 0;
    const bool known = IncStamp(s.fence_key.load(std::memory_order_relaxed), stamp);
    s.prev_stamp.store(stamp, std::memory_order_relaxed);
    s.prev_known.store(known, std::memory_order_relaxed);
    s.fence_key.store(Key(id, value), std::memory_order_relaxed);
    s.gen.store(gen, std::memory_order_release);
}

/// vi2 (Nx2Cow::OpenReaders): the reply.
inline void Open() {
    Get().reply_ns.store(NowNs(), std::memory_order_relaxed);
}

inline void NoteGap(u64 phys, u64 device_page, u8 reply_info, u32 gen) {
    State& s = Get();
    const u32 i = s.gap_count.fetch_add(1, std::memory_order_relaxed);
    if (i >= MaxGaps) {
        Add(GapsDropped);
        return;
    }
    Gap& gap = s.gaps[i];
    gap.phys = phys;
    gap.device_page = static_cast<u32>(device_page);
    gap.info = reply_info;
    gap.classified = false;
    gap.gen.store(gen, std::memory_order_release);
}

/// vi2, after Nx2Cow::Close: the closing epoch's gaps. new_map(device page) tells whether a
/// device mapping made after the arm covers the page.
template <typename NewMap>
void ClassifyGaps(NewMap&& new_map) {
    State& s = Get();
    const Nx2Cow::State& cow = Nx2Cow::Get();
    const u32 gen = cow.gen.load(std::memory_order_acquire);
    const Nx2Cow::TouchSet& set = *cow.touch[gen & 1];
    const u32 n = (std::min)(s.gap_count.load(std::memory_order_acquire), MaxGaps);
    for (u32 i = 0; i < n; ++i) {
        Gap& gap = s.gaps[i];
        const u32 gap_gen = gap.gen.load(std::memory_order_acquire);
        if (gap.classified || gap_gen == 0) {
            continue;
        }
        gap.classified = true;
        if (gap_gen != gen) {
            ++s.gap_unclassified;
            continue;
        }
        u8 code = gap.info;
        if (const u32 slot = set.Find(gap.phys);
            slot != Nx2Cow::SetSlots &&
            (set.bits[slot].load(std::memory_order_relaxed) & Nx2Cow::BitHostWritten) != 0) {
            code |= GapHostWritten;
        }
        if (new_map(gap.device_page)) {
            code |= GapNewMap;
        }
        ++s.gap_codes[code % NumGapCodes];
    }
}

/// vi2, once per frame: the verify count once a second.
inline void FrameClosed() {
    State& s = Get();
    if (!Nx2Perf::Verify()) {
        return;
    }
    const s64 t = NowNs();
    const u64 count = s.verify_differences.load(std::memory_order_relaxed);
    if (count != s.verify_logged && t - s.verify_log_ns >= 1'000'000'000) {
        LOG_INFO(HW_GPU, "Nx2Perf run_ahead_raw_probe: {} verify differences so far", count);
        s.verify_logged = count;
        s.verify_log_ns = t;
    }
}

inline std::string GapName(u8 code) {
    if ((code & InfoValid) == 0) {
        return "unclassified";
    }
    static constexpr std::array<std::string_view, 4> types{"mem", "rcached", "debug", "none"};
    std::string name = fmt::format("{}.{}", (code & InfoArmed) != 0 ? "armed" : "unarmed",
                                   types[code & InfoTypeMask]);
    name += (code & InfoMulti) != 0 ? ".multi" : "";
    name += (code & GapNewMap) != 0 ? ".newmap" : "";
    name += (code & GapHostWritten) != 0 ? ".hostw" : "";
    return name;
}

/// The Nx2RunAheadRaw line for a window of frames; sites are (hits, address), most hits first.
inline std::string Report(size_t frames, u64 unarmed_changed,
                          std::span<const std::pair<u64, uintptr_t>> sites) {
    State& s = Get();
    const double n = frames == 0 ? 1.0 : static_cast<double>(frames);
    const auto per = [n](u64 value) { return static_cast<double>(value) / n; };
    std::array<u64, NumCounters> v{};
    for (size_t i = 0; i < NumCounters; ++i) {
        v[i] = s.c[i].exchange(0, std::memory_order_relaxed);
    }
    const auto pct = [](std::array<s64, NumSamples>& values, size_t count,
                        size_t percent) -> double {
        if (count == 0) {
            return 0.0;
        }
        const size_t index = (percent * count + 99) / 100 - 1;
        std::nth_element(values.begin(), values.begin() + index, values.begin() + count);
        return static_cast<double>(values[index]) / 1e6;
    };
    double land50 = 0, land90 = 0, reply50 = 0, reply90 = 0;
    {
        std::scoped_lock lock{s.mutex};
        land50 = pct(s.land_after_read, s.land_n, 50);
        land90 = pct(s.land_after_read, s.land_n, 90);
        reply50 = pct(s.read_after_reply, s.reply_n, 50);
        reply90 = pct(s.read_after_reply, s.reply_n, 90);
        s.land_n = 0;
        s.reply_n = 0;
    }
    static constexpr std::array<std::string_view, NumClasses> class_names{
        "known.cov.fresh",    "known.cov.stale",    "known.unc.fresh",    "known.unc.stale",
        "nowriter.cov.fresh", "nowriter.cov.stale", "nowriter.unc.fresh", "nowriter.unc.stale"};
    std::string classes;
    for (size_t i = 0; i < NumClasses; ++i) {
        classes += fmt::format(" {}={:.2f}", class_names[i], per(v[Class0 + i]));
    }
    const u64 stale = v[Class0 + 1] + v[Class0 + 3] + v[Class0 + 5] + v[Class0 + 7];
    const u64 stale_uncovered = v[Class0 + 3] + v[Class0 + 7];
    const u64 stale_no_writer = v[Class0 + 5] + v[Class0 + 7];
    // Gap classes, most pages first.
    std::array<std::pair<u64, u8>, NumGapCodes> codes{};
    u64 sampled = s.gap_unclassified;
    for (size_t i = 0; i < NumGapCodes; ++i) {
        codes[i] = {s.gap_codes[i], static_cast<u8>(i)};
        sampled += s.gap_codes[i];
    }
    std::sort(codes.begin(), codes.end(), std::greater<>{});
    std::string gaps;
    size_t gap_classes = 0;
    for (const auto& [count, code] : codes) {
        if (count != 0) {
            ++gap_classes;
            gaps += fmt::format(" {}:{}", GapName(code), count);
        }
    }
    const u64 gap_unclassified = s.gap_unclassified;
    s.gap_codes.fill(0);
    s.gap_unclassified = 0;
    for (Gap& gap : s.gaps) {
        gap.gen.store(0, std::memory_order_relaxed);
    }
    s.gap_count.store(0, std::memory_order_release);
    // Unsized read sites with pre-imaged hits, as RVAs of the executable.
    const uintptr_t base = s.module_base.load(std::memory_order_relaxed);
    std::string top;
    size_t shown = 0;
    for (const auto& [hits, address] : sites) {
        if (hits == 0 || shown == 8) {
            continue;
        }
        ++shown;
        if (base != 0 && address >= base) {
            top += fmt::format(" {:#x}:{}", address - base, hits);
        } else {
            top += fmt::format(" abs{:#x}:{}", address, hits);
        }
    }
    return fmt::format(
        "Nx2RunAheadRaw: frames={} reads={:.1f}/frame frame_n_reads={:.2f}/frame "
        "resolved={:.2f}/frame class(per frame; writer known/no writer yet at the read x "
        "covered/uncovered by the reading thread's last wait x fresh/stale):{} stale={:.2f}/frame "
        "stale_bytes={:.1f}/frame stale_uncovered={:.2f}/frame "
        "stale_no_writer_yet={:.2f}/frame stale_uncovered_any_wait={:.2f}/frame "
        "landed_before_read={} land_after_read_ms p50/p90={:.2f}/{:.2f} "
        "read_after_reply_ms p50/p90={:.2f}/{:.2f} tl_wait_reads={} untracked={} no_epoch={} "
        "prev_unknown={} after_fence={} no_frame_writer={} unlanded={} pending_overflow={} "
        "truncated={} gaps(sampled {} of {} unarmed_changed, {} classes; armed/type/multi at "
        "the reply, newmap/hostw at the close):{} gap_dropped={} gap_unclassified={} "
        "unsized_sites_rva(base {:#x}):{} verify_differences={}",
        frames, per(v[Reads]), per(v[Recorded] + v[LandedBefore]), per(v[Resolved]), classes,
        per(stale), per(v[StaleBytes]), per(stale_uncovered), per(stale_no_writer),
        per(v[StaleUncoveredAny]), v[LandedBefore], land50, land90, reply50, reply90,
        v[TlWaitReads], v[Untracked], v[NoEpoch], v[PrevUnknown], v[AfterFence],
        v[NoFrameWriter], v[Unlanded], v[PendingOverflow], v[Truncated], sampled,
        unarmed_changed, gap_classes, gaps, v[GapsDropped], gap_unclassified, base, top,
        s.verify_differences.load(std::memory_order_relaxed));
}

} // namespace VideoCommon::Nx2Raw

// Nx2Perf run_ahead_snapshot_probe (measurement only, needs run_ahead_cow_probe and frames_ahead).
// At vi2's reply for present N, S_N is the union of the pages the front and back read in the last
// 16 epochs (the cow probe's touch sets and the ledger's frame sets) plus the tracker pages the
// guest wrote since their upload (the war_guard arm set). vi2 copies S_N into a fixed pool and
// hashes each copy. Each hazard the cow probe finds (bytes an epoch-N read gets that differ from
// the page's pre-image) is classed (a) covered: the page is in S_N and its copy equals the
// pre-image (the page as of the reply's arm); (b) trap-covered: its first write after the reply
// trapped through HandleRasterizerWrite; (c) residual. It also times the guest's first read of
// GPU-written data after the reply. Nothing waits and no read is redirected.
namespace VideoCommon::Nx2Snap {

inline bool On() {
    static const bool on = [] {
        if (!Nx2Perf::On(Nx2Perf::Opt::RunAheadSnapshotProbe)) {
            return false;
        }
        if (!Nx2Cow::On() || !Nx2Perf::On(Nx2Perf::Opt::FramesAhead)) {
            LOG_ERROR(HW_GPU, "Nx2Perf run_ahead_snapshot_probe: needs frames_ahead, "
                              "run_ahead_ledger and run_ahead_cow_probe on; the probe stays off");
            return false;
        }
        return true;
    }();
    return on;
}

// Nx2Perf run_ahead_overlay_budget (measurement only, with the snapshot probe): at the reply, the
// union pages a guest write would not trap on (the copy set without the arm set) and a timed copy
// of them; at the close, the arm pages read and changed this epoch and whether each change
// trapped; the hazard bytes split by S_N source, read through multi-page spans, and the
// front_macro residual's page category. Nothing waits, nothing is protected.
inline bool BudgetOn() {
    static const bool on = [] {
        if (!Nx2Perf::On(Nx2Perf::Opt::RunAheadOverlayBudget)) {
            return false;
        }
        if (!On()) {
            LOG_WARNING(HW_GPU, "Nx2Perf run_ahead_overlay_budget: needs frames_ahead, "
                                "run_ahead_ledger, run_ahead_cow_probe and "
                                "run_ahead_snapshot_probe on; the probe stays off");
            return false;
        }
        return true;
    }();
    return on;
}

inline constexpr u64 PageBits = Nx2Cow::PageBits;
inline constexpr u64 PageSize = Nx2Cow::PageSize;
inline constexpr size_t K = 16;
inline constexpr u32 RingPages = 16384; ///< Pages one ring epoch keeps
inline constexpr u32 PoolPages = 8192;  ///< 32 MiB, allocated once (S_N measured ~3-4k pages)
inline constexpr u32 TableSlots = 65536;
inline constexpr u32 TableLimit = TableSlots / 4 * 3;
inline constexpr u32 MaxArmPages = 131072; ///< The war_guard set's capacity
inline constexpr u32 NoPool = ~0U;
inline constexpr size_t NumSites = 32;
inline constexpr u32 WindowFrames = Nx2Cow::WindowFrames;
static_assert(TableSlots == Nx2Cow::SetSlots, "Table shares TouchSet::Home");

inline constexpr u8 FromUnion = 1;  ///< Read in one of the last K epochs
inline constexpr u8 FromArmSet = 2; ///< In the war_guard arm set at the reply

inline constexpr u8 PathTracked = 1;   ///< Pre-imaged through HandleRasterizerWrite
inline constexpr u8 PathUntracked = 2; ///< Pre-imaged by the cow arm's trap

inline constexpr u8 VerdictUnknown = 0;
inline constexpr u8 VerdictEqual = 1;
inline constexpr u8 VerdictDiffers = 2;

enum ResCat : size_t {
    Bank,
    UFast,
    UCached,
    Storage,
    Desc,
    OtherCat,
    FrontList,
    FrontMacro,
    NumResCats,
};
inline constexpr std::array<std::string_view, NumResCats> res_cat_names{
    "bank", "ufast", "ucached", "storage", "desc", "other", "front_list", "front_macro"};

enum Tracker : size_t { Untracked, Unregistered, HostWritten, NumTrackers };
inline constexpr std::array<std::string_view, NumTrackers> tracker_names{"untracked",
                                                                         "unregistered", "hostw"};

enum Reason : size_t { Miss, Stale, PoolFull, NoSnapshot, NumReasons };
inline constexpr std::array<std::string_view, NumReasons> reason_names{"miss", "stale",
                                                                       "pool_full", "no_snapshot"};

/// run_ahead_overlay_budget: the page of a front_macro residual.
enum ResidCat : size_t { Vi2Delta, Download, PreCopy, OtherResid, NumResidCats };
inline constexpr std::array<std::string_view, NumResidCats> resid_cat_names{
    "vi2_delta", "download", "pre_copy", "other"};
inline constexpr u32 NumResidRecords = 64;
inline constexpr size_t HostBitWords = Nx2Cow::NumPhysPages / 64;
inline constexpr size_t NumHostBitSets = 3; ///< By gen % 3: late readers keep their set

enum Counter : size_t {
    BytesA,
    BytesB,
    BytesC,
    ResCat0,
    ResTracker0 = ResCat0 + NumResCats,
    ResReason0 = ResTracker0 + NumTrackers,
    TrappedGpuReads = ResReason0 + NumReasons,
    UntrappedGpuReads,
    UntrappedPreReply, ///< Its kept area is older than the reply
    SiteOverflow,
    // run_ahead_overlay_budget
    BytesAUnion,
    BytesAArm,
    BytesABoth,
    SpanGetSpan, ///< Hazard bytes read through a GetSpan over several pages
    SpanBank,    ///< Hazard bytes of bank compare reads (the contiguous base_ptr path)
    Resid0,      ///< + ResidCat: front_macro residual bytes
    ResidEvents0 = Resid0 + NumResidCats,
    SpanSiteOverflow = ResidEvents0 + NumResidCats,
    NumCounters,
};

/// run_ahead_overlay_budget: one front_macro residual, logged by vi2.
struct ResidRecord {
    std::atomic<bool> ready{false};
    u64 device_addr = 0;
    u64 phys = 0;
    u64 bytes = 0;
    uintptr_t site = 0;
    u32 gen = 0;
    u8 cat = 0;
};

/// S_N of one epoch, keyed by physical page; vi2 builds it before the readers open.
struct Table {
    std::atomic<u32> gen{0}; ///< The epoch it holds, 0 while vi2 builds it
    std::array<std::atomic<u32>, TableSlots> key{}; ///< phys + 1, 0 when empty
    std::array<u8, TableSlots> from{};
    std::array<u32, TableSlots> pool{}; ///< Pool page or NoPool
    std::array<u64, TableSlots> pool_hash{};
    std::array<std::atomic<u8>, TableSlots> verdict{};
    std::array<std::atomic<u64>, TableSlots> ref_hash{}; ///< HashPage of the pre-image
    std::array<u32, TableSlots> used{};
    u32 used_count = 0;
    u64 overflow = 0;
    std::array<u64, TableSlots> sorted{}; ///< Verify: phys << 32 | slot, sorted
    u32 sorted_count = 0;

    void Clear() {
        for (u32 i = 0; i < used_count; ++i) {
            key[used[i]].store(0, std::memory_order_relaxed);
        }
        used_count = 0;
        sorted_count = 0;
        overflow = 0;
    }

    void Insert(u64 phys, u8 source) {
        const u32 want = static_cast<u32>(phys) + 1;
        for (u32 slot = Nx2Cow::TouchSet::Home(want);; slot = (slot + 1) & (TableSlots - 1)) {
            const u32 current = key[slot].load(std::memory_order_relaxed);
            if (current == want) {
                from[slot] |= source;
                return;
            }
            if (current != 0) {
                continue;
            }
            if (used_count >= TableLimit) {
                ++overflow;
                return;
            }
            from[slot] = source;
            pool[slot] = NoPool;
            pool_hash[slot] = 0;
            verdict[slot].store(VerdictUnknown, std::memory_order_relaxed);
            ref_hash[slot].store(0, std::memory_order_relaxed);
            key[slot].store(want, std::memory_order_relaxed);
            used[used_count++] = slot;
            return;
        }
    }

    u32 Find(u64 phys) const {
        const u32 want = static_cast<u32>(phys) + 1;
        u32 slot = Nx2Cow::TouchSet::Home(want);
        for (u32 probes = 0; probes < TableSlots; ++probes, slot = (slot + 1) & (TableSlots - 1)) {
            const u32 current = key[slot].load(std::memory_order_relaxed);
            if (current == want) {
                return slot;
            }
            if (current == 0) {
                return TableSlots;
            }
        }
        return TableSlots;
    }

    /// Verify's old answer: a binary search of the sorted pages.
    u32 FindSorted(u64 phys) const {
        const u64* const begin = sorted.data();
        const u64* const end = begin + sorted_count;
        const u64* const it = std::lower_bound(begin, end, phys << 32);
        return it != end && (*it >> 32) == phys ? static_cast<u32>(*it) : TableSlots;
    }
};

struct State {
    std::atomic<bool> ready{false}; ///< The pool and tables exist (vi2 sets it once)
    std::unique_ptr<u8[]> pool;
    std::array<std::unique_ptr<Table>, 2> tables; ///< By gen & 1
    std::array<std::atomic<u8>, Nx2Cow::PoolSlots> pre_path{}; ///< Per cow pool slot (Path*)
    std::atomic<s64> reply_ns{0};
    std::atomic<u32> first_claim{0};
    std::atomic<u32> first_done{0};
    std::atomic<s64> first_ns{0};
    std::array<std::atomic<u64>, NumCounters> c{};
    std::array<std::atomic<uintptr_t>, NumSites> site_addr{};
    std::array<std::atomic<u64>, NumSites> site_bytes{};
    std::atomic<uintptr_t> module_base{0};
    std::atomic<u64> verify_differences{0};

    // run_ahead_overlay_budget
    std::atomic<bool> budget_ready{false}; ///< host_bits exist (vi2 sets it once)
    std::array<std::atomic<u8>, Nx2Cow::PoolSlots> pre_early{}; ///< Trapped before S_N was copied
    std::array<std::unique_ptr<std::atomic<u64>[]>, NumHostBitSets> host_bits; ///< Physical pages
    std::atomic<u32> host_target{0}; ///< The set host writes go to
    std::atomic<u64> vi2_delta_phys{~u64{0}};
    std::array<std::atomic<uintptr_t>, NumSites> span_site_addr{};
    std::array<std::atomic<u64>, NumSites> span_site_bytes{};
    std::array<ResidRecord, NumResidRecords> records{};
    std::atomic<u32> record_claim{0};
    std::atomic<u64> budget_verify_differences{0};
};

inline State& Get() {
    static State state;
    return state;
}

inline s64 NowNs() {
    return Nx2WarProbe::NowNs();
}

template <typename... Args>
void BudgetVerifyDifference(fmt::format_string<Args...> format, Args&&... args) {
    const u64 count = Get().budget_verify_differences.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 64) {
        LOG_ERROR(HW_GPU, "Nx2Perf run_ahead_overlay_budget: {} ({} so far)",
                  fmt::format(format, std::forward<Args>(args)...), count);
    }
}

/// Device memory and the GPU's memory manager, before a host write lands (any time).
inline void HostWritePages(u64 device_addr, u64 size) {
    State& s = Get();
    if (size == 0 || !s.budget_ready.load(std::memory_order_acquire)) {
        return;
    }
    std::atomic<u64>* const bits =
        s.host_bits[s.host_target.load(std::memory_order_acquire) % NumHostBitSets].get();
    const u64 end = device_addr + size;
    for (u64 page = device_addr >> PageBits; page <= (end - 1) >> PageBits; ++page) {
        const u8* const host = Nx2Cow::DevicePointer(page << PageBits);
        if (host == nullptr) {
            continue;
        }
        const u64 phys = Nx2Cow::PhysOf(host);
        if (phys < Nx2Cow::NumPhysPages) {
            bits[phys >> 6].fetch_or(u64{1} << (phys & 63), std::memory_order_relaxed);
        }
    }
}

/// Whether a host write reached phys between the replies before and at gen.
inline bool HostWrittenBefore(u32 gen, u64 phys) {
    State& s = Get();
    if (!s.budget_ready.load(std::memory_order_acquire) || phys >= Nx2Cow::NumPhysPages) {
        return false;
    }
    return ((s.host_bits[gen % NumHostBitSets][phys >> 6].load(std::memory_order_relaxed) >>
             (phys & 63)) &
            1) != 0;
}

/// vi2, right after OoT's frame-delta Write32 (before the reply): the page it wrote.
inline void NoteVi2Delta(u64 phys) {
    Get().vi2_delta_phys.store(phys, std::memory_order_relaxed);
}

// Nx2Perf run_ahead_own_write_probe (measurement only, with the overlay budget): the byte ranges
// the emulated GPU writes into guest memory each epoch (the fencing thread, the split back, query
// results, downloads). The front_macro residual and the unarmed_changed pages are split into
// bytes inside those ranges (GPU-own, a write-through overlay covers them) and the rest (guest).
inline bool OwnOn() {
    static const bool on = [] {
        if (!Nx2Perf::On(Nx2Perf::Opt::RunAheadOwnWriteProbe)) {
            return false;
        }
        if (!BudgetOn()) {
            LOG_WARNING(HW_GPU, "Nx2Perf run_ahead_own_write_probe: needs frames_ahead, "
                                "run_ahead_ledger, run_ahead_cow_probe, run_ahead_snapshot_probe "
                                "and run_ahead_overlay_budget on; the probe stays off");
            return false;
        }
        return true;
    }();
    return on;
}

enum class OwnClass : u8 { Own, Guest, Unknown };

/// The own ranges of one page in one epoch, as OwnRangeTable::Find saw them.
struct OwnPage {
    static constexpr u32 MaxRanges = 8;
    std::array<u16, MaxRanges> begin{};
    std::array<u16, MaxRanges> end{};
    u32 count = 0;
    bool overflow = false; ///< The page had more ranges than it keeps
    bool lost = false;     ///< Not found, and an insert found no slot this epoch

    bool Contains(u32 offset) const {
        for (u32 i = 0; i < count; ++i) {
            if (offset >= begin[i] && offset < end[i]) {
                return true;
            }
        }
        return false;
    }

    OwnClass Classify(u32 offset) const {
        if (Contains(offset)) {
            return OwnClass::Own;
        }
        return overflow || lost ? OwnClass::Unknown : OwnClass::Guest;
    }
};

/// Byte ranges GPU-own writers wrote, keyed by physical page and epoch: open addressing,
/// lock-free inserts, fixed size. Overlapping or touching ranges merge.
struct OwnRangeTable {
    static constexpr u32 Slots = 4096;
    static constexpr u32 MaxProbes = 64;
    static constexpr u32 MaxRanges = OwnPage::MaxRanges;
    static constexpr u32 PageBytes = 4096;

    struct Slot {
        std::atomic<u64> key{0}; ///< gen << 32 | phys + 1, 0 when empty
        std::atomic<u32> count{0};
        std::atomic<u32> overflow{0};
        std::array<std::atomic<u32>, MaxRanges> range{}; ///< Pack(gen, begin, end), 0 unset
    };

    std::array<Slot, Slots> slots{};
    std::array<std::atomic<u32>, Slots> used{};
    std::atomic<u32> used_count{0};
    std::atomic<u32> lost_gen{0}; ///< gen + 1 of an insert that found no slot
    std::atomic<u64> table_overflow{0};
    std::atomic<u64> range_overflow{0};

    static u64 Key(u64 phys, u32 gen) {
        return (u64{gen} << 32) | (phys + 1);
    }
    static u32 Home(u64 key) {
        static_assert(Slots == 1U << 12);
        return static_cast<u32>((key * 0x9E3779B97F4A7C15ULL) >> 52);
    }
    static u32 Pack(u32 gen, u32 begin, u32 end) {
        return ((gen & 63) << 26) | (begin << 13) | end;
    }
    static u32 Tag(u32 range) {
        return range >> 26;
    }
    static u32 Begin(u32 range) {
        return (range >> 13) & 0x1fff;
    }
    static u32 End(u32 range) {
        return range & 0x1fff;
    }

    /// [begin, end) of phys was written in gen. Returns false when no slot was free.
    bool Insert(u64 phys, u32 gen, u32 begin, u32 end) {
        if (begin >= end || end > PageBytes) {
            return true;
        }
        const u64 want = Key(phys, gen);
        u32 slot = Home(want);
        for (u32 probes = 0; probes < MaxProbes; ++probes, slot = (slot + 1) & (Slots - 1)) {
            u64 current = slots[slot].key.load(std::memory_order_acquire);
            if (current == 0 &&
                slots[slot].key.compare_exchange_strong(current, want, std::memory_order_acq_rel)) {
                const u32 n = used_count.fetch_add(1, std::memory_order_acq_rel);
                used[n & (Slots - 1)].store(slot, std::memory_order_release);
                current = want;
            }
            if (current == want) {
                AddRange(slots[slot], gen, begin, end);
                return true;
            }
        }
        table_overflow.fetch_add(1, std::memory_order_relaxed);
        lost_gen.store(gen + 1, std::memory_order_release);
        return false;
    }

    OwnPage Find(u64 phys, u32 gen) const {
        OwnPage page;
        const u64 want = Key(phys, gen);
        u32 slot = Home(want);
        for (u32 probes = 0; probes < MaxProbes; ++probes, slot = (slot + 1) & (Slots - 1)) {
            const u64 current = slots[slot].key.load(std::memory_order_acquire);
            if (current == want) {
                const Slot& e = slots[slot];
                page.overflow = e.overflow.load(std::memory_order_acquire) != 0;
                const u32 n = (std::min)(e.count.load(std::memory_order_acquire), MaxRanges);
                for (u32 i = 0; i < n; ++i) {
                    const u32 range = e.range[i].load(std::memory_order_acquire);
                    if (range == 0 || Tag(range) != (gen & 63)) {
                        continue;
                    }
                    page.begin[page.count] = static_cast<u16>(Begin(range));
                    page.end[page.count] = static_cast<u16>(End(range));
                    ++page.count;
                }
                return page;
            }
            if (current == 0) {
                break;
            }
        }
        page.lost = lost_gen.load(std::memory_order_acquire) == gen + 1;
        return page;
    }

    /// vi2, for a table no writer of the epoch it holds is expected to use any more.
    void Clear() {
        const u32 n = (std::min)(used_count.load(std::memory_order_acquire), Slots);
        for (u32 i = 0; i < n; ++i) {
            Slot& e = slots[used[i].load(std::memory_order_acquire)];
            e.count.store(0, std::memory_order_relaxed);
            e.overflow.store(0, std::memory_order_relaxed);
            for (auto& range : e.range) {
                range.store(0, std::memory_order_relaxed);
            }
            e.key.store(0, std::memory_order_release);
        }
        used_count.store(0, std::memory_order_release);
        lost_gen.store(0, std::memory_order_release);
    }

private:
    void AddRange(Slot& e, u32 gen, u32 begin, u32 end) {
        const u32 tag = gen & 63;
        const u32 n = (std::min)(e.count.load(std::memory_order_acquire), MaxRanges);
        for (u32 i = 0; i < n; ++i) {
            u32 range = e.range[i].load(std::memory_order_acquire);
            while (range != 0 && Tag(range) == tag && begin <= End(range) && end >= Begin(range)) {
                const u32 merged_begin = (std::min)(begin, Begin(range));
                const u32 merged_end = (std::max)(end, End(range));
                if (merged_begin == Begin(range) && merged_end == End(range)) {
                    return;
                }
                if (e.range[i].compare_exchange_weak(range, Pack(gen, merged_begin, merged_end),
                                                     std::memory_order_acq_rel)) {
                    return;
                }
            }
        }
        const u32 index = e.count.fetch_add(1, std::memory_order_acq_rel);
        if (index >= MaxRanges) {
            e.overflow.store(1, std::memory_order_release);
            range_overflow.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        e.range[index].store(Pack(gen, begin, end), std::memory_order_release);
    }
};

/// The ring's own writes of one page in one epoch, newest first (Verify).
struct OwnRefs {
    static constexpr u32 Max = 64;
    std::array<u16, Max> begin{};
    std::array<u16, Max> end{};
    std::array<u64, Max> data{};
    u32 count = 0;
    bool complete = false; ///< Every own write of the page in the epoch is here

    int Covering(u32 offset) const {
        for (u32 i = 0; i < count; ++i) {
            if (offset >= begin[i] && offset < end[i]) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
};

/// Verify: each own write's page, range and bytes, newest last; old writes are overwritten.
struct OwnRing {
    static constexpr u64 Entries = 65536;
    static constexpr u64 Bytes = u64{8} << 20;
    static constexpr u64 NoData = ~u64{0};

    struct Entry {
        std::atomic<u64> seq{0}; ///< Claim index + 1 once written
        std::atomic<u64> key{0};
        std::atomic<u32> range{0}; ///< begin << 16 | end
        std::atomic<u64> data{0};  ///< Byte position, or NoData
    };

    std::unique_ptr<Entry[]> entries = std::make_unique<Entry[]>(Entries);
    std::unique_ptr<u8[]> bytes = std::make_unique<u8[]>(Bytes);
    std::atomic<u64> claim{0};
    std::atomic<u64> byte_claim{0};

    void Push(u64 phys, u32 gen, u32 begin, u32 end, const u8* src) {
        const u64 n = claim.fetch_add(1, std::memory_order_acq_rel);
        u64 data = NoData;
        if (src != nullptr) {
            const u64 len = end - begin;
            data = byte_claim.fetch_add(len, std::memory_order_acq_rel);
            const u64 at = data % Bytes;
            const u64 first = (std::min)(len, Bytes - at);
            std::memcpy(bytes.get() + at, src, first);
            std::memcpy(bytes.get(), src + first, len - first);
        }
        Entry& e = entries[n % Entries];
        e.seq.store(0, std::memory_order_release);
        e.key.store(OwnRangeTable::Key(phys, gen), std::memory_order_relaxed);
        e.range.store((begin << 16) | end, std::memory_order_relaxed);
        e.data.store(data, std::memory_order_relaxed);
        e.seq.store(n + 1, std::memory_order_release);
    }

    OwnRefs Collect(u64 phys, u32 gen) const {
        OwnRefs refs;
        const u64 want = OwnRangeTable::Key(phys, gen);
        const u64 head = claim.load(std::memory_order_acquire);
        const u64 oldest = head > Entries ? head - Entries : 0;
        refs.complete = oldest == 0;
        for (u64 n = head; n > oldest; --n) {
            const Entry& e = entries[(n - 1) % Entries];
            if (e.seq.load(std::memory_order_acquire) != n) {
                continue;
            }
            const u64 key = e.key.load(std::memory_order_relaxed);
            const u32 range = e.range.load(std::memory_order_relaxed);
            const u64 data = e.data.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (e.seq.load(std::memory_order_relaxed) != n) {
                continue;
            }
            // A writer that read gen - 1 just before vi2's Begin can push after early gen
            // writes (fence releases land right at the reply): stop two epochs back.
            if (static_cast<u32>(key >> 32) + 1 < gen) {
                refs.complete = true; // Reached an older epoch
                break;
            }
            if (key != want) {
                continue;
            }
            if (refs.count == OwnRefs::Max) {
                refs.complete = false;
                break;
            }
            refs.begin[refs.count] = static_cast<u16>(range >> 16);
            refs.end[refs.count] = static_cast<u16>(range & 0xffff);
            refs.data[refs.count] = data;
            ++refs.count;
        }
        return refs;
    }

    /// The byte the ref's write stored at offset; false when not kept or overwritten since.
    bool ByteOf(const OwnRefs& refs, int ref, u32 offset, u8& out) const {
        const u64 data = refs.data[ref];
        if (data == NoData) {
            return false;
        }
        const u64 position = data + (offset - refs.begin[ref]);
        out = bytes[position % Bytes];
        std::atomic_thread_fence(std::memory_order_acquire);
        return byte_claim.load(std::memory_order_acquire) <= position + Bytes;
    }
};

inline constexpr size_t NumOwnTables = 3; ///< By gen % 3: late readers keep their table

enum OwnCounter : size_t {
    OwnWrites,
    OwnBytes,
    OwnPointerWrites,
    OwnPointerBytes,
    OwnOutsideBytes, ///< Own bytes outside the physical backing (not recorded)
    OwnResidOwn,
    OwnResidGuest,
    OwnResidUnknown,
    Who2Writes, ///< Host writes by a thread without a GPU role, not GPU-own, epoch open
    Who2Bytes,
    Who2SiteOverflow,
    FrontNonOwnBytes,
    OwnVerifyChecked,
    OwnVerifyUnchecked,
    OwnVerifyDifferences,
    NumOwnCounters,
};

struct OwnState {
    std::mutex init_mutex;
    std::array<std::unique_ptr<OwnRangeTable>, NumOwnTables> tables;
    std::unique_ptr<OwnRing> ring; ///< Verify only
    std::array<std::atomic<u64>, NumOwnCounters> c{};
    std::array<std::atomic<uintptr_t>, NumSites> site_addr{};
    std::array<std::atomic<u64>, NumSites> site_bytes{};
};

inline OwnState& OwnGet() {
    static OwnState state;
    return state;
}

inline void OwnAdd(OwnCounter counter, u64 amount = 1) {
    OwnGet().c[counter].fetch_add(amount, std::memory_order_relaxed);
}

template <typename... Args>
void OwnVerifyDifference(fmt::format_string<Args...> format, Args&&... args) {
    const u64 count =
        OwnGet().c[OwnVerifyDifferences].fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 64) {
        LOG_ERROR(HW_GPU, "Nx2Perf run_ahead_own_write_probe: {} ({} so far)",
                  fmt::format(format, std::forward<Args>(args)...), count);
    }
}

/// vi2, once: the tables (and the Verify ring); the hooks record from then on.
inline void OwnInit() {
    OwnState& o = OwnGet();
    std::scoped_lock lock{o.init_mutex};
    if (g_own.load(std::memory_order_acquire)) {
        return;
    }
    for (auto& table : o.tables) {
        table = std::make_unique<OwnRangeTable>();
    }
    if (Nx2Perf::Verify()) {
        o.ring = std::make_unique<OwnRing>();
    }
    g_own.store(true, std::memory_order_release);
}

inline void OwnRecord(u64 phys, u64 begin, u64 end, const u8* src) {
    OwnState& o = OwnGet();
    const u32 gen = Nx2Cow::Get().gen.load(std::memory_order_acquire);
    o.tables[gen % NumOwnTables]->Insert(phys, gen, static_cast<u32>(begin),
                                         static_cast<u32>(end));
    if (o.ring) [[unlikely]] {
        o.ring->Push(phys, gen, static_cast<u32>(begin), static_cast<u32>(end), src);
    }
}

inline void NoteWho2Site(const void* site, u64 bytes) {
    OwnState& o = OwnGet();
    const uintptr_t address = site == nullptr ? 1 : reinterpret_cast<uintptr_t>(site);
    size_t i = static_cast<size_t>((u64{address} * 0x9E3779B97F4A7C15ULL) >> 59);
    for (size_t probes = 0; probes < NumSites; ++probes, i = (i + 1) % NumSites) {
        uintptr_t current = o.site_addr[i].load(std::memory_order_acquire);
        if (current == 0 &&
            o.site_addr[i].compare_exchange_strong(current, address, std::memory_order_acq_rel)) {
            current = address;
        }
        if (current == address) {
            o.site_bytes[i].fetch_add(bytes, std::memory_order_relaxed);
            return;
        }
    }
    OwnAdd(Who2SiteOverflow);
}

/// Nx2Cow::HostWriteSlow: GPU-own writes record their ranges; the others count by site.
inline void OwnHostWrite(u64 device_addr, u64 size, const void* src, const void* site) {
    if (size == 0 || !g_own.load(std::memory_order_acquire)) {
        return;
    }
    if (!t_gpu_own) {
        if (Nx2Cow::Get().open.load(std::memory_order_acquire)) {
            if (Nx2Cow::t_role == Nx2Cow::Role::None) {
                OwnAdd(Who2Writes);
                OwnAdd(Who2Bytes, size);
                NoteWho2Site(site, size);
            } else if (Nx2Cow::t_role == Nx2Cow::Role::Front) {
                OwnAdd(FrontNonOwnBytes, size);
            }
        }
        return;
    }
    OwnAdd(OwnWrites);
    OwnAdd(OwnBytes, size);
    const u8* const bytes = static_cast<const u8*>(src);
    const u64 end = device_addr + size;
    for (u64 page = device_addr >> PageBits; page <= (end - 1) >> PageBits; ++page) {
        const u64 first = (std::max)(device_addr, page << PageBits);
        const u64 last = (std::min)(end, (page + 1) << PageBits);
        const u8* const host = Nx2Cow::DevicePointer(page << PageBits);
        const u64 phys = host != nullptr ? Nx2Cow::PhysOf(host) : Nx2Cow::NumPhysPages;
        if (phys >= Nx2Cow::NumPhysPages) {
            OwnAdd(OwnOutsideBytes, last - first);
            continue;
        }
        OwnRecord(phys, first - (page << PageBits), last - (page << PageBits),
                  bytes != nullptr ? bytes + (first - device_addr) : nullptr);
    }
}

/// NoteOwnPointer: a raw store of GPU results at a host pointer into the physical backing.
inline void OwnPointerWrite(const void* host, u64 size, const void* src) {
    if (host == nullptr || size == 0 || !g_own.load(std::memory_order_acquire)) {
        return;
    }
    OwnAdd(OwnPointerWrites);
    OwnAdd(OwnPointerBytes, size);
    const uintptr_t base = Nx2Cow::Get().physical_base.load(std::memory_order_relaxed);
    const uintptr_t at = reinterpret_cast<uintptr_t>(host);
    if (base == 0 || at < base) {
        OwnAdd(OwnOutsideBytes, size);
        return;
    }
    const u8* const bytes = static_cast<const u8*>(src);
    const u64 offset = at - base;
    const u64 end = offset + size;
    for (u64 page = offset >> PageBits; page <= (end - 1) >> PageBits; ++page) {
        const u64 first = (std::max)(offset, page << PageBits);
        const u64 last = (std::min)(end, (page + 1) << PageBits);
        if (page >= Nx2Cow::NumPhysPages) {
            OwnAdd(OwnOutsideBytes, last - first);
            continue;
        }
        OwnRecord(page, first - (page << PageBits), last - (page << PageBits),
                  bytes != nullptr ? bytes + (first - offset) : nullptr);
    }
}

/// The class of each byte of one page that differs from its reference copy. Under Verify the
/// ring's newest own write of the byte is the old answer: a byte classed own must still hold
/// what that write stored.
class OwnPageCheck {
public:
    OwnPageCheck(u64 phys_, u32 gen_)
        : phys{phys_}, gen{gen_}, page{OwnGet().tables[gen_ % NumOwnTables]->Find(phys_, gen_)} {}

    OwnClass Byte(u32 offset, u8 now) {
        const OwnClass cls = page.Classify(offset);
        const OwnState& o = OwnGet();
        if (!o.ring || cls == OwnClass::Unknown) [[likely]] {
            return cls;
        }
        if (!have_refs) {
            refs = o.ring->Collect(phys, gen);
            have_refs = true;
        }
        const int ref = refs.Covering(offset);
        if (ref < 0) {
            if (cls == OwnClass::Own) {
                if (refs.complete) {
                    OwnVerifyDifference("page {:#x}+{:#x} epoch {}: the range table says own, "
                                        "the write ring has no own write of it",
                                        phys << PageBits, offset, gen);
                    return OwnClass::Guest;
                }
                OwnAdd(OwnVerifyUnchecked);
            }
            return cls;
        }
        if (cls == OwnClass::Guest) {
            OwnVerifyDifference("page {:#x}+{:#x} epoch {}: the range table says guest, the "
                                "write ring has an own write of it",
                                phys << PageBits, offset, gen);
        }
        u8 stored = 0;
        if (!o.ring->ByteOf(refs, ref, offset, stored)) {
            OwnAdd(OwnVerifyUnchecked);
            return OwnClass::Own;
        }
        if (stored != now) {
            OwnVerifyDifference("page {:#x}+{:#x} epoch {}: classed own but holds {:#04x}, the "
                                "newest own write stored {:#04x}",
                                phys << PageBits, offset, gen, now, stored);
            return OwnClass::Guest;
        }
        OwnAdd(OwnVerifyChecked);
        return OwnClass::Own;
    }

private:
    u64 phys;
    u32 gen;
    OwnPage page;
    OwnRefs refs{};
    bool have_refs = false;
};

/// BudgetResidual: the front_macro residual's differing bytes (pre-image vs now over the read)
/// by writer. Bytes that no longer differ when re-read count as unknown, so the classes sum to
/// the residual.
inline void OwnResidual(u64 phys, u32 gen, const u8* pre, u64 offset, u64 len, u64 bytes) {
    const uintptr_t base = Nx2Cow::Get().physical_base.load(std::memory_order_relaxed);
    u64 counted = 0;
    u64 own = 0;
    u64 guest = 0;
    if (base != 0 && phys < Nx2Cow::NumPhysPages && offset + len <= PageSize) {
        const u8* const now = reinterpret_cast<const u8*>(base) + (phys << PageBits);
        OwnPageCheck check{phys, gen};
        for (u64 i = offset; i < offset + len && counted < bytes; ++i) {
            const u8 value = now[i];
            if (pre[i] == value) {
                continue;
            }
            ++counted;
            switch (check.Byte(static_cast<u32>(i), value)) {
            case OwnClass::Own:
                ++own;
                break;
            case OwnClass::Guest:
                ++guest;
                break;
            default:
                break;
            }
        }
    }
    OwnAdd(OwnResidOwn, own);
    OwnAdd(OwnResidGuest, guest);
    OwnAdd(OwnResidUnknown, bytes - own - guest);
}

inline void Add(size_t counter, u64 amount = 1) {
    Get().c[counter].fetch_add(amount, std::memory_order_relaxed);
}

template <typename... Args>
void VerifyDifference(fmt::format_string<Args...> format, Args&&... args) {
    const u64 count = Get().verify_differences.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 64) {
        LOG_ERROR(HW_GPU, "Nx2Perf run_ahead_snapshot_probe: {} ({} so far)",
                  fmt::format(format, std::forward<Args>(args)...), count);
    }
}

/// The executable's base, for sites as RVAs (core memory sets it on Windows).
inline void SetModuleBase(uintptr_t base) {
    Get().module_base.store(base, std::memory_order_relaxed);
}

inline void NotePreImage(u32 pool_slot, bool tracked) {
    if (pool_slot < Nx2Cow::PoolSlots) {
        Get().pre_path[pool_slot].store(tracked ? PathTracked : PathUntracked,
                                        std::memory_order_release);
        if (BudgetOn()) [[unlikely]] {
            // Taken before vi2 published this epoch's S_N: the copy may be newer than it.
            State& s = Get();
            const u32 gen = Nx2Cow::Get().gen.load(std::memory_order_acquire);
            const bool early = !s.ready.load(std::memory_order_acquire) ||
                               s.tables[gen & 1]->gen.load(std::memory_order_acquire) != gen;
            s.pre_early[pool_slot].store(early ? 1 : 0, std::memory_order_release);
        }
    }
}

inline size_t ResCatOf(size_t role, size_t category) {
    if (role == 0) {
        return t_front_macro ? FrontMacro : FrontList;
    }
    using C = Nx2Ledger::Category;
    switch (static_cast<C>(category)) {
    case C::BankCompare:
        return Bank;
    case C::UniformFast:
        return UFast;
    case C::UniformCached:
        return UCached;
    case C::Storage:
        return Storage;
    case C::Descriptor:
        return Desc;
    default:
        return OtherCat;
    }
}

/// Residual bytes by call site (nullptr: unknown).
inline void NoteSite(const void* site, u64 bytes) {
    State& s = Get();
    const uintptr_t address = site == nullptr ? 1 : reinterpret_cast<uintptr_t>(site);
    size_t i = static_cast<size_t>((u64{address} * 0x9E3779B97F4A7C15ULL) >> 59);
    for (size_t probes = 0; probes < NumSites; ++probes, i = (i + 1) % NumSites) {
        uintptr_t current = s.site_addr[i].load(std::memory_order_acquire);
        if (current == 0 &&
            s.site_addr[i].compare_exchange_strong(current, address, std::memory_order_acq_rel)) {
            current = address;
        }
        if (current == address) {
            s.site_bytes[i].fetch_add(bytes, std::memory_order_relaxed);
            return;
        }
    }
    Add(SiteOverflow);
}

/// run_ahead_overlay_budget: span hazard bytes by call site.
inline void NoteSpanSite(const void* site, u64 bytes) {
    State& s = Get();
    const uintptr_t address = site == nullptr ? 1 : reinterpret_cast<uintptr_t>(site);
    size_t i = static_cast<size_t>((u64{address} * 0x9E3779B97F4A7C15ULL) >> 59);
    for (size_t probes = 0; probes < NumSites; ++probes, i = (i + 1) % NumSites) {
        uintptr_t current = s.span_site_addr[i].load(std::memory_order_acquire);
        if (current == 0 && s.span_site_addr[i].compare_exchange_strong(
                                current, address, std::memory_order_acq_rel)) {
            current = address;
        }
        if (current == address) {
            s.span_site_bytes[i].fetch_add(bytes, std::memory_order_relaxed);
            return;
        }
    }
    Add(SpanSiteOverflow);
}

/// run_ahead_overlay_budget: hazard bytes a per-page overlay could not redirect.
inline void BudgetSpan(size_t role, size_t category, u64 bytes) {
    if (t_span) {
        Add(SpanGetSpan, bytes);
        NoteSpanSite(t_site, bytes);
    } else if (role == 1 && category == static_cast<size_t>(Nx2Ledger::Category::BankCompare)) {
        Add(SpanBank, bytes);
        NoteSpanSite(t_site, bytes);
    }
}

/// run_ahead_overlay_budget: a front_macro residual's page category and its record.
inline void BudgetResidual(u64 phys, u32 pool_slot, u64 bytes, u32 touch_bits, u32 gen,
                           u64 device_addr, const u8* pre, u64 len) {
    State& s = Get();
    if (g_own.load(std::memory_order_relaxed)) [[unlikely]] {
        OwnResidual(phys, gen, pre, device_addr & (PageSize - 1), len, bytes);
    }
    ResidCat cat = OtherResid;
    if (phys == s.vi2_delta_phys.load(std::memory_order_relaxed)) {
        cat = Vi2Delta;
    } else if ((touch_bits & Nx2Cow::BitHostWritten) != 0 || HostWrittenBefore(gen, phys)) {
        cat = Download;
    } else if (s.pre_early[pool_slot % Nx2Cow::PoolSlots].load(std::memory_order_acquire) != 0) {
        cat = PreCopy;
    }
    Add(Resid0 + cat, bytes);
    Add(ResidEvents0 + cat);
    if (s.record_claim.load(std::memory_order_relaxed) >= NumResidRecords) {
        return;
    }
    const u32 n = s.record_claim.fetch_add(1, std::memory_order_relaxed);
    if (n >= NumResidRecords) {
        return;
    }
    ResidRecord& record = s.records[n];
    record.device_addr = device_addr;
    record.phys = phys;
    record.bytes = bytes;
    record.site = reinterpret_cast<uintptr_t>(t_site);
    record.gen = gen;
    record.cat = static_cast<u8>(cat);
    record.ready.store(true, std::memory_order_release);
}

/// Nx2Cow::ReadPage: an epoch-gen read of len bytes of phys (at device_addr) got `bytes` bytes
/// that differ from its pre-image (cow pool slot pool_slot, page pre).
inline void Hazard(u64 phys, u32 pool_slot, const u8* pre, u64 bytes, size_t role,
                   size_t category, u32 touch_bits, u32 gen, u64 device_addr, u64 len) {
    State& s = Get();
    const bool budget = BudgetOn();
    if (budget) [[unlikely]] {
        BudgetSpan(role, category, bytes);
    }
    bool covered = false;
    Reason reason = NoSnapshot;
    u8 from = 0;
    if (s.ready.load(std::memory_order_acquire)) {
        Table& t = *s.tables[gen & 1];
        if (t.gen.load(std::memory_order_acquire) == gen) {
            u32 slot = t.Find(phys);
            if (Nx2Perf::Verify()) {
                const u32 old = t.FindSorted(phys);
                if (old != slot) {
                    VerifyDifference("page {:#x}: the table says slot {}, the sorted list {}",
                                     phys << PageBits, slot, old);
                    slot = old;
                }
            }
            if (slot == TableSlots) {
                reason = Miss;
            } else if (const u32 index = t.pool[slot]; index == NoPool) {
                from = t.from[slot];
                reason = PoolFull;
            } else {
                from = t.from[slot];
                reason = Stale;
                u8 verdict = t.verdict[slot].load(std::memory_order_acquire);
                if (verdict == VerdictUnknown) {
                    const u64 ref = Nx2Ledger::HashPage(pre);
                    bool equal = ref == t.pool_hash[slot];
                    if (Nx2Perf::Verify()) {
                        const bool old = std::memcmp(s.pool.get() + size_t{index} * PageSize, pre,
                                                     PageSize) == 0;
                        if (old != equal) {
                            VerifyDifference("page {:#x}: the pool copy is {} by its hash, {} by "
                                             "a byte compare",
                                             phys << PageBits, equal ? "equal" : "different",
                                             old ? "equal" : "different");
                            equal = old;
                        }
                    }
                    t.ref_hash[slot].store(ref, std::memory_order_relaxed);
                    const u8 mine = equal ? VerdictEqual : VerdictDiffers;
                    u8 expected = VerdictUnknown;
                    verdict = t.verdict[slot].compare_exchange_strong(expected, mine,
                                                                      std::memory_order_acq_rel)
                                  ? mine
                                  : expected;
                }
                covered = verdict == VerdictEqual;
            }
            if (t.gen.load(std::memory_order_acquire) != gen) {
                // vi2 rebuilt the table meanwhile (a read two epochs late).
                covered = false;
                reason = NoSnapshot;
                from = 0;
            }
        }
    }
    if (covered) {
        Add(BytesA, bytes);
        if (budget) [[unlikely]] {
            Add((from & FromArmSet) == 0   ? BytesAUnion
                : (from & FromUnion) == 0 ? BytesAArm
                                          : BytesABoth,
                bytes);
        }
        return;
    }
    if (s.pre_path[pool_slot % Nx2Cow::PoolSlots].load(std::memory_order_acquire) == PathTracked) {
        Add(BytesB, bytes);
        return;
    }
    Add(BytesC, bytes);
    Add(ResCat0 + ResCatOf(role, category), bytes);
    const Tracker tracker = (touch_bits & Nx2Cow::BitHostWritten) != 0 ? HostWritten
                            : (from & FromArmSet) != 0                ? Untracked
                                                                      : Unregistered;
    Add(ResTracker0 + tracker, bytes);
    Add(ResReason0 + reason, bytes);
    NoteSite(t_site, bytes);
    if (budget && ResCatOf(role, category) == FrontMacro) [[unlikely]] {
        BudgetResidual(phys, pool_slot, bytes, touch_bits, gen, device_addr, pre, len);
    }
}

/// The cow epoch the guest reads in (core memory tags its kept read areas with it).
inline u32 EpochGen() {
    return Nx2Cow::Get().gen.load(std::memory_order_acquire);
}

/// A guest read of GPU-written data while an epoch is open: trapped (it reached GetFlushArea) or
/// served from a read area core memory kept since area_gen's epoch.
inline void GpuRead(bool trapped, u32 area_gen) {
    const Nx2Cow::State& cow = Nx2Cow::Get();
    if (!cow.open.load(std::memory_order_acquire)) {
        return;
    }
    const u32 gen = cow.gen.load(std::memory_order_acquire);
    State& s = Get();
    if (trapped) {
        Add(TrappedGpuReads);
    } else {
        Add(UntrappedGpuReads);
        if (area_gen != gen) {
            Add(UntrappedPreReply);
        }
    }
    u32 claimed = s.first_claim.load(std::memory_order_relaxed);
    if (claimed != gen &&
        s.first_claim.compare_exchange_strong(claimed, gen, std::memory_order_acq_rel)) {
        s.first_ns.store(NowNs() - s.reply_ns.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
        s.first_done.store(gen, std::memory_order_release);
    }
}

/// GetFlushArea's answer on this thread: whether the area holds GPU-written data.
inline thread_local bool t_flush_gpu = false;

/// BufferCache::GetFlushArea, before it returns an area for a trapped guest read.
inline void FlushArea(bool gpu_written) {
    t_flush_gpu = gpu_written;
    if (gpu_written && !Nx2Ledger::g_is_back) {
        GpuRead(true, 0);
    }
}

/// Maxwell3D::FrontDmaRead: the front's reads in its scope are macro DMA reads.
class FrontMacroScope {
public:
    FrontMacroScope() : on{On()} {
        if (on) [[unlikely]] {
            previous = t_front_macro;
            t_front_macro = true;
        }
    }
    ~FrontMacroScope() {
        if (on) [[unlikely]] {
            t_front_macro = previous;
        }
    }
    FrontMacroScope(const FrontMacroScope&) = delete;
    FrontMacroScope& operator=(const FrontMacroScope&) = delete;

private:
    bool on;
    bool previous = false;
};

/// vi2's side: builds and copies S_N at each reply, checks and logs at each close.
class Vi2Window {
public:
    Vi2Window()
        : arm{std::make_unique<std::array<u32, MaxArmPages>>()}, ring{std::make_unique<Ring>()} {
        State& s = Get();
        if (!s.ready.load(std::memory_order_acquire)) {
            s.pool = std::make_unique<u8[]>(size_t{PoolPages} * PageSize);
            s.tables[0] = std::make_unique<Table>();
            s.tables[1] = std::make_unique<Table>();
            s.ready.store(true, std::memory_order_release);
        }
        LOG_INFO(HW_GPU,
                 "Nx2RunAheadSnapshot: probe on (measurement only; EDEN_NX2_PERF_ON=frames_ahead,"
                 "run_ahead_ledger,run_ahead_cow_probe,run_ahead_snapshot_probe; flashes there "
                 "are expected, not a result). S_N = pages the front and back read in the last {} "
                 "epochs + the war_guard arm set at the reply, copied into a {}-page pool. "
                 "Hazard bytes: a = page in S_N and its copy equals the pre-image (the page at "
                 "the reply's arm), b = its first write after the reply trapped through "
                 "HandleRasterizerWrite, c = residual. Residual tracker classes: untracked = in "
                 "the arm set at the reply (tracker-registered, unprotected), unregistered = "
                 "outside it, hostw = written through device memory this epoch. Sites are "
                 "return addresses (RVAs). Decision rules: union_snapshot PASS when c p99 == 0 "
                 "and copy_ms p50 < 1.5; raw_hold PASS when every untrapped read of GPU-written "
                 "data hits a core-kept area (re-armable at the reply) and t_first p50 >= 3 ms "
                 "(fastmem reads of kept areas are not visible to the probe)",
                 K, PoolPages);
        if (BudgetOn()) [[unlikely]] {
            budget_phys = std::make_unique<std::array<u32, TableSlots>>();
            budget_class = std::make_unique<std::array<u8, TableSlots>>();
            if (!s.budget_ready.load(std::memory_order_acquire)) {
                for (auto& bits : s.host_bits) {
                    bits = std::make_unique<std::atomic<u64>[]>(HostBitWords);
                }
                s.budget_ready.store(true, std::memory_order_release);
            }
            LOG_INFO(HW_GPU,
                     "Nx2RunAheadOverlayBudget: probe on (measurement only, with "
                     "run_ahead_snapshot_probe). Hazard bytes of class a split by S_N source "
                     "(union only, arm set only, both). unprot_union = union pages a guest write "
                     "would not trap on (arm set or untracked), copied alone and timed. "
                     "arm_needed = arm pages read this epoch and changed since the reply; "
                     "unarmed_changed = those changed without a trap (no pre-image), as a "
                     "re-protected arm set would miss them. span_hazard_bytes = hazard bytes read "
                     "through a GetSpan over several pages or a bank compare (base_ptr path). "
                     "front_macro residual pages: vi2_delta = vi2's OoT frame-delta page, "
                     "download = host-written since the previous reply, pre_copy = trapped "
                     "before S_N was copied, other");
        }
        if (OwnOn()) [[unlikely]] {
            own_copy_gen = std::make_unique<std::array<u32, TableSlots>>();
            OwnInit();
            LOG_INFO(HW_GPU,
                     "Nx2RunAheadOwnWrite: probe on (measurement only, with "
                     "run_ahead_overlay_budget). GPU-own writes = host writes on the fencing "
                     "thread and the split back, query result stores and downloads; their byte "
                     "ranges are kept per physical page and epoch. Each front_macro residual "
                     "byte (pre-image vs now) and each unarmed_changed page's bytes (reply copy "
                     "vs now) are classed own (inside a range), guest, or unknown (range or table "
                     "overflow, no reply copy). Decision: PASS when resid_guest == 0 and "
                     "unarmed_guest_outside_copy == 0");
        }
    }

    /// run_ahead_overlay_budget: vi2's classifier of physical pages (Nx2Ledger::TrackerClass).
    void SetBudgetClassifier(std::function<void(std::span<const u32>, std::span<u8>)> classify) {
        budget_classify = std::move(classify);
    }

    /// Scratch for core memory's arm set (physical pages).
    std::span<u32> ArmScratch() {
        return {arm->data(), arm->size()};
    }

    /// vi2, after the cow arm and before the readers open: S_N for the new epoch.
    /// ledger_pages: this present's ledger frame set (device pages); the first arm_count pages
    /// of ArmScratch hold the arm set, arm_total its full size.
    void Reply(std::span<const u64> ledger_pages, size_t arm_count, u64 arm_total) {
        State& s = Get();
        const Nx2Cow::State& cow = Nx2Cow::Get();
        const u32 gen = cow.gen.load(std::memory_order_acquire);
        const uintptr_t base = cow.physical_base.load(std::memory_order_relaxed);
        if (!have_base) {
            window_base = Take();
            epoch_base = window_base;
            have_base = true;
            if (g_own.load(std::memory_order_relaxed)) [[unlikely]] {
                own_base = OwnTake();
            }
        }
        if (BudgetOn()) [[unlikely]] {
            BudgetBegin(gen);
        }
        const s64 start = NowNs();
        // The ring's new epoch: the closed epoch's front and back reads, the ledger frame set.
        auto& entry = ring->pages[ring->head];
        u32 n = 0;
        const auto push = [&](u64 phys) {
            if (n == RingPages) {
                ++sum.ring_overflow;
                return;
            }
            entry[n++] = static_cast<u32>(phys);
        };
        if (gen >= 2) {
            const Nx2Cow::TouchSet& closed = *cow.touch[(gen - 1) & 1];
            const u32 used = (std::min)(closed.used_count.load(std::memory_order_acquire),
                                        Nx2Cow::SetSlots);
            for (u32 i = 0; i < used; ++i) {
                const u32 slot = closed.used[i].load(std::memory_order_acquire);
                if ((closed.bits[slot].load(std::memory_order_relaxed) &
                     (Nx2Cow::BitFront | Nx2Cow::BitBack)) == 0) {
                    continue;
                }
                if (const u32 key = closed.key[slot].load(std::memory_order_relaxed); key != 0) {
                    push(key - 1);
                }
            }
        }
        for (const u64 page : ledger_pages) {
            if (const u8* const host = Nx2Cow::DevicePointer(page << PageBits); host != nullptr) {
                push(Nx2Cow::PhysOf(host));
            }
        }
        ring->size[ring->head] = n;
        ring->head = (ring->head + 1) % K;
        ring->count = (std::min)(ring->count + 1, K);

        Table& t = *s.tables[gen & 1];
        t.gen.store(0, std::memory_order_release);
        t.Clear();
        for (size_t j = 0; j < ring->count; ++j) {
            const size_t r = (ring->head + K - 1 - j) % K;
            for (u32 i = 0; i < ring->size[r]; ++i) {
                t.Insert(ring->pages[r][i], FromUnion);
            }
        }
        const u32 union_pages = t.used_count;
        for (size_t i = 0; i < arm_count; ++i) {
            t.Insert((*arm)[i], FromArmSet);
        }
        const s64 built = NowNs();
        s64 budget_ns = 0;
        if (BudgetOn()) [[unlikely]] {
            BudgetReply(t, union_pages, base, gen);
            budget_ns = NowNs() - built;
        }
        u8* const pool = s.pool.get();
        u32 pooled = 0;
        for (u32 i = 0; i < t.used_count; ++i) {
            const u32 slot = t.used[i];
            const u64 phys = t.key[slot].load(std::memory_order_relaxed) - 1;
            if (pooled == PoolPages || base == 0 || phys >= Nx2Cow::NumPhysPages) {
                ++sum.pool_overflow;
                continue;
            }
            std::memcpy(pool + size_t{pooled} * PageSize,
                        reinterpret_cast<const u8*>(base) + (phys << PageBits), PageSize);
            t.pool[slot] = pooled++;
        }
        const s64 copied = NowNs();
        for (u32 i = 0; i < t.used_count; ++i) {
            const u32 slot = t.used[i];
            if (t.pool[slot] != NoPool) {
                t.pool_hash[slot] =
                    Nx2Ledger::HashPage(pool + size_t{t.pool[slot]} * PageSize);
            }
        }
        const s64 hashed = NowNs();
        if (Nx2Perf::Verify()) {
            for (u32 i = 0; i < t.used_count; ++i) {
                const u32 slot = t.used[i];
                t.sorted[i] = (u64{t.key[slot].load(std::memory_order_relaxed) - 1} << 32) | slot;
            }
            std::sort(t.sorted.begin(), t.sorted.begin() + t.used_count);
            t.sorted_count = t.used_count;
        }
        sum.table_overflow += t.overflow;
        sum.union_pages += union_pages;
        sum.arm_pages += t.used_count - union_pages;
        sum.arm_total += arm_total;
        sum.arm_overflow += arm_total > arm_count ? arm_total - arm_count : 0;
        if (frames < WindowFrames) {
            metrics[PagesM][frames] = static_cast<float>(t.used_count);
            metrics[KibM][frames] = static_cast<float>(pooled) * 4.0f;
            metrics[BuildMsM][frames] = static_cast<float>(built - start) / 1e6f;
            metrics[CopyMsM][frames] = static_cast<float>(copied - built - budget_ns) / 1e6f;
            metrics[HashMsM][frames] = static_cast<float>(hashed - copied) / 1e6f;
        }
        s.reply_ns.store(NowNs(), std::memory_order_relaxed);
        t.gen.store(gen, std::memory_order_release);
        epoch_gen = gen;
    }

    /// vi2, after Nx2Cow::Close: the epoch's self-checks; the report lines every WindowFrames.
    std::array<std::string, 4> Close() {
        std::array<std::string, 4> lines{};
        State& s = Get();
        if (epoch_gen == 0) {
            return lines;
        }
        const u32 gen = std::exchange(epoch_gen, 0);
        // Every class-(a) pool page still hashes to its pre-image's hash.
        const Table& t = *s.tables[gen & 1];
        if (t.gen.load(std::memory_order_acquire) == gen) {
            for (u32 i = 0; i < t.used_count; ++i) {
                const u32 slot = t.used[i];
                if (t.verdict[slot].load(std::memory_order_acquire) != VerdictEqual) {
                    continue;
                }
                const u8* const copy = s.pool.get() + size_t{t.pool[slot]} * PageSize;
                if (Nx2Ledger::HashPage(copy) != t.ref_hash[slot].load(std::memory_order_relaxed)) {
                    const u64 count = ++differs;
                    if (count <= 64) {
                        LOG_ERROR(HW_GPU,
                                  "Nx2Perf run_ahead_snapshot_probe: class-a pool page {:#x} "
                                  "differs from its reply hash ({} so far)",
                                  u64{t.key[slot].load(std::memory_order_relaxed) - 1} << PageBits,
                                  count);
                    }
                }
            }
        }
        if (s.first_done.load(std::memory_order_acquire) == gen) {
            if (n_first < WindowFrames) {
                t_first[n_first++] =
                    static_cast<float>(s.first_ns.load(std::memory_order_relaxed)) / 1e6f;
            }
        } else {
            ++no_gpu_read;
        }
        const Counts now = Take();
        if (frames < WindowFrames) {
            for (size_t i = 0; i < 3; ++i) {
                metrics[AM + i][frames] =
                    static_cast<float>(now.c[BytesA + i] - epoch_base.c[BytesA + i]);
            }
        }
        epoch_base = now;
        ++frames;
        if (Nx2Perf::Verify()) {
            const s64 time = NowNs();
            const u64 count = s.verify_differences.load(std::memory_order_relaxed);
            if (count != verify_logged && time - verify_log_ns >= 1'000'000'000) {
                LOG_INFO(HW_GPU, "Nx2Perf run_ahead_snapshot_probe: {} verify differences so far",
                         count);
                verify_logged = count;
                verify_log_ns = time;
            }
        }
        if (frames < WindowFrames) {
            return lines;
        }
        Report(now, lines);
        if (BudgetOn()) [[unlikely]] {
            BudgetReport(now, lines);
            bsum = BudgetSums{};
        }
        frames = 0;
        n_first = 0;
        no_gpu_read = 0;
        sum = Sums{};
        window_base = now;
        return lines;
    }

    /// run_ahead_overlay_budget, vi2 before Nx2Cow::Close: the arm pages the front or back read
    /// this epoch that changed since the reply, and whether each change trapped (has a
    /// pre-image). A change without one bypassed the trap hooks (host or raw pointer writes).
    void BudgetArmCheck() {
        if (epoch_gen == 0) {
            return;
        }
        const u32 gen = epoch_gen;
        State& s = Get();
        const Table& t = *s.tables[gen & 1];
        const Nx2Cow::State& cow = Nx2Cow::Get();
        const uintptr_t base = cow.physical_base.load(std::memory_order_relaxed);
        if (base == 0 || t.gen.load(std::memory_order_acquire) != gen) {
            return;
        }
        const Nx2Cow::TouchSet& touch = *cow.touch[gen & 1];
        for (u32 i = 0; i < t.used_count; ++i) {
            const u32 slot = t.used[i];
            if ((t.from[slot] & FromArmSet) == 0) {
                continue;
            }
            const u64 phys = t.key[slot].load(std::memory_order_relaxed) - 1;
            if (phys >= Nx2Cow::NumPhysPages) {
                continue;
            }
            const u32 touch_slot = touch.Find(phys);
            if (touch_slot == Nx2Cow::SetSlots) {
                continue;
            }
            const u32 bits = touch.bits[touch_slot].load(std::memory_order_relaxed);
            if ((bits & (Nx2Cow::BitFront | Nx2Cow::BitBack)) == 0) {
                continue;
            }
            ++bsum.arm_read;
            const u8* const now = reinterpret_cast<const u8*>(base) + (phys << PageBits);
            bool changed = false;
            if (const u32 index = t.pool[slot]; index != NoPool) {
                changed = Nx2Ledger::HashPage(now) != t.pool_hash[slot];
                if (Nx2Perf::Verify()) {
                    const bool old =
                        std::memcmp(now, s.pool.get() + size_t{index} * PageSize, PageSize) != 0;
                    if (old != changed) {
                        BudgetVerifyDifference("arm page {:#x}: {} by its hash, {} by a byte "
                                               "compare with the reply copy",
                                               phys << PageBits, changed ? "changed" : "unchanged",
                                               old ? "changed" : "unchanged");
                        changed = old;
                    }
                }
            } else if (u64 reply_hash = 0; cow.reply->Find(phys, reply_hash)) {
                changed = Nx2Ledger::HashPage(now) != reply_hash;
            } else {
                ++bsum.arm_unknown; // Neither pooled nor hashed at the reply
                continue;
            }
            if (!changed) {
                continue;
            }
            ++bsum.arm_needed;
            if (Nx2Cow::PreImageEntry(phys, gen) != 0) {
                ++bsum.arm_trapped;
                continue;
            }
            ++bsum.unarmed_changed;
            if ((bits & Nx2Cow::BitHostWritten) != 0) {
                ++bsum.unarmed_hostw;
            }
            if (g_own.load(std::memory_order_relaxed)) [[unlikely]] {
                const u32 index = t.pool[slot];
                OwnUnarmed(slot, phys, gen,
                           index == NoPool ? nullptr : s.pool.get() + size_t{index} * PageSize,
                           now, (bits & Nx2Cow::BitHostWritten) != 0);
            }
        }
    }

    /// run_ahead_overlay_budget, vi2 after Close: the front_macro residual records not logged
    /// yet (at most NumResidRecords in all), the window lines and the verify count.
    /// cpu_of(device address) is the guest address behind it.
    template <typename CpuOf>
    std::vector<std::string> BudgetLines(CpuOf&& cpu_of) {
        std::vector<std::string> out;
        State& s = Get();
        const uintptr_t module = s.module_base.load(std::memory_order_relaxed);
        while (records_logged < NumResidRecords &&
               s.records[records_logged].ready.load(std::memory_order_acquire)) {
            const ResidRecord& r = s.records[records_logged++];
            out.push_back(fmt::format(
                "Nx2Perf run_ahead_overlay_budget: front_macro residual #{} epoch={} device={:#x} "
                "va={:#x} phys_page={:#x} bytes={} site_rva={:#x} category={}",
                records_logged, r.gen, r.device_addr, static_cast<u64>(cpu_of(r.device_addr)),
                r.phys, r.bytes, module != 0 && r.site >= module ? r.site - module : r.site,
                resid_cat_names[r.cat]));
        }
        for (std::string& line : budget_lines) {
            if (!line.empty()) {
                out.push_back(std::move(line));
                line.clear();
            }
        }
        if (Nx2Perf::Verify()) {
            const s64 time = NowNs();
            const u64 count = s.budget_verify_differences.load(std::memory_order_relaxed);
            if (count != budget_verify_logged && time - budget_verify_log_ns >= 1'000'000'000) {
                out.push_back(fmt::format(
                    "Nx2Perf run_ahead_overlay_budget: {} verify differences so far", count));
                budget_verify_logged = count;
                budget_verify_log_ns = time;
            }
            if (g_own.load(std::memory_order_relaxed)) [[unlikely]] {
                const u64 own =
                    OwnGet().c[OwnVerifyDifferences].load(std::memory_order_relaxed);
                if (own != own_verify_logged && time - own_verify_log_ns >= 1'000'000'000) {
                    out.push_back(fmt::format(
                        "Nx2Perf run_ahead_own_write_probe: {} verify differences so far", own));
                    own_verify_logged = own;
                    own_verify_log_ns = time;
                }
            }
        }
        return out;
    }

private:
    enum Metric : size_t {
        PagesM,
        KibM,
        BuildMsM,
        CopyMsM,
        HashMsM,
        AM,
        BM,
        CM,
        UnprotM,    ///< run_ahead_overlay_budget: unprotected union pages
        UCopyMsM,   ///< Their timed copy
        NumMetrics,
    };

    struct Counts {
        std::array<u64, NumCounters> c{};
        std::array<u64, NumSites> site{};
        std::array<u64, NumSites> span_site{};
        u64 cow_hazard = 0;
    };

    struct BudgetSums {
        u64 classes[Nx2Ledger::NumTrackerClasses]{};
        u64 unprot = 0;
        u64 ucopy_overflow = 0;
        u64 arm_read = 0;
        u64 arm_needed = 0;
        u64 arm_trapped = 0;
        u64 unarmed_changed = 0;
        u64 unarmed_hostw = 0;
        u64 arm_unknown = 0;
    };

    /// run_ahead_overlay_budget, vi2 at the reply for gen: host writes from here count for gen+1.
    void BudgetBegin(u32 gen) {
        State& s = Get();
        std::atomic<u64>* const next = s.host_bits[(gen + 1) % NumHostBitSets].get();
        for (size_t i = 0; i < HostBitWords; ++i) {
            next[i].store(0, std::memory_order_relaxed);
        }
        s.host_target.store((gen + 1) % NumHostBitSets, std::memory_order_release);
        if (g_own.load(std::memory_order_relaxed)) [[unlikely]] {
            OwnGet().tables[(gen + 1) % NumOwnTables]->Clear();
        }
    }

    /// run_ahead_overlay_budget, vi2 at the reply after S_N is built: the union pages' tracker
    /// state and a timed copy of those a guest write would not trap on. It writes pool pages
    /// the S_N copy right after overwrites.
    void BudgetReply(const Table& t, u32 union_pages, uintptr_t base, u32 gen) {
        using TC = Nx2Ledger::TrackerClass;
        const u32 n = (std::min)(union_pages, TableSlots);
        auto& phys = *budget_phys;
        auto& classes = *budget_class;
        for (u32 i = 0; i < n; ++i) {
            phys[i] = t.key[t.used[i]].load(std::memory_order_relaxed) - 1;
            classes[i] = static_cast<u8>(TC::NoCpu);
        }
        if (budget_classify) {
            budget_classify(std::span<const u32>(phys.data(), n), std::span<u8>(classes.data(), n));
        }
        u32 unprot = 0;
        for (u32 i = 0; i < n; ++i) {
            const size_t cls = (std::min<size_t>)(classes[i], Nx2Ledger::NumTrackerClasses - 1);
            ++bsum.classes[cls];
            unprot += IsUnprotected(classes[i]) ? 1 : 0;
        }
        if (g_own.load(std::memory_order_relaxed)) [[unlikely]] {
            for (u32 i = 0; i < n; ++i) {
                if (IsUnprotected(classes[i])) {
                    (*own_copy_gen)[t.used[i]] = gen;
                }
            }
        }
        u8* const pool = Get().pool.get();
        const s64 copy_start = NowNs();
        u32 copied = 0;
        for (u32 i = 0; i < n; ++i) {
            if (!IsUnprotected(classes[i])) {
                continue;
            }
            if (copied == PoolPages || base == 0 || phys[i] >= Nx2Cow::NumPhysPages) {
                ++bsum.ucopy_overflow;
                continue;
            }
            std::memcpy(pool + size_t{copied++} * PageSize,
                        reinterpret_cast<const u8*>(base) + (u64{phys[i]} << PageBits), PageSize);
        }
        const s64 copy_end = NowNs();
        bsum.unprot += unprot;
        if (frames < WindowFrames) {
            metrics[UnprotM][frames] = static_cast<float>(unprot);
            metrics[UCopyMsM][frames] = static_cast<float>(copy_end - copy_start) / 1e6f;
        }
    }

    static bool IsUnprotected(u8 cls) {
        using TC = Nx2Ledger::TrackerClass;
        return cls == static_cast<u8>(TC::ArmSet) || cls == static_cast<u8>(TC::Untracked);
    }

    void BudgetReport(const Counts& now, std::array<std::string, 4>& lines) {
        State& s = Get();
        const auto delta = [&](size_t counter) {
            return now.c[counter] - window_base.c[counter];
        };
        const u64 a_union = delta(BytesAUnion);
        const u64 a_arm = delta(BytesAArm);
        const u64 a_both = delta(BytesABoth);
        const u64 b = delta(BytesB);
        const u64 c = delta(BytesC);
        const u64 split = a_union + a_arm + a_both + b + c;
        const u64 cow_hazard = now.cow_hazard - window_base.cow_hazard;
        lines[1] += fmt::format("; budget self-check mismatch={} (a_union+a_arm+a_both+b+c {} vs "
                                "cow hazard bytes {}; a {} vs a_union+a_arm+a_both {})",
                                static_cast<s64>(cow_hazard - split), split, cow_hazard,
                                delta(BytesA), a_union + a_arm + a_both);
        const float unprot50 = MetricPct(UnprotM, 50);
        const float unprot99 = MetricPct(UnprotM, 99);
        const float ucopy50 = MetricPct(UCopyMsM, 50);
        const float ucopy99 = MetricPct(UCopyMsM, 99);
        constexpr float mib_per_page = static_cast<float>(PageSize) / (1024.0f * 1024.0f);
        const u64 span_getspan = delta(SpanGetSpan);
        const u64 span_bank = delta(SpanBank);
        const u64 span_total = span_getspan + span_bank;
        // Span hazard sites this window, most bytes first.
        std::array<std::pair<u64, uintptr_t>, NumSites> sites{};
        for (size_t i = 0; i < NumSites; ++i) {
            sites[i] = {now.span_site[i] - window_base.span_site[i],
                        s.span_site_addr[i].load(std::memory_order_relaxed)};
        }
        std::sort(sites.begin(), sites.end(), std::greater<>{});
        const uintptr_t module = s.module_base.load(std::memory_order_relaxed);
        std::string top;
        size_t shown = 0;
        bool unknown_site = delta(SpanSiteOverflow) != 0;
        for (const auto& [bytes, address] : sites) {
            if (bytes == 0) {
                continue;
            }
            unknown_site |= address == 1;
            if (shown == 8) {
                continue;
            }
            ++shown;
            if (address == 1) {
                top += fmt::format(" unknown:{}", bytes);
            } else if (module != 0 && address >= module) {
                top += fmt::format(" {:#x}:{}", address - module, bytes);
            } else {
                top += fmt::format(" abs{:#x}:{}", address, bytes);
            }
        }
        std::string resid;
        std::string resid_events;
        u64 resid_total = 0;
        for (size_t i = 0; i < NumResidCats; ++i) {
            resid += fmt::format("{}{}", i == 0 ? "" : ",", delta(Resid0 + i));
            resid_events += fmt::format("{}{}", i == 0 ? "" : ",", delta(ResidEvents0 + i));
            resid_total += delta(Resid0 + i);
        }
        const u64 resid_other = delta(Resid0 + OtherResid);
        using TC = Nx2Ledger::TrackerClass;
        budget_lines[0] = fmt::format(
            "Nx2Perf run_ahead_overlay_budget: a_union={} a_arm={} a_both={} b={} c={} "
            "unprot_union_pages p50/p99={:.0f}/{:.0f} MiB={:.1f}/{:.1f} copy_ms "
            "p50/p99={:.3f}/{:.3f} arm_needed={} unarmed_changed={} span_hazard_bytes={} "
            "residual{{vi2_delta,download,pre_copy,other}}={} (events {}) | epochs={} union "
            "pages by class per epoch: tracked={:.1f} arm_set={:.1f} untracked={:.1f} "
            "no_cpu={:.1f}; ucopy_overflow={}; arm pages read={} changed={} trapped={} "
            "unarmed_changed hostw={} unknown(no reply copy or hash)={}; span getspan={} "
            "bank={} top_span_sites_rva(base {:#x}):{} span_site_overflow={}; "
            "budget_verify_differences={}",
            a_union, a_arm, a_both, b, c, unprot50, unprot99, unprot50 * mib_per_page,
            unprot99 * mib_per_page, ucopy50, ucopy99, bsum.arm_needed, bsum.unarmed_changed,
            span_total, resid, resid_events, frames,
            static_cast<double>(bsum.classes[static_cast<size_t>(TC::Tracked)]) / frames,
            static_cast<double>(bsum.classes[static_cast<size_t>(TC::ArmSet)]) / frames,
            static_cast<double>(bsum.classes[static_cast<size_t>(TC::Untracked)]) / frames,
            static_cast<double>(bsum.classes[static_cast<size_t>(TC::NoCpu)]) / frames,
            bsum.ucopy_overflow, bsum.arm_read, bsum.arm_needed, bsum.arm_trapped,
            bsum.unarmed_hostw, bsum.arm_unknown, span_getspan, span_bank, module, top,
            delta(SpanSiteOverflow),
            s.budget_verify_differences.load(std::memory_order_relaxed));
        const bool copy_pass = unprot99 <= 3072.0f && ucopy50 <= 0.6f;
        const char* const arm_verdict = bsum.unarmed_changed != 0 ? "FAIL"
                                        : bsum.arm_unknown != 0  ? "INCONCLUSIVE"
                                                                 : "PASS";
        const char* const span_verdict = span_total == 0 ? "PASS"
                                         : unknown_site  ? "FAIL"
                                                         : "PASS (sites named)";
        const char* const resid_verdict = resid_total == 0  ? "PASS (no residual)"
                                          : resid_other == 0 ? "PASS"
                                                             : "FAIL";
        budget_lines[1] = fmt::format(
            "Nx2Perf run_ahead_overlay_budget: decision copy_set {} (unprot_union p99 {:.0f} "
            "pages, needs <= 3072; copy_ms p50 {:.3f}, needs <= 0.6); arm_trap {} (arm_needed "
            "{}, unarmed_changed {}, needs 0; unknown {}); span {} (span_hazard_bytes {}, needs 0 "
            "or named sites); front_macro_residual {} ({} B, unattributed {} B)",
            copy_pass ? "PASS" : "FAIL", unprot99, ucopy50, arm_verdict, bsum.arm_needed,
            bsum.unarmed_changed, bsum.arm_unknown, span_verdict, span_total, resid_verdict,
            resid_total, resid_other);
        if (g_own.load(std::memory_order_relaxed)) [[unlikely]] {
            OwnReport(resid_total, module);
        }
    }

    using OwnCounts = std::pair<std::array<u64, NumOwnCounters>, std::array<u64, NumSites>>;

    static OwnCounts OwnTake() {
        const OwnState& o = OwnGet();
        OwnCounts counts{};
        for (size_t i = 0; i < NumOwnCounters; ++i) {
            counts.first[i] = o.c[i].load(std::memory_order_relaxed);
        }
        for (size_t i = 0; i < NumSites; ++i) {
            counts.second[i] = o.site_bytes[i].load(std::memory_order_relaxed);
        }
        return counts;
    }

    /// run_ahead_own_write_probe, BudgetArmCheck: one unarmed_changed page's bytes against its
    /// reply copy (nullptr: none, only a hash), by writer. hostw: host-written this epoch.
    void OwnUnarmed(u32 slot, u64 phys, u32 gen, const u8* copy, const u8* now, bool hostw) {
        bool any_diff = false;
        bool any_guest = false;
        bool any_unknown = false;
        u32 first_guest = 0;
        if (copy != nullptr) {
            OwnPageCheck check{phys, gen};
            for (u32 chunk = 0; chunk < PageSize && !any_guest; chunk += 64) {
                if (std::memcmp(copy + chunk, now + chunk, 64) == 0) {
                    continue;
                }
                for (u32 i = chunk; i < chunk + 64; ++i) {
                    const u8 value = now[i];
                    if (copy[i] == value) {
                        continue;
                    }
                    any_diff = true;
                    const OwnClass cls = check.Byte(i, value);
                    if (cls == OwnClass::Guest && !any_guest) {
                        any_guest = true;
                        first_guest = i;
                    } else if (cls == OwnClass::Unknown) {
                        any_unknown = true;
                    }
                }
            }
        }
        if (any_guest) {
            ++osum.unarmed_guest;
            const bool outside = (*own_copy_gen)[slot] != gen;
            osum.unarmed_guest_outside_copy += outside ? 1 : 0;
            osum.unarmed_guest_hostw += hostw ? 1 : 0;
            if (osum.guest_pages < osum.guest_page.size()) {
                osum.guest_page[osum.guest_pages++] = {(phys << PageBits) + first_guest, outside,
                                                       hostw};
            }
        } else if (any_diff && !any_unknown) {
            ++osum.unarmed_own;
        } else {
            ++osum.unarmed_unknown; // No reply copy, a dropped range, or no byte differs now
        }
    }

    void OwnReport(u64 resid_total, uintptr_t module) {
        const OwnCounts now = OwnTake();
        const auto delta = [&](size_t counter) {
            return now.first[counter] - own_base.first[counter];
        };
        const u64 r_own = delta(OwnResidOwn);
        const u64 r_guest = delta(OwnResidGuest);
        const u64 r_unknown = delta(OwnResidUnknown);
        const u64 r_sum = r_own + r_guest + r_unknown;
        const u64 u_sum = osum.unarmed_own + osum.unarmed_guest + osum.unarmed_unknown;
        // Who=2 writes that are not GPU-own this window, most bytes first.
        const OwnState& o = OwnGet();
        std::array<std::pair<u64, uintptr_t>, NumSites> sites{};
        for (size_t i = 0; i < NumSites; ++i) {
            sites[i] = {now.second[i] - own_base.second[i],
                        o.site_addr[i].load(std::memory_order_relaxed)};
        }
        std::sort(sites.begin(), sites.end(), std::greater<>{});
        std::string top;
        size_t shown = 0;
        for (const auto& [bytes, address] : sites) {
            if (bytes == 0 || shown == 8) {
                continue;
            }
            ++shown;
            if (address == 1) {
                top += fmt::format(" unknown:{}", bytes);
            } else if (module != 0 && address >= module) {
                top += fmt::format(" {:#x}:{}", address - module, bytes);
            } else {
                top += fmt::format(" abs{:#x}:{}", address, bytes);
            }
        }
        std::string pages;
        for (u32 i = 0; i < osum.guest_pages; ++i) {
            const OwnGuestPage& page = osum.guest_page[i];
            pages += fmt::format(" {:#x}{}{}", page.address, page.outside ? "(outside)" : "",
                                 page.hostw ? "(hostw)" : "");
        }
        u64 table_overflow = 0;
        u64 range_overflow = 0;
        for (const auto& table : o.tables) {
            table_overflow += table->table_overflow.load(std::memory_order_relaxed);
            range_overflow += table->range_overflow.load(std::memory_order_relaxed);
        }
        budget_lines[2] = fmt::format(
            "Nx2Perf run_ahead_own_write_probe: resid_own={} resid_guest={} resid_unknown={} "
            "(sum {} vs front_macro residual {}, mismatch {}) unarmed{{all_own,any_guest,"
            "unknown}}={},{},{} (sum {} vs unarmed_changed {}, mismatch {}) "
            "unarmed_guest_outside_copy={} any_guest hostw={} guest_pages(phys+first guest "
            "byte):{} | own writes={} "
            "bytes={} pointer writes={} bytes={} outside_bytes={}; who2_nonown writes={} "
            "bytes={} top_who2_nonown_sites_rva(base {:#x}):{} site_overflow={}; "
            "front_nonown_bytes={}; table_overflow={} range_overflow={} (all windows); verify "
            "own_checked={} unchecked={} own_verify_differences={}",
            r_own, r_guest, r_unknown, r_sum, resid_total,
            static_cast<s64>(resid_total - r_sum), osum.unarmed_own, osum.unarmed_guest,
            osum.unarmed_unknown, u_sum, bsum.unarmed_changed,
            static_cast<s64>(bsum.unarmed_changed - u_sum), osum.unarmed_guest_outside_copy,
            osum.unarmed_guest_hostw, pages, delta(OwnWrites), delta(OwnBytes), delta(OwnPointerWrites),
            delta(OwnPointerBytes), delta(OwnOutsideBytes), delta(Who2Writes), delta(Who2Bytes),
            module, top, delta(Who2SiteOverflow), delta(FrontNonOwnBytes), table_overflow,
            range_overflow, delta(OwnVerifyChecked), delta(OwnVerifyUnchecked),
            now.first[OwnVerifyDifferences]);
        const bool pass = r_guest == 0 && osum.unarmed_guest_outside_copy == 0;
        budget_lines[3] = fmt::format(
            "Nx2Perf run_ahead_own_write_probe: decision {} (resid_guest {} B, needs 0; "
            "unarmed_guest_outside_copy {}, needs 0; unknown: resid {} B, unarmed {} pages){}",
            pass ? "PASS" : "FAIL", r_guest, osum.unarmed_guest_outside_copy, r_unknown,
            osum.unarmed_unknown,
            pass ? "" : "; close the writers: the who2 sites above, guest pages listed");
        own_base = now;
        osum = OwnSums{};
    }

    struct Sums {
        u64 union_pages = 0;
        u64 arm_pages = 0;
        u64 arm_total = 0;
        u64 arm_overflow = 0;
        u64 pool_overflow = 0;
        u64 table_overflow = 0;
        u64 ring_overflow = 0;
    };

    struct Ring {
        std::array<std::array<u32, RingPages>, K> pages{};
        std::array<u32, K> size{};
        size_t head = 0;
        size_t count = 0;
    };

    static Counts Take() {
        State& s = Get();
        const Nx2Cow::State& cow = Nx2Cow::Get();
        Counts counts;
        for (size_t i = 0; i < NumCounters; ++i) {
            counts.c[i] = s.c[i].load(std::memory_order_relaxed);
        }
        for (size_t i = 0; i < NumSites; ++i) {
            counts.site[i] = s.site_bytes[i].load(std::memory_order_relaxed);
        }
        if (BudgetOn()) [[unlikely]] {
            for (size_t i = 0; i < NumSites; ++i) {
                counts.span_site[i] = s.span_site_bytes[i].load(std::memory_order_relaxed);
            }
        }
        for (size_t r = 0; r < Nx2Cow::NumRoles; ++r) {
            for (size_t c = 0; c < Nx2Cow::NumCats; ++c) {
                counts.cow_hazard +=
                    cow.cat[Nx2Cow::CatIndex(r, c, Nx2Cow::HazardBytes)].load(
                        std::memory_order_relaxed);
            }
        }
        return counts;
    }

    float Pct(std::array<float, WindowFrames>& values, size_t n, size_t percent) {
        if (n == 0) {
            return 0.0f;
        }
        const size_t index = (percent * n + 99) / 100 - 1;
        std::nth_element(values.begin(), values.begin() + index, values.begin() + n);
        return values[index];
    }

    float MetricPct(Metric metric, size_t percent) {
        std::copy_n(metrics[metric].begin(), frames, sorted.begin());
        return Pct(sorted, frames, percent);
    }

    void Report(const Counts& now, std::array<std::string, 4>& lines) {
        State& s = Get();
        const double n = static_cast<double>(frames);
        const auto delta = [&](size_t counter) {
            return now.c[counter] - window_base.c[counter];
        };
        const auto per = [n](u64 value) { return static_cast<double>(value) / n; };
        const auto range = [](size_t first, size_t count, auto&& names, auto&& value) {
            std::string out;
            for (size_t i = 0; i < count; ++i) {
                out += fmt::format(" {}={:.1f}", names[i], value(first + i));
            }
            return out;
        };
        const auto per_delta = [&](size_t counter) { return per(delta(counter)); };
        const std::string by_cat = range(ResCat0, NumResCats, res_cat_names, per_delta);
        const std::string by_tracker = range(ResTracker0, NumTrackers, tracker_names, per_delta);
        const std::string by_reason = range(ResReason0, NumReasons, reason_names, per_delta);
        // Residual sites this window, most bytes first.
        std::array<std::pair<u64, uintptr_t>, NumSites> sites{};
        for (size_t i = 0; i < NumSites; ++i) {
            sites[i] = {now.site[i] - window_base.site[i],
                        s.site_addr[i].load(std::memory_order_relaxed)};
        }
        std::sort(sites.begin(), sites.end(), std::greater<>{});
        const uintptr_t module = s.module_base.load(std::memory_order_relaxed);
        std::string top;
        size_t shown = 0;
        for (const auto& [bytes, address] : sites) {
            if (bytes == 0 || shown == 8) {
                continue;
            }
            ++shown;
            if (address == 1) {
                top += fmt::format(" unknown:{}", bytes);
            } else if (module != 0 && address >= module) {
                top += fmt::format(" {:#x}:{}", address - module, bytes);
            } else {
                top += fmt::format(" abs{:#x}:{}", address, bytes);
            }
        }
        const float pages50 = MetricPct(PagesM, 50);
        const float pages99 = MetricPct(PagesM, 99);
        const float kib50 = MetricPct(KibM, 50);
        const float kib99 = MetricPct(KibM, 99);
        const float build50 = MetricPct(BuildMsM, 50);
        const float build99 = MetricPct(BuildMsM, 99);
        const float copy50 = MetricPct(CopyMsM, 50);
        const float copy99 = MetricPct(CopyMsM, 99);
        const float hash50 = MetricPct(HashMsM, 50);
        const float hash99 = MetricPct(HashMsM, 99);
        const float a50 = MetricPct(AM, 50);
        const float a99 = MetricPct(AM, 99);
        const float b50 = MetricPct(BM, 50);
        const float b99 = MetricPct(BM, 99);
        const float c50 = MetricPct(CM, 50);
        const float c99 = MetricPct(CM, 99);
        const float first10 = Pct(t_first, n_first, 10);
        const float first50 = Pct(t_first, n_first, 50);
        const float first90 = Pct(t_first, n_first, 90);
        const u64 untrapped = delta(UntrappedGpuReads);
        const u64 untrapped_pre = delta(UntrappedPreReply);
        const u64 trapped = delta(TrappedGpuReads);
        const u64 abc = delta(BytesA) + delta(BytesB) + delta(BytesC);
        const u64 cow_hazard = now.cow_hazard - window_base.cow_hazard;
        lines[0] = fmt::format(
            "Nx2Perf run_ahead_snapshot_probe: epochs={} pages p50/p99={:.0f}/{:.0f} (union "
            "{:.0f} + arm set {:.0f} per epoch; arm set total {:.0f}, arm_overflow {}) bytes "
            "p50/p99={:.0f}/{:.0f} KiB copy_ms p50/p99={:.3f}/{:.3f} hash_ms p50/p99={:.3f}/{:.3f} "
            "build_ms p50/p99={:.3f}/{:.3f} overflow={} (pool {} pages) table_overflow={} "
            "ring_overflow={} hazard_bytes a/b/c p50/p99={:.0f}/{:.0f} {:.0f}/{:.0f} "
            "{:.0f}/{:.0f} (window totals {}/{}/{}) residual_by_cat(B/epoch):{} "
            "residual_by_tracker:{} residual_by_reason:{} top_residual_sites_rva(base {:#x}):{} "
            "site_overflow={} untrapped_gpu_reads={} (area kept before the reply {}) "
            "trapped_gpu_reads={} t_first_ms p10/p50/p90={:.2f}/{:.2f}/{:.2f} ({} epochs, {} "
            "without a GPU data read) verify_differences={}",
            frames, pages50, pages99, per(sum.union_pages), per(sum.arm_pages),
            per(sum.arm_total), sum.arm_overflow, kib50, kib99, copy50, copy99, hash50, hash99,
            build50, build99, sum.pool_overflow, PoolPages, sum.table_overflow, sum.ring_overflow,
            a50, a99, b50, b99, c50, c99, delta(BytesA), delta(BytesB), delta(BytesC), by_cat,
            by_tracker, by_reason, module, top, delta(SiteOverflow), untrapped, untrapped_pre,
            trapped, first10, first50, first90, n_first, no_gpu_read,
            s.verify_differences.load(std::memory_order_relaxed));
        lines[1] = fmt::format(
            "Nx2Perf run_ahead_snapshot_probe: self-check mismatch={} (a+b+c {} vs cow hazard "
            "bytes {} this window), differs={} (class-a pool pages that differ from their reply "
            "hash, all windows)",
            static_cast<s64>(cow_hazard - abc), abc, cow_hazard, differs);
        const bool union_pass = c99 == 0.0f && copy50 < 1.5f;
        lines[2] = fmt::format(
            "Nx2Perf run_ahead_snapshot_probe: union_snapshot rule {} (residual c p99 {:.0f} B, "
            "needs 0; copy_ms p50 {:.3f}, needs < 1.5)",
            union_pass ? "PASS" : "FAIL", c99, copy50);
        // Each visible untrapped read hits a core-kept read area, which a reply re-arm drops.
        const u64 rearmable = untrapped;
        const bool raw_pass = (untrapped == 0 || rearmable == untrapped) && n_first != 0 &&
                              first50 >= 3.0f;
        lines[3] = fmt::format(
            "Nx2Perf run_ahead_snapshot_probe: raw_hold rule {} (untrapped_gpu_reads {}, "
            "re-armable at the reply {}, {} of them in areas kept before the reply; t_first p50 "
            "{:.2f} ms over {} epochs, needs >= 3)",
            raw_pass ? "PASS" : "FAIL", untrapped, rearmable, untrapped_pre, first50, n_first);
    }

    std::unique_ptr<std::array<u32, MaxArmPages>> arm;
    std::unique_ptr<Ring> ring;
    std::array<std::array<float, WindowFrames>, NumMetrics> metrics{};
    std::array<float, WindowFrames> sorted{};
    std::array<float, WindowFrames> t_first{};
    size_t n_first = 0;
    u64 no_gpu_read = 0;
    size_t frames = 0;
    u32 epoch_gen = 0;
    bool have_base = false;
    Counts epoch_base{};
    Counts window_base{};
    Sums sum{};
    u64 differs = 0;
    u64 verify_logged = 0;
    s64 verify_log_ns = 0;

    // run_ahead_overlay_budget
    std::unique_ptr<std::array<u32, TableSlots>> budget_phys;
    std::unique_ptr<std::array<u8, TableSlots>> budget_class;
    std::function<void(std::span<const u32>, std::span<u8>)> budget_classify;
    BudgetSums bsum{};
    std::array<std::string, 4> budget_lines{};
    u32 records_logged = 0;
    u64 budget_verify_logged = 0;
    s64 budget_verify_log_ns = 0;

    // run_ahead_own_write_probe
    struct OwnGuestPage {
        u64 address = 0; ///< Physical address of its first guest byte
        bool outside = false;
        bool hostw = false;
    };
    struct OwnSums {
        u64 unarmed_own = 0;
        u64 unarmed_guest = 0;
        u64 unarmed_unknown = 0;
        u64 unarmed_guest_outside_copy = 0;
        u64 unarmed_guest_hostw = 0;
        std::array<OwnGuestPage, 8> guest_page{};
        u32 guest_pages = 0;
    };
    std::unique_ptr<std::array<u32, TableSlots>> own_copy_gen; ///< Gen a slot was in the copy set
    OwnSums osum{};
    OwnCounts own_base{};
    u64 own_verify_logged = 0;
    s64 own_verify_log_ns = 0;
};

} // namespace VideoCommon::Nx2Snap

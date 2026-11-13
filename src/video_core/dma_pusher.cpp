// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <bit>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "common/cityhash.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/memory.h"
#include "video_core/nx2_profile.h"
#include "video_core/dma_pusher.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/guest_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_perf.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra {


constexpr u32 MacroRegistersStart = 0xE00;

namespace {
// EDEN_NX2_LIST_STATS=1: which entries the front waits on (a GPU-written list's copy still on the
// GPU, the compute and copy work before an entry, a drain and flush), by the method the entry
// continues, its words left and the entry size. The top keys are logged every 4000 waits, and the
// guest words of each key's first entries.
namespace ListStats {
enum class Wait : u8 { CopyPending, WritersWaited, Flushed };
constexpr std::array<const char*, 3> wait_names{"copy pending", "writers waited", "flushed"};
struct Key {
    u8 wait;
    u8 engine;
    u32 method;
    u32 left;
    u32 size;
    auto operator<=>(const Key&) const = default;
};

bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_LIST_STATS") != nullptr;
    return enabled;
}

void Note(MemoryManager& memory, Wait wait, u8 engine, u32 method, u32 left, GPUVAddr address,
          u32 size) {
    static std::map<Key, u64> counts;
    static u64 total{};
    const Key key{static_cast<u8>(wait), engine, left != 0 ? method : 0, left, size};
    const u64 count = ++counts[key];
    if (count <= 3) {
        std::array<u32, 12> words{};
        memory.ReadBlockUnsafe(address, words.data(),
                               std::min<size_t>(size, words.size()) * sizeof(u32));
        LOG_INFO(HW_GPU,
                 "List stats sample: {} at {:#x}, size {}, continues method {:#x} of engine {} "
                 "({} left): {:08x}",
                 wait_names[key.wait], address, size, key.method, engine, left,
                 fmt::join(std::span{words.data(), std::min<size_t>(size, words.size())}, " "));
    }
    if (++total % 4000 != 0) {
        return;
    }
    std::vector<std::pair<u64, Key>> sorted;
    for (const auto& [entry_key, entry_count] : counts) {
        sorted.emplace_back(entry_count, entry_key);
    }
    std::ranges::sort(sorted, std::greater{}, &std::pair<u64, Key>::first);
    std::string line;
    for (size_t i = 0; i < sorted.size() && i < 16; ++i) {
        const auto& [entry_count, entry_key] = sorted[i];
        line += fmt::format(" | {} {}: method {:#x} engine {} left {} size {}", entry_count,
                            wait_names[entry_key.wait], entry_key.method, entry_key.engine,
                            entry_key.left, entry_key.size);
    }
    LOG_INFO(HW_GPU, "List stats after {} waits:{}", total, line);
}

/// The entry that last waited for its copy (resumed when it is read next), and the words guest
/// memory held for it then.
GPUVAddr pending_address{};
std::vector<u32> pending_guest;

// Writers waits by the kinds of queued work they waited for (GpuSplit::SyncedWriterKinds:
// bit 0 draws, 1 compute, 2 copies, 7 earlier writers only), with their time.
void NoteWritersWait(u32 kinds, u64 ns, bool became_written) {
    static std::map<u32, std::pair<u64, u64>> by_kinds;
    static u64 total{};
    static u64 written{};
    auto& [count, time] = by_kinds[kinds];
    ++count;
    time += ns;
    written += became_written ? 1 : 0;
    if (++total % 1000 != 0) {
        return;
    }
    std::string line = fmt::format(" {} found the entry GPU-written after the wait", written);
    for (const auto& [entry_kinds, entry] : by_kinds) {
        line += fmt::format(" | {}{}{}{}: {} waits, {:.1f} ms total, {:.0f} us each",
                            (entry_kinds & 1) ? "draw " : "", (entry_kinds & 2) ? "compute " : "",
                            (entry_kinds & 4) ? "copy " : "",
                            (entry_kinds & 0x80) ? "earlier " : "", entry.first,
                            static_cast<double>(entry.second) / 1e6,
                            static_cast<double>(entry.second) / 1e3 /
                                static_cast<double>(entry.first));
    }
    LOG_INFO(HW_GPU, "List stats writers after {} waits:{}", total, line);
}

// GPU-written lists read from their copies: would the words guest memory held (what reading
// without the wait gives) or the previous list of that size or address have been the same?
void NoteListRead(MemoryManager& memory, GPUVAddr address, std::span<const u32> words) {
    struct Counts {
        u64 lists{};
        u64 resumed{};
        u64 guest_same{};
        u64 size_same{};
        u64 address_same{};
        u64 resumed_guest_same{};
        u64 resumed_size_same{};
        u64 resumed_address_same{};
        u64 resumed_guest_before_same{};
    };
    static Counts counts;
    static std::map<size_t, std::vector<u32>> by_size;
    static std::map<GPUVAddr, std::vector<u32>> by_address;
    static std::vector<u32> guest;
    const bool resumed = std::exchange(pending_address, GPUVAddr{}) == address;
    guest.resize(words.size());
    memory.ReadBlockUnsafe(address, guest.data(), words.size() * sizeof(u32));
    const auto same = [&words](const std::vector<u32>& other) {
        return std::ranges::equal(other, words);
    };
    const bool guest_same = same(guest);
    auto& size_entry = by_size[words.size()];
    const bool size_same = same(size_entry);
    if (by_address.size() > 65536) {
        by_address.clear();
    }
    auto& address_entry = by_address[address];
    const bool address_same = same(address_entry);
    ++counts.lists;
    counts.guest_same += guest_same ? 1 : 0;
    counts.size_same += size_same ? 1 : 0;
    counts.address_same += address_same ? 1 : 0;
    if (resumed) {
        ++counts.resumed;
        counts.resumed_guest_same += guest_same ? 1 : 0;
        counts.resumed_size_same += size_same ? 1 : 0;
        counts.resumed_address_same += address_same ? 1 : 0;
        counts.resumed_guest_before_same += same(pending_guest) ? 1 : 0;
        if (!size_same && counts.resumed - counts.resumed_size_same <= 4 && !size_entry.empty()) {
            std::string diff;
            for (size_t i = 0; i < words.size() && diff.size() < 400; ++i) {
                if (words[i] != size_entry[i]) {
                    diff += fmt::format(" [{}] {:08x}->{:08x}", i, size_entry[i], words[i]);
                }
            }
            LOG_INFO(HW_GPU, "List stats change: size {} at {:#x}:{}", words.size(), address,
                     diff);
        }
    }
    size_entry.assign(words.begin(), words.end());
    address_entry.assign(words.begin(), words.end());
    if (counts.lists % 2000 != 0) {
        return;
    }
    LOG_INFO(HW_GPU,
             "List stats copies after {} lists: same as guest memory {}, as the last of its size "
             "{}, as the last at its address {} | of the {} that waited: {}, {}, {}, as guest "
             "memory before the wait {}",
             counts.lists, counts.guest_same, counts.size_same, counts.address_same,
             counts.resumed, counts.resumed_guest_same, counts.resumed_size_same,
             counts.resumed_address_same, counts.resumed_guest_before_same);
}
} // namespace ListStats

} // Anonymous namespace

// Compute dispatches, uploads and reports and DMA copies write the command lists
// ReadGpuWrittenCommands reads. Plain native compute state (Frizti 99b3b49) does not.
bool DmaPusher::IsMemoryWriter(const Engines::EngineInterface& engine, Engines::EngineTypes type,
                               u32 method) {
    if (method >= MacroRegistersStart) {
        return type == Engines::EngineTypes::KeplerCompute ||
               type == Engines::EngineTypes::MaxwellDMA;
    }
    if (type == Engines::EngineTypes::KeplerCompute) {
        return static_cast<const Engines::KeplerCompute&>(engine).MayWriteMemory(method);
    }
    if (type == Engines::EngineTypes::MaxwellDMA) {
        return method < engine.execution_mask.size() && engine.execution_mask[method];
    }
    return false;
}
constexpr u32 ComputeInline = 0x6D;

DmaPusher::DmaPusher(Core::System& system_, MemoryManager& memory_manager_, Control::ChannelState& channel_state_)
    : system{system_}
    , memory_manager{memory_manager_}
    , channel_state{channel_state_}
    , signal_sync{false}
    , synced{false}
{}

DmaPusher::~DmaPusher() = default;

bool DmaPusher::CanResumeAcquire() {
    if (pending_list_tick) {
        return rasterizer->IsGpuTickDone(*pending_list_tick);
    }
    return puller.CanResumeAcquire(*this);
}

void DmaPusher::DispatchCalls() {
    // The sub-index is kept: a channel suspended on a semaphore resumes inside its command list.
    dma_state.is_last_call = true;
    while (system.IsPoweredOn()) {
        if (!Step()) {
            break;
        }
    }
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        // Submits and releases signaled fences: their writes belong to the entries that queued them.
        split.Defer(
            [gpu = &system.GPU()] {
                gpu->FlushCommands();
                gpu->OnCommandListEnd();
            },
            VideoCommon::GpuSplit::Effect::None);
        split.Publish();
        return;
    }
    system.GPU().FlushCommands();
    system.GPU().OnCommandListEnd();
}

bool DmaPusher::Step() {
    if (!ib_enable || dma_pushbuffer.empty()) {
        return false;
    }
    // Waiting for the GPU's copy of a command list this channel reads next (ReadGpuWrittenCommands).
    if (pending_list_tick) {
        if (!rasterizer->IsGpuTickDone(*pending_list_tick)) {
            return false;
        }
        pending_list_tick.reset();
        resumed_list_read = true;
    }
    // Only for the entry that suspended: an acquire still waiting, the safe-read path or a stopped
    // split must not leave it set for a later list (which would then skip its writer wait).
    SCOPE_EXIT {
        resumed_list_read = false;
    };
    const u64 suspensions_before = suspensions.load(std::memory_order_relaxed);
    if (!puller.ResumeAcquire(*this)) {
        return false;
    }
    // Lists queued while the channel waited were read from memory, not as submitted: the game
    // writes the commands a list runs before it releases the acquire, so the words at submit time
    // could be older. Reading them as late as the parse is too late in Hyrule Warriors: Age of
    // Imprisonment fights: a command ring the game had already refilled for a later frame, read
    // as this frame's commands (methods on unbound subchannels, garbage QMDs, then a channel
    // waiting forever on a release it never got). The acquire is released now, so the commands
    // are written and the memory not yet reused: copy them here. EDEN_NX2_RESNAPSHOT=0: off.
    if (ResnapshotEnabled() && suspensions.load(std::memory_order_relaxed) != suspensions_before) {
        const u64 now = suspensions.load(std::memory_order_acquire);
        for (CommandList& list : dma_pushbuffer) {
            if (!list.submitted_offsets.empty()) {
                RefreshSnapshot(list, now);
            }
        }
    }

    CommandList& command_list = dma_pushbuffer.front();

    const size_t prefetch_size = command_list.prefetch_command_list.size();
    const size_t command_list_size = command_list.command_lists.size();

    if (prefetch_size == 0 && command_list_size == 0) {
        dma_pushbuffer.pop_front();
        dma_pushbuffer_subindex = 0;
        return true;
    }

    if (prefetch_size > 0) {
        const std::span<const CommandHeader> commands{command_list.prefetch_command_list};
        command_resume_offset +=
            ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        if (puller.HasPendingAcquire()) {
            return false;
        }
        command_resume_offset = 0;
        dma_pushbuffer.pop_front();
        return true;
    }

    auto& current_command = command_list.command_lists[dma_pushbuffer_subindex];
    const CommandListHeader& header = current_command;
    dma_state.dma_get = header.addr;

    if (signal_sync && !synced) {
        std::unique_lock lk(sync_mutex);
        sync_cv.wait(lk, [this]() { return synced; });
        signal_sync = false;
        synced = false;
    }

    if (header.size > 0) {
        // The entry is the 5-word record of a DrawIndexedIndirect call the host GPU draws from
        // guest memory (Maxwell3D::FrontTakeIndirectRecord): its words are not needed fresh.
        bool skip_record_read = false;
        if (header.sync && g_nx2_ampere.load(std::memory_order_relaxed)) {
            VideoCommon::GpuSplit::Get().NoteSync();
        }
        // EDEN_NX2_FETCH_DRAIN=1: wait for the back before every entry, as before the split
        // overlapped the two halves again.
        static const bool fetch_drain = std::getenv("EDEN_NX2_FETCH_DRAIN") != nullptr;
        if (fetch_drain && g_nx2_ampere.load(std::memory_order_relaxed)) {
            VideoCommon::GpuSplit::Get().Drain(VideoCommon::DrainSite::FetchDrain);
        }
        if (subchannels[dma_state.subchannel] && dma_state.method_count) {
            const auto engine = subchannel_type[dma_state.subchannel];
            const bool kepler_payload = engine == Engines::EngineTypes::KeplerCompute && dma_state.method == ComputeInline && dma_state.non_incrementing;
            const bool macro_payload = engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart;
            if (kepler_payload || macro_payload) {
                const size_t words = std::min<size_t>(dma_state.method_count, header.size);
                auto* const engine_state = subchannels[dma_state.subchannel];
                if (macro_payload && words == 5 && header.size == 5 &&
                    dma_state.method_count == 5 && command_resume_offset == 0 &&
                    VideoCommon::GpuSplit::Get().Active() &&
                    static_cast<Engines::Maxwell3D*>(engine_state)
                        ->FrontTakeIndirectRecord(dma_state.method, dma_state.dma_get)) {
                    skip_record_read = true;
                }
                if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
                    const bool front_3d_macro =
                        macro_payload && split.FrontMacros() &&
                        static_cast<Engines::Maxwell3D*>(engine_state)
                            ->GetNativeMacroEngine()
                            .native_turing;
                    if (!front_3d_macro) {
                        // The caches behind IsMemoryDirty belong to the split's back thread.
                        split.Defer(
                            [engine_state, memory = &memory_manager,
                             address = GPUVAddr{dma_state.dma_get}, bytes = words * sizeof(u32)] {
                                engine_state->current_dirty = memory->IsMemoryDirty(address, bytes);
                            },
                            VideoCommon::GpuSplit::Effect::None);
                    }
                } else {
                    engine_state->current_dirty = memory_manager.IsMemoryDirty(dma_state.dma_get, words * sizeof(u32));
                }
            }
        }
        if (command_resume_offset == 0) {
            // A method's data continues across GP entries, whatever their level: title 4 ends a
            // main entry after a macro's first word and supplies the remaining parameters in a
            // 5-word subroutine entry. EDEN_NX2_PARK_SPLIT_METHODS restores the old guess (an
            // entry elsewhere is a call that starts clean; resume where the split one ended).
            static const bool park_split_methods = std::getenv("EDEN_NX2_PARK_SPLIT_METHODS") != nullptr;
            if (g_nx2_ampere) {
                if (static u32 split_logs{}; dma_state.method_count != 0 && split_logs < 64) {
                    ++split_logs;
                    LOG_DEBUG(HW_GPU, "Split method {:#x} ({} left) on ch {}: next entry {:#x} size {} raw {:#x} (level bit {}, priv bit {}), expected {:#x}, parked {}",
                             dma_state.method, dma_state.method_count, channel_state.bind_id,
                             u64{header.addr}, u64{header.size}, header.raw, u64{header.is_push_buffer},
                             u64{header.allow_flush}, split_method_end, parked_methods.size());
                    std::array<u32, 8> next_words{};
                    std::array<u32, 4> prev_words{};
                    memory_manager.ReadBlockUnsafe(header.addr, next_words.data(),
                                             std::min<size_t>(header.size, 8) * sizeof(u32));
                    memory_manager.ReadBlockUnsafe(split_method_end - sizeof(prev_words), prev_words.data(),
                                             sizeof(prev_words));
                    LOG_DEBUG(HW_GPU, "  previous entry ends {:08x}; next entry starts {:08x}",
                              fmt::join(prev_words, " "), fmt::join(next_words, " "));
                }
                if (park_split_methods) {
                    ResolveSplitMethod(header.addr);
                }
            }
            if (dma_pushbuffer_subindex < command_list.submit_hashes.size()) {
                CheckSubmitHash(header, command_list.submit_hashes[dma_pushbuffer_subindex]);
            }
        }
        // A resumed entry is read again: the words after the acquire may be new by now.
        // A safe read flushes the caches, which belong to the split's back thread while it runs
        // (the split only starts with unsafe reads; the settings can change mid-game).
        const bool use_safe = !VideoCommon::GpuSplit::Get().Active() &&
                              (Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh() : Settings::IsDMALevelSafe());
        // The split's back marks the pages GPU work writes as it runs that work: a page first
        // written by a dispatch or copy still queued for it reads as unwritten, and its words as
        // stale (Fast Fusion's culling fills in instanced draws' arguments; stale ones asked for
        // billions of instances and lost the device loading a race).
        bool gpu_written = !skip_record_read &&
            IsGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
        // Pages no GPU work has written yet may still get the list from compute or copy work
        // queued before it. The GPU fetches the list after that work only past a host-level wait
        // (NoteSync): later work is not ordered before the fetch, and the list does not wait for
        // it (nor for draws, which write memory too; draw-written lists wait for the last sync).
        const auto note_wait = [&](ListStats::Wait wait) {
            if (ListStats::Enabled()) [[unlikely]] {
                ListStats::Note(memory_manager, wait,
                                static_cast<u8>(subchannel_type[dma_state.subchannel]),
                                dma_state.method, dma_state.method_count, dma_state.dma_get,
                                static_cast<u32>(header.size));
            }
        };
        auto& split_for_list = VideoCommon::GpuSplit::Get();
        const bool speculated = !skip_record_read && !gpu_written && split_for_list.Active() &&
                                SpeculateList(dma_state.dma_get, size_t{header.size} * sizeof(u32));
        if (auto& split = split_for_list;
            !speculated && !skip_record_read && !gpu_written && split.Active()) {
            const bool stats = ListStats::Enabled();
            const u32 kinds = stats ? split.SyncedWriterKinds() : 0;
            const auto wait_start = stats ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            if (split.WaitForSyncedWriters()) {
                note_wait(ListStats::Wait::WritersWaited);
                gpu_written =
                    IsGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
                if (stats) [[unlikely]] {
                    ListStats::NoteWritersWait(
                        kinds,
                        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - wait_start)
                                             .count()),
                        gpu_written);
                }
            }
        }
        // The words as submitted, unless this is a resumed entry or the GPU has newer ones the
        // safe read below would flush (with the split, the front can't tell: it reads memory
        // unflushed anyway). Entries on pages the GPU writes (culling) always take the flushing
        // read: their words at submit time are an older frame's.
        const bool submitted = command_resume_offset < header.size &&
                               dma_pushbuffer_subindex < command_list.submitted_offsets.size() &&
                               (command_list.snapshot_suspensions & 1) == 0 &&
                               command_list.snapshot_suspensions ==
                                   suspensions.load(std::memory_order_acquire) &&
                               !std::ranges::binary_search(
                                   command_list.deferred_entries,
                                   static_cast<u32>(dma_pushbuffer_subindex)) &&
                               !gpu_written &&
                               !(use_safe && memory_manager.IsMemoryDirty(dma_state.dma_get,
                                                                          header.size * sizeof(u32)));
        // Off by default: the check below reads memory and allocates for each entry it checks.
        // EDEN_NX2_PERF_VERIFY=1 turns it on.
        static const bool check_parsed_entries = VideoCommon::Nx2Perf::Verify();
        if (check_parsed_entries && !submitted &&
            dma_pushbuffer_subindex < command_list.submitted_offsets.size()) {
            // An entry parsed from memory instead of its submit-time copy is the way a command
            // ring the game already refilled gets read as this frame's commands (garbage methods,
            // unbound subchannels, render targets of absurd size). Count how often the words
            // differ, by the reason the copy was skipped. Bounded: the first entries only.
            enum Reason : u32 { Resumed, SuspendedCopy, SuspensionsChanged, GpuWritten, SafeDirty, Count };
            static std::array<std::atomic<u64>, Count> checked{};
            static std::array<std::atomic<u64>, Count> differing{};
            static std::atomic<u64> total_checked{};
            static constexpr std::array<const char*, Count> names{
                "resumed entry", "copied during a suspension", "suspension count changed",
                "GPU-written page", "safe read of dirty memory"};
            if (total_checked.load(std::memory_order_relaxed) < 50000) {
                const Reason reason = command_resume_offset != 0                      ? Resumed
                                      : (command_list.snapshot_suspensions & 1) != 0  ? SuspendedCopy
                                      : command_list.snapshot_suspensions !=
                                              suspensions.load(std::memory_order_acquire)
                                          ? SuspensionsChanged
                                      : gpu_written ? GpuWritten
                                                    : SafeDirty;
                std::vector<u32> live(header.size);
                memory_manager.ReadBlockUnsafe(dma_state.dma_get, live.data(),
                                               live.size() * sizeof(u32));
                const CommandHeader* const copy = command_list.submitted_words.data() +
                                                  command_list.submitted_offsets[dma_pushbuffer_subindex];
                u32 first = ~0U, words = 0;
                for (u32 i = 0; i < header.size; ++i) {
                    if (copy[i].argument != live[i]) {
                        first = std::min(first, i);
                        ++words;
                    }
                }
                ++checked[reason];
                const u64 n = total_checked.fetch_add(1) + 1;
                if (words != 0) {
                    const u64 seen = differing[reason].fetch_add(1);
                    if (seen < 4) {
                        LOG_WARNING(HW_GPU,
                                    "Entry {:#x}+{:#x} parsed from memory ({}) differs from its "
                                    "submit-time copy in {} words, first at word {}",
                                    u64{header.addr}, u64{header.size}, names[reason], words, first);
                    }
                }
                if ((n % 10000) == 0) {
                    for (u32 r = 0; r < Count; ++r) {
                        LOG_INFO(HW_GPU, "Entries parsed from memory ({}): {} checked, {} differ from the submit-time copy",
                                 names[r], checked[r].load(), differing[r].load());
                    }
                }
            }
        }
        if (submitted) {
            const std::span<const CommandHeader> commands{
                command_list.submitted_words.data() +
                    command_list.submitted_offsets[dma_pushbuffer_subindex],
                header.size};
            if (command_resume_offset == 0) {
                RecordParsedEntry(header, commands);
            }
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else if (use_safe) {
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::SafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            RecordParsedEntry(header, commands);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else if (skip_record_read) {
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::UnsafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            RecordParsedEntry(header, commands);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else if (bool pending = false;
                   ReadGpuWrittenCommands(dma_state.dma_get, static_cast<u32>(header.size),
                                          &pending) ||
                   pending) {
            if (pending) {
                // Parsed again once the GPU copied the list; other channels run meanwhile.
                note_wait(ListStats::Wait::CopyPending);
                ListStats::pending_address = dma_state.dma_get;
                if (ListStats::Enabled()) [[unlikely]] {
                    ListStats::pending_guest.resize(header.size);
                    memory_manager.ReadBlockUnsafe(dma_state.dma_get,
                                                   ListStats::pending_guest.data(),
                                                   size_t{header.size} * sizeof(u32));
                }
                return false;
            }
            if (ListStats::Enabled()) [[unlikely]] {
                ListStats::NoteListRead(
                    memory_manager, dma_state.dma_get,
                    std::span{reinterpret_cast<const u32*>(command_headers.data()), header.size});
            }
            const std::span<const CommandHeader> commands{command_headers.data(), header.size};
            RecordParsedEntry(header, commands);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else {
            if (gpu_written) {
                note_wait(ListStats::Wait::Flushed);
            }
            FlushGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::UnsafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            RecordParsedEntry(header, commands);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        }
        if (puller.HasPendingAcquire()) {
            return false;
        }
        command_resume_offset = 0;
        if (dma_state.method_count != 0) {
            split_method_end = header.addr + header.size * sizeof(u32);
        }
    }

    if (++dma_pushbuffer_subindex >= command_list_size) {
        // A split method resumes within the submission that split it; ring addresses repeat
        // every frame, so a stale one could otherwise swallow the headers of a later entry.
        for (const auto& parked : parked_methods) {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1) < 16) {
                LOG_WARNING(HW_GPU,
                            "Submission ended with parked method {:#x} (sub {}, {} words left, "
                            "resume {:#x})",
                            parked.method, parked.subchannel, parked.method_count,
                            parked.resume_address);
            }
        }
        parked_methods.clear();
        dma_pushbuffer.pop_front();
        dma_pushbuffer_subindex = 0;
    } else {
        signal_sync = command_list.command_lists[dma_pushbuffer_subindex].sync && Settings::values.sync_memory_operations.GetValue();
    }

    if (signal_sync) {
        // SignalFence invalidates the GPU caches, which can record copies into the scheduler: the
        // back's state, which a drained back still submits from in its idle hook.
        VideoCommon::GpuSplit::Get().Run([this] {
            rasterizer->SignalFence([this]() {
                std::scoped_lock lk(sync_mutex);
                synced = true;
                sync_cv.notify_all();
            });
        }, VideoCommon::DrainSite::GpSyncFence);
    }

    return true;
}

// EDEN_NX2_SEM_WATCH=<gpu address>: log every method whose argument is the low word of an
// address in the 16 bytes there (finds the engine and method that release a semaphore).
static const u32 g_semaphore_watch = [] {
    const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
    return value ? static_cast<u32>(std::strtoull(value, nullptr, 16)) : 0U;
}();

// EDEN_NX2_PB_CHECK also keeps the last parsed entries, for DumpCurrentEntry.
static const bool g_record_parsed_entries = std::getenv("EDEN_NX2_PB_CHECK") != nullptr;

size_t DmaPusher::ProcessCommands(std::span<const CommandHeader> commands, size_t word_offset) {
    for (size_t index = 0; index < commands.size();) {
        // Data word of methods command
        if (dma_state.method_count && dma_state.non_incrementing) {
            auto const& command_header = commands[index]; //must ref (MUltiMethod re)
            dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
            u32 max_write = u32(std::min<std::size_t>(index + dma_state.method_count, commands.size()) - index);
            if (g_nx2_ampere && dma_state.method < non_puller_methods) {
                max_write = 1; // a semaphore acquire may suspend the channel after any word
            }
            CallMultiMethod(&command_header.argument, max_write);
            dma_state.method_count -= max_write;
            dma_state.is_last_call = true;
            index += max_write;
        } else if (dma_state.method_count) {
            if (!dma_state.non_incrementing && !dma_increment_once &&
                dma_state.method >= non_puller_methods && dma_state.method_count > 1 &&
                g_semaphore_watch == 0 && FrontRun(commands, word_offset, index)) {
                if (puller.HasPendingAcquire()) {
                    return index;
                }
                continue;
            }
            // Fast-path batching for consecutive non-execution methods in increasing mode
            if (!dma_state.non_incrementing && !dma_increment_once &&
                dma_state.method >= non_puller_methods &&
                !VideoCommon::GpuSplit::Get().Active() &&
                g_semaphore_watch == 0) {
                auto subchannel = subchannels[dma_state.subchannel];
                if (subchannel != nullptr) {
                    const size_t available = std::min<size_t>(dma_state.method_count, commands.size() - index);
                    u32 batch_count = 0;
                    while (batch_count < available &&
                           (dma_state.method + batch_count) < subchannel->execution_mask.size() &&
                           !subchannel->execution_mask[dma_state.method + batch_count]) {
                        ++batch_count;
                    }
                    if (batch_count > 1) {
                        subchannel->method_sink.reserve(subchannel->method_sink.size() + batch_count);
                        for (u32 i = 0; i < batch_count; ++i) {
                            subchannel->method_sink.emplace_back(dma_state.method + i, commands[index + i].argument);
                        }
                        dma_state.method += batch_count;
                        dma_state.method_count -= batch_count;
                        index += batch_count;
                        continue;
                    }
                }
            }

            auto const command_header = commands[index]; //can copy
            dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
            dma_state.is_last_call = dma_state.method_count <= 1;
            CallMethod(command_header.argument);
            dma_state.method += !dma_state.non_incrementing ? 1 : 0;
            dma_state.non_incrementing |= dma_increment_once;
            dma_state.method_count--;
            index++;
        } else {
            auto const command_header = commands[index]; //can copy
            // No command active - this is the first word of a new one
            switch (command_header.mode) {
            case SubmissionMode::Increasing:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = false;
                break;
            case SubmissionMode::NonIncreasing:
                SetState(command_header);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::Inline:
                dma_state.method = command_header.method;
                dma_state.subchannel = command_header.subchannel;
                dma_state.dma_word_offset = u64(-s64(dma_state.dma_get)); // negate to set address as 0
                CallMethod(command_header.arg_count);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::IncreaseOnce:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = true;
                break;
            default:
                break;
            }
            index++;
        }
        if (puller.HasPendingAcquire()) {
            return index;
        }
    }
    return commands.size();
}

// Nx2Perf FrontRun: the rest of an incrementing 3D command below the macro range, all of it in
// `commands`, written on the split front in one call (Maxwell3D::FrontCallMethodRun): the same
// FrontCallMethod sequence CallMethod makes word by word, then the state that loop leaves.
bool DmaPusher::FrontRun(std::span<const CommandHeader> commands, size_t word_offset,
                         size_t& index) {
    if (!VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::FrontRun)) {
        return false;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    const u32 method = dma_state.method;
    const u32 count = dma_state.method_count;
    if (!split.Active() || !split.FrontMacros() || count > commands.size() - index ||
        method + count > MacroRegistersStart || puller.HasPendingAcquire() ||
        subchannel_type[dma_state.subchannel] != Engines::EngineTypes::Maxwell3D) {
        return false;
    }
    auto* const subchannel = subchannels[dma_state.subchannel];
    if (subchannel == nullptr) {
        return false;
    }
    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
    if (!maxwell3d->GetNativeMacroEngine().native_turing) {
        return false;
    }
    if (VideoCommon::Nx2Perf::Verify()) [[unlikely]] {
        VerifyFrontRun(*maxwell3d, commands, word_offset, index);
        return true;
    }
    maxwell3d->FrontCallMethodRun(system, split, method, &commands[index].argument, count, true,
                                  count);
    dma_state.dma_word_offset = u64((word_offset + index + count - 1) * sizeof(u32));
    dma_state.is_last_call = true;
    dma_state.method = method + count;
    dma_state.method_count = 0;
    index += count;
    return true;
}

// EDEN_NX2_PERF_VERIFY: the words go through the per-word path (authoritative), and the writes
// FrontCallMethod gets and the state left are compared with what FrontRun would have made.
void DmaPusher::VerifyFrontRun(Engines::Maxwell3D& maxwell3d,
                               std::span<const CommandHeader> commands, size_t word_offset,
                               size_t& index) {
    const u32 method = dma_state.method;
    const u32 count = dma_state.method_count;
    const size_t start = index;
    thread_local std::vector<std::array<u32, 3>> expected;
    thread_local std::vector<std::array<u32, 3>> recorded;
    expected.clear();
    recorded.clear();
    for (u32 i = 0; i < count; ++i) {
        expected.push_back({method + i, commands[start + i].argument, count - i <= 1 ? 1U : 0U});
    }
    maxwell3d.front_run_record = &recorded;
    bool acquire = false;
    for (u32 i = 0; i < count; ++i) {
        const auto command_header = commands[index];
        dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
        dma_state.is_last_call = dma_state.method_count <= 1;
        CallMethod(command_header.argument);
        dma_state.method += !dma_state.non_incrementing ? 1 : 0;
        dma_state.non_incrementing |= dma_increment_once;
        dma_state.method_count--;
        index++;
        if (puller.HasPendingAcquire()) {
            acquire = true; // ProcessCommands returns here, as it would have after this word
            break;
        }
    }
    maxwell3d.front_run_record = nullptr;
    const bool state_ok = dma_state.method == method + count && dma_state.method_count == 0 &&
                          dma_state.is_last_call && !dma_state.non_incrementing &&
                          index == start + count &&
                          dma_state.dma_word_offset ==
                              u64((word_offset + start + count - 1) * sizeof(u32));
    if (acquire || !state_ok || recorded != expected) {
        static std::atomic<u32> reports{};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 64) {
            size_t first = 0;
            while (first < recorded.size() && first < expected.size() &&
                   recorded[first] == expected[first]) {
                ++first;
            }
            LOG_ERROR(HW_GPU,
                      "Nx2Perf front: run method {:#x} count {}: {} of {} writes as expected "
                      "(first difference at {}), state {}, acquire mid-run {}",
                      method, count, recorded.size(), expected.size(), first,
                      state_ok ? "same" : "differs", acquire);
        }
    }
}

// NVN on Switch 2 ends GPFIFO entries in the middle of a method's data and calls another command
// buffer (e.g. a compute dispatch) before the entry that carries the rest, which starts right
// where the first one ended. An entry starting anywhere else is such a call: run it with a clean
// method state and resume the unfinished method when the stream continues at that address.
void DmaPusher::ResolveSplitMethod(GPUVAddr entry_address) {
    const auto continued = std::ranges::find(parked_methods, entry_address,
                                             &ParkedMethod::resume_address);
    std::optional<ParkedMethod> resume;
    if (continued != parked_methods.end()) {
        resume = *continued;
        parked_methods.erase(continued);
    }
    if (dma_state.method_count != 0 && entry_address != split_method_end) {
        if (parked_methods.size() == MaxParkedMethods) {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1) < 8) {
                const auto& oldest = parked_methods.front();
                LOG_WARNING(HW_GPU,
                            "Dropping parked method {:#x} (sub {}, {} words left, resume {:#x})",
                            oldest.method, oldest.subchannel, oldest.method_count,
                            oldest.resume_address);
            }
            parked_methods.erase(parked_methods.begin());
        }
        parked_methods.push_back(ParkedMethod{
            .method = dma_state.method,
            .subchannel = dma_state.subchannel,
            .method_count = dma_state.method_count,
            .non_incrementing = dma_state.non_incrementing,
            .increment_once = dma_increment_once,
            .resume_address = split_method_end,
        });
        dma_state.method_count = 0;
        dma_increment_once = false;
    }
    if (resume && dma_state.method_count == 0) {
        dma_state.method = resume->method;
        dma_state.subchannel = resume->subchannel;
        dma_state.method_count = resume->method_count;
        dma_state.non_incrementing = resume->non_incrementing;
        dma_increment_once = resume->increment_once;
    }
}

void DmaPusher::SetState(const CommandHeader& command_header) {
    dma_state.method = command_header.method;
    dma_state.subchannel = command_header.subchannel;
    dma_state.method_count = command_header.method_count;
}


void DmaPusher::WatchArgument(u32 method, u32 argument) const {
    const u32 watch = g_semaphore_watch;
    if ((argument & ~0xfU) == watch) {
        LOG_INFO(HW_GPU, "Semaphore watch: channel {} subchannel {} method {:#x} argument {:#x}",
                 channel_state.bind_id, dma_state.subchannel, method, argument);
    }
}

bool DmaPusher::WantsSegment() const {
    // Only macro parameters and compute uploads read the engine's current_dma_segment.
    const auto engine = subchannel_type[dma_state.subchannel];
    return (engine == Engines::EngineTypes::KeplerCompute &&
            (dma_state.method == ComputeInline || dma_state.method >= MacroRegistersStart)) ||
           (engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart);
}

void DmaPusher::CallMethod(u32 argument) {
    if (g_semaphore_watch != 0) [[unlikely]] {
        WatchArgument(dma_state.method, argument);
    }
    auto& split = VideoCommon::GpuSplit::Get();
    if (dma_state.method < non_puller_methods) {
        puller.CallPullerMethod(*this, Engines::Puller::MethodCall{
            dma_state.method,
            argument,
            dma_state.subchannel,
            dma_state.method_count,
        });
    } else {
        auto subchannel = subchannels[dma_state.subchannel];
        if (subchannel == nullptr) {
            ReportUnboundSubchannel();
            return;
        }
        // Compute dispatches and DMA copies write the command lists ReadGpuWrittenCommands reads.
        const auto engine_type = subchannel_type[dma_state.subchannel];
        const bool writer = IsMemoryWriter(*subchannel, engine_type, dma_state.method);
        // Copies and blits are GPU work for the front's reads (GpuSplit::Effect).
        const auto effect = engine_type == Engines::EngineTypes::MaxwellDMA ||
                                    engine_type == Engines::EngineTypes::Fermi2D
                                ? VideoCommon::GpuSplit::Effect::Gpu
                                : VideoCommon::GpuSplit::Effect::Unknown;
        SCOPE_EXIT {
            if (writer) {
                split.NoteWriter(engine_type == Engines::EngineTypes::KeplerCompute
                                     ? VideoCommon::GpuSplit::WriterKind::Compute
                                     : VideoCommon::GpuSplit::WriterKind::Copy);
            }
        };
        if (split.Active()) {
            const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
            const GPUVAddr* const opt_segment = WantsSegment() ? &segment : nullptr;
            if (split.FrontMacros()) {
                const auto type = subchannel_type[dma_state.subchannel];
                if (type == Engines::EngineTypes::Maxwell3D) {
                    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
                    if (maxwell3d->GetNativeMacroEngine().native_turing) {
                        maxwell3d->FrontCallMethod(system, split, dma_state.method, argument,
                                                   dma_state.is_last_call);
                        return;
                    }
                } else if (type == Engines::EngineTypes::KeplerCompute) {
                    auto* const compute = static_cast<Engines::KeplerCompute*>(subchannel);
                    if (compute->FrontRunsMacros()) {
                        compute->FrontCallMethod(system, split, dma_state.method, argument,
                                                 dma_state.is_last_call, opt_segment);
                        return;
                    }
                }
            }
            split.Method(subchannel, dma_state.method, argument, dma_state.is_last_call,
                         opt_segment, effect);
            if (engine_type == Engines::EngineTypes::MaxwellDMA) {
                NoteDmaSemaphore(dma_state.method, argument);
            }
            return;
        }
        if (!subchannel->execution_mask[dma_state.method]) {
            subchannel->method_sink.emplace_back(dma_state.method, argument);
        } else {
            VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
            subchannel->ConsumeSink(system);
            subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
            subchannel->CallMethod(system, dma_state.method, argument, dma_state.is_last_call);
        }
    }
}

void DmaPusher::CallMultiMethod(const u32* base_start, u32 num_methods) {
    if (g_semaphore_watch != 0) [[unlikely]] {
        for (u32 i = 0; i < num_methods; ++i) {
            WatchArgument(dma_state.method + (dma_state.non_incrementing ? 0 : i), base_start[i]);
        }
    }
    auto& split = VideoCommon::GpuSplit::Get();
    if (dma_state.method < non_puller_methods) {
        puller.CallMultiMethod(*this, dma_state.method, dma_state.subchannel, base_start, num_methods, dma_state.method_count);
    } else {
        auto subchannel = subchannels[dma_state.subchannel];
        if (subchannel == nullptr) {
            ReportUnboundSubchannel();
            return;
        }
        // Compute dispatches and DMA copies write the command lists ReadGpuWrittenCommands reads.
        const auto engine_type = subchannel_type[dma_state.subchannel];
        const bool writer = IsMemoryWriter(*subchannel, engine_type, dma_state.method);
        // Copies and blits are GPU work for the front's reads (GpuSplit::Effect).
        const auto effect = engine_type == Engines::EngineTypes::MaxwellDMA ||
                                    engine_type == Engines::EngineTypes::Fermi2D
                                ? VideoCommon::GpuSplit::Effect::Gpu
                                : VideoCommon::GpuSplit::Effect::Unknown;
        SCOPE_EXIT {
            if (writer) {
                split.NoteWriter(engine_type == Engines::EngineTypes::KeplerCompute
                                     ? VideoCommon::GpuSplit::WriterKind::Compute
                                     : VideoCommon::GpuSplit::WriterKind::Copy);
            }
        };
        if (split.Active()) {
            const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
            const GPUVAddr* const opt_segment = WantsSegment() ? &segment : nullptr;
            if (split.FrontMacros()) {
                const auto type = subchannel_type[dma_state.subchannel];
                if (type == Engines::EngineTypes::Maxwell3D) {
                    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
                    if (maxwell3d->GetNativeMacroEngine().native_turing) {
                        maxwell3d->FrontCallMultiMethod(system, split, dma_state.method, base_start,
                                                        num_methods, dma_state.method_count);
                        return;
                    }
                } else if (type == Engines::EngineTypes::KeplerCompute) {
                    auto* const compute = static_cast<Engines::KeplerCompute*>(subchannel);
                    if (compute->FrontRunsMacros()) {
                        compute->FrontCallMultiMethod(system, split, dma_state.method, base_start,
                                                      num_methods, dma_state.method_count, opt_segment);
                        return;
                    }
                }
            }
            split.MultiMethod(subchannel, dma_state.method, base_start, num_methods,
                              dma_state.method_count, opt_segment, effect);
            if (engine_type == Engines::EngineTypes::MaxwellDMA) {
                for (u32 i = 0; i < num_methods; ++i) {
                    NoteDmaSemaphore(dma_state.method, base_start[i]);
                }
            }
            return;
        }
        VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
        subchannel->ConsumeSink(system);
        subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
        subchannel->CallMultiMethod(system, dma_state.method, base_start, num_methods, dma_state.method_count);
    }
}

void DmaPusher::NoteDmaSemaphore(u32 method, u32 argument) {
    using Engines::MaxwellDMA;
    if (method >= MaxwellDMA::SemaphoreMethod && method < MaxwellDMA::SemaphoreMethod + 3) {
        dma_semaphore[method - MaxwellDMA::SemaphoreMethod] = argument;
        return;
    }
    if (method != MaxwellDMA::LaunchDmaMethod) {
        return;
    }
    // As MaxwellDMA::ReleaseSemaphore writes it.
    using SemaphoreType = MaxwellDMA::LaunchDMA::SemaphoreType;
    const auto type = std::bit_cast<MaxwellDMA::LaunchDMA>(argument).semaphore_type.Value();
    if (type != SemaphoreType::RELEASE_ONE_WORD_SEMAPHORE &&
        type != SemaphoreType::RELEASE_FOUR_WORD_SEMAPHORE) {
        return;
    }
    const GPUVAddr address = MaxwellDMA::PackedGPUVAddr{dma_semaphore[0], dma_semaphore[1]};
    const auto device = memory_manager.GpuToCpuAddress(address);
    if (!device) {
        return;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    split.NoteSemaphoreWrite(*device, dma_semaphore[2], true);
    if (type == SemaphoreType::RELEASE_FOUR_WORD_SEMAPHORE) {
        // The payload as 64 bits (a timestamp follows).
        split.NoteSemaphoreWrite(*device + 4, 0, true);
    }
}

void DmaPusher::RefreshSnapshot(CommandList& list, u64 now) {
    list.snapshot_suspensions = now;
    for (size_t i = 0; i < list.command_lists.size() && i < list.submitted_offsets.size(); ++i) {
        const CommandListHeader& entry = list.command_lists[i];
        const u32 offset = list.submitted_offsets[i];
        if (offset + entry.size > list.submitted_words.size()) {
            continue;
        }
        memory_manager.ReadBlockUnsafe(entry.addr, list.submitted_words.data() + offset,
                                       entry.size * sizeof(u32));
    }
    list.deferred_entries.clear();
}

void DmaPusher::RefreshDeferredEntries(CommandList& list) {
    for (const u32 i : list.deferred_entries) {
        if (i >= list.command_lists.size() || i >= list.submitted_offsets.size()) {
            continue;
        }
        const CommandListHeader& entry = list.command_lists[i];
        const u32 offset = list.submitted_offsets[i];
        if (offset + entry.size > list.submitted_words.size()) {
            continue;
        }
        memory_manager.ReadBlockUnsafe(entry.addr, list.submitted_words.data() + offset,
                                       entry.size * sizeof(u32));
    }
    list.deferred_entries.clear();
}

namespace {
/// EDEN_NX2_LIST_SPECULATE: longer lists wait for their writers as before.
constexpr size_t MaxSpeculatedListPages = 16;

bool ListSpeculation() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_LIST_SPECULATE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}
} // Anonymous namespace

bool DmaPusher::SpeculateList(GPUVAddr address, size_t size) {
    // EDEN_NX2_LIST_SPECULATE=1 (opt-in, not proven safe): parse a command list on pages no GPU
    // work has written without waiting for the compute and copy work sent before the last host
    // sync. In Donkey Kong Bananza that work never wrote one of the lists it held up (0 of
    // 11,000), while the waits cost the front ~20 ms a frame and left the back idle. The back
    // checks the list's pages once it ran that work (the GPU-written bits are set as it records
    // writes and never clear): a page set by then is a hazard, logged and counted, and the page's
    // lists take the GPU-written path from then on.
    if (!ListSpeculation() || size == 0 ||
        !VideoCommon::GpuSplit::Get().SyncedWritersQueued()) {
        return false; // nothing to wait for: read it as before
    }
    namespace Profile = VideoCommon::Nx2Profile;
    const GPUVAddr first_page = address & ~GPUVAddr{0xfff};
    const GPUVAddr last_page = (address + size - 1) & ~GPUVAddr{0xfff};
    if (last_page - first_page >= MaxSpeculatedListPages * 0x1000) {
        Profile::Add(Profile::Counter::ListSpeculationLong, 1);
        return false;
    }
    std::array<DAddr, MaxSpeculatedListPages> devices{};
    size_t num_pages = 0;
    for (GPUVAddr page = first_page; page <= last_page; page += 0x1000) {
        const auto device = memory_manager.GpuToCpuAddress(page);
        if (!device) {
            Profile::Add(Profile::Counter::ListSpeculationUnmapped, 1);
            return false;
        }
        devices[num_pages++] = *device;
    }
    Profile::Add(Profile::Counter::ListsSpeculated, 1);
    for (size_t i = 0; i < num_pages; ++i) {
        VideoCommon::GpuSplit::Get().Defer(
            [page = devices[i], address, size, channel = channel_state.bind_id] {
                if (!Tegra::Nx2IsGpuWritten(page)) {
                    return;
                }
                VideoCommon::Nx2Profile::Add(
                    VideoCommon::Nx2Profile::Counter::ListSpeculationHazards, 1);
                static std::atomic<u32> logged{};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
                    LOG_ERROR(HW_GPU,
                              "List speculation hazard: channel {} parsed {:#x}+{:#x} before the "
                              "work that writes its pages (device {:#x}) ran",
                              channel, address, size, page);
                }
            },
            VideoCommon::GpuSplit::Effect::None);
    }
    return true;
}

bool DmaPusher::IsGpuWrittenCommands(GPUVAddr address, size_t size) {
    if (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) || size == 0) {
        return false;
    }
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page);
            device && Tegra::Nx2IsGpuWritten(*device)) {
            return true;
        }
    }
    return false;
}

bool DmaPusher::ReadSpeculatedGpuWrittenCommands(GPUVAddr address, u32 count) {
    // EDEN_NX2_LIST_SPECULATE=1: read a list on GPU-written pages from guest memory instead of
    // waiting for the queued writers and the GPU's copy of the page. Donkey Kong Bananza's
    // texture binding lists share pages with storage the GPU writes, and guest memory held the
    // same words as the copy in every check (26,000 of 26,000). The back compares each read with
    // the page's copy once the GPU made it; a page that differed once waits from then on.
    namespace Profile = VideoCommon::Nx2Profile;
    const GPUVAddr end = address + u64{count} * sizeof(u32);
    if (((end - 1) & ~GPUVAddr{0xfff}) - (address & ~GPUVAddr{0xfff}) >=
        MaxSpeculatedListPages * 0x1000) {
        Profile::Add(Profile::Counter::ListSpeculationLong, 1);
        return false;
    }
    struct Part {
        DAddr device;
        u32 size;
    };
    std::array<Part, MaxSpeculatedListPages> parts{};
    size_t num_parts = 0;
    for (GPUVAddr chunk = address; chunk < end;) {
        const GPUVAddr chunk_end = (std::min)(end, (chunk & ~GPUVAddr{0xfff}) + 0x1000);
        const auto device = memory_manager.GpuToCpuAddress(chunk);
        if (!device) {
            Profile::Add(Profile::Counter::ListSpeculationUnmapped, 1);
            return false;
        }
        if (!rasterizer->Nx2ListSpeculationAllowed(*device & ~DAddr{0xfff})) {
            Profile::Add(Profile::Counter::ListSpeculationHazardPage, 1);
            return false;
        }
        parts[num_parts++] = {*device, static_cast<u32>(chunk_end - chunk)};
        chunk = chunk_end;
    }
    command_headers.resize_destructive(count);
    u8* const out = reinterpret_cast<u8*>(command_headers.data());
    memory_manager.ReadBlockUnsafe(address, out, size_t{count} * sizeof(u32));
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::ListsSpeculated, 1);
    size_t offset = 0;
    for (size_t i = 0; i < num_parts; ++i) {
        const Part part = parts[i];
        const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(out + offset), part.size);
        offset += part.size;
        if (!Tegra::Nx2IsGpuWritten(part.device)) {
            continue;
        }
        VideoCommon::GpuSplit::Get().Defer(
            [rasterizer = rasterizer, part, hash] {
                rasterizer->Nx2CheckSpeculatedList(part.device, part.size, hash);
            },
            VideoCommon::GpuSplit::Effect::None);
    }
    return true;
}

bool DmaPusher::ReadGpuWrittenCommands(GPUVAddr address, u32 count, bool* pending) {
    // The pages are marked as command list pages: from the next write on, the buffer cache copies
    // them to host memory right after the dispatch or copy that writes them and submits. The
    // front then waits only for the back to run that work and for the GPU to finish the copy, not
    // for everything queued and a cache flush (FlushGpuWrittenCommands: ~400 GPU round trips a
    // second in a Mario Kart World race, during which the back had nothing to do). Pages
    // FlushGpuWrittenCommands flushed whole and no GPU work wrote since are read from guest
    // memory.
    if (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) || count == 0 ||
        rasterizer == nullptr) {
        return false;
    }
    const GPUVAddr end = address + u64{count} * sizeof(u32);
    bool written = false;
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < end; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page);
            device && Tegra::Nx2IsGpuWritten(*device)) {
            Tegra::Nx2MarkCommandListPage(*device);
            written = true;
        }
    }
    if (!written) {
        return false;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    // Resuming after the copy's tick: the back ran the list's writer before the suspension
    // (this wait ran then), and waiting again would wait for the other channels' work queued
    // meanwhile too.
    const bool resumed = std::exchange(resumed_list_read, false);
    if (split.Active() && !resumed) {
        if (ListSpeculation() && split.WritersQueued() &&
            ReadSpeculatedGpuWrittenCommands(address, count)) {
            return true;
        }
        split.WaitForWriters();
    }
    bool past_sync = !split.Active();
    using GpuWrittenList = VideoCore::RasterizerInterface::GpuWrittenList;
    command_headers.resize_destructive(count);
    u8* const out = reinterpret_cast<u8*>(command_headers.data());
    for (GPUVAddr chunk = address; chunk < end;) {
        const GPUVAddr chunk_end = (std::min)(end, (chunk & ~GPUVAddr{0xfff}) + 0x1000);
        const std::span<u8> part{out + (chunk - address), static_cast<size_t>(chunk_end - chunk)};
        const auto device = memory_manager.GpuToCpuAddress(chunk);
        if (!device) {
            return false;
        }
        GpuWrittenList where = GpuWrittenList::Synced;
        if (Tegra::Nx2IsGpuWritten(*device)) {
            u64 tick = 0;
            u64* const pending_tick = pending != nullptr ? &tick : nullptr;
            where = rasterizer->ReadGpuWrittenCommandList(*device, part, past_sync, pending_tick);
            if (where == GpuWrittenList::NeedsSync) {
                split.WaitForSync();
                past_sync = true;
                where = rasterizer->ReadGpuWrittenCommandList(*device, part, true, pending_tick);
            }
            if (where == GpuWrittenList::Pending) {
                // The copy is still on the GPU: the channel waits for it without blocking the
                // others (Step parses this entry again once IsGpuTickDone).
                pending_list_tick = tick;
                *pending = true;
                return false;
            }
        }
        if (where == GpuWrittenList::Unknown) {
            return false;
        }
        if (where == GpuWrittenList::Synced) {
            memory_manager.ReadBlockUnsafe(chunk, part.data(), part.size());
        }
        chunk = chunk_end;
    }
    return true;
}

void DmaPusher::FlushGpuWrittenCommands(GPUVAddr address, size_t size) {
    // Title 1 runs small command lists its culling dispatches fill in each frame. Accurate GPU
    // mode reads every command list through the caches (safe reads); here only the ones on pages
    // the GPU has written are, or their draws run with an older frame's words (trees vanish).
    // These are the lists ReadGpuWrittenCommands can't take from a copy or guest memory (a page's
    // first list, or pages draws write too).
    if (!IsGpuWrittenCommands(address, size)) {
        return;
    }
    // The caches belong to the split's back: it first runs what the front queued (the dispatch
    // that writes these words among it). The whole pages are flushed: their next lists read
    // guest memory until GPU work writes them again.
    VideoCommon::GpuSplit::Get().Drain(VideoCommon::DrainSite::ListFlush);
    const GPUVAddr first_page = address & ~GPUVAddr{0xfff};
    const GPUVAddr end_page = (address + size + 0xfff) & ~GPUVAddr{0xfff};
    if (memory_manager.IsMemoryDirty(first_page, end_page - first_page,
                                     VideoCommon::CacheType::BufferCache)) {
        // The download records into the Vulkan scheduler, which the split's back owns: a drained
        // back still submits from its idle hook, so the front must not flush these pages itself.
        VideoCommon::GpuSplit::Get().Run(
            [memory = &memory_manager, first_page, size = end_page - first_page] {
                memory->FlushRegion(first_page, size, VideoCommon::CacheType::BufferCache);
            },
            VideoCommon::DrainSite::ListFlush);
    }
    if (rasterizer == nullptr) {
        return;
    }
    for (GPUVAddr page = first_page; page < end_page; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page);
            device && Tegra::Nx2IsCommandListPage(*device)) {
            rasterizer->NoteGpuWrittenCommandListSynced(*device);
        }
    }
}

void DmaPusher::ReportUnboundSubchannel() {
    // Methods sent to a subchannel with no engine (no SetObject yet) are dropped.
    static std::atomic<u32> reported{};
    if (reported.fetch_add(1) < 8) {
        LOG_WARNING(HW_GPU, "Dropping method {:#x} on unbound subchannel {}", dma_state.method,
                    dma_state.subchannel);
    }
}

void DmaPusher::CheckSubmitHash(const CommandListHeader& header, u64 submit_hash) {
    std::vector<u32> words(header.size);
    memory_manager.ReadBlockUnsafe(header.addr, words.data(), words.size() * sizeof(u32));
    static std::atomic<u64> checked{};
    static std::atomic<u64> changed{};
    ++checked;
    if (HashPushbufferWords(words) != submit_hash) {
        if (changed.fetch_add(1) < 16) {
            LOG_WARNING(HW_GPU, "pushbuffer entry {:#x}+{:#x} changed between submit and execution",
                        header.addr.Value(), header.size.Value());
        }
    }
    if ((checked % 20000) == 0) {
        LOG_WARNING(HW_GPU, "pushbuffer check: {} of {} entries changed after submit",
                    changed.load(), checked.load());
    }
}

void DmaPusher::RecordParsedEntry(const CommandListHeader& header,
                                  std::span<const CommandHeader> commands) {
    if (!g_record_parsed_entries) {
        return;
    }
    u64 hash = 0xcbf29ce484222325ULL;
    for (const CommandHeader& command : commands) {
        hash = (hash ^ command.argument) * 0x100000001b3ULL;
    }
    parsed_entries[parsed_entry_count++ % parsed_entries.size()] = {header.addr, static_cast<u32>(header.size), hash};
}

void DmaPusher::DumpCurrentEntry(const char* reason) {
    // Entries whose memory changed after they were parsed (the CPU rewrote them meanwhile).
    for (size_t i = 0; i < std::min(parsed_entry_count, parsed_entries.size()); ++i) {
        const auto& entry = parsed_entries[(parsed_entry_count - 1 - i) % parsed_entries.size()];
        std::vector<u32> words(entry.size);
        memory_manager.ReadBlockUnsafe(entry.address, words.data(), words.size() * sizeof(u32));
        u64 hash = 0xcbf29ce484222325ULL;
        for (const u32 word : words) {
            hash = (hash ^ word) * 0x100000001b3ULL;
        }
        LOG_WARNING(HW_GPU, "  parsed -{}: {:#x}+{:#x} {}", i, entry.address, entry.size,
                    hash == entry.hash ? "unchanged" : "CHANGED since parsed");
    }
    // EDEN_NX2_PB_DUMP=<file>: also write the whole command list (u64 address, u32 size and the
    // current words of every entry, then the current entry index) for offline replay.
    if (static const char* const path = std::getenv("EDEN_NX2_PB_DUMP");
        path != nullptr && !dma_pushbuffer.empty()) {
        if (std::FILE* file = std::fopen(path, "wb")) {
            const auto& lists = dma_pushbuffer.front().command_lists;
            const u64 current = dma_pushbuffer_subindex;
            const u64 count = lists.size();
            std::fwrite(&count, sizeof(count), 1, file);
            std::fwrite(&current, sizeof(current), 1, file);
            for (const CommandListHeader& header : lists) {
                const u64 address = header.addr;
                const u32 size = static_cast<u32>(header.size);
                std::vector<u32> words(size);
                memory_manager.ReadBlockUnsafe(address, words.data(), words.size() * sizeof(u32));
                std::fwrite(&address, sizeof(address), 1, file);
                std::fwrite(&size, sizeof(size), 1, file);
                std::fwrite(words.data(), sizeof(u32), words.size(), file);
            }
            std::fclose(file);
        }
    }
    LOG_WARNING(HW_GPU, "pushbuffer dump ({}): method={:#x} sub={} count={} non_inc={} get={:#x}",
                reason, dma_state.method, dma_state.subchannel, dma_state.method_count,
                dma_state.non_incrementing, dma_state.dma_get);
    if (dma_pushbuffer.empty()) {
        return;
    }
    const auto& lists = dma_pushbuffer.front().command_lists;
    const size_t first = dma_pushbuffer_subindex >= 2 ? dma_pushbuffer_subindex - 2 : 0;
    for (size_t i = first; i <= dma_pushbuffer_subindex && i < lists.size(); ++i) {
        const CommandListHeader& header = lists[i];
        LOG_WARNING(HW_GPU, "  entry {} of {}: addr={:#x} size={:#x}", i, lists.size(),
                    header.addr.Value(), header.size.Value());
        for (u64 word = 0; word < std::min<u64>(header.size, 0x80); word += 8) {
            std::string gpu_line;
            std::string cpu_line;
            for (u64 j = word; j < std::min<u64>(header.size, word + 8); ++j) {
                const GPUVAddr address = header.addr + j * sizeof(u32);
                const u32 gpu_value = memory_manager.Read<u32>(address);
                const u32 cpu_value = system.ApplicationMemory().Read32(address);
                gpu_line += fmt::format(" {:08x}", gpu_value);
                cpu_line += gpu_value == cpu_value ? std::string(" ........")
                                                   : fmt::format(" {:08x}", cpu_value);
            }
            LOG_WARNING(HW_GPU, "    +{:03x}:{}", word * 4, gpu_line);
            LOG_WARNING(HW_GPU, "    cpu :{}", cpu_line);
        }
    }
}

void DmaPusher::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

} // namespace Tegra

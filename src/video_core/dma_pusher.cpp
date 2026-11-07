// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/memory.h"
#include "video_core/nx2_profile.h"
#include "video_core/dma_pusher.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/guest_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra {

constexpr u32 MacroRegistersStart = 0xE00;
constexpr u32 ComputeInline = 0x6D;

bool DmaPusher::IsMemoryWriter(const Engines::EngineInterface& engine, Engines::EngineTypes type,
                               u32 method) {
    // Compute dispatches, DMA copies and inline uploads write what the front reads (the command
    // lists ReadGpuWrittenCommands reads, MME DMA reads); the register writes that set them up
    // do not, and moved the writer mark past queued draws the front then waited for.
    // EDEN_NX2_WRITER_CLASSIFY=0: every method of these engines counts, as before.
    static const bool classify = [] {
        const char* value = std::getenv("EDEN_NX2_WRITER_CLASSIFY");
        return value == nullptr || value[0] != '0';
    }();
    switch (type) {
    case Engines::EngineTypes::KeplerCompute:
        return !classify ||
               static_cast<const Engines::KeplerCompute&>(engine).MayWriteMemory(method);
    case Engines::EngineTypes::MaxwellDMA:
    case Engines::EngineTypes::KeplerMemory:
        return !classify || method >= MacroRegistersStart || engine.execution_mask[method];
    default:
        return false;
    }
}

namespace {
// Command list (GPFIFO) entries by their sync flag, logged every 10 s (the thread parsing
// command lists): whether the entries on GPU-written pages carry it, and whether the split's
// writer waits (EDEN_NX2_WRITER_WAIT) ever found a page newly written.
struct EntrySyncStats {
    std::chrono::steady_clock::time_point window_start{};
    u64 entries{};
    u64 synced{};          ///< Of entries: with the sync flag
    u64 written{};         ///< On pages known GPU-written before any wait
    u64 written_synced{};  ///< Of written: with the sync flag
    u64 waits{};           ///< The front waited for queued compute and copy work
    u64 waits_synced{};    ///< Of waits: with the sync flag
    u64 newly_written{};   ///< Of waits: the pages were GPU-written after it (the wait mattered)
};
EntrySyncStats g_entry_sync_stats;

void NoteEntrySync(bool sync, bool written, bool waited, bool newly_written) {
    EntrySyncStats& stats = g_entry_sync_stats;
    ++stats.entries;
    stats.synced += sync ? 1 : 0;
    stats.written += written ? 1 : 0;
    stats.written_synced += written && sync ? 1 : 0;
    stats.waits += waited ? 1 : 0;
    stats.waits_synced += waited && sync ? 1 : 0;
    stats.newly_written += newly_written ? 1 : 0;
    if ((stats.entries & 1023) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (stats.window_start == std::chrono::steady_clock::time_point{}) {
        stats.window_start = now;
        return;
    }
    if (now - stats.window_start < std::chrono::seconds{10}) {
        return;
    }
    LOG_INFO(HW_GPU,
             "Command list entries in {:.1f} s: {} ({} with the sync flag), {} on GPU-written "
             "pages ({} with the sync flag); {} waited for queued compute and copy work ({} with "
             "the sync flag), {} found their pages newly GPU-written",
             std::chrono::duration<double>(now - stats.window_start).count(), stats.entries,
             stats.synced, stats.written, stats.written_synced, stats.waits, stats.waits_synced,
             stats.newly_written);
    stats = {};
    stats.window_start = now;
}
} // Anonymous namespace

DmaPusher::DmaPusher(Core::System& system_, MemoryManager& memory_manager_, Control::ChannelState& channel_state_)
    : system{system_}
    , memory_manager{memory_manager_}
    , channel_state{channel_state_}
    , signal_sync{false}
    , synced{false}
{}

DmaPusher::~DmaPusher() = default;

void DmaPusher::DispatchCalls() {
    // The sub-index is kept: a channel suspended on a semaphore resumes inside its command list.
    dma_state.is_last_call = true;
    while (system.IsPoweredOn()) {
        if (!Step()) {
            break;
        }
    }
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer([gpu = &system.GPU()] {
            gpu->FlushCommands();
            gpu->OnCommandListEnd();
        });
        split.Publish();
        return;
    }
    system.GPU().FlushCommands();
    system.GPU().OnCommandListEnd();
}

namespace {
// EDEN_NX2_RESUME_SAME_WORDS=1: an entry resumed after an acquire continues from the words its
// parse started from (DmaPusher::entry_record) instead of memory read again, which can mix two
// versions of an entry rewritten after submit.
bool ResumeSameWords() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_RESUME_SAME_WORDS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// EDEN_NX2_RESNAPSHOT_AGE_MS=<ms> (0: no limit): a list submitted longer ago than that keeps its
// submit-time words when its channel resumes from an acquire (or it is queued after one),
// instead of reading memory again, and an entry of it resumed after an acquire continues from
// them. Games write lists they submitted before releasing the acquire soon after the submit;
// after a long wait the game may have reused that memory for later frames. Ocarina of Time
// defaults to 1 ms, that is it always keeps its copies: it rewrites a list's memory for a frame
// two ahead in under 500 ms, and on the phone its lists run 0.5-2 s after their submit. A list
// copied again at a resume while younger than 500 ms waited for 0x8e1, where its submit-time
// words waited for the 0x8bb the semaphore already held: every channel deadlocked after the
// save loaded.
u64 ResnapshotMaxAgeNs() {
    static const s64 forced = [] {
        const char* value = std::getenv("EDEN_NX2_RESNAPSHOT_AGE_MS");
        return value != nullptr ? static_cast<s64>(std::strtoull(value, nullptr, 10)) : s64{-1};
    }();
    if (forced >= 0) {
        return static_cast<u64>(forced) * 1'000'000;
    }
    constexpr u64 OcarinaOfTime = 0x0400486024DEC000ULL;
    const u64 program = Tegra::g_nx2_program_id.load(std::memory_order_relaxed);
    const u64 limit = program == OcarinaOfTime ? u64{1'000'000} : u64{0};
    // Once per title: which limit applies.
    if (static std::atomic<u64> logged{~u64{0}};
        logged.load(std::memory_order_relaxed) != program &&
        logged.exchange(program, std::memory_order_relaxed) != program) {
        LOG_INFO(HW_GPU, "Command list copies: program {:016X} keeps copies older than {} ms{}",
                 program, limit / 1'000'000, limit == 0 ? " (no limit)" : "");
    }
    return limit;
}

u64 SteadyNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

// Whether the list is old enough to keep its copy (ResnapshotMaxAgeNs).
bool KeepsCopy(const Tegra::CommandList& list) {
    const u64 max_age = ResnapshotMaxAgeNs();
    return max_age != 0 && list.submit_ns != 0 && SteadyNs() - list.submit_ns > max_age;
}
} // Anonymous namespace

// Before an entry's words are parsed: keep them when parsing from its start.
void DmaPusher::NoteEntryWords(const CommandListHeader& header, const char* source,
                               std::span<const CommandHeader> commands, size_t resume_offset) {
    if (!record_parsed_entries && !ResumeSameWords()) {
        return;
    }
    EntryRecord& record = entry_record;
    record.resumed_at = resume_offset;
    if (resume_offset != 0 && record.address == header.addr && record.size == header.size) {
        return; // a resumed parse: the record keeps the first parse's words
    }
    record.address = header.addr;
    record.size = static_cast<u32>(header.size);
    record.index = dma_pushbuffer_subindex;
    record.source = source;
    record.words.assign(commands.begin(), commands.end());
    record.header_count = 0;
}

bool DmaPusher::Step() {
    if (!ib_enable || dma_pushbuffer.empty()) {
        return false;
    }
    const u64 suspensions_before = suspensions.load(std::memory_order_relaxed);
    const auto suspended_since = puller.PendingSince();
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
        size_t lists = 0;
        size_t entries = 0;
        size_t changed = 0;
        size_t kept = 0;
        u64 oldest_ns = 0;
        const u64 now_ns = SteadyNs();
        for (CommandList& list : dma_pushbuffer) {
            if (!list.submitted_offsets.empty()) {
                ++lists;
                kept += KeepsCopy(list) ? 1 : 0;
                entries += list.command_lists.size();
                oldest_ns = (std::max)(oldest_ns, now_ns - list.submit_ns);
                changed += RefreshSnapshot(list, now, 2);
            }
        }
        // EDEN_NX2_PB_CHECK: a long wait whose queued lists changed since they were copied (the
        // game may have reused their memory meanwhile).
        const auto waited = std::chrono::steady_clock::now() - suspended_since;
        if (record_parsed_entries && (changed != 0 || kept != 0) &&
            waited > std::chrono::milliseconds{100}) {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1) < 32) {
                LOG_WARNING(HW_GPU,
                            "Channel {} resumed after {} ms: {} queued lists ({} entries, oldest "
                            "submitted {} ms ago), {} kept their copy, the others read again: {} "
                            "entries changed since copied",
                            channel_state.bind_id,
                            std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(),
                            lists, entries, oldest_ns / 1'000'000, kept, changed);
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
    if (command_resume_offset == 0) {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::GpfifoEntries, 1);
    }

    if (signal_sync && !synced) {
        std::unique_lock lk(sync_mutex);
        sync_cv.wait(lk, [this]() { return synced.load(); });
        signal_sync = false;
        synced = false;
    }

    if (header.size > 0) {
        if (subchannels[dma_state.subchannel] && dma_state.method_count) {
            const auto engine = subchannel_type[dma_state.subchannel];
            const bool kepler_payload = engine == Engines::EngineTypes::KeplerCompute && dma_state.method == ComputeInline && dma_state.non_incrementing;
            const bool macro_payload = engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart;
            if (kepler_payload || macro_payload) {
                const size_t words = std::min<size_t>(dma_state.method_count, header.size);
                auto* const engine_state = subchannels[dma_state.subchannel];
                if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
                    // The caches behind IsMemoryDirty belong to the split's back thread.
                    split.Defer([engine_state, memory = &memory_manager, address = GPUVAddr{dma_state.dma_get},
                                 bytes = words * sizeof(u32)] {
                        engine_state->current_dirty = memory->IsMemoryDirty(address, bytes);
                    });
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
        bool gpu_written =
            IsGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
        // Only with the command list series on (nx2_command_lists): without it no page is ever
        // marked, IsGpuWrittenCommands is always false, and the wait (for every compute dispatch
        // or copy queued for the back) ran for every entry, ~130,000 times a second, for nothing.
        // EDEN_NX2_WRITER_WAIT=0 skips it, as sylvieistaken/selevet-eglantine f28449539b does:
        // only pages already GPU-written wait, and a page's first write can be read a frame late.
        // EDEN_NX2_WRITER_WAIT=sync: only entries with the GPFIFO sync flag (GP_ENTRY1_SYNC: the
        // host waits for the work before the entry before fetching it) wait; the bits marking a
        // page GPU-written never clear, so the wait only matters for a page's first write. The
        // entries' flags are logged every 10 s (NoteEntrySync) to check the GPU-written ones
        // carry it.
        static const char writer_wait = [] {
            const char* value = std::getenv("EDEN_NX2_WRITER_WAIT");
            return value == nullptr || value[0] == '\0' ? '1' : value[0];
        }();
        const bool wait_writers = writer_wait == 's' ? header.sync != 0 : writer_wait != '0';
        const bool written_before = gpu_written;
        bool writers_waited = false;
        if (!gpu_written && wait_writers && Tegra::g_nx2_ampere.load(std::memory_order_relaxed) &&
            Tegra::Nx2CommandLists() && VideoCommon::GpuSplit::Get().Active() &&
            VideoCommon::GpuSplit::Get().WaitForWriters()) {
            writers_waited = true;
            gpu_written =
                IsGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
        }
        if (Tegra::g_nx2_ampere.load(std::memory_order_relaxed) && Tegra::Nx2CommandLists()) {
            NoteEntrySync(header.sync != 0, written_before, writers_waited,
                          gpu_written && !written_before);
        }
        // The words as submitted, unless this is a resumed entry or the GPU has newer ones the
        // safe read below would flush (with the split, the front can't tell: it reads memory
        // unflushed anyway). Entries on pages the GPU writes (culling) always take the flushing
        // read: their words at submit time are an older frame's.
        const bool submitted = command_resume_offset == 0 &&
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
        const bool resume_same = ResumeSameWords() && command_resume_offset != 0 &&
                                 entry_record.address == header.addr &&
                                 entry_record.size == header.size &&
                                 entry_record.words.size() == header.size;
        // An entry first parsed from its list's copy continues from it after an acquire when the
        // list keeps its copy (KeepsCopy): memory may hold a later frame's commands by now.
        const bool resume_copy =
            command_resume_offset != 0 && entry_from_copy &&
            dma_pushbuffer_subindex < command_list.submitted_offsets.size() &&
            command_list.submitted_offsets[dma_pushbuffer_subindex] + header.size <=
                command_list.submitted_words.size() &&
            KeepsCopy(command_list);
        if (command_resume_offset == 0) {
            entry_from_copy = submitted;
        }
        if (resume_same) {
            const std::span<const CommandHeader> commands{entry_record.words};
            entry_record.resumed_at = command_resume_offset;
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else if (resume_copy) {
            const std::span<const CommandHeader> commands{
                command_list.submitted_words.data() +
                    command_list.submitted_offsets[dma_pushbuffer_subindex],
                header.size};
            NoteEntryWords(header, "snapshot (resumed)", commands, command_resume_offset);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else if (submitted) {
            const std::span<const CommandHeader> commands{
                command_list.submitted_words.data() +
                    command_list.submitted_offsets[dma_pushbuffer_subindex],
                header.size};
            RecordParsedEntry(header, commands);
            NoteEntryWords(header, command_list.copied_at == 0   ? "snapshot"
                                   : command_list.copied_at == 1 ? "snapshot copied when queued"
                                                                 : "snapshot copied at resume",
                           commands, 0);
            command_resume_offset += ProcessCommands(commands, 0);
        } else if (use_safe) {
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::SafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            RecordParsedEntry(header, commands);
            NoteEntryWords(header, "memory (safe)", commands, command_resume_offset);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else if (ReadGpuWrittenCommands(dma_state.dma_get, static_cast<u32>(header.size))) {
            const std::span<const CommandHeader> commands{command_headers.data(), header.size};
            RecordParsedEntry(header, commands);
            NoteEntryWords(header, "GPU-written list copy", commands, command_resume_offset);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else {
            FlushGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::UnsafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            RecordParsedEntry(header, commands);
            NoteEntryWords(header, "memory", commands, command_resume_offset);
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
        // A rejected block cannot continue past its submission; do not let its skip eat the
        // headers of the next one.
        dma_state.skip_words = 0;
        dma_pushbuffer.pop_front();
        dma_pushbuffer_subindex = 0;
    } else {
        signal_sync = command_list.command_lists[dma_pushbuffer_subindex].sync && Settings::values.sync_memory_operations.GetValue();
    }

    if (signal_sync) {
        VideoCommon::GpuSplit::Get().Drain(VideoCommon::Nx2Profile::Zone::DrainSync);
        rasterizer->SignalFence([this]() {
            std::scoped_lock lk(sync_mutex);
            synced = true;
            sync_cv.notify_all();
        });
    }

    return true;
}


size_t DmaPusher::ProcessCommands(std::span<const CommandHeader> commands, size_t word_offset) {
    for (size_t index = 0; index < commands.size();) {
        // Discard the rest of a command block that was rejected (illegal method). The block's
        // declared count still tells where the next header starts, so the stream stays aligned
        // instead of running thousands of bogus methods.
        if (dma_state.skip_words != 0) {
            const u32 skip =
                static_cast<u32>(std::min<std::size_t>(dma_state.skip_words, commands.size() - index));
            index += skip;
            dma_state.skip_words -= skip;
            continue;
        }
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
            // A whole incrementing run within the 3D engine's constant buffer offset and data
            // registers goes to the split's back as one entry (Maxwell3D::LoadConstantRun), not
            // a method call per word. EDEN_NX2_CB_BATCH_PB=0 turns it off.
            static const bool cb_runs = [] {
                const char* value = std::getenv("EDEN_NX2_CB_BATCH_PB");
                return value == nullptr || value[0] != '0';
            }();
            using Engines::Maxwell3D;
            constexpr u32 cb_offset = MAXWELL3D_REG_INDEX(const_buffer.offset);
            constexpr u32 cb_last = MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15;
            if (cb_runs && !dma_state.non_incrementing && !dma_increment_once &&
                dma_state.method >= cb_offset && dma_state.method <= cb_last &&
                dma_state.method_count <= Engines::Maxwell3D::MaxConstantRun &&
                dma_state.method_count <= cb_last - dma_state.method + 1 &&
                index + dma_state.method_count <= commands.size() && semaphore_watch == 0 &&
                subchannel_type[dma_state.subchannel] == Engines::EngineTypes::Maxwell3D &&
                subchannels[dma_state.subchannel] != nullptr) {
                auto& split = VideoCommon::GpuSplit::Get();
                auto* const maxwell3d =
                    static_cast<Engines::Maxwell3D*>(subchannels[dma_state.subchannel]);
                if (split.Active() && split.FrontMacros() &&
                    maxwell3d->GetNativeMacroEngine().native_turing &&
                    maxwell3d->FrontLoadConstantRun(system, split, dma_state.method,
                                                    &commands[index].argument,
                                                    dma_state.method_count)) {
                    index += dma_state.method_count;
                    dma_state.method += dma_state.method_count;
                    dma_state.method_count = 0;
                    dma_state.is_last_call = true;
                    continue;
                }
            }
            // Fast-path batching for consecutive non-execution methods in increasing mode
            if (!dma_state.non_incrementing && !dma_increment_once &&
                dma_state.method >= non_puller_methods &&
                !VideoCommon::GpuSplit::Get().Active() &&
                semaphore_watch == 0) {
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
            if (record_parsed_entries) [[unlikely]] {
                entry_record.headers[entry_record.header_count++ % entry_record.headers.size()] = {
                    static_cast<u32>(word_offset + index), command_header.argument};
            }
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
    const u32 watch = semaphore_watch;
    if ((argument & ~0xfU) == watch) {
        LOG_INFO(HW_GPU, "Semaphore watch: channel {} subchannel {} method {:#x} argument {:#x}",
                 channel_state.bind_id, dma_state.subchannel, method, argument);
    }
}

bool DmaPusher::WantsSegment() const {
    // Only macro parameters and compute uploads read the engine's current_dma_segment.
    const auto engine = subchannel_type[dma_state.subchannel];
    return engine == Engines::EngineTypes::KeplerCompute ||
           (engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart);
}

void DmaPusher::CallMethod(u32 argument) {
    // Method 0x1 is illegal in the host class: a stream that reaches it has been misparsed (for
    // example, a split method continued into a different command buffer). Discard the rest of
    // the block using its declared count instead of running thousands of bogus methods against
    // the engines; the next header then parses from an aligned position.
    if (dma_state.method == static_cast<u32>(BufferMethods::Illegal)) {
        const u32 remaining = dma_state.method_count > 0 ? dma_state.method_count - 1 : 0;
        static std::atomic<u32> reported{};
        const u32 report = reported.fetch_add(1);
        if (report < 8) {
            LOG_ERROR(HW_GPU,
                      "Illegal host method on subchannel {}: discarding {} words of the block",
                      dma_state.subchannel, remaining);
        }
        if (report < 4 && record_parsed_entries) {
            LogIllegalBlock();
        }
        dma_state.skip_words = remaining;
        // The caller's decrement then ends the block. An inline header (no block active,
        // method_count 0) must stay at 0: at 1 its next word would be read as data.
        dma_state.method_count = dma_state.method_count > 0 ? 1 : 0;
        dma_state.non_incrementing = false;
        return;
    }
    if (semaphore_watch != 0) [[unlikely]] {
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
        // Compute dispatches, DMA copies and inline uploads write what the front reads (the
        // command lists ReadGpuWrittenCommands reads, MME DMA reads): IsMemoryWriter.
        const auto engine_type = subchannel_type[dma_state.subchannel];
        const bool writer = IsMemoryWriter(*subchannel, engine_type, dma_state.method);
        // DMA copies, inline uploads and 2D blits may write guest memory as they run, anywhere as
        // far as the front knows: MME DMA reads wait for them (shader writes go to the buffer
        // cache). 2D from jojo-chaechae/Eglantine 90eab341bd.
        const bool direct_writer = engine_type == Engines::EngineTypes::MaxwellDMA ||
                                   engine_type == Engines::EngineTypes::KeplerMemory ||
                                   engine_type == Engines::EngineTypes::Fermi2D;
        SCOPE_EXIT {
            if (writer) {
                split.NoteWriter();
            }
            if (direct_writer && split.Active()) {
                split.NotePendingWriteAnywhere();
            }
        };
        if (split.Active()) {
            const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
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
                                                 dma_state.is_last_call, &segment);
                        return;
                    }
                }
            }
            split.Method(subchannel, dma_state.method, argument, dma_state.is_last_call,
                         WantsSegment() ? &segment : nullptr);
            return;
        }
        if (!subchannel->execution_mask[dma_state.method]) {
            subchannel->method_sink.emplace_back(dma_state.method, argument);
        } else {
            VideoCommon::Nx2Profile::SampledScope<4> method_scope{VideoCommon::Nx2Profile::Zone::Method};
            subchannel->ConsumeSink(system);
            subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
            subchannel->CallMethod(system, dma_state.method, argument, dma_state.is_last_call);
        }
    }
}

void DmaPusher::CallMultiMethod(const u32* base_start, u32 num_methods) {
    if (semaphore_watch != 0) [[unlikely]] {
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
        // Compute dispatches, DMA copies and inline uploads write what the front reads (the
        // command lists ReadGpuWrittenCommands reads, MME DMA reads): IsMemoryWriter.
        const auto engine_type = subchannel_type[dma_state.subchannel];
        const bool writer = IsMemoryWriter(*subchannel, engine_type, dma_state.method);
        // DMA copies, inline uploads and 2D blits may write guest memory as they run, anywhere as
        // far as the front knows: MME DMA reads wait for them (shader writes go to the buffer
        // cache). 2D from jojo-chaechae/Eglantine 90eab341bd.
        const bool direct_writer = engine_type == Engines::EngineTypes::MaxwellDMA ||
                                   engine_type == Engines::EngineTypes::KeplerMemory ||
                                   engine_type == Engines::EngineTypes::Fermi2D;
        SCOPE_EXIT {
            if (writer) {
                split.NoteWriter();
            }
            if (direct_writer && split.Active()) {
                split.NotePendingWriteAnywhere();
            }
        };
        if (split.Active()) {
            const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
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
                                                      num_methods, dma_state.method_count, &segment);
                        return;
                    }
                }
            }
            split.MultiMethod(subchannel, dma_state.method, base_start, num_methods,
                              dma_state.method_count, WantsSegment() ? &segment : nullptr);
            return;
        }
        VideoCommon::Nx2Profile::SampledScope<4> method_scope{VideoCommon::Nx2Profile::Zone::Method};
        subchannel->ConsumeSink(system);
        subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
        subchannel->CallMultiMethod(system, dma_state.method, base_start, num_methods, dma_state.method_count);
    }
}

size_t DmaPusher::RefreshSnapshot(CommandList& list, u64 now, u8 at) {
    list.snapshot_suspensions = now;
    // EDEN_NX2_RESNAPSHOT_AGE_MS: an old list keeps its copy (entries never copied are read).
    if (const u64 max_age = ResnapshotMaxAgeNs();
        max_age != 0 && list.submit_ns != 0 && SteadyNs() - list.submit_ns > max_age) {
        RefreshDeferredEntries(list);
        return 0;
    }
    if (record_parsed_entries && list.first_words.empty()) {
        list.first_words = list.submitted_words;
    }
    list.copied_at = at;
    size_t changed = 0;
    std::vector<CommandHeader> fresh;
    for (size_t i = 0; i < list.command_lists.size() && i < list.submitted_offsets.size(); ++i) {
        const CommandListHeader& entry = list.command_lists[i];
        const u32 offset = list.submitted_offsets[i];
        if (offset + entry.size > list.submitted_words.size()) {
            continue;
        }
        CommandHeader* const words = list.submitted_words.data() + offset;
        const size_t bytes = size_t{entry.size} * sizeof(u32);
        if (!record_parsed_entries) {
            memory_manager.ReadBlockUnsafe(entry.addr, words, bytes);
            continue;
        }
        fresh.resize(entry.size);
        memory_manager.ReadBlockUnsafe(entry.addr, fresh.data(), bytes);
        if (std::memcmp(fresh.data(), words, bytes) != 0 &&
            !std::ranges::binary_search(list.deferred_entries, static_cast<u32>(i))) {
            ++changed;
        }
        std::memcpy(words, fresh.data(), bytes);
    }
    list.deferred_entries.clear();
    return changed;
}

std::string DmaPusher::DescribeCurrentEntry() {
    if (dma_pushbuffer.empty()) {
        return "no list queued";
    }
    const CommandList& list = dma_pushbuffer.front();
    if (dma_pushbuffer_subindex >= list.command_lists.size()) {
        return "in a built-in list";
    }
    const CommandListHeader& header = list.command_lists[dma_pushbuffer_subindex];
    static constexpr std::array copies{"at submit", "again when queued", "again at a resume"};
    std::string text = fmt::format(
        "entry {} of {} at {:#x}+{:#x} words, stopped at word {}; list copied {}, submitted {} ms "
        "ago{}",
        dma_pushbuffer_subindex, list.command_lists.size(), header.addr.Value(),
        header.size.Value(), command_resume_offset, copies[(std::min<size_t>)(list.copied_at, 2)],
        list.submit_ns != 0 ? (SteadyNs() - list.submit_ns) / 1'000'000 : 0,
        KeepsCopy(list) ? " (old enough to keep that copy now)" : "");
    if (record_parsed_entries && entry_record.address == header.addr) {
        text += fmt::format(", parsed from {}", entry_record.source);
    }
    // The words before the stop (the acquire), as copied at submit, as copied now and in memory.
    std::vector<u32> memory(header.size);
    memory_manager.ReadBlockUnsafe(header.addr, memory.data(), memory.size() * sizeof(u32));
    const size_t stop = (std::min<size_t>)(command_resume_offset, header.size);
    const size_t first = stop >= 8 ? stop - 8 : 0;
    const auto row = [&](const std::vector<CommandHeader>& words) {
        if (dma_pushbuffer_subindex >= list.submitted_offsets.size()) {
            return std::string(" (none)");
        }
        const size_t offset = list.submitted_offsets[dma_pushbuffer_subindex];
        if (offset + header.size > words.size()) {
            return std::string(" (none)");
        }
        std::string line;
        for (size_t i = first; i < stop; ++i) {
            line += fmt::format(" {:08x}", words[offset + i].argument);
        }
        return line;
    };
    std::string memory_line;
    for (size_t i = first; i < stop; ++i) {
        memory_line += fmt::format(" {:08x}", memory[i]);
    }
    text += fmt::format("; words {}..{}: copy{} | memory{}", first, stop, row(list.submitted_words),
                        memory_line);
    if (!list.first_words.empty()) {
        text += fmt::format(" | submit-time copy{}", row(list.first_words));
    }
    return text;
}

// The entry the block is in, as parsed (its source), as submitted and as memory holds it now,
// around the block's header (the last one parsed). EDEN_NX2_PB_CHECK.
void DmaPusher::LogIllegalBlock() {
    const EntryRecord& record = entry_record;
    const auto& [position, raw] =
        record.headers[(record.header_count + record.headers.size() - 1) % record.headers.size()];
    LOG_ERROR(HW_GPU, "  {}", DescribeCurrentEntry());
    LOG_ERROR(HW_GPU,
              "  entry {} on channel {}: {:#x}+{:#x} words from {}, resumed at word {}; block "
              "header {:#010x} at word {}",
              record.index, channel_state.bind_id, record.address, record.size, record.source,
              record.resumed_at, raw, position);
    std::string headers;
    for (u32 i = 0; i < (std::min)(record.header_count, u32{8}); ++i) {
        const auto& [at, word] =
            record.headers[(record.header_count - 1 - i) % record.headers.size()];
        headers += fmt::format(" {:#010x}@{}", word, at);
    }
    LOG_ERROR(HW_GPU, "  last headers (newest first):{}", headers);
    std::span<const CommandHeader> submitted{};
    if (!dma_pushbuffer.empty()) {
        // The submit-time words, also when a later copy replaced them.
        const CommandList& list = dma_pushbuffer.front();
        const auto& words = list.first_words.empty() ? list.submitted_words : list.first_words;
        if (record.index < list.submitted_offsets.size() &&
            list.submitted_offsets[record.index] + record.size <= words.size()) {
            submitted = {words.data() + list.submitted_offsets[record.index], record.size};
        }
    }
    std::vector<u32> memory(record.size);
    memory_manager.ReadBlockUnsafe(record.address, memory.data(), memory.size() * sizeof(u32));
    const u32 first = position >= 16 ? position - 16 : 0;
    const u32 last = (std::min)(record.size, first + 64);
    for (u32 row = first; row < last; row += 8) {
        std::string parsed_line, submitted_line, memory_line;
        for (u32 i = row; i < (std::min)(last, row + 8); ++i) {
            const u32 parsed_word = i < record.words.size() ? record.words[i].argument : 0xdeadbeef;
            parsed_line += fmt::format(" {:08x}", parsed_word);
            submitted_line += submitted.empty() ? std::string(" --------")
                              : submitted[i].argument == parsed_word
                                  ? std::string(" ........")
                                  : fmt::format(" {:08x}", submitted[i].argument);
            memory_line += memory[i] == parsed_word ? std::string(" ........")
                                                    : fmt::format(" {:08x}", memory[i]);
        }
        LOG_ERROR(HW_GPU, "  +{:04x} parsed   :{}", row, parsed_line);
        LOG_ERROR(HW_GPU, "        submitted:{}", submitted_line);
        LOG_ERROR(HW_GPU, "        memory   :{}", memory_line);
    }
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

bool DmaPusher::IsGpuWrittenCommands(GPUVAddr address, size_t size) {
    if (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) || !Tegra::Nx2CommandLists() ||
        size == 0) {
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

bool DmaPusher::ReadGpuWrittenCommands(GPUVAddr address, u32 count) {
    // The pages are marked as command list pages: from the next write on, the buffer cache copies
    // them to host memory right after the dispatch or copy that writes them and submits. The
    // front then waits only for the back to run that work and for the GPU to finish the copy, not
    // for everything queued and a cache flush (FlushGpuWrittenCommands: ~400 GPU round trips a
    // second in a Mario Kart World race, during which the back had nothing to do). Pages
    // FlushGpuWrittenCommands flushed whole and no GPU work wrote since are read from guest
    // memory.
    if (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) || !Tegra::Nx2CommandLists() ||
        count == 0 || rasterizer == nullptr) {
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
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.WaitForWriters();
    }
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
            VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::CommandListCopy};
            where = rasterizer->ReadGpuWrittenCommandList(*device, part);
        }
        if (where == GpuWrittenList::Unknown) {
            return false;
        }
        if (where == GpuWrittenList::Synced) {
            memory_manager.ReadBlockUnsafe(chunk, part.data(), part.size());
        }
        chunk = chunk_end;
    }
    // EDEN_NX2_CMD_LIST_VERIFY=1: also flush the list's pages (after a drain) and read them,
    // logging the lists whose copy or synced guest memory differs; the flushed words are run.
    static const bool verify = [] {
        const char* value = std::getenv("EDEN_NX2_CMD_LIST_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    if (verify) {
        VideoCommon::GpuSplit::Get().Drain();
        const GPUVAddr first_page = address & ~GPUVAddr{0xfff};
        const GPUVAddr end_page = (end + 0xfff) & ~GPUVAddr{0xfff};
        memory_manager.FlushRegion(first_page, end_page - first_page,
                                   VideoCommon::CacheType::BufferCache);
        std::vector<u8> flushed(size_t{count} * sizeof(u32));
        memory_manager.ReadBlockUnsafe(address, flushed.data(), flushed.size());
        if (std::memcmp(flushed.data(), out, flushed.size()) != 0) {
            static std::atomic<u32> mismatches{};
            if (mismatches.fetch_add(1, std::memory_order_relaxed) < 64) {
                size_t word = 0;
                while (std::memcmp(flushed.data() + word * 4, out + word * 4, 4) == 0) {
                    ++word;
                }
                u32 copied_word{};
                u32 flushed_word{};
                std::memcpy(&copied_word, out + word * 4, 4);
                std::memcpy(&flushed_word, flushed.data() + word * 4, 4);
                LOG_WARNING(HW_GPU,
                            "GPU-written command list {:#x} ({} words) differs from its flushed "
                            "pages at word {}: {:#010x}, flushed {:#010x}",
                            address, count, word, copied_word, flushed_word);
            }
            std::memcpy(out, flushed.data(), flushed.size());
        }
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
    VideoCommon::GpuSplit::Get().Drain(VideoCommon::Nx2Profile::Zone::DrainCommandList);
    const GPUVAddr first_page = address & ~GPUVAddr{0xfff};
    const GPUVAddr end_page = (address + size + 0xfff) & ~GPUVAddr{0xfff};
    if (memory_manager.IsMemoryDirty(first_page, end_page - first_page,
                                     VideoCommon::CacheType::BufferCache)) {
        FlushCommandPages(first_page, end_page);
    }
    NoteCommandPagesSynced(first_page, end_page);
}

void DmaPusher::FlushCommandPages(GPUVAddr first_page, GPUVAddr end_page) {
    // Each download waits for the GPU, and most GPU-written lists of a frame are written together
    // before the first is read. Pages downloaded here before that are dirty again go in the same
    // wait, and their lists then read guest memory (from sylvieistaken/selevet-eglantine
    // bce3c83739). EDEN_NX2_CMD_BATCH=0: one range per wait.
    namespace Profile = VideoCommon::Nx2Profile;
    Profile::Scope scope{Profile::Zone::CommandListFlush};
    Profile::Add(Profile::Counter::CommandListFlushes, 1);
    static const bool batch = [] {
        const char* value = std::getenv("EDEN_NX2_CMD_BATCH");
        return value == nullptr || value[0] != '0';
    }();
    constexpr auto buffers = VideoCommon::CacheType::BufferCache;
    if (!batch || rasterizer == nullptr || !rasterizer->BeginFlushBatch()) {
        memory_manager.FlushRegion(first_page, end_page - first_page, buffers);
        return;
    }
    memory_manager.FlushRegion(first_page, end_page - first_page, buffers);
    boost::container::small_vector<GPUVAddr, 16> along;
    for (const GPUVAddr page : flushed_command_pages) {
        if ((page < first_page || page >= end_page) &&
            memory_manager.IsMemoryDirty(page, 0x1000, buffers)) {
            memory_manager.FlushRegion(page, 0x1000, buffers);
            along.push_back(page);
        }
    }
    rasterizer->EndFlushBatch();
    // The split is drained: these hold every GPU write so far (a later one unsyncs them).
    for (const GPUVAddr page : along) {
        NoteCommandPagesSynced(page, page + 0x1000);
    }
    Profile::Add(Profile::Counter::CommandListPagesAlong, along.size());
    for (GPUVAddr page = first_page; page < end_page; page += 0x1000) {
        if (std::ranges::find(flushed_command_pages, page) != flushed_command_pages.end()) {
            continue;
        }
        if (flushed_command_pages.size() < MaxFlushedCommandPages) {
            flushed_command_pages.push_back(page);
        } else {
            flushed_command_pages[flushed_command_page_next++ % MaxFlushedCommandPages] = page;
        }
    }
}

void DmaPusher::NoteCommandPagesSynced(GPUVAddr first_page, GPUVAddr end_page) {
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
    if (!record_parsed_entries) {
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

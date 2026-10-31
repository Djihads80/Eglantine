// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <string>
#include <fmt/format.h>

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
#ifdef ARCHITECTURE_x86_64
#include "video_core/macro_turing_jit.h"
#endif

namespace Tegra {

constexpr u32 MacroRegistersStart = 0xE00;
constexpr u32 ComputeInline = 0x6D;

namespace GpfifoCapture {
namespace {

constexpr std::size_t MaxBytes = 8 * 1024 * 1024;
constexpr std::size_t MaxRecords = 4096;
constexpr std::size_t MaxRecordBytes = 64 * 1024;
constexpr std::size_t MaxEntries = 1024;
constexpr std::size_t MaxWords = 4096;
std::atomic<bool> finished{};
std::atomic<u64> failures{};
std::atomic<u64> next_submit_id{};

const std::filesystem::path& Directory() noexcept {
    static const auto directory = []() noexcept {
        try {
#ifdef _WIN32
            const auto* value = _wgetenv(L"EDEN_NX2_GPFIFO_DUMP");
            const auto path = value ? std::filesystem::path{value} : std::filesystem::path{};
#else
            const auto* value = std::getenv("EDEN_NX2_GPFIFO_DUMP");
            const auto path = value ? std::filesystem::u8path(value) : std::filesystem::path{};
#endif
            std::error_code error;
            return !path.empty() && std::filesystem::is_directory(path, error)
                       ? path : std::filesystem::path{};
        } catch (...) {
            return std::filesystem::path{};
        }
    }();
    return directory;
}

struct Store {
    std::mutex mutex;
    std::deque<std::pair<u64, std::string>> records;
    std::size_t bytes{};
    u64 sequence{}, evicted_records{}, evicted_bytes{}, truncated_records{};
};

Store& Storage() {
    static Store store;
    return store;
}

u64 Append(std::string record) {
    auto& store = Storage();
    std::scoped_lock lock{store.mutex};
    if (finished.load(std::memory_order_relaxed)) return 0;
    const auto sequence = ++store.sequence;
    record = fmt::format("record={} {}\n", sequence, record);
    if (record.size() > MaxRecordBytes) {
        const auto original = record.size();
        record.resize(MaxRecordBytes - 128);
        record += fmt::format("\nTRUNCATED record original_bytes={} limit={}\n", original, MaxRecordBytes);
        ++store.truncated_records;
    }
    while (!store.records.empty() &&
           (store.bytes + record.size() > MaxBytes || store.records.size() >= MaxRecords)) {
        store.bytes -= store.records.front().second.size();
        store.evicted_bytes += store.records.front().second.size();
        ++store.evicted_records;
        store.records.pop_front();
    }
    store.records.emplace_back(sequence, std::move(record));
    store.bytes += store.records.back().second.size();
    return sequence;
}

void AppendWords(std::string& record, std::span<const CommandHeader> commands,
                 std::size_t word_offset) {
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (commands.size() > MaxWords && i == MaxWords / 2) {
            const auto next = commands.size() - MaxWords / 2;
            record += fmt::format("\nTRUNCATED words first={} count={}\n", word_offset + i, next - i);
            i = next;
        }
        if (i % 8 == 0 || (commands.size() > MaxWords && i == commands.size() - MaxWords / 2)) {
            record += fmt::format("\nword[{}]", word_offset + i);
        }
        record += fmt::format(" {:08x}", commands[i].argument);
    }
    record += '\n';
}

}

bool Enabled() noexcept {
    return !Directory().empty() && !finished.load(std::memory_order_relaxed);
}

u64 BeginSubmission(s32 fd, u32 ioctl, u32 variant, u64 session,
                    std::span<const u8> input, std::size_t inline_size) noexcept try {
    if (!Enabled()) return 0;
    const auto id = ++next_submit_id;
    auto record = fmt::format("submit id={} fd={} session={} ioctl={:08x} variant={} input_bytes={} inline_bytes={} params_bytes={} params=",
                             id, fd, session, ioctl, variant, input.size(), inline_size,
                             std::min<std::size_t>(24, input.size()));
    for (const auto byte : input.first(std::min<std::size_t>(24, input.size()))) {
        record += fmt::format("{:02x}", byte);
    }
    if (input.size() >= 24) {
        std::array<u32, 6> params{};
        for (std::size_t i = 0; i < 24; ++i) {
            params[i / 4] |= u32{input[i]} << ((i % 4) * 8);
        }
        record += fmt::format(" address={:x} num_entries={} flags={:08x} fence_in=({}, {})",
            u64{params[0]} | (u64{params[1]} << 32), params[2], params[3], params[4], params[5]);
    }
    Append(std::move(record));
    return id;
} catch (...) {
    ++failures;
    return 0;
}

void RecordEnqueue(u64 submit_id, s32 channel, const char* origin, CommandList& entries) noexcept try {
    if (!Enabled()) return;
    entries.capture_submit_id = submit_id;
    entries.capture_origin = origin;
    auto record = fmt::format("enqueue submit={} channel={} origin={} entries={} prefetch_words={}\n",
                             submit_id, channel, origin, entries.command_lists.size(),
                             entries.prefetch_command_list.size());
    const auto count = std::min(MaxEntries, entries.command_lists.size());
    for (std::size_t i = 0; i < count; ++i) {
        record += fmt::format("entry[{}] raw={:016x}\n", i, entries.command_lists[i].raw);
    }
    if (count < entries.command_lists.size()) {
        record += fmt::format("TRUNCATED entries first={} count={}\n", count,
                              entries.command_lists.size() - count);
    }
    AppendWords(record, entries.prefetch_command_list, 0);
    entries.capture_enqueue_id = Append(std::move(record));
} catch (...) {
    ++failures;
}

void Flush(const char* reason) noexcept try {
    if (!Enabled()) return;
    const auto path = Directory() / "yoshi-gpfifo.txt";
    auto& store = Storage();
    std::scoped_lock lock{store.mutex};
    if (finished.exchange(true)) return;
#ifdef _WIN32
    std::FILE* file = _wfopen(path.c_str(), L"wx");
#else
    std::FILE* file = std::fopen(path.c_str(), "wx");
#endif
    if (!file) return;
    std::fprintf(file, "GPFIFO_CAPTURE version=1 reason=%s max_bytes=%zu max_records=%zu max_record_bytes=%zu max_entries=%zu max_words=%zu\n",
                 reason, MaxBytes, MaxRecords, MaxRecordBytes, MaxEntries, MaxWords);
    std::fprintf(file, "EVICTED records=%llu bytes=%llu first_retained=%llu last_record=%llu\n",
                 static_cast<unsigned long long>(store.evicted_records),
                 static_cast<unsigned long long>(store.evicted_bytes),
                 static_cast<unsigned long long>(store.records.empty() ? 0 : store.records.front().first),
                 static_cast<unsigned long long>(store.sequence));
    std::fprintf(file, "TRUNCATED oversized_records=%llu diagnostic_failures=%llu\n",
                 static_cast<unsigned long long>(store.truncated_records),
                 static_cast<unsigned long long>(failures.load()));
    for (const auto& item : store.records) {
        std::fwrite(item.second.data(), 1, item.second.size(), file);
    }
    std::fputs("END_CAPTURE anomaly may interrupt execution before its post-state record\n", file);
    std::fclose(file);
} catch (...) {
    ++failures;
}

}

#ifdef ARCHITECTURE_x86_64
namespace {

constexpr u32 NativeBindMethod = offsetof(Engines::Maxwell3D::Regs, load_mme.start_address) / sizeof(u32);

struct BindPacketTrace {
    const DmaPusher* owner{};
    bool has_header{};
    u32 header{};
    GPUVAddr header_address{};
    u64 header_word_offset{};
    u64 entry_raw{};
    std::size_t entry_index{}, entry_size{}, word_offset{}, words_first{}, words_count{};
    std::array<u32, 17> words{};
};

std::atomic<bool> bind_packet_captured{};

BindPacketTrace* GetBindPacketTrace(const DmaPusher& pusher) {
    if (TuringMacroJit::RtDump::Directory().empty() ||
        bind_packet_captured.load(std::memory_order_relaxed)) return nullptr;
    static thread_local std::array<BindPacketTrace, 32> traces{};
    static thread_local std::size_t next{};
    for (auto& trace : traces) {
        if (trace.owner == &pusher) return &trace;
    }
    auto& trace = traces[next++ % traces.size()];
    trace = {};
    trace.owner = &pusher;
    return &trace;
}

void TraceBindPacket(const DmaPusher& pusher, std::span<const CommandHeader> commands,
                     std::size_t index, std::size_t word_offset) {
    auto* trace = GetBindPacketTrace(pusher);
    if (!trace) return;
    trace->entry_index = pusher.dma_pushbuffer_subindex;
    trace->entry_raw = 0;
    trace->entry_size = 0;
    if (!pusher.dma_pushbuffer.empty()) {
        const auto& entries = pusher.dma_pushbuffer.front().command_lists;
        if (trace->entry_index < entries.size()) {
            trace->entry_raw = entries[trace->entry_index].raw;
            trace->entry_size = entries[trace->entry_index].size;
        }
    }
    trace->word_offset = word_offset + index;
    const std::size_t first = index > 8 ? index - 8 : 0;
    trace->words_first = word_offset + first;
    trace->words_count = std::min(trace->words.size(), commands.size() - first);
    for (std::size_t i = 0; i < trace->words_count; ++i) trace->words[i] = commands[first + i].argument;
    if (pusher.dma_state.method_count == 0) {
        trace->has_header = true;
        trace->header = commands[index].argument;
        trace->header_address = pusher.dma_state.dma_get;
        trace->header_word_offset = trace->word_offset;
    }
}

bool DumpInvalidNativeBind(DmaPusher& pusher, u32 pointer, std::size_t positions_size,
                            std::span<const u32> arguments) noexcept try {
    const std::size_t available = pointer < positions_size ? positions_size - pointer : 0;
    if (arguments.size() > available) GpfifoCapture::Flush("native-bind-outside-positions-span");
    auto* trace = GetBindPacketTrace(pusher);
    if (!trace) return false;
    if (arguments.size() <= available || bind_packet_captured.exchange(true)) return false;
    const auto path = TuringMacroJit::RtDump::Directory() / "yoshi-mme-bind.txt";
    const auto& state = pusher.dma_state;
#ifdef _WIN32
    std::FILE* file = _wfopen(path.c_str(), L"wx");
#else
    std::FILE* file = std::fopen(path.c_str(), "wx");
#endif
    if (file) {
        std::fprintf(file, "pointer=%u positions_size=%zu first_invalid_argument=%zu raw_argument=%08x arguments=%zu\n",
                     pointer, positions_size, available, arguments[available], arguments.size());
        std::fprintf(file, "channel=%u method=%x subchannel=%u remaining=%u non_incrementing=%u last_call=%u dma_get=%llx dma_word_offset=%llu\n",
                     pusher.channel_state.bind_id, state.method, state.subchannel, state.method_count,
                     state.non_incrementing ? 1U : 0U, state.is_last_call ? 1U : 0U,
                     static_cast<unsigned long long>(state.dma_get), static_cast<unsigned long long>(state.dma_word_offset));
        std::fprintf(file, "entry_index=%zu entry_raw=%016llx entry_size=%zu input_word_offset=%zu header_known=%u header=%08x header_entry_address=%llx header_word_offset=%llu\n",
                     trace->entry_index, static_cast<unsigned long long>(trace->entry_raw), trace->entry_size,
                     trace->word_offset, trace->has_header ? 1U : 0U, trace->header,
                     static_cast<unsigned long long>(trace->header_address), static_cast<unsigned long long>(trace->header_word_offset));
        std::fprintf(file, "parsed_words_first=%zu parsed_words_count=%zu\n", trace->words_first, trace->words_count);
        for (std::size_t i = 0; i < trace->words_count; ++i) {
            std::fprintf(file, "%zu %08x\n", trace->words_first + i, trace->words[i]);
        }
        const std::size_t first = available > 8 ? available - 8 : 0;
        const std::size_t count = std::min<std::size_t>(17, arguments.size() - first);
        std::fprintf(file, "argument_words_first=%zu argument_words_count=%zu\n", first, count);
        for (std::size_t i = 0; i < count; ++i) {
            std::fprintf(file, "%zu %08x\n", first + i, arguments[first + i]);
        }
        std::fclose(file);
    }
    pusher.DumpCurrentEntry("native macro bind outside positions span");
    return true;
} catch (...) {
    return false;
}

} // namespace
#endif

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

bool DmaPusher::Step() {
    if (!ib_enable || dma_pushbuffer.empty()) {
        return false;
    }
    if (puller.HasPendingAcquire()) RecordCapture("acquire-resume-attempt");
    if (!puller.ResumeAcquire(*this)) {
        return false;
    }

    CommandList& command_list = dma_pushbuffer.front();

    const size_t prefetch_size = command_list.prefetch_command_list.size();
    const size_t command_list_size = command_list.command_lists.size();

    if (prefetch_size == 0 && command_list_size == 0) {
        dma_pushbuffer.pop();
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
        dma_pushbuffer.pop();
        return true;
    }

    auto& current_command = command_list.command_lists[dma_pushbuffer_subindex];
    const CommandListHeader& header = current_command;
    RecordCapture("entry-begin");
    dma_state.dma_get = header.addr;

    if (signal_sync && !synced) {
        std::unique_lock lk(sync_mutex);
        sync_cv.wait(lk, [this]() { return synced; });
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
                // Parameters continued in an entry of their own can be arguments the GPU wrote
                // (Donkey Kong Bananza's terrain: the culling pass counts each mesh's triangles
                // into a 5-word indirect draw record and the colour pass draws with it). Read
                // without a flush, the macro sees the older guest words (a zero count, so the
                // terrain is never drawn). EDEN_NX2_PBFLUSH=0 turns this off.
                static const bool flush_parameters = [] {
                    const char* value = std::getenv("EDEN_NX2_PBFLUSH");
                    return value == nullptr || value[0] != '0';
                }();
                bool flushed_parameters = false;
                if (flush_parameters && words <= 64) {
                    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
                        split.Drain();
                    }
                    const GPUVAddr address = dma_state.dma_get;
                    const size_t bytes = words * sizeof(u32);
                    if (memory_manager.IsMemoryDirty(address, bytes, VideoCommon::CacheType::BufferCache)) {
                        memory_manager.FlushRegion(address, bytes, VideoCommon::CacheType::BufferCache);
                        flushed_parameters = true;
                        static std::atomic<u32> flush_logs{};
                        if (flush_logs.fetch_add(1) < 20) {
                            LOG_INFO(HW_GPU, "Macro parameters at {:#x} ({} words) were written by the GPU: flushed",
                                     address, words);
                        }
                    }
                }
                if (flushed_parameters) {
                    engine_state->current_dirty = false;
                } else if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
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
        if (use_safe) {
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::SafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            RecordParsedEntry(header, commands);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else {
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
        dma_pushbuffer.pop();
        dma_pushbuffer_subindex = 0;
    } else {
        signal_sync = command_list.command_lists[dma_pushbuffer_subindex].sync && Settings::values.sync_memory_operations.GetValue();
    }

    if (signal_sync) {
        VideoCommon::GpuSplit::Get().Drain();
        rasterizer->SignalFence([this]() {
            std::scoped_lock lk(sync_mutex);
            synced = true;
            sync_cv.notify_all();
        });
    }

    return true;
}

void DmaPusher::RecordCapture(const char* phase, std::span<const CommandHeader> commands,
                              std::size_t word_offset) const noexcept try {
    if (!GpfifoCapture::Enabled()) return;
    const CommandList* list = dma_pushbuffer.empty() ? nullptr : &dma_pushbuffer.front();
    const auto* entry = list && dma_pushbuffer_subindex < list->command_lists.size()
                            ? &list->command_lists[dma_pushbuffer_subindex] : nullptr;
    const auto raw = entry ? entry->raw : 0;
    auto record = fmt::format("{} submit={} enqueue={} origin={} channel={} entry={} entries={} prefetch_words={} raw={:016x} addr={:x} size={} fetch={} priv={} level={} sync={} word_offset={} read_words={} resume_offset={}\n",
        phase, list ? list->capture_submit_id : 0, list ? list->capture_enqueue_id : 0,
        list ? list->capture_origin : "none", channel_state.bind_id, dma_pushbuffer_subindex,
        list ? list->command_lists.size() : 0, list ? list->prefetch_command_list.size() : 0,
        raw, entry ? entry->addr.Value() : 0, entry ? entry->size.Value() : 0,
        raw & 1, (raw >> 40) & 1, (raw >> 41) & 1, raw >> 63,
        word_offset, commands.size(), command_resume_offset);
    record += fmt::format("state method={:x} subchannel={} remaining={} length_pending={} dma_get={:x} dma_word_offset={} non_incrementing={} increment_once={} last_call={} pending_acquire={} split_end={:x} parked={}\n",
        dma_state.method, dma_state.subchannel, dma_state.method_count, dma_state.length_pending,
        dma_state.dma_get, dma_state.dma_word_offset, dma_state.non_incrementing,
        static_cast<bool>(dma_increment_once), dma_state.is_last_call, puller.HasPendingAcquire(),
        split_method_end, parked_methods.size());
    for (std::size_t i = 0; i < subchannels.size(); ++i) {
        record += fmt::format("binding[{}] bound={} type={} engine={:x}\n", i, subchannels[i] != nullptr,
                              subchannels[i] ? static_cast<u32>(subchannel_type[i]) : 0,
                              reinterpret_cast<std::uintptr_t>(subchannels[i]));
    }
    for (const auto& parked : parked_methods) {
        record += fmt::format("parked method={:x} subchannel={} remaining={} non_incrementing={} increment_once={} resume={:x}\n",
            parked.method, parked.subchannel, parked.method_count, parked.non_incrementing,
            parked.increment_once, parked.resume_address);
    }
    GpfifoCapture::AppendWords(record, commands, word_offset);
    GpfifoCapture::Append(std::move(record));
} catch (...) {
    ++GpfifoCapture::failures;
}

size_t DmaPusher::ProcessCommands(std::span<const CommandHeader> commands, size_t word_offset) {
    RecordCapture("execute-begin", commands, word_offset);
    for (size_t index = 0; index < commands.size();) {
#ifdef ARCHITECTURE_x86_64
        TraceBindPacket(*this, commands, index, word_offset);
#endif
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
            RecordCapture("execute-suspend", {}, word_offset + index);
            return index;
        }
    }
    RecordCapture("execute-end", {}, word_offset + commands.size());
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

// EDEN_NX2_SEM_WATCH=<gpu address>: log every method whose argument is the low word of an
// address in the 16 bytes there (finds the engine and method that release a semaphore).
static const u32 g_semaphore_watch = [] {
    const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
    return value ? static_cast<u32>(std::strtoull(value, nullptr, 16)) : 0U;
}();

// EDEN_NX2_PB_CHECK also keeps the last parsed entries, for DumpCurrentEntry.
static const bool g_record_parsed_entries = std::getenv("EDEN_NX2_PB_CHECK") != nullptr;

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
    return engine == Engines::EngineTypes::KeplerCompute ||
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
            VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
            subchannel->ConsumeSink(system);
            subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
#ifdef ARCHITECTURE_x86_64
            if (dma_state.method == NativeBindMethod &&
                subchannel_type[dma_state.subchannel] == Engines::EngineTypes::Maxwell3D) {
                auto& maxwell3d = *static_cast<Engines::Maxwell3D*>(subchannel);
                if (maxwell3d.GetNativeMacroEngine().native_turing) {
                    DumpInvalidNativeBind(*this, maxwell3d.regs.load_mme.start_address_ptr,
                                          maxwell3d.GetNativeMacroPositions().size(), {&argument, 1});
                }
            }
#endif
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
        VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
        subchannel->ConsumeSink(system);
        subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
#ifdef ARCHITECTURE_x86_64
        if (dma_state.method == NativeBindMethod &&
            subchannel_type[dma_state.subchannel] == Engines::EngineTypes::Maxwell3D) {
            auto& maxwell3d = *static_cast<Engines::Maxwell3D*>(subchannel);
            if (maxwell3d.GetNativeMacroEngine().native_turing) {
                DumpInvalidNativeBind(*this, maxwell3d.regs.load_mme.start_address_ptr,
                                      maxwell3d.GetNativeMacroPositions().size(), {base_start, num_methods});
            }
        }
#endif
        subchannel->CallMultiMethod(system, dma_state.method, base_start, num_methods, dma_state.method_count);
    }
}

void DmaPusher::FlushGpuWrittenCommands(GPUVAddr address, size_t size) {
    // Title 1 runs small command lists its culling dispatches fill in each frame. Accurate GPU
    // mode reads every command list through the caches (safe reads); here only the ones on pages
    // the GPU has written are, or their draws run with an older frame's words (trees vanish).
    if (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) || size == 0) {
        return;
    }
    // The caches belong to the split's back, and the words may be written by work the front queued
    // that the back has not run (or marked written) yet: it runs all of it first.
    auto& split = VideoCommon::GpuSplit::Get();
    if (split.Active()) {
        split.Drain();
    }
    bool written = false;
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page);
            device && Tegra::Nx2IsGpuWritten(*device)) {
            written = true;
            break;
        }
    }
    if (!written) {
        return;
    }
    if (memory_manager.IsMemoryDirty(address, size, VideoCommon::CacheType::BufferCache)) {
        memory_manager.FlushRegion(address, size, VideoCommon::CacheType::BufferCache);
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

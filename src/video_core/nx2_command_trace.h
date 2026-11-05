// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_command_writer_trace.h"

namespace Core { extern std::atomic<u64> g_nx2_present_count; }

namespace VideoCommon::CommandTrace {

constexpr u64 FirstPresent = 3300, LastPresent = 3500;
constexpr size_t SampleWords = 4096;
constexpr bool InWindow(u64 present) { return present >= FirstPresent && present <= LastPresent; }
enum class EventKind : u32 { SubmitCopy, SubmitEnd, Refresh, Parse, Acquire, Resume, Stall,
                             ReleaseIssued, ReleasePublishedCpu, QmdRead, ReleasePublishedGpu, InvalidBind, AllocationFailure };
enum class Source : u32 { None, Submitted, Refreshed, GpuCopy, SafeCopy, SafeDirect,
                          UnsafeCopy, UnsafeDirect, Prefetch, Qmd };
struct Origin {
    u64 submission{}, present{}, asid{}, entry{}, offset{}, generation{};
    s32 channel{-1};
    Source source{};
};
struct Event {
    Origin origin{};
    u64 ns{}, a{}, b{}, c{}, d{};
    EventKind kind{};
    std::array<u32, 8> words{};
};

// Value-only bounded history. No guest/session/engine pointer survives a callsite.
template <size_t Capacity>
class History {
public:
    void Append(const Event& event) { records[total++ % Capacity] = event; }
    std::vector<Event> Snapshot() const {
        std::vector<Event> result;
        const auto count = std::min<u64>(total, Capacity);
        result.reserve(count);
        for (u64 i = total - count; i < total; ++i) result.push_back(records[i % Capacity]);
        return result;
    }
    u64 Total() const { return total; }
private:
    std::array<Event, Capacity> records{};
    u64 total{};
};

template <typename T> u32 Word(const T& value) {
    if constexpr (requires { value.argument; }) return value.argument;
    else return value;
}
template <typename T> u64 Fingerprint(std::span<const T> words) {
    u64 hash = 0xcbf29ce484222325ULL;
    for (const auto& word : words.first(std::min(words.size(), SampleWords))) {
        hash = (hash ^ Word(word)) * 0x100000001b3ULL;
    }
    return hash;
}
struct Difference { u64 offset{std::numeric_limits<u64>::max()}; u32 before{}, after{}; };
template <typename T>
Difference FirstDifference(std::span<const T> before, std::span<const T> after) {
    const auto count = std::min(before.size(), after.size());
    for (size_t i = 0; i < count; ++i) {
        if (Word(before[i]) != Word(after[i])) return {i, Word(before[i]), Word(after[i])};
    }
    return {};
}


// Full words only for small OWNED command entries. The parser state saved at refresh describes
// the active channel, not each future entry. Actual entry state is attached only when parsed.
struct ParserState {
    u32 method{}, count{}, subchannel{}, flags{}, length_pending{};
    u64 dma_get{}, dma_word_offset{};
};
struct SmallEntryDifference {
    Origin origin{};
    u64 ns{}, new_generation{}, entry_index{}, queue_index{}, current_entry_index{};
    u64 resume_offset{}, refresh_ordinal{}, first_difference{}, entry_words{};
    ParserState refresh_parser{};
    std::array<u32, 16> before{}, after{};
    // Marks entry into ProcessCommands. The main Parse event records actual consumed words.
    bool parsed{};
    u64 parse_ns{}, parse_offset{}, available_parse_words{};
    Source parse_source{};
    ParserState parse_parser{};
};
template <typename T>
std::optional<SmallEntryDifference> CaptureSmallEntry(std::span<const T> before,
                                                     std::span<const T> after,
                                                     SmallEntryDifference metadata) {
    if (before.empty() || before.size() > 16 || before.size() != after.size()) {
        return {};
    }
    metadata.entry_words = before.size();
    for (size_t i = 0; i < before.size(); ++i) {
        metadata.before[i] = Word(before[i]);
        metadata.after[i] = Word(after[i]);
    }
    return metadata;
}
class SmallEntryHistory {
public:
    void Append(SmallEntryDifference value) {
        if (records.size() < 128) {
            records.push_back(std::move(value));
        }
    }
    void ObserveParse(Origin origin, ParserState parser, u64 ns, u64 words) {
        for (auto& e : records) {
            if (!e.parsed && e.ns <= ns && e.origin.asid == origin.asid &&
                e.origin.channel == origin.channel && e.origin.submission == origin.submission &&
                e.origin.entry == origin.entry && e.new_generation == origin.generation) {
                e.parsed = true;
                e.parse_ns = ns;
                e.parse_offset = origin.offset;
                e.available_parse_words = words;
                e.parse_source = origin.source;
                e.parse_parser = parser;
            }
        }
    }
    const std::vector<SmallEntryDifference>& Entries() const {
        return records;
    }
private:
    std::vector<SmallEntryDifference> records;
};

// Caller supplies an OWNED diagnostic baseline. Observe receives the exact scalar already
// loaded by the parser; Opaque explicitly excludes live bulk spans rather than rereading them.
template <typename T>
class Consumption {
public:
    explicit Consumption(std::span<const T> baseline_) : baseline{baseline_} {}
    void Observe(u64 offset, u32 word) {
        hash = (hash ^ word) * 0x100000001b3ULL;
        ++observed;
        if (sample_count < samples.size()) samples[sample_count++] = word;
        if (difference.offset == std::numeric_limits<u64>::max() && offset < baseline.size() &&
            Word(baseline[offset]) != word) difference = {offset, Word(baseline[offset]), word};
    }
    void Opaque(u64 count) { opaque += count; }
    Event Finish(Origin origin, u64 consumed) const {
        return {.origin = origin, .a = hash, .b = consumed, .c = difference.offset, .d = opaque,
                .kind = EventKind::Parse,
                .words = {difference.before, difference.after, observed, sample_count,
                          samples[0], samples[1], samples[2], samples[3]}};
    }
private:
    std::span<const T> baseline;
    Difference difference;
    u64 hash{0xcbf29ce484222325ULL}, opaque{};
    u32 observed{}, sample_count{};
    std::array<u32, 4> samples{};
};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* gate = std::getenv("EDEN_NX2_COMMAND_TRACE");
        const char* path = std::getenv("EDEN_NX2_COMMAND_TRACE_FILE");
        return gate && std::string_view{gate} == "1" && path && *path;
    }();
    return enabled;
}
inline u64 Present() { return Core::g_nx2_present_count.load(std::memory_order_relaxed); }
inline void Dump(const char* reason);
inline bool Active() {
    if (!Enabled()) return false;
    const auto present = Present();
    if (present > LastPresent) Dump("window-ended");
    return InWindow(present);
}
inline u64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline u64 NextSubmission() {
    if (!Enabled()) return 0;
    static std::atomic<u64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}
struct State {
    std::mutex mutex;
    History<16384> history;
    std::vector<Event> first_differences;
    SmallEntryHistory small_entries;
    bool dumped{};
};
inline State& GetState() { static State state; return state; }
inline void Record(Event event) {
    if (!Enabled() || (!InWindow(event.origin.present) && !InWindow(Present()))) return;
    event.ns = Now();
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (state.dumped) return;
    state.history.Append(event);
    if ((event.kind == EventKind::Refresh || event.kind == EventKind::Parse) &&
        event.c != std::numeric_limits<u64>::max() && state.first_differences.size() < 128) {
        state.first_differences.push_back(event);
    }
}

inline void RecordSmallDifference(SmallEntryDifference detail) {
    if (!Active()) {
        return;
    }
    detail.ns = Now();
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!state.dumped) {
        state.small_entries.Append(std::move(detail));
    }
}
inline void RecordSmallParse(Origin origin, ParserState parser, u64 words) {
    if (!Active()) {
        return;
    }
    const auto ns = Now();
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!state.dumped) {
        state.small_entries.ObserveParse(origin, parser, ns, words);
    }
}

inline thread_local Origin current_origin{};
class ScopedOrigin {
public:
    explicit ScopedOrigin(Origin origin) : previous{current_origin} { current_origin = origin; }
    ~ScopedOrigin() { current_origin = previous; }
private:
    Origin previous;
};
inline Origin CurrentOrigin(u64 asid) {
    auto origin = current_origin;
    origin.asid = asid;
    if (!origin.present) origin.present = Present();
    return origin;
}

// Triggered only at an existing stall report. No I/O on normal submissions or methods.
inline void Dump(const char* reason) {
    if (!Enabled()) return;
    auto& state = GetState();
    std::vector<Event> history, differences;
    std::vector<SmallEntryDifference> small_entries;
    u64 total{};
    {
        std::scoped_lock lock{state.mutex};
        if (state.dumped || state.history.Total() == 0) return;
        state.dumped = true;
        total = state.history.Total();
        history = state.history.Snapshot();
        differences = state.first_differences;
        small_entries = state.small_entries.Entries();
    }
    const char* path = std::getenv("EDEN_NX2_COMMAND_TRACE_FILE");
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        LOG_ERROR(HW_GPU, "Command provenance trace could not open output: {}", path);
        return;
    }
    file << "# reason=" << reason << " total=" << total << " retained=" << history.size()
         << " first_differences=" << differences.size() << " sampled_words_max=" << SampleWords << '\n';
    file << "section,kind,ns,present,submission,asid,channel,entry,offset,generation,source,a,b,c,d,w0,w1,w2,w3,w4,w5,w6,w7\n";
    const auto write = [&](std::string_view section, const Event& e) {
        file << section << ',' << static_cast<u32>(e.kind) << ',' << e.ns << ',' << e.origin.present
             << ',' << e.origin.submission << ',' << e.origin.asid << ',' << e.origin.channel
             << ',' << e.origin.entry << ',' << e.origin.offset << ',' << e.origin.generation
             << ',' << static_cast<u32>(e.origin.source) << ',' << e.a << ',' << e.b << ',' << e.c << ',' << e.d;
        for (auto word : e.words) file << ',' << word;
        file << '\n';
    };
    for (const auto& event : history) write("history", event);
    for (const auto& event : differences) write("first_difference", event);

    // A companion CSV avoids changing the existing bounded event schema. It contains only
    // <=16-word command-entry snapshots that were already copied by normal execution.
    std::ofstream details(std::string{path} + ".entries.csv", std::ios::trunc);
    if (details) {
        details << "ns,present,submission,asid,channel,entry,old_generation,new_generation,entry_words,entry_index,queue_index,current_entry_index,resume_offset,refresh_ordinal,first_difference";
        for (const auto* prefix : {"refresh", "parse"}) {
            details << ',' << prefix << "_method," << prefix << "_count," << prefix
                    << "_subchannel," << prefix << "_flags," << prefix << "_length_pending,"
                    << prefix << "_dma_get," << prefix << "_dma_word_offset";
        }
        details << ",entered_parse,parse_ns,parse_offset,available_parse_words,parse_source";
        for (u32 i = 0; i < 16; ++i) {
            details << ",old" << i;
        }
        for (u32 i = 0; i < 16; ++i) {
            details << ",new" << i;
        }
        details << '\n';
        for (const auto& e : small_entries) {
            details << e.ns << ',' << e.origin.present << ',' << e.origin.submission << ','
                    << e.origin.asid << ',' << e.origin.channel << ',' << e.origin.entry << ','
                    << e.origin.generation << ',' << e.new_generation << ',' << e.entry_words
                    << ',' << e.entry_index << ',' << e.queue_index << ',' << e.current_entry_index
                    << ',' << e.resume_offset << ',' << e.refresh_ordinal << ',' << e.first_difference;
            for (const auto& p : {e.refresh_parser, e.parse_parser}) {
                details << ',' << p.method << ',' << p.count << ',' << p.subchannel << ','
                        << p.flags << ',' << p.length_pending << ',' << p.dma_get << ',' << p.dma_word_offset;
            }
            details << ',' << e.parsed << ',' << e.parse_ns << ',' << e.parse_offset << ','
                    << e.available_parse_words << ',' << static_cast<u32>(e.parse_source);
            for (auto word : e.before) {
                details << ',' << word;
            }
            for (auto word : e.after) {
                details << ',' << word;
            }
            details << '\n';
        }
    } else {
        LOG_ERROR(HW_GPU, "Command small-entry trace could not open companion output: {}.entries.csv", path);
    }

    VideoCommon::CommandWriterTrace::Dump(path, reason);
    LOG_WARNING(HW_GPU, "Command provenance trace dumped {} of {} records, {} first differences to {} ({})",
                history.size(), total, differences.size(), path, reason);
}
} // namespace VideoCommon::CommandTrace

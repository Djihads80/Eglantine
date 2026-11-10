// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "video_core/shader_warm/shader_archives.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/shader_warm/warm_records.h"

namespace VideoCommon::ShaderWarm {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view StageReading = "Reading game files";
constexpr std::string_view StageExtracting = "Extracting shaders";
constexpr std::string_view StageConverting = "Converting";
constexpr std::string_view StageWriting = "Writing";
/// Conversions in flight (each holds a copy of its program).
constexpr size_t MaxPendingJobs = 2048;
/// Share of the progress bar for reading and extracting the archives; conversions run alongside
/// and only their tail after the last archive shows in the rest.
constexpr double ExtractionShare = 0.95;

fs::path TitleDir(u64 title_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
           fmt::format("{:016x}", title_id);
}

struct ArchiveFile {
    std::string path; ///< Inside the RomFS, '/' separated (the archive identity)
    FileSys::VirtualFile file;
};

/// Appends every file under `dir` to `out`, recursively, with its '/' separated path.
void ListFiles(const FileSys::VirtualDir& dir, const std::string& prefix,
               std::vector<ArchiveFile>& out) {
    for (const auto& file : dir->GetFiles()) {
        out.push_back({prefix + file->GetName(), file});
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        ListFiles(sub, prefix + sub->GetName() + "/", out);
    }
}

/// The game's RomFS trees in the order the offline pipeline read them: a .nup gives its base
/// and newest update package (both are read, like the exported packages 0 and 1); other
/// formats give the loader's RomFS.
struct GameFiles {
    std::unique_ptr<FileSys::NupContainer> nup;
    std::unique_ptr<Loader::AppLoader> loader;
    std::vector<FileSys::VirtualDir> romfs;
};

/// Opens the game file and collects its RomFS trees into `game`. Returns the error text when the
/// file cannot be opened or has no RomFS.
std::optional<std::string> OpenGame(Core::System& system, const fs::path& game_path,
                                    GameFiles& game) {
    FileSys::VirtualFilesystem filesystem = system.GetFilesystem();
    if (filesystem == nullptr) {
        filesystem = std::make_shared<FileSys::RealVfsFilesystem>();
    }
    const auto file =
        filesystem->OpenFile(Common::FS::PathToUTF8String(game_path), FileSys::OpenMode::Read);
    if (file == nullptr) {
        return "Cannot open the game file";
    }
    if (FileSys::NupContainer::IsNupFile(file)) {
        // Small memory cache: every member is read once.
        game.nup = std::make_unique<FileSys::NupContainer>(file, 128 * 1024 * 1024);
        if (game.nup->GetStatus() != Loader::ResultStatus::Success) {
            return "The game file is not a valid package";
        }
        std::vector<size_t> packages;
        if (const auto base = game.nup->FindBasePackage()) {
            packages.push_back(*base);
        }
        if (const auto update = game.nup->FindBestUpdatePackage()) {
            packages.push_back(*update);
        }
        std::sort(packages.begin(), packages.end());
        for (const size_t package : packages) {
            if (auto dir = game.nup->GetRomFSDir(package)) {
                game.romfs.push_back(std::move(dir));
            }
        }
    } else {
        game.loader = Loader::GetLoader(system, file);
        if (game.loader == nullptr) {
            return "Unsupported game file format";
        }
        FileSys::VirtualFile romfs;
        if (game.loader->ReadRomFS(romfs) != Loader::ResultStatus::Success || romfs == nullptr) {
            return "Cannot read the game's RomFS (are the keys installed?)";
        }
        if (auto dir = FileSys::ExtractRomFS(romfs)) {
            game.romfs.push_back(std::move(dir));
        }
    }
    if (game.romfs.empty()) {
        return "The game has no RomFS";
    }
    return std::nullopt;
}

struct PairHash {
    u128 a{};
    u128 b{};
    bool operator==(const PairHash&) const = default;
};
struct PairHasher {
    size_t operator()(const PairHash& h) const noexcept {
        return static_cast<size_t>(h.a[0] ^ (h.b[0] * 0x9E3779B97F4A7C15ULL));
    }
};

u128 Hash128(std::span<const u8> bytes) {
    return Common::CityHash128(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct Job {
    size_t seq{};
    std::vector<u8> program;
    ComputeControl control{};
};

struct JobResult {
    bool ok{};
    std::vector<u8> bytes; ///< The full compute entry
    ComputeKey compute_key{};
};

class Converter {
public:
    /// Starts `threads` workers that convert pushed jobs until `stop` is requested.
    Converter(size_t threads, std::stop_token stop_) : stop{std::move(stop_)} {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] { Work(); });
        }
    }

    /// Lets the workers finish the queued jobs, then joins them.
    ~Converter() {
        {
            std::scoped_lock lock{mutex};
            closing = true;
        }
        jobs_cv.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }

    void Push(Job job) {
        {
            std::scoped_lock lock{mutex};
            jobs.push_back(std::move(job));
            ++pushed;
        }
        jobs_cv.notify_one();
    }

    /// Next result in submission order; waits when `wait` is set, else empty when not ready.
    std::optional<JobResult> Next(bool wait) {
        std::unique_lock lock{mutex};
        for (;;) {
            const auto it = results.find(next);
            if (it != results.end()) {
                JobResult result = std::move(it->second);
                results.erase(it);
                ++next;
                return result;
            }
            if (!wait || next == pushed || stop.stop_requested()) {
                return std::nullopt;
            }
            results_cv.wait_for(lock, std::chrono::milliseconds{100});
        }
    }

    size_t Pending() {
        std::scoped_lock lock{mutex};
        return pushed - next;
    }

private:
    /// Worker loop: takes jobs in order, converts each one (skipped once stopping) and files the
    /// result under its sequence number. Returns once closing with no job left.
    void Work() {
        Common::SetCurrentThreadName("ShaderWarmBuild");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        for (;;) {
            Job job;
            {
                std::unique_lock lock{mutex};
                jobs_cv.wait(lock, [this] { return closing || !jobs.empty(); });
                if (jobs.empty()) {
                    return;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            JobResult result;
            if (!stop.stop_requested()) {
                result = Convert(job);
            }
            {
                std::scoped_lock lock{mutex};
                results.emplace(job.seq, std::move(result));
            }
            results_cv.notify_all();
        }
    }

    /// Lowers one program and makes its compute entry. The result is not ok when lowering or
    /// conversion failed.
    static JobResult Convert(const Job& job) {
        JobResult result;
        try {
            LoweredCode lowered;
            if (const auto error = LowerProgram(job.program, lowered)) {
                LOG_WARNING(Render_Vulkan, "Shader precache: program not converted: {}", *error);
                return result;
            }
            result.bytes = ComputeEntry(lowered, job.control, result.compute_key);
            result.ok = true;
        } catch (const std::exception& e) {
            LOG_DEBUG(Render_Vulkan, "Shader precache: conversion failed: {}", e.what());
        }
        return result;
    }

    std::stop_token stop;
    std::mutex mutex;
    std::condition_variable jobs_cv;
    std::condition_variable results_cv;
    std::deque<Job> jobs;
    std::map<size_t, JobResult> results;
    size_t pushed{};
    size_t next{};
    bool closing{};
    std::vector<std::thread> workers;
};

template <typename T>
void Write(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

void WriteHeader(std::ofstream& out) {
    out.write("yuzucach", 8);
    Write(out, WarmCacheVersion);
}

/// Records `entries` for the finished precache file at `warm_file` (see WarmCountPath).
void WriteWarmCount(const fs::path& warm_file, u64 entries) {
    std::error_code ec;
    const auto size = fs::file_size(warm_file, ec);
    if (ec) {
        return;
    }
    std::ofstream out(WarmCountPath(warm_file), std::ios::trunc);
    out << entries << ' ' << size << '\n';
}

} // Anonymous namespace

fs::path ComputeWarmPath(u64 title_id) {
    return TitleDir(title_id) / "warm-compute.bin";
}

fs::path WarmMarkerPath(const fs::path& warm_file) {
    fs::path marker = warm_file;
    marker.replace_extension(".done");
    return marker;
}

fs::path WarmCountPath(const fs::path& warm_file) {
    fs::path count = warm_file;
    count.replace_extension(".count");
    return count;
}

u64 ReadWarmCount(const fs::path& warm_file) {
    std::ifstream in(WarmCountPath(warm_file));
    u64 entries{};
    u64 size{};
    if (!(in >> entries >> size)) {
        return 0;
    }
    std::error_code ec;
    const auto actual = fs::file_size(warm_file, ec);
    return !ec && actual == size ? entries : 0;
}

std::optional<WarmStamp> CurrentWarmStamp(const fs::path& warm_file, u64 entries, u32 driver,
                                          u32 device_id, u32 cache_version) {
    std::error_code ec;
    const auto size = fs::file_size(warm_file, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto time = fs::last_write_time(warm_file, ec);
    if (ec) {
        return std::nullopt;
    }
    return WarmStamp{size,      static_cast<s64>(time.time_since_epoch().count()),
                     entries,   driver,
                     device_id, cache_version};
}

std::optional<WarmStamp> ReadWarmMarker(const fs::path& warm_file) {
    std::ifstream in(WarmMarkerPath(warm_file));
    WarmStamp stamp;
    if (!(in >> stamp.size >> stamp.write_time >> stamp.entries >> stamp.driver >>
          stamp.device_id >> stamp.cache_version)) {
        return std::nullopt;
    }
    return stamp;
}

bool WriteWarmMarker(const fs::path& warm_file, const WarmStamp& stamp) {
    std::ofstream out(WarmMarkerPath(warm_file), std::ios::trunc);
    out << stamp.size << ' ' << stamp.write_time << ' ' << stamp.entries << ' ' << stamp.driver
        << ' ' << stamp.device_id << ' ' << stamp.cache_version << '\n';
    out.close();
    return static_cast<bool>(out);
}

bool HasWarmFile(u64 title_id) {
    std::error_code ec;
    return fs::exists(ComputeWarmPath(title_id), ec);
}

bool DeleteWarmFiles(u64 title_id, std::string* error) {
    const auto warm = ComputeWarmPath(title_id);
    fs::path tmp = warm;
    tmp += ".tmp";
    bool ok = true;
    for (const auto& path : {warm, WarmMarkerPath(warm), WarmCountPath(warm), tmp}) {
        std::error_code ec;
        fs::remove(path, ec);
        if (ec) {
            ok = false;
            if (error != nullptr) {
                *error = ec.message();
            }
        }
    }
    return ok;
}

Result BuildWarmFiles(Core::System& system, const fs::path& game_path, u64 title_id,
                      const std::function<void(const Progress&)>& progress,
                      std::stop_token stop) {
    return BuildWarmFilesWith(system, game_path, title_id, BuildOptions{}, progress,
                              std::move(stop));
}

Result BuildWarmFilesWith(Core::System& system, const fs::path& game_path, u64 title_id,
                          const BuildOptions& options,
                          const std::function<void(const Progress&)>& progress,
                          std::stop_token stop) {
    Result result;
    u64 total_bytes = 0;
    u64 bytes_done = 0;
    size_t seq = 0;
    size_t consumed = 0;
    double reported_fraction = 0.0;
    auto last_report = std::chrono::steady_clock::time_point{};
    const auto report = [&](std::string_view stage, size_t done, size_t total) {
        if (!progress) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool last = total != 0 && done == total;
        if (!last && now - last_report < std::chrono::milliseconds{50}) {
            return;
        }
        last_report = now;
        const double extracted =
            total_bytes != 0 ? static_cast<double>(bytes_done) / static_cast<double>(total_bytes)
                             : 0.0;
        double fraction = ExtractionShare * extracted;
        if (stage == StageConverting) {
            fraction = ExtractionShare + (1.0 - ExtractionShare) *
                                             (seq != 0 ? static_cast<double>(consumed) /
                                                             static_cast<double>(seq)
                                                       : 1.0);
        } else if (stage == StageWriting) {
            fraction = last ? 1.0 : reported_fraction;
        }
        reported_fraction = std::max(reported_fraction, std::min(fraction, 1.0));
        progress(Progress{std::string(stage), done, total, reported_fraction, seq});
    };
    const auto begin = std::chrono::steady_clock::now();
    report(StageReading, 0, 0);

    const fs::path output_dir = options.output_dir.empty() ? TitleDir(title_id) : options.output_dir;
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    const fs::path compute_path = output_dir / "warm-compute.bin";
    fs::path compute_tmp = compute_path;
    compute_tmp += ".tmp";
    std::ofstream compute_out;
    const auto fail = [&](std::string error) {
        compute_out.close();
        std::error_code ignored;
        fs::remove(compute_tmp, ignored);
        result.ok = false;
        result.error = std::move(error);
        LOG_WARNING(Render_Vulkan, "Shader precache build for {:016x} failed: {}", title_id,
                    result.error);
        return result;
    };

    GameFiles game;
    if (const auto error = OpenGame(system, game_path, game)) {
        return fail(*error);
    }

    // Archive list, in the offline pipeline's order: every shader archive of each package, then
    // each package's effects, sorted by path inside a package (see ClassifyGameFile).
    std::vector<ArchiveFile> archives;
    std::vector<ArchiveFile> effects;
    ZstdDictionaries dictionaries;
    FileSys::VirtualFile dictionary_pack;
    LOG_INFO(Render_Vulkan, "Shader precache build: {} RomFS tree(s)", game.romfs.size());
    for (const auto& romfs : game.romfs) {
        std::vector<ArchiveFile> files;
        ListFiles(romfs, "", files);
        std::sort(files.begin(), files.end(),
                  [](const ArchiveFile& a, const ArchiveFile& b) { return a.path < b.path; });
        LOG_INFO(Render_Vulkan, "Shader precache build: RomFS tree with {} files", files.size());
        std::map<std::string, size_t> extensions;
        for (const auto& file : files) {
            const auto dot = file.path.rfind('.');
            ++extensions[dot == std::string::npos || dot < file.path.rfind('/')
                             ? std::string("(none)")
                             : file.path.substr(dot)];
        }
        std::string summary;
        for (const auto& [extension, count] : extensions) {
            summary += fmt::format(" {}={}", extension, count);
        }
        LOG_INFO(Render_Vulkan, "Shader precache build: files by extension:{}", summary);
        std::map<std::string, size_t> sampled;
        size_t shader_like = 0;
        for (const auto& file : files) {
            const auto dot = file.path.rfind('.');
            const std::string extension = dot == std::string::npos || dot < file.path.rfind('/')
                                              ? std::string("(none)")
                                              : file.path.substr(dot);
            std::string lower = file.path;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool looks_like_shader = lower.find("shader") != std::string::npos ||
                                           lower.find("bnsh") != std::string::npos ||
                                           lower.find("bfsha") != std::string::npos ||
                                           lower.find("pipeline") != std::string::npos;
            if (looks_like_shader && shader_like < 60) {
                ++shader_like;
            } else if (!looks_like_shader && ++sampled[extension] > 3) {
                continue;
            }
            LOG_INFO(Render_Vulkan, "Shader precache build: RomFS file {} ({} bytes)", file.path,
                     file.file->GetSize());
        }
        std::vector<ArchiveFile> package_effects;
        for (auto& file : files) {
            switch (ClassifyGameFile(file.path)) {
            case GameFileKind::ShaderArchive:
                archives.push_back(file);
                break;
            case GameFileKind::Effect:
                package_effects.push_back(file);
                break;
            case GameFileKind::ZstdDictionaries:
                dictionary_pack = file.file; // the newest package wins
                break;
            case GameFileKind::Other:
                break;
            }
        }
        effects.insert(effects.end(), package_effects.begin(), package_effects.end());
    }
    archives.insert(archives.end(), effects.begin(), effects.end());
    for (const auto& archive : archives) {
        total_bytes += archive.file->GetSize();
    }
    if (dictionary_pack) {
        try {
            dictionaries.Load(dictionary_pack->ReadAllBytes());
        } catch (const ArchiveError& e) {
            LOG_WARNING(Render_Vulkan, "Shader precache: zstd dictionary pack: {}", e.what());
        }
    }
    // Without archives the file is written empty: the precache then has only the pipeline cache
    // of earlier sessions to build.
    result.archives = archives.size();
    LOG_INFO(Render_Vulkan, "Shader precache build: {} archives, {} zstd dictionaries",
             archives.size(), dictionaries.Count());

    compute_out.open(compute_tmp, std::ios::binary | std::ios::trunc);
    if (!compute_out) {
        return fail("Cannot write to " + Common::FS::PathToUTF8String(output_dir));
    }
    WriteHeader(compute_out);

    std::unordered_set<PairHash, PairHasher> compute_identities;
    std::unordered_set<std::string> compute_keys;
    size_t archive_errors = 0;
    size_t member_errors = 0;

    const size_t threads =
        options.threads != 0
            ? options.threads
            : std::clamp<size_t>(std::thread::hardware_concurrency() / 4, 2, 6);
    auto converter = std::make_unique<Converter>(threads, stop);

    // Consumes finished conversions in submission order (first program wins on equal keys).
    const auto consume = [&](JobResult&& r) {
        ++consumed;
        if (!r.ok) {
            ++result.skipped;
            return;
        }
        const std::string key(reinterpret_cast<const char*>(r.compute_key.data()),
                              r.compute_key.size());
        if (compute_keys.insert(key).second) {
            compute_out.write(reinterpret_cast<const char*>(r.bytes.data()),
                              static_cast<std::streamsize>(r.bytes.size()));
            ++result.compute_programs;
        }
    };
    const auto drain = [&] {
        while (auto r = converter->Next(false)) {
            consume(std::move(*r));
        }
    };

    for (size_t archive_index = 0; archive_index < archives.size(); ++archive_index) {
        if (stop.stop_requested()) {
            converter.reset();
            return fail("Stopped");
        }
        report(StageExtracting, archive_index, archives.size());
        const auto& archive = archives[archive_index];
        const auto data = archive.file->ReadAllBytes();
        // Half of an archive's share when read, the rest when walked.
        const u64 archive_bytes = archive.file->GetSize();
        bytes_done += archive_bytes / 2;
        report(StageExtracting, archive_index, archives.size());
        const auto name = archive.path.substr(archive.path.rfind('/') + 1);
        size_t archive_records = 0;
        size_t archive_compute = 0;
        size_t archive_duplicates = 0;
        size_t archive_bad_control = 0;
        const size_t errors_before = member_errors;
        const auto on_record = [&](const ShaderRecord& record) {
            ++archive_records;
            if (record.stage != ProgramStage::Compute) {
                return;
            }
            ++archive_compute;
            if (!compute_identities.insert(PairHash{Hash128(record.code), Hash128(record.control)})
                     .second) {
                ++archive_duplicates;
                return;
            }
            const auto values = ReadComputeControl(record.control);
            if (!values || !values->workgroup[0] || !values->workgroup[1] ||
                !values->workgroup[2]) {
                ++archive_bad_control;
                ++result.skipped;
                return;
            }
            Job job;
            job.control = *values;
            job.seq = seq++;
            job.program.assign(record.code.begin(), record.code.end());
            converter->Push(std::move(job));
            while (converter->Pending() > MaxPendingJobs && !stop.stop_requested()) {
                if (auto r = converter->Next(true)) {
                    consume(std::move(*r));
                }
            }
        };
        const auto on_member_error = [&](const std::string& error) {
            ++member_errors;
            LOG_WARNING(Render_Vulkan, "Shader precache: {}: {}", archive.path, error);
        };
        try {
            WalkArchive(data, name, &dictionaries, on_record, on_member_error);
        } catch (const ArchiveError& e) {
            ++archive_errors;
            LOG_WARNING(Render_Vulkan, "Shader precache: {}: {}", archive.path, e.what());
        }
        LOG_INFO(Render_Vulkan,
                 "Shader precache: {} ({} bytes): {} records, {} compute ({} duplicate, {} bad "
                 "control), {} member errors",
                 archive.path, archive_bytes, archive_records, archive_compute, archive_duplicates,
                 archive_bad_control, member_errors - errors_before);
        bytes_done += archive_bytes - archive_bytes / 2;
        drain();
    }
    report(StageExtracting, archives.size(), archives.size());

    const size_t total_jobs = seq;
    while (consumed < total_jobs) {
        if (stop.stop_requested()) {
            converter.reset();
            return fail("Stopped");
        }
        report(StageConverting, consumed, total_jobs);
        if (auto r = converter->Next(true)) {
            consume(std::move(*r));
        }
    }
    report(StageConverting, total_jobs, total_jobs);
    converter.reset();

    report(StageWriting, 0, 1);
    compute_out.close();
    if (!compute_out) {
        return fail("Writing the precache file failed");
    }
    // A new file: the marker and count of the old one no longer apply.
    std::error_code ignored;
    fs::remove(WarmMarkerPath(compute_path), ignored);
    fs::remove(WarmCountPath(compute_path), ignored);
    fs::rename(compute_tmp, compute_path, ec);
    if (ec) {
        return fail("Cannot replace " + Common::FS::PathToUTF8String(compute_path));
    }
    WriteWarmCount(compute_path, result.compute_programs);
    report(StageWriting, 1, 1);
    result.ok = true;
    LOG_INFO(Render_Vulkan,
             "Shader precache build for {:016x}: {} compute entries, {} programs skipped, {} "
             "archive and {} member errors, {:.1f} s",
             title_id, result.compute_programs, result.skipped, archive_errors, member_errors,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
    return result;
}

} // namespace VideoCommon::ShaderWarm

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/nup2_writer.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <future>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/alignment.h"
#include "common/logging.h"
#include <optional>

#include <fmt/format.h>
#include <openssl/evp.h>

#include "core/file_sys/nup2_container.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"
#include "core/loader/nso_verify.h"

namespace FileSys::Nup2 {

namespace {

struct Member {
    std::string name;
    VirtualFile file;
    u64 size{};
    u64 offset{};
};

template <typename T>
void Put(std::vector<u8>& buffer, std::size_t offset, T value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(T));
}

void PutString(std::vector<u8>& buffer, std::size_t offset, std::size_t max, const std::string& text) {
    std::memcpy(buffer.data() + offset, text.data(), std::min(text.size(), max - 1));
}

bool Fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

/// An output file written at explicit offsets: safe to call WriteAt from several threads, in any order. The size is
/// set up front, so the gaps between members read back as zeros.
class PositionalFile {
public:
    PositionalFile(const std::filesystem::path& path, u64 size) {
#ifdef _WIN32
        handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER end;
        end.QuadPart = static_cast<LONGLONG>(size);
        if (handle != INVALID_HANDLE_VALUE && (!SetFilePointerEx(handle, end, nullptr, FILE_BEGIN) || !SetEndOfFile(handle))) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
#else
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0 && ftruncate(fd, static_cast<off_t>(size)) != 0) {
            ::close(fd);
            fd = -1;
        }
#endif
    }

    ~PositionalFile() {
        Close();
    }

    void Close() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
#else
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
#endif
    }

    bool IsOpen() const {
#ifdef _WIN32
        return handle != INVALID_HANDLE_VALUE;
#else
        return fd >= 0;
#endif
    }

    bool WriteAt(u64 offset, const u8* data, std::size_t size) {
        while (size > 0) {
#ifdef _WIN32
            OVERLAPPED position{};
            position.Offset = static_cast<DWORD>(offset & 0xffffffffu);
            position.OffsetHigh = static_cast<DWORD>(offset >> 32);
            DWORD done = 0;
            if (!WriteFile(handle, data, static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30)), &done, &position)) {
                return false;
            }
#else
            const ssize_t done = pwrite(fd, data, size, static_cast<off_t>(offset));
            if (done <= 0) {
                return false;
            }
#endif
            offset += static_cast<u64>(done);
            data += done;
            size -= static_cast<std::size_t>(done);
        }
        return true;
    }

private:
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    int fd{-1};
#endif
};

} // namespace

PackResult PackFromFiles(const std::vector<ExeFsSource>& exefs, const VirtualFile& romfs,
                         const std::filesystem::path& output, const PackOptions& options,
                         const std::function<void(u64, u64)>& progress, std::string* error,
                         const std::atomic_bool* cancel) {
    namespace fs = std::filesystem;
    if (romfs == nullptr) {
        Fail(error, "no RomFS");
        return PackResult::RomFsMissing;
    }

    std::vector<Member> members;
    for (const auto& source : exefs) {
        if (source.file == nullptr || source.name.empty() || source.name.front() == '.' || source.name.size() >= 48) {
            continue;
        }
        members.push_back({source.name, source.file, static_cast<u64>(source.file->GetSize()), 0});
    }
    std::ranges::sort(members, {}, &Member::name);
    const auto has = [&](std::string_view name) {
        return std::ranges::any_of(members, [&](const Member& m) { return m.name == name; });
    };
    if (!has("main") || !has("main.npdm")) {
        Fail(error, "ExeFS needs main and main.npdm");
        return PackResult::ExeFsMissing;
    }
    // A bad segment hash means a corrupt or edited dump. Packing still goes ahead; the user is told.
    for (const auto& member : members) {
        const auto report = Loader::VerifyNSOSegmentHashes(*member.file);
        if (report.header_valid && !report.FlaggedMatch()) {
            LOG_WARNING(Loader, "NUP v2: {} has NSO segment hashes that do not match its contents (corrupt or modified)",
                        member.name);
        }
    }

    u64 title_id = options.title_id;
    if (title_id == 0) {
        const auto npdm = std::ranges::find(members, "main.npdm", &Member::name);
        ProgramMetadata metadata;
        if (metadata.Load(npdm->file) != Loader::ResultStatus::Success) {
            Fail(error, "main.npdm could not be parsed");
            return PackResult::BadMetadata;
        }
        title_id = metadata.GetTitleID();
    }

    // Layout: superblock, ExeFS table, ExeFS members, RomFS (all sizes are known up front).
    const u64 meta_offset = Nup2Container::SuperblockSize;
    const u64 meta_size = members.size() * Nup2Container::EntrySize;
    u64 cursor = Common::AlignUp(meta_offset + meta_size, Nup2Container::ExeFsAlignment);
    for (auto& member : members) {
        member.offset = cursor;
        cursor = Common::AlignUp(cursor + member.size, Nup2Container::ExeFsAlignment);
    }
    const u64 romfs_offset = Common::AlignUp(cursor, Nup2Container::RomFsAlignment);
    const u64 romfs_size = static_cast<u64>(romfs->GetSize());
    const u64 file_size = romfs_offset + romfs_size;

    std::vector<u8> head(static_cast<std::size_t>(meta_offset + meta_size));
    std::ranges::copy(Nup2Container::Magic, head.begin());
    Put<u32>(head, 0x04, Nup2Container::FormatRevision);
    Put<u64>(head, 0x08, title_id);
    Put<u32>(head, 0x10, options.program_version);
    Put<u32>(head, 0x14, 0);
    Put<u64>(head, 0x18, file_size);
    Put<u64>(head, 0x20, meta_offset);
    Put<u64>(head, 0x28, meta_size);
    Put<u64>(head, 0x30, romfs_offset);
    Put<u64>(head, 0x38, romfs_size);
    Put<u32>(head, 0x40, static_cast<u32>(members.size()));
    PutString(head, 0x48, 32, options.display_version);
    PutString(head, 0x68, 128, options.title_name);
    for (std::size_t i = 0; i < members.size(); ++i) {
        const std::size_t base = static_cast<std::size_t>(meta_offset) + i * Nup2Container::EntrySize;
        Put<u64>(head, base, members[i].offset);
        Put<u64>(head, base + 8, members[i].size);
        PutString(head, base + 16, 48, members[i].name);
    }

    // Every destination is fixed by the header, so the data is written positionally: the file is sized first and
    // each job (an ExeFS member or a 32 MiB slice of the RomFS) reads its source and writes it at its own offset,
    // from whichever thread picks it up, in any order.
    PositionalFile out(output, file_size);
    if (!out.IsOpen()) {
        Fail(error, "cannot create the output file");
        return PackResult::IoError;
    }
    if (!out.WriteAt(0, head.data(), head.size())) {
        Fail(error, "write failed");
        return PackResult::IoError;
    }

    struct Job {
        VirtualFile source;
        u64 source_offset;
        u64 size;
        u64 destination;
        std::string name;
        std::size_t slice; ///< RomFS slice number (the hash is fed in this order); NotASlice for ExeFS members
    };
    constexpr std::size_t NotASlice = ~std::size_t{0};
    std::vector<Job> jobs;
    for (const auto& member : members) {
        jobs.push_back({member.file, 0, member.size, member.offset, member.name, NotASlice});
    }
    constexpr u64 SliceSize = u64{32} << 20;
    for (u64 begin = 0; begin < romfs_size; begin += SliceSize) {
        jobs.push_back({romfs, begin, std::min(SliceSize, romfs_size - begin), romfs_offset + begin, "RomFS",
                        static_cast<std::size_t>(begin / SliceSize)});
    }

    // The RomFS SHA-256 goes into the header. Slices are read in parallel but hashed in order: a worker waits for
    // its turn, feeds the hash, then writes its slice outside the lock.
    struct DigestContextDeleter {
        void operator()(EVP_MD_CTX* context) const {
            EVP_MD_CTX_free(context);
        }
    };
    const std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> digest{EVP_MD_CTX_new()};
    if (digest == nullptr || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
        Fail(error, "cannot start the RomFS SHA-256");
        return PackResult::IoError;
    }
    std::mutex digest_mutex;
    std::condition_variable digest_turn_changed;
    std::size_t next_slice_to_hash = 0;

    const unsigned workers = std::clamp(std::thread::hardware_concurrency(), 2u, 8u);
    std::atomic<std::size_t> next_job{0};
    std::atomic<u64> done_bytes{head.size()};
    std::atomic<bool> failed{false};
    std::atomic<bool> cancelled{false};
    std::mutex message_mutex;
    std::string failure;
    {
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < workers; ++t) {
            pool.emplace_back([&] {
                std::vector<u8> buffer;
                // Marks the run failed and wakes workers waiting for their turn to hash.
                const auto abort = [&] {
                    failed = true;
                    std::scoped_lock lock{digest_mutex};
                    digest_turn_changed.notify_all();
                };
                while (!failed) {
                    if (cancel != nullptr && cancel->load()) {
                        cancelled = true;
                        abort();
                        return;
                    }
                    const std::size_t index = next_job.fetch_add(1);
                    if (index >= jobs.size()) {
                        return;
                    }
                    const Job& job = jobs[index];
                    buffer.resize(static_cast<std::size_t>(job.size));
                    if (job.size != 0) {
                        const char* step = nullptr;
                        if (job.source->Read(buffer.data(), buffer.size(), static_cast<std::size_t>(job.source_offset)) !=
                            buffer.size()) {
                            step = "read from the source";
                        } else {
                            if (job.slice != NotASlice) {
                                std::unique_lock lock{digest_mutex};
                                digest_turn_changed.wait(lock, [&] { return failed || next_slice_to_hash == job.slice; });
                                if (failed) {
                                    return;
                                }
                                EVP_DigestUpdate(digest.get(), buffer.data(), buffer.size());
                                ++next_slice_to_hash;
                                digest_turn_changed.notify_all();
                            }
                            if (!out.WriteAt(job.destination, buffer.data(), buffer.size())) {
                                step = "write to the output";
                            }
                        }
                        if (step != nullptr) {
                            {
                                std::scoped_lock lock{message_mutex};
                                if (!failed) {
                                    failure = fmt::format(
                                        "cannot copy {}: failed to {} ({} bytes at source offset {:#x})", job.name,
                                        step, job.size, job.source_offset);
                                }
                            }
                            abort();
                            return;
                        }
                    }
                    const u64 total = done_bytes.fetch_add(job.size) + job.size;
                    if (progress) {
                        std::scoped_lock lock{message_mutex};
                        progress(total, file_size);
                    }
                }
            });
        }
    }
    if (failed) {
        out.Close();
        std::error_code remove_error;
        fs::remove(output, remove_error);
        if (cancelled) {
            return PackResult::Cancelled;
        }
        Fail(error, failure);
        return PackResult::IoError;
    }

    // Record the RomFS hash now that every slice has been fed to it.
    Nup2Container::Sha256Digest romfs_hash{};
    unsigned int hash_length = 0;
    if (EVP_DigestFinal_ex(digest.get(), romfs_hash.data(), &hash_length) != 1 || hash_length != romfs_hash.size()) {
        out.Close();
        std::error_code remove_error;
        fs::remove(output, remove_error);
        Fail(error, "cannot finish the RomFS SHA-256");
        return PackResult::IoError;
    }
    Put<u32>(head, 0x14, Nup2Container::FlagRomFsSha256);
    std::ranges::copy(romfs_hash, head.begin() + Nup2Container::RomFsSha256Offset);
    if (!out.WriteAt(0, head.data(), head.size())) {
        out.Close();
        std::error_code remove_error;
        fs::remove(output, remove_error);
        Fail(error, "write failed while recording the RomFS SHA-256");
        return PackResult::IoError;
    }
    if (progress) {
        progress(file_size, file_size);
    }
    return PackResult::Success;
}

PackResult ConvertFromNup1(const VirtualFile& source, const std::filesystem::path& output,
                           const std::function<void(u64, u64)>& progress, std::string* error,
                           const std::atomic_bool* cancel) {
    NupContainer nup(source);
    if (nup.GetStatus() != Loader::ResultStatus::Success) {
        Fail(error, "not a readable NUP v1 file");
        return PackResult::IoError;
    }
    // The same package choice as the v1 loader: the base program (with the best update's ExeFS), or a standalone
    // update that carries the complete tree.
    const auto base_index = nup.FindBasePackage();
    const auto update_index = nup.FindBestUpdatePackage();
    std::optional<std::size_t> program_index, exefs_index;
    if (base_index && nup.GetExeFSDir(*base_index) != nullptr) {
        program_index = base_index;
        exefs_index = update_index && nup.GetExeFSDir(*update_index) != nullptr ? update_index : base_index;
    } else if (update_index && nup.GetExeFSDir(*update_index) != nullptr) {
        program_index = update_index;
        exefs_index = update_index;
    }
    if (!program_index || !exefs_index) {
        Fail(error, "no package with a usable ExeFS");
        return PackResult::ExeFsMissing;
    }
    const auto& program = nup.GetPackages()[*program_index];
    const auto version_text = [](const NupContainer::PackageInfo& package) {
        if (!package.display_version.empty()) {
            return package.display_version;
        }
        return package.version == 0 ? std::string{}
                                    : fmt::format("{}.{}.{}", package.version >> 16, (package.version >> 8) & 0xFF,
                                                  package.version & 0xFF);
    };
    PackOptions options;
    options.title_id = program.title_id;
    options.title_name = !program.title_name.empty() ? program.title_name : program.source_dump;
    const auto& versioned = update_index ? nup.GetPackages()[*update_index] : program;
    options.display_version = version_text(versioned);
    options.program_version = versioned.version;

    std::vector<ExeFsSource> exefs;
    for (const auto& file : nup.GetExeFSDir(*exefs_index)->GetFiles()) {
        exefs.push_back({file->GetName(), file});
    }
    // The game list icon travels as an extra table entry; loaders that don't know it ignore it.
    std::vector<u8> icon = program.icon;
    if (icon.empty() && !program.localized_icons.empty()) {
        icon = program.localized_icons.front().second;
    }
    if (!icon.empty()) {
        exefs.push_back({std::string(Nup2Container::IconName), std::make_shared<VectorVfsFile>(std::move(icon))});
    }
    const auto base_romfs = base_index ? nup.GetRomFSDir(*base_index) : VirtualDir{};
    const auto update_romfs = update_index ? nup.GetRomFSDir(*update_index) : VirtualDir{};
    const auto merged = NupContainer::MergeShadowDirs(base_romfs, update_romfs);
    const auto romfs = merged != nullptr ? CreateRomFS(merged) : nullptr;
    if (romfs == nullptr) {
        Fail(error, "no RomFS in the package");
        return PackResult::RomFsMissing;
    }
    return PackFromFiles(exefs, romfs, output, options, progress, error, cancel);
}

PackResult ConvertFromDump(const std::filesystem::path& dump_root, const std::filesystem::path& output,
                           const std::function<void(u64, u64)>& progress, std::string* error,
                           const std::atomic_bool* cancel) {
    namespace fs = std::filesystem;
    const auto lower = [](std::string text) {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const auto find_dir = [&](const fs::path& parent, std::string_view name) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(parent, ec)) {
            if (entry.is_directory(ec) && lower(entry.path().filename().string()) == name) {
                return entry.path();
            }
        }
        return fs::path{};
    };

    const auto base = find_dir(dump_root, "base");
    const auto updates = find_dir(dump_root, "updates");
    const fs::path base_dir = !base.empty() ? base : (updates.empty() ? dump_root : fs::path{});
    fs::path update_dir;
    u32 update_version = 0;
    if (!updates.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(updates, ec)) {
            const auto name = entry.path().filename().string();
            if (!entry.is_directory(ec) || name.empty() || !std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; })) {
                continue;
            }
            const auto version = static_cast<u32>(std::stoull(name));
            if (update_dir.empty() || version > update_version) {
                update_dir = entry.path();
                update_version = version;
            }
        }
    }

    RealVfsFilesystem vfs;
    const auto open = [&](const fs::path& dir) -> VirtualDir {
        return dir.empty() ? nullptr : vfs.OpenDirectory(dir.string(), OpenMode::Read);
    };
    const auto exefs_in = [&](const fs::path& dir) -> VirtualDir {
        const auto exefs = dir.empty() ? VirtualDir{} : open(find_dir(dir, "exefs"));
        return exefs != nullptr && exefs->GetFile("main") != nullptr ? exefs : nullptr;
    };
    auto exefs_dir = exefs_in(update_dir);
    if (exefs_dir == nullptr) {
        exefs_dir = exefs_in(base_dir);
    }
    if (exefs_dir == nullptr) {
        Fail(error, "no ExeFS with a main in the dump");
        return PackResult::ExeFsMissing;
    }
    const auto romfs_in = [&](const fs::path& dir) -> VirtualDir {
        return dir.empty() ? nullptr : open(find_dir(dir, "romfs"));
    };
    const auto merged = NupContainer::MergeShadowDirs(romfs_in(base_dir), romfs_in(update_dir));
    const auto romfs = merged != nullptr ? CreateRomFS(merged) : nullptr;
    if (romfs == nullptr) {
        Fail(error, "no RomFS in the dump");
        return PackResult::RomFsMissing;
    }

    const auto metadata = Nup::ProbeMetadata(dump_root);
    PackOptions options;
    options.title_name = metadata.title_name.empty() ? dump_root.filename().string() : metadata.title_name;
    options.display_version = metadata.display_version;
    options.program_version = update_version;

    std::vector<ExeFsSource> exefs;
    for (const auto& file : exefs_dir->GetFiles()) {
        exefs.push_back({file->GetName(), file});
    }
    if (!metadata.icon.empty()) {
        exefs.push_back({std::string(Nup2Container::IconName), std::make_shared<VectorVfsFile>(metadata.icon)});
    }
    return PackFromFiles(exefs, romfs, output, options, progress, error, cancel);
}

bool VerifyRomFsHash(const VirtualFile& v2_file, const std::function<void(u64, u64)>& progress, std::string* error) {
    Nup2Container v2(v2_file);
    if (!v2.IsValid()) {
        Fail(error, "not a readable NUP v2 file");
        return false;
    }
    if (!v2.GetRomFsSha256()) {
        Fail(error, "this file has no RomFS SHA-256 recorded");
        return false;
    }
    const auto romfs = v2.GetRomFs();
    if (romfs == nullptr) {
        Fail(error, "no RomFS in the file");
        return false;
    }
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    if (digest == nullptr || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
        Fail(error, "cannot start the SHA-256");
        return false;
    }
    constexpr u64 ChunkSize = u64{32} << 20;
    const u64 size = romfs->GetSize();
    // Two buffers: the next chunk is read on another thread while the current one is hashed, so the time is the
    // slower of the disk and the hash instead of their sum.
    std::array<std::vector<u8>, 2> buffers;
    const auto read_chunk = [&](u64 offset, std::vector<u8>& buffer) {
        const auto length = static_cast<std::size_t>(std::min(ChunkSize, size - offset));
        buffer.resize(length);
        return romfs->Read(buffer.data(), length, static_cast<std::size_t>(offset)) == length;
    };
    std::future<bool> pending;
    if (size != 0) {
        pending = std::async(std::launch::async, read_chunk, u64{0}, std::ref(buffers[0]));
    }
    for (u64 offset = 0, index = 0; offset < size; offset += ChunkSize, ++index) {
        if (!pending.get()) {
            Fail(error, fmt::format("cannot read the RomFS at offset {:#x}", offset));
            return false;
        }
        auto& current = buffers[index % 2];
        const u64 next_offset = offset + ChunkSize;
        if (next_offset < size) {
            pending = std::async(std::launch::async, read_chunk, next_offset, std::ref(buffers[(index + 1) % 2]));
        }
        EVP_DigestUpdate(digest.get(), current.data(), current.size());
        if (progress) {
            progress(offset + current.size(), size);
        }
    }
    Nup2Container::Sha256Digest actual{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(digest.get(), actual.data(), &length) != 1 || length != actual.size()) {
        Fail(error, "cannot finish the SHA-256");
        return false;
    }
    if (actual != *v2.GetRomFsSha256()) {
        Fail(error, "the RomFS does not match the SHA-256 recorded in the header (corrupt or modified)");
        return false;
    }
    return true;
}

bool VerifyAgainstNup1(const VirtualFile& v2_file, const VirtualFile& v1_file,
                       const std::function<void(u64, u64)>& progress, std::string* error) {
    Nup2Container v2(v2_file);
    NupContainer v1(v1_file);
    if (!v2.IsValid() || v1.GetStatus() != Loader::ResultStatus::Success) {
        Fail(error, "cannot open one of the files");
        return false;
    }
    const auto base_index = v1.FindBasePackage();
    const auto update_index = v1.FindBestUpdatePackage();
    const auto merged = NupContainer::MergeShadowDirs(base_index ? v1.GetRomFSDir(*base_index) : VirtualDir{},
                                                      update_index ? v1.GetRomFSDir(*update_index) : VirtualDir{});
    const auto expected = merged != nullptr ? CreateRomFS(merged) : nullptr;
    const auto actual = v2.GetRomFs();
    if (expected == nullptr || actual == nullptr || expected->GetSize() != actual->GetSize()) {
        Fail(error, "RomFS sizes differ or the v1 RomFS cannot be built");
        return false;
    }
    constexpr u64 SliceSize = u64{32} << 20;
    const u64 size = expected->GetSize();
    const u64 slices = (size + SliceSize - 1) / SliceSize;
    std::atomic<u64> next{0}, done{0}, first_bad{~u64{0}};
    std::mutex mutex;
    {
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < std::clamp(std::thread::hardware_concurrency(), 2u, 8u); ++t) {
            pool.emplace_back([&] {
                std::vector<u8> a, b;
                while (first_bad == ~u64{0}) {
                    const u64 index = next.fetch_add(1);
                    if (index >= slices) {
                        return;
                    }
                    const u64 begin = index * SliceSize;
                    const auto length = static_cast<std::size_t>(std::min(SliceSize, size - begin));
                    a.resize(length);
                    b.resize(length);
                    const bool read_ok = expected->Read(a.data(), length, static_cast<std::size_t>(begin)) == length &&
                                         actual->Read(b.data(), length, static_cast<std::size_t>(begin)) == length;
                    if (!read_ok || a != b) {
                        std::size_t at = 0;
                        while (read_ok && a[at] == b[at]) {
                            ++at;
                        }
                        u64 expected_first = ~u64{0};
                        first_bad.compare_exchange_strong(expected_first, begin + at);
                        return;
                    }
                    const u64 total = done.fetch_add(length) + length;
                    if (progress) {
                        std::scoped_lock lock{mutex};
                        progress(total, size);
                    }
                }
            });
        }
    }
    if (first_bad != ~u64{0}) {
        Fail(error, fmt::format("RomFS differs at offset {:#x}", first_bad.load()));
        return false;
    }
    return true;
}

PackResult Pack(const std::filesystem::path& exefs_dir, const std::filesystem::path& romfs_image,
                const std::filesystem::path& output, const PackOptions& options,
                const std::function<void(u64, u64)>& progress, std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(exefs_dir, ec)) {
        Fail(error, "ExeFS folder not found");
        return PackResult::ExeFsMissing;
    }
    if (!fs::is_regular_file(romfs_image, ec)) {
        Fail(error, "RomFS image not found");
        return PackResult::RomFsMissing;
    }
    static const auto vfs = std::make_shared<RealVfsFilesystem>();
    std::vector<ExeFsSource> exefs;
    for (const auto& entry : fs::directory_iterator(exefs_dir, ec)) {
        if (entry.is_regular_file()) {
            exefs.push_back({entry.path().filename().string(), vfs->OpenFile(entry.path().string(), OpenMode::Read)});
        }
    }
    return PackFromFiles(exefs, vfs->OpenFile(romfs_image.string(), OpenMode::Read), output, options, progress, error);
}

} // namespace FileSys::Nup2

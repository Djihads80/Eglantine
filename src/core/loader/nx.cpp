// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

#include <fmt/format.h>
#include <nx_disk_source.hpp>
#include <nx_session.hpp>
#include <zstd.h>

#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/decrypted_nca.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nsp.h"
#include "core/loader/nx.h"

namespace FileSys {

namespace {

constexpr std::size_t MaxBlockBytes = 1024 * 1024;

// Decoder contexts belong to the mounted archive, not to the calling threads.
class DecoderPool {
    struct Slot {
        ZSTD_DCtx* context = nullptr;
        bool busy = false;
    };

public:
    explicit DecoderPool(std::size_t limit) : slots(limit) {}
    ~DecoderPool() {
        for (auto& slot : slots) {
            ZSTD_freeDCtx(slot.context);
        }
    }

    class Lease {
    public:
        explicit Lease(DecoderPool& pool_) : pool{pool_} {
            std::unique_lock lock{pool.mutex};
            pool.available.wait(lock, [&] {
                for (auto& candidate : pool.slots) {
                    if (!candidate.busy) {
                        slot = &candidate;
                        return true;
                    }
                }
                return false;
            });
            slot->busy = true;
        }
        ~Lease() {
            {
                std::scoped_lock lock{pool.mutex};
                slot->busy = false;
            }
            pool.available.notify_one();
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        ZSTD_DCtx* Context() {
            if (slot->context == nullptr) {
                slot->context = ZSTD_createDCtx();
                if (slot->context != nullptr &&
                    ZSTD_isError(ZSTD_DCtx_setParameter(slot->context, ZSTD_d_windowLogMax, 20))) {
                    ZSTD_freeDCtx(slot->context);
                    slot->context = nullptr;
                }
            }
            return slot->context;
        }

    private:
        DecoderPool& pool;
        Slot* slot = nullptr;
    };

private:
    std::vector<Slot> slots;
    std::mutex mutex;
    std::condition_variable available;
};

// Decodes one block: a single Zstandard frame without dictionary, at most 1 MiB.
bool Decode(void* opaque, const u8* frame, std::size_t size, u8* output, std::size_t capacity,
            std::size_t* written, std::size_t* consumed) {
    if (size < 6 || capacity > MaxBlockBytes || std::memcmp(frame, "\x28\xb5\x2f\xfd", 4) != 0 ||
        ZSTD_getFrameContentSize(frame, size) != capacity ||
        ZSTD_getDictID_fromFrame(frame, size) != 0 ||
        ZSTD_findFrameCompressedSize(frame, size) != size) {
        return false;
    }
    const unsigned descriptor = frame[4];
    if ((descriptor & 0x1bu) != 0) {
        return false;
    }
    if ((descriptor & 0x20u) == 0) {
        const u64 base = u64{1} << (10 + (frame[5] >> 3));
        if (base + (base >> 3) * (frame[5] & 7u) > MaxBlockBytes) {
            return false;
        }
    }
    DecoderPool::Lease decoder{*static_cast<DecoderPool*>(opaque)};
    const auto context = decoder.Context();
    if (context == nullptr) {
        return false;
    }
    const auto result = ZSTD_decompressDCtx(context, output, capacity, frame, size);
    if (ZSTD_isError(result) || result != capacity) {
        return false;
    }
    *written = result;
    *consumed = size;
    return true;
}

class CodecSource final : public nx::RandomAccessSource {
public:
    CodecSource(std::shared_ptr<nx::RandomAccessSource> source_, std::size_t limit)
        : source{std::move(source_)}, decoder{limit} {}
    u64 size() const noexcept override {
        return source->size();
    }
    bool read_at(u64 offset, u8* output, std::size_t length) override {
        return source->read_at(offset, output, length);
    }

    std::shared_ptr<nx::RandomAccessSource> source;
    DecoderPool decoder;
};

// Reads through the VFS, for files without a host path (Android content URIs, memory).
class VfsSource final : public nx::RandomAccessSource {
public:
    explicit VfsSource(VirtualFile file_) : file{std::move(file_)}, file_size{file->GetSize()} {}
    u64 size() const noexcept override {
        return file_size;
    }
    bool read_at(u64 offset, u8* output, std::size_t length) override {
        if (offset > file_size || length > file_size - offset) {
            return false;
        }
        std::scoped_lock lock{mutex};
        return file->Read(output, length, static_cast<std::size_t>(offset)) == length;
    }

private:
    VirtualFile file;
    u64 file_size;
    std::mutex mutex;
};

class NxFile final : public VfsFile {
public:
    NxFile(std::shared_ptr<nx::Session> session_, nx::FileInfo info_, std::string name_,
           VirtualDir parent_)
        : session{std::move(session_)}, info{info_}, name{std::move(name_)},
          parent{std::move(parent_)}, cursor{std::make_shared<nx::ReadCursor>()} {}

    std::string GetName() const override {
        return name;
    }
    std::size_t GetSize() const override {
        return static_cast<std::size_t>(info.size);
    }
    bool Resize(std::size_t) override {
        return false;
    }
    VirtualDir GetContainingDirectory() const override {
        return parent;
    }
    bool IsWritable() const override {
        return false;
    }
    bool IsReadable() const override {
        return true;
    }
    std::size_t Read(u8* output, std::size_t length, std::size_t offset) const override {
        if (offset > info.size || (length != 0 && output == nullptr)) {
            return 0;
        }
        length = static_cast<std::size_t>(std::min<u64>(length, info.size - offset));
        std::size_t done = 0;
        while (done < length) {
            // read() commits each verified block straight into the output, so a read may span
            // blocks; only the session's max_read_bytes() splits it. On failure, `bytes` is the
            // verified prefix, which is all this returns.
            const auto part =
                static_cast<std::size_t>(std::min<u64>(length - done, session->max_read_bytes()));
            const auto result =
                session->read(info.id, offset + done, part, output + done, part, cursor);
            done += result.bytes;
            if (!result) {
                break;
            }
        }
        return done;
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override {
        return 0;
    }
    bool Rename(std::string_view) override {
        return false;
    }

private:
    std::shared_ptr<nx::Session> session;
    nx::FileInfo info;
    std::string name;
    VirtualDir parent;
    std::shared_ptr<nx::ReadCursor> cursor;
};

class NxDirectory final : public ReadOnlyVfsDirectory,
                          public std::enable_shared_from_this<NxDirectory> {
public:
    NxDirectory(std::shared_ptr<nx::Session> session_, nx::DirectoryHandle handle_,
                std::string name_, VirtualDir parent_ = {})
        : session{std::move(session_)}, handle{handle_}, name{std::move(name_)},
          parent{std::move(parent_)} {}

    // lookup() is a binary search over the directory's sorted entries.
    VirtualFile GetFile(std::string_view file_name) const override {
        const auto entry = session->archive().lookup(handle, file_name);
        if (!entry || entry.value->type != nx::EntryType::file) {
            return nullptr;
        }
        return MakeFile(*entry.value);
    }
    VirtualDir GetSubdirectory(std::string_view dir_name) const override {
        const auto entry = session->archive().lookup(handle, dir_name);
        if (!entry || entry.value->type != nx::EntryType::directory) {
            return nullptr;
        }
        return MakeDirectory(*entry.value);
    }
    std::vector<VirtualFile> GetFiles() const override {
        std::vector<VirtualFile> result;
        session->archive().enumerate(handle, [&](const nx::DirectoryEntryView& entry) {
            if (entry.type == nx::EntryType::file) {
                if (auto file = MakeFile(entry)) {
                    result.push_back(std::move(file));
                }
            }
            return true;
        });
        return result;
    }
    std::vector<VirtualDir> GetSubdirectories() const override {
        std::vector<VirtualDir> result;
        session->archive().enumerate(handle, [&](const nx::DirectoryEntryView& entry) {
            if (entry.type == nx::EntryType::directory) {
                result.push_back(MakeDirectory(entry));
            }
            return true;
        });
        return result;
    }
    std::string GetName() const override {
        return name;
    }
    VirtualDir GetParentDirectory() const override {
        return parent;
    }

private:
    VirtualDir Self() const {
        return std::const_pointer_cast<NxDirectory>(shared_from_this());
    }
    VirtualFile MakeFile(const nx::DirectoryEntryView& entry) const {
        const auto info = session->archive().file_info(entry.file);
        if (!info || !std::in_range<std::size_t>(info.value->size)) {
            return nullptr;
        }
        return std::make_shared<NxFile>(session, *info.value, std::string(entry.name), Self());
    }
    VirtualDir MakeDirectory(const nx::DirectoryEntryView& entry) const {
        return std::make_shared<NxDirectory>(session, entry.directory, std::string(entry.name),
                                             Self());
    }

    std::shared_ptr<nx::Session> session;
    nx::DirectoryHandle handle;
    std::string name;
    VirtualDir parent;
};

// Opens a mounted NX archive. A host path, when given, is read through a pinned read-only
// handle; otherwise reads go through the VFS file.
nx::Result<std::shared_ptr<nx::Session>> OpenNxSession(VirtualFile file, std::string_view host_path,
                                                       nx::SessionOptions options = {}) {
    using SessionResult = nx::Result<std::shared_ptr<nx::Session>>;
    if (options.parallel_reads == 0 || options.parallel_reads > 64) {
        return SessionResult::failure(nx::ErrorCode::invalid_argument,
                                      "invalid NX parallel-read bound");
    }
    if (file == nullptr) {
        return SessionResult::failure(nx::ErrorCode::invalid_argument, "no NX file");
    }
    try {
        std::shared_ptr<nx::RandomAccessSource> source;
        if (!host_path.empty() && !host_path.starts_with("content://")) {
            auto pinned =
                nx::DiskSource::open(std::filesystem::path(Common::FS::ToU8String(host_path)));
            if (!pinned) {
                return SessionResult::failure(pinned.error.code, pinned.error.message);
            }
            source = std::move(*pinned.value);
        } else {
            source = std::make_shared<VfsSource>(std::move(file));
        }
        auto codec = std::make_shared<CodecSource>(std::move(source), options.parallel_reads);
        options.reader.zstd = &Decode;
        options.reader.zstd_context = &codec->decoder;
        return nx::Session::open(std::move(codec), options);
    } catch (const std::bad_alloc&) {
        return SessionResult::failure(nx::ErrorCode::resource_limit, "NX source allocation failed");
    }
}

// The header and Catalog only: no Manifest mount and no prefetch thread, enough to list a game.
struct NxCatalog {
    std::shared_ptr<nx::RandomAccessSource> source;
    nx::Archive archive;
};

std::optional<NxCatalog> OpenNxCatalog(VirtualFile file) {
    if (file == nullptr) {
        return std::nullopt;
    }
    NxCatalog catalog{std::make_shared<VfsSource>(std::move(file)), {}};
    auto opened = nx::Archive::open(*catalog.source);
    if (!opened) {
        return std::nullopt;
    }
    catalog.archive = std::move(*opened.value);
    return catalog;
}

// Read-only view of one snapshot. Every file and directory keeps the session alive.
VirtualDir NxSnapshotDirectory(std::shared_ptr<nx::Session> session, u32 snapshot) {
    auto root = session->archive().root_directory(snapshot);
    if (!root) {
        return nullptr;
    }
    return std::make_shared<NxDirectory>(std::move(session), *root.value, "nx");
}

} // namespace

} // namespace FileSys

namespace Loader {

namespace {

constexpr u32 Absent = 0xffffffffu;

// A standalone update (as added from an update folder) depends on no base snapshot.
enum SnapshotType : u8 { Base = 0, Update = 1, AddOn = 2, StandaloneUpdate = 3 };

bool IsUpdate(u8 type) {
    return type == Update || type == StandaloneUpdate;
}

// The Catalog language tag for the configured system language.
std::string_view PreferredLanguageTag() {
    switch (Settings::values.language_index.GetValue()) {
    case Settings::Language::EnglishBritish:
        return "en-gb";
    case Settings::Language::Japanese:
        return "ja";
    case Settings::Language::French:
        return "fr";
    case Settings::Language::FrenchCanadian:
        return "fr-ca";
    case Settings::Language::German:
        return "de";
    case Settings::Language::Spanish:
        return "es";
    case Settings::Language::SpanishLatin:
        return "es-419";
    case Settings::Language::Italian:
        return "it";
    case Settings::Language::Dutch:
        return "nl";
    case Settings::Language::Portuguese:
        return "pt";
    case Settings::Language::PortugueseBrazilian:
        return "pt-br";
    case Settings::Language::Russian:
        return "ru";
    case Settings::Language::Korean:
        return "ko";
    case Settings::Language::Chinese:
    case Settings::Language::ChineseSimplified:
        return "zh-hans";
    case Settings::Language::ChineseTraditional:
    case Settings::Language::Taiwanese:
        return "zh-hant";
    case Settings::Language::Polish:
        return "pl";
    case Settings::Language::Thai:
        return "th";
    default:
        return "en-us";
    }
}

bool UpdateDisabled(u64 program_id, u32 version) {
    const auto it = Settings::values.disabled_addons.find(program_id);
    if (it == Settings::values.disabled_addons.end()) {
        return false;
    }
    const auto key = fmt::format("Update@{}", version);
    return std::find(it->second.begin(), it->second.end(), "Update") != it->second.end() ||
           std::find(it->second.begin(), it->second.end(), key) != it->second.end();
}

std::string HostPath(const FileSys::VirtualFile& file) {
    if (file == nullptr) {
        return {};
    }
    auto path = file->GetFullPath();
    if (path.starts_with("content://")) {
        return path;
    }
    return Common::FS::IsFile(Common::FS::ToU8String(path)) ? path : std::string{};
}

void CopyText(std::span<char> out, std::string_view text) {
    const auto length = std::min(text.size(), out.size() - 1);
    text.copy(out.data(), length);
    out[length] = '\0';
}

} // namespace

struct AppLoader_NX::Impl {
    Core::System* system = nullptr;
    ResultStatus status = ResultStatus::ErrorBadPFSHeader;
    std::shared_ptr<nx::Session> session;
    u32 app = Absent;
    u32 selected = Absent;
    u64 program_id = 0;
    std::vector<u64> program_ids;

    // An extracted-tree snapshot (exefs/ and romfs/ at its root).
    bool tree = false;
    FileSys::VirtualDir tree_romfs_dir;
    FileSys::VirtualFile tree_romfs;
    std::once_flag tree_romfs_once;
    std::unique_ptr<AppLoader_DeconstructedRomDirectory> tree_executable;
    std::unique_ptr<FileSys::NACP> nacp;

    // Native NCA content: the base snapshot seen as an NSP; updates and add-ons are given to
    // the content provider, as separate update files would be.
    std::unique_ptr<AppLoader_NSP> native;
    std::vector<FileSys::VirtualFile> native_extras;

    const nx::Archive& Archive() const {
        return session->archive();
    }

    bool IsTree(u32 si) const {
        static constexpr std::array<u8, 16> none{};
        const auto& s = Archive().snapshots()[si];
        if (s.content_id != none || (s.flags & 2) != 0 || s.type == AddOn) {
            return false;
        }
        const auto root = FileSys::NxSnapshotDirectory(session, si);
        return root != nullptr && root->GetSubdirectory("exefs") != nullptr;
    }

    // The snapshot the program starts from: its newest enabled update, else its base.
    u32 Latest(u32 pi) const {
        u32 found = Absent;
        const auto id = Archive().programs()[pi].id;
        const auto& snapshots = Archive().snapshots();
        for (u32 i = 0; i < snapshots.size(); ++i) {
            const auto& s = snapshots[i];
            if (s.program_index != pi || s.type == AddOn) {
                continue;
            }
            if (IsUpdate(s.type) && UpdateDisabled(id, s.numeric_version)) {
                continue;
            }
            if (found == Absent || s.numeric_version > snapshots[found].numeric_version ||
                (s.numeric_version == snapshots[found].numeric_version &&
                 s.type > snapshots[found].type)) {
                found = i;
            }
        }
        return found;
    }

    // The base snapshot an update depends on.
    u32 BaseOf(u32 si) const {
        if (Archive().snapshots()[si].type == Base) {
            return si;
        }
        u32 result = Absent;
        Archive().dependencies(si, [&](const nx::Dependency& d) {
            if (d.kind != 1) {
                return true;
            }
            const auto& snapshots = Archive().snapshots();
            for (u32 i = 0; i < snapshots.size(); ++i) {
                if (snapshots[i].identity == d.target_identity) {
                    result = i;
                }
            }
            return true;
        });
        return result;
    }

    // The display version of a snapshot. An update added from a bare folder records none:
    // its version number is shown instead.
    std::string DisplayVersion(u32 si) const {
        std::string version{Archive().snapshot_display_version(si)};
        const auto& s = Archive().snapshots()[si];
        if (version.empty() && IsUpdate(s.type)) {
            version = fmt::format("v{}", s.numeric_version);
        }
        return version;
    }

    void LoadNacp(u32 si) {
        for (const u32 index :
             {Archive().snapshots()[si].nacp_index, Archive().applications()[app].nacp_index}) {
            if (index == Absent) {
                continue;
            }
            auto bytes = session->media(index);
            if (!bytes || bytes.value->size() < sizeof(FileSys::RawNACP)) {
                continue;
            }
            nacp = std::make_unique<FileSys::NACP>(
                std::make_shared<FileSys::VectorVfsFile>(std::move(*bytes.value), "control.nacp"));
            return;
        }
        // No native NACP: describe the title from the Catalog, as the NUP loader does.
        nacp = std::make_unique<FileSys::NACP>();
        auto& raw = nacp->raw;
        CopyText(raw.language_entries.language_entries[0].application_name, Archive().title(app));
        CopyText(raw.language_entries.language_entries[0].developer_name, Archive().publisher(app));
        CopyText(raw.version_string, DisplayVersion(si));
        raw.save_data_owner_id = Archive().applications()[app].id;
        nacp->language_entries.assign(raw.language_entries.language_entries.begin(),
                                      raw.language_entries.language_entries.end());
    }

    void OpenTree(u32 pi) {
        const auto& s = Archive().snapshots()[selected];
        const auto root = FileSys::NxSnapshotDirectory(session, selected);
        const auto exefs = root != nullptr ? root->GetSubdirectory("exefs") : nullptr;
        if (exefs == nullptr) {
            return;
        }
        program_id = Archive().programs()[pi].id;

        const auto main = exefs->GetFile("main");
        std::array<u8, 96> nso{};
        if (main == nullptr || main->Read(nso.data(), nso.size()) != nso.size() ||
            std::memcmp(nso.data(), "NSO0", 4) != 0) {
            status = ResultStatus::ErrorLoadingNSO;
            return;
        }
        static constexpr std::array<u8, 32> no_build_id{};
        if (s.build_id != no_build_id &&
            !std::equal(s.build_id.begin(), s.build_id.end(), nso.begin() + 64)) {
            status = ResultStatus::ErrorLoadingNSO;
            return;
        }
        FileSys::ProgramMetadata metadata;
        const auto npdm = exefs->GetFile("main.npdm");
        if (npdm == nullptr || metadata.Load(npdm) != ResultStatus::Success ||
            metadata.GetTitleID() != program_id) {
            status = ResultStatus::ErrorBadNPDMHeader;
            return;
        }
        // Built on first use: listing the game needs only its metadata.
        tree_romfs_dir = root->GetSubdirectory("romfs");
        LoadNacp(selected);
        tree_executable = std::make_unique<AppLoader_DeconstructedRomDirectory>(exefs, true);
        tree = true;
        status = ResultStatus::Success;
    }

    FileSys::VirtualFile SnapshotPackage(u32 si) const {
        const auto root = FileSys::NxSnapshotDirectory(session, si);
        if (root == nullptr) {
            return nullptr;
        }
        // Native snapshots hold decrypted NCAs: their tickets and certificates are left out so
        // no title key is imported.
        auto files = root->GetFiles();
        std::erase_if(files, [](const FileSys::VirtualFile& entry) {
            const auto name = entry->GetName();
            return name.ends_with(".tik") || name.ends_with(".cert");
        });
        return FileSys::BuildPartitionFile(fmt::format("snapshot{}.nsp", si), std::move(files));
    }

    void OpenNative(u32 pi, std::size_t program_index) {
        const auto base = BaseOf(selected);
        if (base == Absent) {
            status = ResultStatus::ErrorNSPMissingProgramNCA;
            return;
        }
        program_id = Archive().programs()[pi].id;
        auto package = SnapshotPackage(base);
        if (package == nullptr) {
            return;
        }
        // RomFS updates also need the base NCA in the content provider.
        native_extras.push_back(package);
        native = std::make_unique<AppLoader_NSP>(
            std::move(package), system->GetFileSystemController(), system->GetContentProvider(),
            program_id, program_index);
        const auto& owner = Archive().applications()[app];
        for (u32 si = owner.first_snapshot; si < owner.first_snapshot + owner.snapshot_count;
             ++si) {
            const auto type = Archive().snapshots()[si].type;
            if ((type == Update || type == AddOn) && !Archive().snapshots()[si].addon_title_id()) {
                if (auto extra = SnapshotPackage(si)) {
                    native_extras.push_back(std::move(extra));
                }
            }
        }
        LoadNacp(selected);
        status = ResultStatus::Success;
    }

    void Open(FileSys::VirtualFile nx_file, u64 requested, std::size_t program_index) {
        auto opened = FileSys::OpenNxSession(nx_file, HostPath(nx_file));
        if (!opened) {
            LOG_ERROR(Loader, "Cannot open NX archive {}: {}", nx_file->GetName(),
                      opened.error.message);
            return;
        }
        session = std::move(*opened.value);
        const auto& a = Archive();
        for (u32 i = 0; i < a.applications().size(); ++i) {
            const auto& owner = a.applications()[i];
            if (owner.platform != 1 && owner.platform != 2) {
                continue;
            }
            bool matches = requested == 0 || owner.id == requested;
            for (u32 p = owner.first_program;
                 p != Absent && p < owner.first_program + owner.program_count; ++p) {
                matches |= a.programs()[p].id == requested;
            }
            if (matches && app == Absent) {
                app = i;
            }
        }
        if (app == Absent) {
            status = ResultStatus::ErrorNSPMissingProgramNCA;
            return;
        }
        const auto& owner = a.applications()[app];
        u32 pi = Absent;
        for (u32 p = owner.first_program;
             p != Absent && p < owner.first_program + owner.program_count; ++p) {
            program_ids.push_back(a.programs()[p].id);
            const bool wanted = requested != 0 && requested != owner.id
                                    ? a.programs()[p].id == requested
                                    : a.programs()[p].kind == 0;
            if (wanted && pi == Absent) {
                pi = p;
            }
        }
        if (pi == Absent) {
            status = ResultStatus::ErrorNSPMissingProgramNCA;
            return;
        }
        // A program with its own snapshots is selected here; a native snapshot holding every
        // program leaves the index to the NSP.
        if (program_index != 0) {
            const u64 target = a.programs()[pi].id + program_index;
            for (u32 p = owner.first_program;
                 p != Absent && p < owner.first_program + owner.program_count; ++p) {
                if (a.programs()[p].id == target && Latest(p) != Absent) {
                    pi = p;
                    program_index = 0;
                }
            }
        }
        selected = Latest(pi);
        if (selected == Absent) {
            status = ResultStatus::ErrorNSPMissingProgramNCA;
            return;
        }
        if (IsTree(selected)) {
            if (program_index != 0) {
                status = ResultStatus::ErrorNSPMissingProgramNCA;
                return;
            }
            OpenTree(pi);
        } else {
            OpenNative(pi, program_index);
        }
    }
};

AppLoader_NX::AppLoader_NX(Core::System& system, FileSys::VirtualFile file_, u64 program_id,
                           std::size_t program_index)
    : AppLoader(file_), impl{std::make_unique<Impl>()} {
    impl->system = &system;
    impl->Open(std::move(file_), program_id, program_index);
}

AppLoader_NX::~AppLoader_NX() = default;

FileType AppLoader_NX::IdentifyType(const FileSys::VirtualFile& nx_file) {
    // Two 4 KiB header slots; the reader recovers an archive whose first slot is damaged.
    constexpr std::size_t PageSize = 4096;
    if (nx_file == nullptr || nx_file->GetSize() < 2 * PageSize) {
        return FileType::Error;
    }
    for (const std::size_t page : {std::size_t{0}, PageSize}) {
        std::array<u8, 4> magic{};
        if (nx_file->Read(magic.data(), magic.size(), page) == magic.size() &&
            std::memcmp(magic.data(), "NXAR", 4) == 0) {
            return FileType::NX;
        }
    }
    return FileType::Error;
}

bool AppLoader_NX::IsBootable(const FileSys::VirtualFile& nx_file) {
    if (IdentifyType(nx_file) != FileType::NX) {
        return false;
    }
    const auto catalog = FileSys::OpenNxCatalog(nx_file);
    if (!catalog) {
        return false;
    }
    const auto& a = catalog->archive;
    return std::any_of(a.snapshots().begin(), a.snapshots().end(), [&](const nx::Snapshot& s) {
        return s.type != AddOn && s.program_index < a.programs().size() &&
               a.programs()[s.program_index].kind == 0 &&
               (a.applications()[a.programs()[s.program_index].application_index].platform == 1 ||
                a.applications()[a.programs()[s.program_index].application_index].platform == 2);
    });
}

u8 AppLoader_NX::ReadPlatform(const FileSys::VirtualFile& nx_file, u64 program_id) {
    if (IdentifyType(nx_file) != FileType::NX) {
        return 0;
    }
    const auto catalog = FileSys::OpenNxCatalog(nx_file);
    if (!catalog) {
        return 0;
    }
    const auto& a = catalog->archive;
    for (const auto& program : a.programs()) {
        const auto& owner = a.applications()[program.application_index];
        if (program_id == 0 || program.id == program_id || owner.id == program_id) {
            return owner.platform;
        }
    }
    return 0;
}

void AppLoader_NX::RegisterContent(FileSys::ManualContentProvider& provider) const {
    if (impl->status != ResultStatus::Success) {
        return;
    }
    for (const auto& extra : impl->native_extras) {
        provider.AddEntriesFromContainer(extra, false, impl->program_id);
    }
    // Add-ons converted from NUP packages and extracted dumps have no NCA: their RomFS is
    // served as one.
    const auto& owner = impl->Archive().applications()[impl->app];
    for (u32 si = owner.first_snapshot; si < owner.first_snapshot + owner.snapshot_count; ++si) {
        const auto title_id = impl->Archive().snapshots()[si].addon_title_id();
        if (!title_id) {
            continue;
        }
        const auto root = FileSys::NxSnapshotDirectory(impl->session, si);
        const auto romfs = root != nullptr ? root->GetSubdirectory("romfs") : nullptr;
        if (romfs == nullptr) {
            LOG_WARNING(Loader, "NX add-on {:016X} has no romfs folder", *title_id);
            continue;
        }
        provider.AddEntry(FileSys::TitleType::AOC, FileSys::ContentRecordType::Data, *title_id,
                          FileSys::BuildAddOnContentNca(*title_id, FileSys::CreateRomFS(romfs)));
    }
}

AppLoader::LoadResult AppLoader_NX::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }
    if (impl->status != ResultStatus::Success) {
        return {impl->status, {}};
    }
    // The frontend owns this provider; the game list fills it the same way.
    if (const auto* slot = system.GetContentProviderUnion().GetSlotProvider(
            FileSys::ContentProviderUnionSlot::FrontendManual)) {
        RegisterContent(*const_cast<FileSys::ManualContentProvider*>(
            static_cast<const FileSys::ManualContentProvider*>(slot)));
    }
    if (!impl->tree) {
        const auto result = impl->native->Load(process, system);
        is_loaded = result.first == ResultStatus::Success;
        return result;
    }
    const auto result = impl->tree_executable->Load(process, system);
    if (result.first != ResultStatus::Success) {
        return result;
    }
    system.GetFileSystemController().RegisterProcess(
        process.GetProcessId(), impl->program_id,
        std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                system.GetFileSystemController()));
    is_loaded = true;
    return result;
}

ResultStatus AppLoader_NX::VerifyIntegrity(std::function<bool(size_t, size_t)> progress_callback) {
    if (impl->session == nullptr) {
        return impl->status;
    }
    if (progress_callback && !progress_callback(0, 1)) {
        return ResultStatus::ErrorIntegrityVerificationFailed;
    }
    if (!impl->session->verify_all()) {
        return ResultStatus::ErrorIntegrityVerificationFailed;
    }
    if (progress_callback) {
        progress_callback(1, 1);
    }
    return ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadRomFS(FileSys::VirtualFile& out_file) {
    if (impl->status != ResultStatus::Success) {
        return impl->status;
    }
    if (!impl->tree) {
        return impl->native->ReadRomFS(out_file);
    }
    std::call_once(impl->tree_romfs_once, [this] {
        if (impl->tree_romfs_dir != nullptr) {
            impl->tree_romfs = FileSys::CreateRomFS(impl->tree_romfs_dir);
        }
    });
    out_file = impl->tree_romfs;
    return out_file != nullptr ? ResultStatus::Success : ResultStatus::ErrorNoRomFS;
}

ResultStatus AppLoader_NX::ReadPatchedRomFS(FileSys::VirtualFile& out_file) {
    if (impl->status != ResultStatus::Success || impl->tree) {
        return ReadRomFS(out_file);
    }
    const auto package = impl->SnapshotPackage(impl->BaseOf(impl->selected));
    if (package == nullptr) {
        return ResultStatus::ErrorNSPMissingProgramNCA;
    }
    const FileSys::NSP nsp{package, impl->program_id};
    const auto base_nca = nsp.GetNCA(impl->program_id, FileSys::ContentRecordType::Program);
    if (base_nca == nullptr || base_nca->GetRomFS() == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    // The updates of the archive are found through the frontend provider, as when it runs.
    auto& system = *impl->system;
    if (const auto* slot = system.GetContentProviderUnion().GetSlotProvider(
            FileSys::ContentProviderUnionSlot::FrontendManual)) {
        RegisterContent(*const_cast<FileSys::ManualContentProvider*>(
            static_cast<const FileSys::ManualContentProvider*>(slot)));
    }
    const FileSys::PatchManager pm{impl->program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    out_file = pm.PatchRomFS(base_nca.get(), base_nca->GetRomFS(),
                             FileSys::ContentRecordType::Program, nullptr, false);
    return out_file != nullptr ? ResultStatus::Success : ResultStatus::ErrorNoRomFS;
}

ResultStatus AppLoader_NX::ReadUpdateRaw(FileSys::VirtualFile& out_file) {
    if (impl->status != ResultStatus::Success || impl->tree) {
        return ResultStatus::ErrorNoPackedUpdate;
    }
    return impl->native->ReadUpdateRaw(out_file);
}

bool AppLoader_NX::IsRomFSUpdatable() const {
    // A tree snapshot is already the complete updated view.
    return !impl->tree;
}

ResultStatus AppLoader_NX::ReadProgramId(u64& out_program_id) {
    if (impl->status != ResultStatus::Success) {
        return impl->status;
    }
    out_program_id = impl->program_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadProgramIds(std::vector<u64>& out_program_ids) {
    if (impl->status != ResultStatus::Success) {
        return impl->status;
    }
    out_program_ids.insert(out_program_ids.end(), impl->program_ids.begin(),
                           impl->program_ids.end());
    return ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadIcon(std::vector<u8>& buffer) {
    if (impl->status != ResultStatus::Success) {
        return impl->status;
    }
    const auto& a = impl->Archive();
    const auto& owner = a.applications()[impl->app];
    u32 icon = Absent;
    for (const auto tag : {PreferredLanguageTag(), std::string_view{"en-us"}}) {
        for (u32 i = owner.first_localized; i < owner.first_localized + owner.localized_count;
             ++i) {
            if (icon == Absent && a.localized_language(i) == tag) {
                icon = a.localized_entries()[i].icon_index;
            }
        }
    }
    if (icon == Absent) {
        icon = owner.icon_index;
    }
    if (icon == Absent) {
        return ResultStatus::ErrorNoIcon;
    }
    auto bytes = impl->session->media(icon);
    if (!bytes) {
        return ResultStatus::ErrorNoIcon;
    }
    buffer = std::move(*bytes.value);
    return ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadOriginalIcon(u64 application_id, std::vector<u8>& buffer) const {
    if (impl->native == nullptr || impl->Archive().applications()[impl->app].id != application_id) {
        return ResultStatus::ErrorNoIcon;
    }
    return impl->native->ReadOriginalIcon(buffer);
}

ResultStatus AppLoader_NX::ReadTitle(std::string& title) {
    if (impl->status != ResultStatus::Success) {
        return impl->status;
    }
    const auto& a = impl->Archive();
    const auto& owner = a.applications()[impl->app];
    for (u32 i = owner.first_localized; i < owner.first_localized + owner.localized_count; ++i) {
        if (a.localized_language(i) == PreferredLanguageTag() && !a.localized_title(i).empty()) {
            title = a.localized_title(i);
            return ResultStatus::Success;
        }
    }
    title = a.title(impl->app);
    return title.empty() ? ResultStatus::ErrorNoControl : ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadPublisher(std::string& publisher) const {
    if (impl->app == Absent) {
        return ResultStatus::ErrorNoControl;
    }
    publisher = impl->Archive().publisher(impl->app);
    return ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadControlData(FileSys::NACP& control) {
    if (impl->status != ResultStatus::Success) {
        return impl->status;
    }
    if (impl->nacp == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    control = *impl->nacp;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadDisplayVersion(std::string& version) {
    if (impl->status != ResultStatus::Success) {
        return impl->status;
    }
    version = impl->DisplayVersion(impl->selected);
    return version.empty() ? AppLoader::ReadDisplayVersion(version) : ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadNSOModules(Modules& modules) {
    if (impl->status != ResultStatus::Success) {
        return impl->status;
    }
    return impl->tree ? impl->tree_executable->ReadNSOModules(modules)
                      : impl->native->ReadNSOModules(modules);
}

} // namespace Loader

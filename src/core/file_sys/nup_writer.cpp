// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/nup_writer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <zstd.h>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/deconstructed_rom_directory.h"

#include <fmt/format.h>

namespace FileSys::Nup {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr std::array<u8, 4> Magic{'N', 'U', 'P', 0x01};
constexpr std::array<u8, 4> TocMagic{'T', 'O', 'C', 0x01};
constexpr u64 SuperblockSize = 0x80;
constexpr u64 TocHeaderSize = 0x30;
constexpr u64 PackageRecordSize = 0x40;
constexpr u64 DirRecordSize = 0x18;
constexpr u64 FileRecordSize = 0x50;
constexpr u64 ExtentAlignment = 16;
constexpr u32 NoDir = 0xFFFFFFFF;

constexpr u8 KindBase = 0;
constexpr u8 KindUpdate = 1;
constexpr u8 KindAddon = 2;
constexpr u8 FlagStandaloneUpdate = 0x01;
constexpr u8 CodecRaw = 0;
constexpr u8 CodecZstd = 1;

constexpr u16 TlvNpdm = 0x0001;
constexpr u16 TlvNacp = 0x0002;
constexpr u16 TlvIcon = 0x0003;
constexpr u16 TlvDisplayVersion = 0x0004;
constexpr u16 TlvTitleName = 0x0005;
constexpr u16 TlvPublisher = 0x0006;
constexpr u16 TlvSourceDump = 0x0008;
constexpr u16 TlvLocalizedTitles = 0x0009;

constexpr int JournalVersion = 1;
constexpr u64 ProgressStep = 64ULL * 1024 * 1024;
constexpr size_t MaxChunkFiles = 4096;
constexpr size_t ReadBufferSize = 4 * 1024 * 1024;

class PackError : public std::runtime_error {
public:
    PackError(PackResult result_, const std::string& message)
        : std::runtime_error(message), result{result_} {}
    PackResult result;
};

u64 AlignUp(u64 value, u64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

std::string ToUtf8(const fs::path& path) {
    const auto u8 = path.generic_u8string();
    return {u8.begin(), u8.end()};
}

fs::path FromUtf8(const std::string& value) {
    return fs::path{std::u8string{value.begin(), value.end()}};
}

std::string Lower(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

fs::path WithSuffix(fs::path path, std::string_view suffix) {
    path += suffix;
    return path;
}

bool IsFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

fs::path FindDirectory(const fs::path& parent, std::string_view name) {
    std::error_code ec;
    if (parent.empty() || !fs::is_directory(parent, ec)) {
        return {};
    }
    const auto wanted = Lower(std::string{name});
    for (const auto& entry : fs::directory_iterator(parent, ec)) {
        if (entry.is_directory(ec) && Lower(ToUtf8(entry.path().filename())) == wanted) {
            return entry.path();
        }
    }
    return {};
}

std::optional<u64> ParseDecimal(const std::string& name) {
    u64 value{};
    const auto* end = name.data() + name.size();
    const auto [ptr, ec] = std::from_chars(name.data(), end, value);
    if (name.empty() || ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

// The exefs folder a dump boots from: the newest updates/<version>, else base, else the root.
// Each must hold main, main.npdm and a romfs folder next to it or under base.
bool HasBootableExeFS(const fs::path& root) {
    const auto bootable = [](const fs::path& exefs) {
        return !exefs.empty() && IsFile(exefs / "main") && IsFile(exefs / "main.npdm");
    };
    fs::path exefs;
    std::optional<u64> best_version;
    if (const auto updates = FindDirectory(root, "updates"); !updates.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(updates, ec)) {
            const auto version = ParseDecimal(ToUtf8(entry.path().filename()));
            const auto candidate = FindDirectory(entry.path(), "exefs");
            if (version && bootable(candidate) && (!best_version || *version > *best_version)) {
                best_version = version;
                exefs = candidate;
            }
        }
    }
    const auto base = FindDirectory(root, "base");
    if (exefs.empty()) {
        exefs = FindDirectory(base, "exefs");
    }
    if (exefs.empty()) {
        exefs = FindDirectory(root, "exefs");
    }
    if (!bootable(exefs)) {
        return false;
    }
    return !FindDirectory(exefs.parent_path(), "romfs").empty() ||
           !FindDirectory(base, "romfs").empty();
}

std::optional<u64> ParseTitleId(const std::string& name) {
    u64 value{};
    const auto* end = name.data() + name.size();
    const auto [ptr, ec] = std::from_chars(name.data(), end, value, 16);
    if (name.size() != 16 || ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::string ToHex(std::span<const u8> data) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const u8 byte : data) {
        out += digits[byte >> 4];
        out += digits[byte & 0xF];
    }
    return out;
}

std::vector<u8> FromHex(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        throw PackError(PackResult::IoError, "malformed journal");
    }
    std::vector<u8> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const auto [ptr, ec] = std::from_chars(hex.data() + i * 2, hex.data() + i * 2 + 2, out[i], 16);
        if (ec != std::errc{} || ptr != hex.data() + i * 2 + 2) {
            throw PackError(PackResult::IoError, "malformed journal");
        }
    }
    return out;
}

std::vector<u8> ReadWholeFile(const fs::path& path) {
    Common::FS::IOFile in{path, Common::FS::FileAccessMode::Read};
    if (!in.IsOpen()) {
        return {};
    }
    std::vector<u8> data(in.GetSize());
    if (in.ReadSpan(std::span{data}) != data.size()) {
        return {};
    }
    return data;
}

// Same as pack.py: the program ID at ACI0 + 0x10, whatever the NPDM header revision.
std::optional<u64> NpdmProgramId(std::span<const u8> npdm) {
    static constexpr std::array<u8, 4> aci0{'A', 'C', 'I', '0'};
    const auto it = std::search(npdm.begin(), npdm.end(), aci0.begin(), aci0.end());
    const auto offset = static_cast<size_t>(it - npdm.begin());
    if (it == npdm.end() || offset + 0x18 > npdm.size()) {
        return std::nullopt;
    }
    u64 id = 0;
    for (size_t i = 0; i < 8; ++i) {
        id |= static_cast<u64>(npdm[offset + 0x10 + i]) << (8 * i);
    }
    return id;
}

void PutLE(std::vector<u8>& buffer, size_t offset, u64 value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        buffer[offset + i] = static_cast<u8>(value >> (8 * i));
    }
}

void PutBytes(std::vector<u8>& buffer, size_t offset, std::span<const u8> bytes) {
    std::memcpy(buffer.data() + offset, bytes.data(), bytes.size());
}

using Digest = std::array<u8, 32>;

class Sha256 {
public:
    Sha256() : ctx{EVP_MD_CTX_new()} {
        if (!ctx) {
            throw PackError(PackResult::IoError, "cannot initialise SHA-256");
        }
        Reset();
    }
    ~Sha256() {
        EVP_MD_CTX_free(ctx);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void Reset() {
        if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
            throw PackError(PackResult::IoError, "cannot initialise SHA-256");
        }
    }

    void Update(const void* data, size_t size) {
        EVP_DigestUpdate(ctx, data, size);
    }

    Digest Final() {
        Digest digest{};
        unsigned int length{};
        EVP_DigestFinal_ex(ctx, digest.data(), &length);
        return digest;
    }

private:
    EVP_MD_CTX* ctx;
};

class ZstdContext {
public:
    // Archives must be byte-identical across platforms, so compression is always
    // single-threaded (zstd's multithreaded frames differ) and every frame parameter is set
    // explicitly instead of relying on library defaults.
    explicit ZstdContext(int level) : cctx{ZSTD_createCCtx()} {
        if (!cctx || ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level)) ||
            ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 0)) ||
            ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 1)) ||
            ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0)) ||
            ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_dictIDFlag, 0))) {
            throw PackError(PackResult::IoError, "cannot initialise zstd");
        }
    }
    ~ZstdContext() {
        ZSTD_freeCCtx(cctx);
    }
    ZstdContext(const ZstdContext&) = delete;
    ZstdContext& operator=(const ZstdContext&) = delete;

    // The pledged size makes zstd record the content size in the frame header, which the
    // format requires.
    void Begin(u64 size) {
        if (ZSTD_isError(ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only)) ||
            ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(cctx, size))) {
            throw PackError(PackResult::IoError, "cannot start zstd frame");
        }
    }

    ZSTD_CCtx* get() const {
        return cctx;
    }

private:
    ZSTD_CCtx* cctx;
};

// One package and the metadata it is written with. The plan is everything that must survive a
// low-storage run deleting its sources, so it is what the journal stores.
struct PlanPackage {
    u8 kind{};
    u8 flags{};
    u64 title_id{};
    u32 version{};
    std::vector<u8> meta;
};

struct PlanEntry {
    u32 package{};
    std::string path;   // inside the package tree, e.g. "romfs/Data/file.bin"
    std::string source; // relative to the dump root
    bool directory{};
};

struct Plan {
    std::vector<PlanPackage> packages;
    std::vector<PlanEntry> entries;
};

struct Extent {
    u64 size{};
    Digest sha256{};
    u64 offset{};
    u64 stored_size{};
    u8 codec{};
};

struct Layout {
    struct Dir {
        std::string name;
        u32 first_child{};
        u32 child_count{};
    };
    struct File {
        std::string name;
        size_t entry{};
    };
    struct Package {
        u32 root_dir{};
        u32 exefs_dir = NoDir;
        u32 romfs_dir = NoDir;
    };
    std::vector<Dir> dirs;
    std::vector<File> files;
    std::vector<u64> children; // FileTag | file index, or dir index
    std::vector<Package> packages;
    std::string pool;
    std::unordered_map<std::string, u32> pool_index;
    u64 toc_size{};
    u64 blob_offset{};

    static constexpr u64 FileTag = 1ULL << 32;

    u32 Intern(const std::string& name) {
        const auto [it, inserted] = pool_index.try_emplace(name, static_cast<u32>(pool.size()));
        if (inserted) {
            pool += name;
        }
        return it->second;
    }
};

// Children sorted by the bytes of their names: std::string compares as unsigned char.
struct TreeDir {
    struct Child {
        std::unique_ptr<TreeDir> dir;
        size_t file_entry{};
    };
    std::string name;
    std::map<std::string, Child> children;

    TreeDir* Descend(const std::string& component) {
        auto& child = children[component];
        if (!child.dir) {
            child.dir = std::make_unique<TreeDir>();
            child.dir->name = component;
        }
        return child.dir.get();
    }
};

// Lays out nodes as pack.py does: directories in pre-order, each directory's child slots
// contiguous, its files numbered in slot order before its subdirectories are visited.
u32 AssignDir(TreeDir& dir, Layout& layout) {
    const auto id = static_cast<u32>(layout.dirs.size());
    layout.dirs.push_back({dir.name, static_cast<u32>(layout.children.size()),
                           static_cast<u32>(dir.children.size())});
    const size_t slot_base = layout.children.size();
    layout.children.resize(slot_base + dir.children.size());
    size_t slot = slot_base;
    for (auto& [name, child] : dir.children) {
        if (!child.dir) {
            layout.children[slot] = Layout::FileTag | layout.files.size();
            layout.files.push_back({name, child.file_entry});
        }
        ++slot;
    }
    slot = slot_base;
    for (auto& [name, child] : dir.children) {
        if (child.dir) {
            layout.children[slot] = AssignDir(*child.dir, layout);
        }
        ++slot;
    }
    return id;
}

Layout BuildLayout(const Plan& plan) {
    std::vector<TreeDir> roots(plan.packages.size());
    for (size_t i = 0; i < plan.entries.size(); ++i) {
        const auto& entry = plan.entries[i];
        TreeDir* dir = &roots.at(entry.package);
        size_t start = 0;
        while (true) {
            const auto slash = entry.path.find('/', start);
            const auto component = entry.path.substr(start, slash - start);
            if (slash == std::string::npos) {
                if (entry.directory) {
                    dir->Descend(component);
                } else {
                    auto& child = dir->children[component];
                    if (child.dir) {
                        throw PackError(PackResult::InvalidSource,
                                        "a file and a folder share the name " + entry.path);
                    }
                    child.file_entry = i;
                }
                break;
            }
            dir = dir->Descend(component);
            start = slash + 1;
        }
    }

    Layout layout;
    for (auto& root : roots) {
        Layout::Package package;
        package.root_dir = AssignDir(root, layout);
        layout.packages.push_back(package);
    }

    std::vector<u64> file_counts(layout.dirs.size(), 0);
    for (std::size_t i = layout.dirs.size(); i-- > 0;) {
        u64 total = 0;
        const auto first = layout.dirs[i].first_child;
        const auto count = layout.dirs[i].child_count;
        for (u32 slot = first; slot < first + count; ++slot) {
            const u64 child = layout.children[slot];
            if (child & Layout::FileTag) {
                total += 1;
            } else {
                total += file_counts[static_cast<std::size_t>(child)];
            }
        }
        file_counts[i] = total;
    }

    for (std::size_t i = 0; i < layout.packages.size(); ++i) {
        const auto& root_dir = layout.dirs[layout.packages[i].root_dir];
        auto& package = layout.packages[i];
        for (u32 slot = root_dir.first_child; slot < root_dir.first_child + root_dir.child_count;
             ++slot) {
            const u64 child = layout.children[slot];
            if (child & Layout::FileTag) {
                continue;
            }
            const auto id = static_cast<u32>(child);
            if (layout.dirs[id].name == "exefs" && file_counts[id] > 0) {
                package.exefs_dir = id;
            } else if (layout.dirs[id].name == "romfs" && file_counts[id] > 0) {
                package.romfs_dir = id;
            }
        }
    }

    if (layout.dirs.size() + layout.files.size() >= NoDir) {
        throw PackError(PackResult::InvalidSource, "the dump has too many files");
    }
    for (const auto& dir : layout.dirs) {
        layout.Intern(dir.name);
    }
    for (const auto& file : layout.files) {
        layout.Intern(file.name);
    }
    if (layout.pool.size() > std::numeric_limits<u32>::max()) {
        throw PackError(PackResult::InvalidSource, "the dump's file names are too long");
    }

    const u64 unpadded = TocHeaderSize + plan.packages.size() * PackageRecordSize +
                         layout.dirs.size() * DirRecordSize + layout.files.size() * FileRecordSize +
                         layout.children.size() * sizeof(u32) + layout.pool.size();
    layout.toc_size = AlignUp(unpadded, ExtentAlignment);
    layout.blob_offset = AlignUp(SuperblockSize + layout.toc_size, ExtentAlignment);
    return layout;
}

std::vector<u8> SerializeTlvs(std::vector<std::pair<u16, std::vector<u8>>> records) {
    std::ranges::stable_sort(records, {}, &std::pair<u16, std::vector<u8>>::first);
    std::vector<u8> out;
    for (const auto& [type, payload] : records) {
        const size_t at = out.size();
        out.resize(at + 8 + payload.size());
        PutLE(out, at, type, 2);
        PutLE(out, at + 4, payload.size(), 4);
        PutBytes(out, at + 8, payload);
    }
    return out;
}

std::vector<u8> StringBytes(const std::string& value) {
    return {value.begin(), value.end()};
}

// The dump's own loader supplies control data and the display version, the same way a loose
// dump reports them when it boots (control.nacp beside it, else the RomFS's version tag).
std::unique_ptr<Loader::AppLoader_DeconstructedRomDirectory> OpenLoader(
    RealVfsFilesystem& vfs, const fs::path& package_dir) {
    const auto exefs = FindDirectory(package_dir, "exefs");
    if (exefs.empty() || !IsFile(exefs / "main")) {
        return nullptr;
    }
    auto main = vfs.OpenFile(Common::FS::PathToUTF8String(exefs / "main"), OpenMode::Read);
    if (!main) {
        return nullptr;
    }
    return std::make_unique<Loader::AppLoader_DeconstructedRomDirectory>(std::move(main));
}

class Discovery {
public:
    Discovery(const fs::path& dump_root_, std::vector<fs::path> excluded_,
              const Metadata& overrides_)
        : dump_root{dump_root_}, excluded{std::move(excluded_)}, overrides{overrides_} {}

    Plan Run() {
        const auto base = FindDirectory(dump_root, "base");
        const auto updates = FindDirectory(dump_root, "updates");
        const auto dlc = FindDirectory(dump_root, "dlc");

        std::optional<u64> base_title_id;
        if (!base.empty() && HasContent(base)) {
            base_title_id = AddPackage(KindBase, base, 0, {});
        } else if (updates.empty() && HasContent(dump_root)) {
            base_title_id = AddPackage(KindBase, dump_root, 0, {"base", "updates", "dlc"});
        }

        std::vector<std::pair<u64, fs::path>> versions;
        std::error_code ec;
        if (!updates.empty()) {
            for (const auto& entry : fs::directory_iterator(updates, ec)) {
                const auto version = ParseDecimal(ToUtf8(entry.path().filename()));
                if (version && entry.is_directory(ec) && HasContent(entry.path())) {
                    if (*version > std::numeric_limits<u32>::max()) {
                        throw PackError(PackResult::InvalidSource,
                                        "update version out of range: " + ToUtf8(entry.path()));
                    }
                    versions.emplace_back(*version, entry.path());
                }
            }
        }
        std::ranges::sort(versions);
        const u8 update_flags = base_title_id ? 0 : FlagStandaloneUpdate;
        for (const auto& [version, path] : versions) {
            AddPackage(KindUpdate, path, static_cast<u32>(version), {}, update_flags,
                       base_title_id.value_or(0));
        }

        if (!dlc.empty()) {
            std::vector<std::pair<std::string, fs::path>> addons;
            for (const auto& title : fs::directory_iterator(dlc, ec)) {
                const auto title_name = ToUtf8(title.path().filename());
                if (!ParseTitleId(title_name) || !title.is_directory(ec)) {
                    continue;
                }
                for (const auto& content : fs::directory_iterator(title.path(), ec)) {
                    if (content.is_directory(ec) && HasContent(content.path())) {
                        addons.emplace_back(title_name + "/" + ToUtf8(content.path().filename()),
                                            content.path());
                    }
                }
            }
            std::ranges::sort(addons);
            for (const auto& [name, path] : addons) {
                AddPackage(KindAddon, path, 0, {}, 0, *ParseTitleId(name.substr(0, 16)));
            }
        }

        if (plan.packages.empty()) {
            throw PackError(PackResult::InvalidSource, "no packages found in the dump");
        }
        AddApplicationMetadata();
        for (size_t i = 0; i < plan.packages.size(); ++i) {
            auto& tlvs = package_tlvs[i];
            tlvs.emplace_back(TlvSourceDump, StringBytes(ToUtf8(dump_root.filename())));
            plan.packages[i].meta = SerializeTlvs(std::move(tlvs));
        }
        return std::move(plan);
    }

private:
    static bool HasContent(const fs::path& dir) {
        return DirHasFiles(FindDirectory(dir, "exefs")) ||
               DirHasFiles(FindDirectory(dir, "romfs"));
    }

    static bool DirHasFiles(const fs::path& dir) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec)) {
                return true;
            }
        }
        return false;
    }

    // Returns the package's title ID: the NPDM program ID, else the folder's or base's.
    u64 AddPackage(u8 kind, const fs::path& dir, u32 version,
                   const std::vector<std::string>& skip, u8 flags = 0, u64 fallback_id = 0) {
        const auto index = static_cast<u32>(plan.packages.size());
        PlanPackage package{kind, flags, fallback_id, version, {}};
        auto& tlvs = package_tlvs.emplace_back();

        const auto exefs = FindDirectory(dir, "exefs");
        const auto romfs = FindDirectory(dir, "romfs");
        fs::path npdm_path = exefs.empty() ? fs::path{} : exefs / "main.npdm";
        if (!IsFile(npdm_path)) {
            npdm_path = dir / "main.npdm";
        }
        if (IsFile(npdm_path)) {
            auto npdm = ReadWholeFile(npdm_path);
            if (const auto id = NpdmProgramId(npdm); kind != KindAddon && id && *id != 0) {
                package.title_id = *id;
            }
            tlvs.emplace_back(TlvNpdm, std::move(npdm));
        }

        if (kind != KindAddon) {
            if (auto loader = OpenLoader(vfs, dir)) {
                std::string display_version;
                if (loader->ReadDisplayVersion(display_version) == Loader::ResultStatus::Success &&
                    !display_version.empty()) {
                    tlvs.emplace_back(TlvDisplayVersion, StringBytes(display_version));
                }
                loaders.emplace_back(index, std::move(loader));
            }
        }
        plan.packages.push_back(std::move(package));

        if (!exefs.empty() && DirHasFiles(exefs)) {
            CollectTree(index, exefs, "exefs");
        }
        if (!romfs.empty() && DirHasFiles(romfs)) {
            CollectTree(index, romfs, "romfs");
        }
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            const auto name = ToUtf8(entry.path().filename());
            const auto lower = Lower(name);
            if (entry.path() == exefs || entry.path() == romfs ||
                std::ranges::find(skip, lower) != skip.end()) {
                continue;
            }
            // Keep the dump's own layout for anything that is not exefs/romfs, the
            // same way the reference packer does: a root-level data/ folder stays
            // data/, and other extras keep their name instead of moving under data/.
            if (entry.is_directory(ec)) {
                CollectTree(index, entry.path(), name);
            } else {
                AddFile(index, entry, name);
            }
        }
        return plan.packages.back().title_id;
    }

    // Names, icon and control data belong to the base package, or the first one without a base.
    // Our dumps rarely carry a control folder; when one does, the base's own wins, then updates.
    void AddApplicationMetadata() {
        const auto app = static_cast<u32>(std::distance(
            plan.packages.begin(), std::ranges::find(plan.packages, KindBase, &PlanPackage::kind)));
        const u32 target = app < plan.packages.size() ? app : 0;
        std::ranges::stable_sort(loaders, [target](const auto& a, const auto& b) {
            return (a.first == target) > (b.first == target);
        });

        auto& tlvs = package_tlvs[target];
        bool have_control = false;
        bool have_icon = false;
        for (const auto& [index, loader] : loaders) {
            NACP control;
            if (!have_control &&
                loader->ReadControlData(control) == Loader::ResultStatus::Success) {
                have_control = true;
                tlvs.emplace_back(TlvNacp, control.GetRawBytes());
                if (const auto name = control.GetApplicationName(); !name.empty()) {
                    tlvs.emplace_back(TlvTitleName, StringBytes(name));
                }
                if (const auto publisher = control.GetDeveloperName(); !publisher.empty()) {
                    tlvs.emplace_back(TlvPublisher, StringBytes(publisher));
                }
            }
            std::vector<u8> icon;
            if (!have_icon && loader->ReadIcon(icon) == Loader::ResultStatus::Success &&
                !icon.empty()) {
                have_icon = true;
                tlvs.emplace_back(TlvIcon, std::move(icon));
            }
        }

        if (!overrides.title_name.empty()) {
            SetTlv(tlvs, TlvTitleName, StringBytes(overrides.title_name));
        }
        if (!overrides.publisher.empty()) {
            SetTlv(tlvs, TlvPublisher, StringBytes(overrides.publisher));
        }
        if (!overrides.icon.empty()) {
            SetTlv(tlvs, TlvIcon, overrides.icon);
        }
        if (!overrides.display_version.empty()) {
            // The loader reports the newest update's version when there is one.
            SetTlv(tlvs, TlvDisplayVersion, StringBytes(overrides.display_version));
            std::optional<u32> newest;
            for (u32 i = 0; i < plan.packages.size(); ++i) {
                if (plan.packages[i].kind == KindUpdate &&
                    (!newest || plan.packages[i].version >= plan.packages[*newest].version)) {
                    newest = i;
                }
            }
            if (newest && *newest != target) {
                SetTlv(package_tlvs[*newest], TlvDisplayVersion,
                       StringBytes(overrides.display_version));
            }
        }
    }

    static void SetTlv(std::vector<std::pair<u16, std::vector<u8>>>& tlvs, u16 type,
                       std::vector<u8> payload) {
        std::erase_if(tlvs, [type](const auto& tlv) { return tlv.first == type; });
        tlvs.emplace_back(type, std::move(payload));
    }

    bool IsExcluded(const fs::path& path) const {
        const auto normal = fs::absolute(path).lexically_normal();
        return std::ranges::find(excluded, normal) != excluded.end();
    }

    void AddFile(u32 package, const fs::directory_entry& entry, const std::string& path) {
        std::error_code ec;
        if (entry.is_symlink(ec)) {
            throw PackError(PackResult::InvalidSource,
                            "symlinks are not supported: " + ToUtf8(entry.path()));
        }
        if (!entry.is_regular_file(ec)) {
            throw PackError(PackResult::InvalidSource,
                            "unsupported file type: " + ToUtf8(entry.path()));
        }
        // Dump-tool listings are excluded by the spec.
        if (entry.path().filename() == ".listing" || IsExcluded(entry.path())) {
            return;
        }
        plan.entries.push_back({package, path, Relative(entry.path()), false});
    }

    void CollectTree(u32 package, const fs::path& root, const std::string& prefix) {
        plan.entries.push_back({package, prefix, Relative(root), true});
        for (auto it = fs::recursive_directory_iterator(root);
             it != fs::recursive_directory_iterator(); ++it) {
            const auto& entry = *it;
            const auto path = prefix + "/" + ToUtf8(fs::relative(entry.path(), root));
            if (!entry.is_symlink() && entry.is_directory()) {
                plan.entries.push_back({package, path, Relative(entry.path()), true});
            } else {
                AddFile(package, entry, path);
            }
        }
    }

    std::string Relative(const fs::path& path) const {
        return ToUtf8(fs::relative(path, dump_root));
    }

    fs::path dump_root;
    std::vector<fs::path> excluded;
    Metadata overrides;
    RealVfsFilesystem vfs;
    Plan plan;
    std::vector<std::vector<std::pair<u16, std::vector<u8>>>> package_tlvs;
    std::vector<std::pair<u32, std::unique_ptr<Loader::AppLoader_DeconstructedRomDirectory>>>
        loaders;
};

Json PlanToJson(const Plan& plan) {
    Json packages = Json::array();
    for (const auto& package : plan.packages) {
        packages.push_back(Json{{"kind", package.kind},
                                {"flags", package.flags},
                                {"title_id", package.title_id},
                                {"version", package.version},
                                {"meta", ToHex(package.meta)}});
    }
    Json entries = Json::array();
    for (const auto& entry : plan.entries) {
        entries.push_back(Json{{"package", entry.package},
                               {"path", entry.path},
                               {"source", entry.source},
                               {"directory", entry.directory}});
    }
    return Json{{"format", "nup-journal"},
                {"version", JournalVersion},
                {"packages", packages},
                {"entries", entries}};
}

Plan PlanFromJson(const Json& json) {
    if (json.at("format") != "nup-journal" || json.at("version") != JournalVersion) {
        throw PackError(PackResult::IoError, "unsupported journal");
    }
    Plan plan;
    for (const auto& package : json.at("packages")) {
        plan.packages.push_back({package.at("kind").get<u8>(), package.at("flags").get<u8>(),
                                 package.at("title_id").get<u64>(),
                                 package.at("version").get<u32>(),
                                 FromHex(package.at("meta").get<std::string>())});
    }
    for (const auto& entry : json.at("entries")) {
        plan.entries.push_back({entry.at("package").get<u32>(), entry.at("path").get<std::string>(),
                                entry.at("source").get<std::string>(),
                                entry.at("directory").get<bool>()});
    }
    return plan;
}

void RemoveFile(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

bool IsEmptyDirectory(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec) && fs::is_empty(path, ec) && !ec;
}

void RemoveEmptyTree(const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> directories;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) {
            directories.push_back(it->path());
        }
    }
    for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
        if (IsEmptyDirectory(*it)) {
            fs::remove(*it, ec);
        }
    }
    if (IsEmptyDirectory(root)) {
        fs::remove(root, ec);
    }
}

void RemoveEmptyParents(fs::path path, const fs::path& dump_root) {
    while (!path.empty() && path.has_relative_path()) {
        std::error_code ec;
        if (fs::equivalent(path, dump_root, ec) || ec || !IsEmptyDirectory(path)) {
            break;
        }
        fs::remove(path, ec);
        path = path.parent_path();
    }
}

class Packer {
public:
    Packer(const fs::path& dump_root_, const fs::path& archive_, const PackOptions& options_,
           const std::function<void(const PackProgress&)>& progress_,
           const std::atomic_bool* cancel_)
        : dump_root{dump_root_}, archive{archive_}, part{WithSuffix(archive_, ".part")},
          journal{WithSuffix(archive_, ".journal")}, options{options_}, progress_cb{progress_},
          cancel{cancel_} {}

    PackResult Run() {
        std::error_code ec;
        if (fs::exists(archive, ec)) {
            throw PackError(PackResult::IoError, "archive already exists: " + ToUtf8(archive));
        }
        // A journal without a complete plan line was cut short before anything was deleted.
        const bool resume =
            options.delete_sources && HasResumableJournal(archive) && LoadJournal();
        if (resume) {
            OpenForResume();
        } else {
            if (!IsLooseDump(dump_root)) {
                throw PackError(PackResult::InvalidSource,
                                "expected exefs/main, exefs/main.npdm and a romfs folder");
            }
            std::vector<fs::path> excluded;
            for (const auto& path : {archive, part, journal}) {
                excluded.push_back(fs::absolute(path).lexically_normal());
            }
            plan = Discovery{dump_root, std::move(excluded), options.metadata}.Run();
            layout = BuildLayout(plan);
            OpenNew();
        }
        extents.resize(layout.files.size());
        done.resize(layout.files.size());
        for (const auto& [index, extent] : journaled) {
            extents[index] = extent;
            done[index] = true;
            Remember(extent);
        }
        CountTotals();
        Report();

        for (size_t i = 0; i < layout.files.size(); ++i) {
            if (done[i]) {
                continue;
            }
            PackFile(i);
            if (options.delete_sources &&
                (chunk_bytes >= options.chunk_bytes || chunk.size() >= MaxChunkFiles)) {
                Checkpoint();
            }
        }
        if (options.delete_sources) {
            Checkpoint();
        }

        Finish();
        out.Close();
        fs::rename(part, archive, ec);
        if (ec) {
            throw PackError(PackResult::IoError, "cannot rename archive: " + ec.message());
        }

        if (options.delete_sources) {
            RemoveFile(journal);
            RemoveSourceFolders();
        }
        return PackResult::Success;
    }

    void Abort() {
        out.Close();
        if (!options.delete_sources) {
            RemoveFile(part);
        }
    }

private:
    void OpenNew() {
        RemoveFile(journal);
        out.Open(part, Common::FS::FileAccessMode::Write);
        if (!out.IsOpen()) {
            throw PackError(PackResult::IoError, "cannot create " + ToUtf8(part));
        }
        WriteZeros(layout.blob_offset);
        cursor = layout.blob_offset;
        if (options.delete_sources) {
            AppendJournal(PlanToJson(plan).dump() + '\n');
        }
    }

    bool LoadJournal() {
        const auto contents =
            Common::FS::ReadStringFromFile(journal, Common::FS::FileType::BinaryFile);
        size_t start = 0;
        bool have_plan = false;
        while (start < contents.size()) {
            const auto line_end = contents.find('\n', start);
            if (line_end == std::string::npos) {
                break;
            }
            const auto line = std::string_view{contents}.substr(start, line_end - start);
            start = line_end + 1;
            Json json;
            try {
                json = Json::parse(line);
            } catch (const Json::exception&) {
                // Only the tail can be torn: the journal is synced before sources are deleted.
                break;
            }
            if (!have_plan) {
                plan = PlanFromJson(json);
                layout = BuildLayout(plan);
                resume_end = layout.blob_offset;
                have_plan = true;
                continue;
            }
            const auto index = json.at("file").get<size_t>();
            const auto sha = FromHex(json.at("sha256").get<std::string>());
            if (index >= layout.files.size() || sha.size() != 32) {
                throw PackError(PackResult::IoError, "malformed journal");
            }
            Extent extent{json.at("size").get<u64>(), {}, json.at("offset").get<u64>(),
                          json.at("stored_size").get<u64>(), json.at("codec").get<u8>()};
            std::ranges::copy(sha, extent.sha256.begin());
            resume_end = std::max(resume_end, extent.offset + extent.stored_size);
            journaled.emplace_back(index, extent);
        }
        return have_plan;
    }

    void OpenForResume() {
        const u64 end = resume_end;
        out.Open(part, Common::FS::FileAccessMode::ReadWrite);
        if (!out.IsOpen()) {
            throw PackError(PackResult::IoError, "cannot open " + ToUtf8(part));
        }
        if (out.GetSize() < end) {
            throw PackError(PackResult::IoError, "partial archive is shorter than its journal; "
                                                 "delete " +
                                                     ToUtf8(part) + " and " + ToUtf8(journal) +
                                                     " to start over");
        }
        if (!out.SetSize(end) || !out.Seek(static_cast<s64>(end))) {
            throw PackError(PackResult::IoError, "cannot truncate " + ToUtf8(part));
        }
        cursor = end;

        // Journaled before an interruption, but the delete never happened.
        for (const auto& [index, extent] : journaled) {
            RemoveFile(SourceOf(index));
        }
    }

    fs::path SourceOf(size_t file_index) const {
        return dump_root / FromUtf8(plan.entries[layout.files[file_index].entry].source);
    }

    void CountTotals() {
        for (size_t i = 0; i < layout.files.size(); ++i) {
            ++progress.files_total;
            if (done[i]) {
                progress.bytes_total += extents[i].size;
                progress.bytes_done += extents[i].size;
                ++progress.files_done;
            } else {
                std::error_code ec;
                const auto size = fs::file_size(SourceOf(i), ec);
                if (ec) {
                    throw PackError(PackResult::InvalidSource,
                                    "missing source file: " + ToUtf8(SourceOf(i)));
                }
                progress.bytes_total += size;
            }
        }
    }

    void Remember(const Extent& extent) {
        if (extent.size == 0) {
            return;
        }
        sizes_seen.insert(extent.size);
        by_hash.try_emplace(extent.sha256, extent);
    }

    bool Cancelled() const {
        return cancel && cancel->load();
    }

    void WriteZeros(u64 count) {
        static constexpr std::array<u8, 4096> zeros{};
        while (count > 0) {
            const auto n = static_cast<size_t>(std::min<u64>(count, zeros.size()));
            Write(zeros.data(), n);
            count -= n;
        }
    }

    void Write(const u8* data, size_t size) {
        if (out.WriteSpan(std::span<const u8>{data, size}) != size) {
            throw PackError(PackResult::IoError, "write failed, the disk may be full");
        }
    }

    void AddProgress(u64 bytes) {
        progress.bytes_done += bytes;
        if (progress.bytes_done - last_report >= ProgressStep) {
            Report();
        }
    }

    // Reads the whole source through `sink`, checking that it still has the expected size. A file
    // read more than once (hashed for dedup, or compressed and then stored raw) counts once.
    template <typename Sink>
    void ReadSource(Common::FS::IOFile& in, u64 size, Sink&& sink) {
        if (!in.Seek(0)) {
            throw PackError(PackResult::IoError, "cannot seek source");
        }
        buffer.resize(ReadBufferSize);
        u64 total = 0;
        while (true) {
            if (Cancelled()) {
                throw PackError(PackResult::Cancelled, "cancelled");
            }
            const auto read = in.ReadSpan(std::span{buffer});
            if (read == 0) {
                break;
            }
            total += read;
            if (total > size) {
                break;
            }
            if (total > file_counted) {
                AddProgress(total - file_counted);
                file_counted = total;
            }
            if (!sink(buffer.data(), read)) {
                return;
            }
        }
        if (total != size) {
            throw PackError(PackResult::IoError, "a source file changed while packing");
        }
    }

    Digest HashSource(Common::FS::IOFile& in, u64 size) {
        hash.Reset();
        ReadSource(in, size, [this](const u8* data, size_t n) {
            hash.Update(data, n);
            return true;
        });
        return hash.Final();
    }

    ZstdContext& Compressor() {
        if (!compressor) {
            compressor = std::make_unique<ZstdContext>(options.zstd_level);
        }
        return *compressor;
    }

    // Streams one zstd frame into the archive. Gives up (returns false) as soon as the output is
    // no smaller than the input.
    bool WriteCompressed(Common::FS::IOFile& in, u64 size, u64& stored) {
        auto& zstd = Compressor();
        zstd.Begin(size);
        out_buffer.resize(ZSTD_CStreamOutSize());
        stored = 0;
        bool fits = true;
        const auto drain = [&](ZSTD_inBuffer& input, ZSTD_EndDirective mode) {
            while (true) {
                ZSTD_outBuffer output{out_buffer.data(), out_buffer.size(), 0};
                const size_t remaining = ZSTD_compressStream2(zstd.get(), &output, &input, mode);
                if (ZSTD_isError(remaining)) {
                    throw PackError(PackResult::IoError,
                                    std::string{"zstd: "} + ZSTD_getErrorName(remaining));
                }
                stored += output.pos;
                if (stored >= size) {
                    return false;
                }
                Write(out_buffer.data(), output.pos);
                const bool finished = mode == ZSTD_e_end ? remaining == 0
                                                         : input.pos == input.size;
                if (finished) {
                    return true;
                }
            }
        };
        hash.Reset();
        ReadSource(in, size, [&](const u8* data, size_t n) {
            hash.Update(data, n);
            ZSTD_inBuffer input{data, n, 0};
            fits = drain(input, ZSTD_e_continue);
            return fits;
        });
        if (!fits) {
            return false;
        }
        ZSTD_inBuffer empty{nullptr, 0, 0};
        return drain(empty, ZSTD_e_end);
    }

    void PackFile(size_t index) {
        const auto source = SourceOf(index);
        Common::FS::IOFile in{source, Common::FS::FileAccessMode::Read};
        if (!in.IsOpen()) {
            throw PackError(PackResult::IoError, "cannot read " + ToUtf8(source));
        }
        Extent extent;
        extent.size = in.GetSize();
        file_counted = 0;

        std::optional<Digest> known_hash;
        if (extent.size == 0) {
            known_hash = HashSource(in, 0);
        } else if (sizes_seen.contains(extent.size)) {
            // Only a file whose size matches an earlier extent can be a duplicate; hash it before
            // writing anything so the shared extent costs no output.
            known_hash = HashSource(in, extent.size);
            if (const auto it = by_hash.find(*known_hash);
                it != by_hash.end() && it->second.size == extent.size) {
                extent = it->second;
                Complete(index, extent);
                return;
            }
        }

        if (extent.size == 0) {
            extent.sha256 = *known_hash;
            Complete(index, extent);
            return;
        }

        const u64 start = AlignUp(cursor, ExtentAlignment);
        Seek(cursor);
        WriteZeros(start - cursor);
        extent.offset = start;

        bool compressed = false;
        if (extent.size <= options.raw_threshold && options.zstd_level > 0) {
            u64 stored = 0;
            compressed = WriteCompressed(in, extent.size, stored);
            if (compressed) {
                extent.codec = CodecZstd;
                extent.stored_size = stored;
                extent.sha256 = hash.Final();
            }
        }
        if (!compressed) {
            Seek(start);
            hash.Reset();
            ReadSource(in, extent.size, [this](const u8* data, size_t n) {
                hash.Update(data, n);
                Write(data, n);
                return true;
            });
            extent.codec = CodecRaw;
            extent.stored_size = extent.size;
            extent.sha256 = hash.Final();
        }
        cursor = start + extent.stored_size;
        Remember(extent);
        Complete(index, extent);
    }

    void Seek(u64 offset) {
        if (!out.Seek(static_cast<s64>(offset))) {
            throw PackError(PackResult::IoError, "cannot seek " + ToUtf8(part));
        }
    }

    void Complete(size_t index, const Extent& extent) {
        extents[index] = extent;
        done[index] = true;
        if (options.delete_sources) {
            chunk.push_back(index);
            chunk_bytes += extent.size;
        }
        ++progress.files_done;
        Report();
    }

    void AppendJournal(const std::string& lines) {
        Common::FS::IOFile log{journal, Common::FS::FileAccessMode::Append};
        if (!log.IsOpen() || log.WriteString(lines) != lines.size() || !log.Commit()) {
            throw PackError(PackResult::IoError, "cannot write " + ToUtf8(journal));
        }
    }

    // Archive data first, then the journal, then the deletes, so an interruption loses nothing.
    void Checkpoint() {
        if (chunk.empty()) {
            return;
        }
        if (!out.Commit()) {
            throw PackError(PackResult::IoError, "cannot sync " + ToUtf8(part));
        }
        std::string lines;
        for (const auto index : chunk) {
            const auto& extent = extents[index];
            lines += Json{{"file", index},
                          {"size", extent.size},
                          {"sha256", ToHex(extent.sha256)},
                          {"offset", extent.offset},
                          {"stored_size", extent.stored_size},
                          {"codec", extent.codec}}
                         .dump();
            lines += '\n';
        }
        AppendJournal(lines);
        for (const auto index : chunk) {
            RemoveFile(SourceOf(index));
        }
        chunk.clear();
        chunk_bytes = 0;
    }

    void Finish() {
        // Meta blocks follow the last extent, as pack.py writes them.
        Seek(cursor);
        std::vector<std::pair<u64, u32>> meta;
        for (const auto& package : plan.packages) {
            if (package.meta.size() > std::numeric_limits<u32>::max()) {
                throw PackError(PackResult::IoError, "package metadata is too large");
            }
            meta.emplace_back(package.meta.empty() ? 0 : cursor,
                              static_cast<u32>(package.meta.size()));
            Write(package.meta.data(), package.meta.size());
            cursor += package.meta.size();
        }
        const u64 file_size = cursor;

        const auto toc = BuildToc(meta);
        std::array<u8, 32> toc_hash{};
        unsigned int hash_length{};
        EVP_Digest(toc.data(), toc.size(), toc_hash.data(), &hash_length, EVP_sha256(), nullptr);

        // UUID: SHA-256 over title_id (u64), version (u32) and kind (u8) of each package.
        Sha256 uuid_hash;
        for (const auto& package : plan.packages) {
            std::vector<u8> identity(13);
            PutLE(identity, 0, package.title_id, 8);
            PutLE(identity, 8, package.version, 4);
            identity[12] = package.kind;
            uuid_hash.Update(identity.data(), identity.size());
        }
        const auto uuid = uuid_hash.Final();

        std::vector<u8> super(SuperblockSize);
        PutBytes(super, 0x00, Magic);
        PutLE(super, 0x04, SuperblockSize, 2);
        PutLE(super, 0x06, 1, 2);
        PutLE(super, 0x08, SuperblockSize, 8);
        PutLE(super, 0x10, toc.size(), 8);
        PutBytes(super, 0x18, toc_hash);
        PutLE(super, 0x38, layout.blob_offset, 8);
        PutLE(super, 0x40, file_size, 8);
        PutLE(super, 0x48, plan.packages.size(), 4);
        PutBytes(super, 0x50, std::span{uuid}.first(16));

        Seek(0);
        Write(super.data(), super.size());
        Write(toc.data(), toc.size());
        if (!out.SetSize(file_size) || !out.Commit()) {
            throw PackError(PackResult::IoError, "cannot finish " + ToUtf8(part));
        }
    }

    // Every field is written little-endian at its documented offset. The package record is
    // 0x40 bytes with 20 reserved bytes at 0x2C, as NupContainer reads it.
    std::vector<u8> BuildToc(const std::vector<std::pair<u64, u32>>& meta) const {
        std::vector<u8> toc(layout.toc_size);
        const u64 dir_count = layout.dirs.size();
        PutBytes(toc, 0x00, TocMagic);
        PutLE(toc, 0x04, plan.packages.size(), 4);
        PutLE(toc, 0x08, dir_count, 8);
        PutLE(toc, 0x10, layout.files.size(), 8);

        size_t pos = TocHeaderSize;
        for (size_t i = 0; i < plan.packages.size(); ++i) {
            const auto& package = plan.packages[i];
            const auto& record = layout.packages[i];
            PutLE(toc, pos + 0x00, package.title_id, 8);
            PutLE(toc, pos + 0x08, record.root_dir, 4);
            PutLE(toc, pos + 0x0C, record.exefs_dir, 4);
            PutLE(toc, pos + 0x10, record.romfs_dir, 4);
            PutLE(toc, pos + 0x14, package.version, 4);
            toc[pos + 0x18] = package.kind;
            toc[pos + 0x19] = package.flags;
            PutLE(toc, pos + 0x20, meta[i].first, 8);
            PutLE(toc, pos + 0x28, meta[i].second, 4);
            pos += PackageRecordSize;
        }
        for (const auto& dir : layout.dirs) {
            PutLE(toc, pos + 0x00, layout.pool_index.at(dir.name), 4);
            PutLE(toc, pos + 0x04, dir.name.size(), 4);
            PutLE(toc, pos + 0x08, dir.first_child, 4);
            PutLE(toc, pos + 0x0C, dir.child_count, 4);
            pos += DirRecordSize;
        }
        for (size_t i = 0; i < layout.files.size(); ++i) {
            const auto& file = layout.files[i];
            const auto& extent = extents[i];
            PutLE(toc, pos + 0x00, layout.pool_index.at(file.name), 4);
            PutLE(toc, pos + 0x04, file.name.size(), 4);
            PutLE(toc, pos + 0x08, extent.size, 8);
            // Empty files have no extent: offset, size and codec are all zero.
            if (extent.size != 0) {
                PutLE(toc, pos + 0x10, extent.offset, 8);
                PutLE(toc, pos + 0x18, extent.stored_size, 8);
                toc[pos + 0x40] = extent.codec;
            }
            PutBytes(toc, pos + 0x20, extent.sha256);
            pos += FileRecordSize;
        }
        for (const u64 child : layout.children) {
            const u64 id = (child & Layout::FileTag) ? dir_count + static_cast<u32>(child) : child;
            PutLE(toc, pos, id, 4);
            pos += sizeof(u32);
        }
        PutLE(toc, 0x18, pos, 8);
        PutLE(toc, 0x20, layout.pool.size(), 8);
        std::memcpy(toc.data() + pos, layout.pool.data(), layout.pool.size());
        return toc;
    }

    void RemoveSourceFolders() {
        for (const auto& entry : plan.entries) {
            const auto depth = std::ranges::count(entry.path, '/');
            const bool top = entry.directory &&
                             (depth == 0 || (depth == 1 && entry.path.starts_with("data/")));
            if (top) {
                const auto source = dump_root / FromUtf8(entry.source);
                RemoveEmptyTree(source);
                RemoveEmptyParents(source.parent_path(), dump_root);
            }
        }
    }

    void Report() {
        last_report = progress.bytes_done;
        if (progress_cb) {
            progress_cb(progress);
        }
    }

    fs::path dump_root;
    fs::path archive;
    fs::path part;
    fs::path journal;
    PackOptions options;
    std::function<void(const PackProgress&)> progress_cb;
    const std::atomic_bool* cancel;

    Plan plan;
    Layout layout;
    std::vector<Extent> extents;
    std::vector<bool> done;
    std::vector<std::pair<size_t, Extent>> journaled;

    struct DigestHash {
        size_t operator()(const Digest& digest) const {
            size_t value;
            std::memcpy(&value, digest.data(), sizeof(value));
            return value;
        }
    };
    std::unordered_map<Digest, Extent, DigestHash> by_hash;
    std::unordered_set<u64> sizes_seen;

    Common::FS::IOFile out;
    u64 cursor = 0;
    u64 resume_end = 0;
    std::vector<size_t> chunk;
    u64 chunk_bytes = 0;
    std::vector<u8> buffer;
    std::vector<u8> out_buffer;
    Sha256 hash;
    std::unique_ptr<ZstdContext> compressor;
    PackProgress progress{};
    u64 last_report = 0;
    u64 file_counted = 0;
};

} // namespace

bool IsLooseDump(const fs::path& dir) {
    return HasBootableExeFS(dir);
}

std::optional<fs::path> DumpRootFromMain(const fs::path& main) {
    // The Qt game list stores the exefs folder rather than exefs/main.
    std::error_code ec;
    const auto exefs = fs::is_directory(main, ec) ? main : main.parent_path();
    if (Lower(ToUtf8(exefs.filename())) != "exefs") {
        return std::nullopt;
    }
    auto root = exefs.parent_path();
    const auto name = Lower(ToUtf8(root.filename()));
    if (name == "base") {
        root = root.parent_path();
    } else if (ParseDecimal(name) && Lower(ToUtf8(root.parent_path().filename())) == "updates") {
        root = root.parent_path().parent_path();
    }
    if (!IsLooseDump(root)) {
        return std::nullopt;
    }
    return root;
}

Metadata ProbeMetadata(const fs::path& dump_root) {
    RealVfsFilesystem vfs;
    const auto base = FindDirectory(dump_root, "base");
    const auto updates = FindDirectory(dump_root, "updates");

    // Newest update first, then the base (or a bare dump's root).
    std::vector<std::pair<u64, fs::path>> update_dirs;
    std::error_code ec;
    if (!updates.empty()) {
        for (const auto& entry : fs::directory_iterator(updates, ec)) {
            const auto version = ParseDecimal(ToUtf8(entry.path().filename()));
            if (version && entry.is_directory(ec)) {
                update_dirs.emplace_back(*version, entry.path());
            }
        }
    }
    std::ranges::sort(update_dirs, std::greater<>{});
    const fs::path base_dir = !base.empty() ? base : (updates.empty() ? dump_root : fs::path{});

    Metadata result;
    const auto probe = [&](const fs::path& dir, const std::string& label, bool names) {
        const auto loader = OpenLoader(vfs, dir);
        if (!loader) {
            return;
        }
        if (result.display_version.empty()) {
            std::string version;
            if (loader->ReadDisplayVersion(version) == Loader::ResultStatus::Success &&
                !version.empty()) {
                result.display_version = version;
                result.version_source = label;
            }
        }
        if (names) {
            NACP control;
            if ((result.title_name.empty() || result.publisher.empty()) &&
                loader->ReadControlData(control) == Loader::ResultStatus::Success) {
                if (result.title_name.empty()) {
                    result.title_name = control.GetApplicationName();
                }
                if (result.publisher.empty()) {
                    result.publisher = control.GetDeveloperName();
                }
            }
            std::vector<u8> icon;
            if (result.icon.empty() &&
                loader->ReadIcon(icon) == Loader::ResultStatus::Success) {
                result.icon = std::move(icon);
            }
        }
    };
    for (const auto& [version, dir] : update_dirs) {
        probe(dir, "update " + std::to_string(version), false);
    }
    if (!base_dir.empty()) {
        probe(base_dir, "base", true);
    }
    // Names and icons fall back to the updates when the base has none.
    for (const auto& [version, dir] : update_dirs) {
        if (!result.title_name.empty() && !result.icon.empty() && !result.publisher.empty()) {
            break;
        }
        probe(dir, "update " + std::to_string(version), true);
    }
    return result;
}

fs::path DefaultArchivePath(const fs::path& dump_root) {
    auto root = dump_root;
    if (!root.has_filename()) {
        root = root.parent_path();
    }
    return root.parent_path() / (root.filename().u8string() + u8".nup");
}

bool HasResumableJournal(const fs::path& archive) {
    std::error_code ec;
    return fs::exists(WithSuffix(archive, ".part"), ec) &&
           fs::exists(WithSuffix(archive, ".journal"), ec);
}

PackResult PackDump(const fs::path& dump_root, const fs::path& archive, const PackOptions& options,
                    const std::function<void(const PackProgress&)>& progress,
                    const std::atomic_bool* cancel, std::string* error_out) {
    Packer packer{dump_root, archive, options, progress, cancel};
    try {
        return packer.Run();
    } catch (const PackError& e) {
        packer.Abort();
        if (error_out) {
            *error_out = e.what();
        }
        return e.result;
    } catch (const std::exception& e) {
        packer.Abort();
        if (error_out) {
            *error_out = e.what();
        }
        return PackResult::IoError;
    }
}

namespace {

class ExtractError : public std::runtime_error {
public:
    ExtractError(ExtractResult result_, const std::string& message)
        : std::runtime_error(message), result{result_} {}
    ExtractResult result;
};

u64 ReadLe(const std::vector<u8>& buf, size_t offset, size_t bytes) {
    u64 value = 0;
    for (size_t i = 0; i < bytes; ++i) {
        value |= static_cast<u64>(buf[offset + i]) << (8 * i);
    }
    return value;
}

void CountDir(const FileSys::VirtualDir& dir, u64& files, u64& bytes) {
    if (dir == nullptr) {
        return;
    }
    for (const auto& file : dir->GetFiles()) {
        ++files;
        bytes += file->GetSize();
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        CountDir(sub, files, bytes);
    }
}

// Archive member names are labels, not paths. Reject anything that is not exactly one
// relative filename component so a crafted archive cannot escape the output directory.
void CheckedName(const std::string& name) {
    const auto path = FromUtf8(name);
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || name.find(':') != std::string::npos ||
        name.find('\0') != std::string::npos || path.filename() != path) {
        throw ExtractError(ExtractResult::InvalidSource, "invalid file name in archive: " + name);
    }
}

void WriteDir(const FileSys::VirtualDir& dir, const fs::path& dest, PackProgress& progress,
              const std::function<void(const PackProgress&)>& progress_cb,
              const std::atomic_bool* cancel, u64& last_report) {
    if (dir == nullptr) {
        return;
    }
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
        throw ExtractError(ExtractResult::IoError, "cannot create " + ToUtf8(dest));
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        if (cancel && cancel->load()) {
            throw ExtractError(ExtractResult::Cancelled, "cancelled");
        }
        CheckedName(sub->GetName());
        WriteDir(sub, dest / FromUtf8(sub->GetName()), progress, progress_cb, cancel,
                 last_report);
    }
    std::vector<u8> buffer(ReadBufferSize);
    for (const auto& file : dir->GetFiles()) {
        if (cancel && cancel->load()) {
            throw ExtractError(ExtractResult::Cancelled, "cancelled");
        }
        CheckedName(file->GetName());
        const auto out_path = dest / FromUtf8(file->GetName());
        Common::FS::IOFile out{out_path, Common::FS::FileAccessMode::Write};
        if (!out.IsOpen()) {
            throw ExtractError(ExtractResult::IoError, "cannot write " + ToUtf8(out_path));
        }
        const u64 size = file->GetSize();
        u64 offset = 0;
        while (offset < size) {
            if (cancel && cancel->load()) {
                throw ExtractError(ExtractResult::Cancelled, "cancelled");
            }
            const size_t chunk =
                static_cast<size_t>(std::min<u64>(buffer.size(), size - offset));
            const size_t got = file->Read(buffer.data(), chunk, static_cast<size_t>(offset));
            if (got == 0) {
                throw ExtractError(ExtractResult::IoError,
                                   "short read of " + file->GetName());
            }
            if (out.WriteSpan(std::span<const u8>{buffer.data(), got}) != got) {
                throw ExtractError(ExtractResult::IoError,
                                   "write failed, the disk may be full");
            }
            offset += got;
            progress.bytes_done += got;
            if (progress.bytes_done - last_report >= ProgressStep) {
                last_report = progress.bytes_done;
                if (progress_cb) {
                    progress_cb(progress);
                }
            }
        }
        out.Close();
        ++progress.files_done;
        if (progress_cb) {
            progress_cb(progress);
        }
    }
}

} // namespace

std::filesystem::path DefaultExtractPath(const std::filesystem::path& archive) {
    auto parent = archive.parent_path();
    auto stem = archive.stem();
    if (stem.empty()) {
        stem = archive.filename();
    }
    return parent / stem;
}

ExtractResult ExtractNup(const std::filesystem::path& archive, const std::filesystem::path& out_dir,
                         const std::function<void(const PackProgress&)>& progress_cb,
                         const std::atomic_bool* cancel, std::string* error_out) {
    // True once this call has claimed out_dir by creating it or verifying that it is
    // empty. Cleanup only removes output this call can safely claim; failures before that
    // point (missing archive, uncreatable or non-empty output) leave everything untouched.
    bool created = false;
    bool claimed = false;
    const auto cleanup = [&] {
        if (!claimed) {
            return;
        }
        std::error_code rm_ec;
        if (created) {
            fs::remove_all(out_dir, rm_ec);
        } else {
            for (auto it = fs::directory_iterator(out_dir, rm_ec);
                 !rm_ec && it != fs::directory_iterator{}; it.increment(rm_ec)) {
                fs::remove_all(it->path(), rm_ec);
            }
        }
    };
    try {
        std::error_code ec;
        if (!fs::is_regular_file(archive, ec)) {
            throw ExtractError(ExtractResult::InvalidSource, "not a file: " + ToUtf8(archive));
        }
        if (fs::exists(out_dir, ec)) {
            if (!fs::is_directory(out_dir, ec)) {
                throw ExtractError(ExtractResult::IoError,
                                   "output exists and is not a directory: " + ToUtf8(out_dir));
            }
            for (auto it = fs::directory_iterator(out_dir, ec); !ec && it != fs::directory_iterator{};
                 it.increment(ec)) {
                throw ExtractError(ExtractResult::IoError,
                                   "output directory already exists: " + ToUtf8(out_dir));
            }
            if (ec) {
                throw ExtractError(ExtractResult::IoError,
                                   "cannot list " + ToUtf8(out_dir) + ": " + ec.message());
            }
            claimed = true;
        } else {
            fs::create_directories(out_dir, ec);
            if (ec) {
                throw ExtractError(ExtractResult::IoError,
                                   "cannot create " + ToUtf8(out_dir) + ": " + ec.message());
            }
            created = true;
            claimed = true;
        }

        RealVfsFilesystem vfs;
        const auto file =
            vfs.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read);
        if (!file) {
            throw ExtractError(ExtractResult::IoError, "cannot open " + ToUtf8(archive));
        }
        FileSys::NupContainer nup{file};
        if (nup.GetStatus() != Loader::ResultStatus::Success) {
            throw ExtractError(ExtractResult::InvalidSource,
                               "not a valid NUP file: " + ToUtf8(archive));
        }
        const auto& packages = nup.GetPackages();
        if (packages.empty()) {
            throw ExtractError(ExtractResult::InvalidSource, "NUP has no packages");
        }

        const bool bare =
            packages.size() == 1 && packages[0].kind == FileSys::NupContainer::PackageKind::Base;

        PackProgress progress{};
        std::vector<std::pair<size_t, fs::path>> targets;
        std::unordered_map<u64, size_t> addon_index;
        for (size_t i = 0; i < packages.size(); ++i) {
            const auto& package = packages[i];
            fs::path dest;
            if (bare) {
                dest = out_dir;
            } else if (package.kind == FileSys::NupContainer::PackageKind::Base) {
                dest = out_dir / "base";
            } else if (package.kind == FileSys::NupContainer::PackageKind::Update) {
                dest = out_dir / "updates" / std::to_string(package.version);
            } else {
                const size_t idx = addon_index[package.title_id]++;
                dest = out_dir / "dlc" / fmt::format("{:016X}", package.title_id) /
                       std::to_string(idx);
            }
            targets.emplace_back(i, dest);
            u64 files = 0, bytes = 0;
            CountDir(nup.GetPackageDir(i), files, bytes);
            progress.files_total += files;
            progress.bytes_total += bytes;
        }
        if (progress_cb) {
            progress_cb(progress);
        }
        u64 last_report = 0;
        for (const auto& [index, dest] : targets) {
            WriteDir(nup.GetPackageDir(index), dest, progress, progress_cb, cancel, last_report);
        }
        return ExtractResult::Success;
    } catch (const ExtractError& e) {
        if (e.result != ExtractResult::Success) {
            cleanup();
        }
        if (error_out) {
            *error_out = e.what();
        }
        return e.result;
    } catch (const std::exception& e) {
        cleanup();
        if (error_out) {
            *error_out = e.what();
        }
        return ExtractResult::IoError;
    }
}

namespace {

std::vector<std::pair<u16, std::vector<u8>>> ParseTlvs(const std::vector<u8>& block) {
    std::vector<std::pair<u16, std::vector<u8>>> out;
    size_t pos = 0;
    while (pos + 8 <= block.size()) {
        const u16 type =
            static_cast<u16>(block[pos] | (static_cast<u16>(block[pos + 1]) << 8));
        const u32 size = static_cast<u32>(block[pos + 4] | (static_cast<u32>(block[pos + 5]) << 8) |
                                          (static_cast<u32>(block[pos + 6]) << 16) |
                                          (static_cast<u32>(block[pos + 7]) << 24));
        pos += 8;
        if (pos + size > block.size()) {
            throw std::runtime_error("malformed meta block");
        }
        out.emplace_back(type, std::vector<u8>(block.begin() + pos, block.begin() + pos + size));
        pos += size;
    }
    return out;
}

void SetOrRemoveTlv(std::vector<std::pair<u16, std::vector<u8>>>& tlvs, u16 type,
                    const std::optional<std::vector<u8>>& value) {
    if (!value) {
        return;
    }
    std::erase_if(tlvs, [type](const auto& tlv) { return tlv.first == type; });
    if (!value->empty()) {
        tlvs.emplace_back(type, *value);
    }
}

// Records of: u8 tag length, tag, u32 name length, name.
std::vector<u8> SerializeLocalizedTitles(
    const std::vector<std::pair<std::string, std::string>>& titles) {
    std::vector<u8> out;
    for (const auto& [lang, name] : titles) {
        if (lang.empty() || lang.size() > 0xFF || name.empty()) {
            continue;
        }
        out.push_back(static_cast<u8>(lang.size()));
        out.insert(out.end(), lang.begin(), lang.end());
        for (size_t b = 0; b < 4; ++b) {
            out.push_back(static_cast<u8>(name.size() >> (8 * b)));
        }
        out.insert(out.end(), name.begin(), name.end());
    }
    return out;
}

// Highest-version Update package, if any. Pack and load treat this as the effective update.
std::optional<size_t> NewestUpdateIndex(
    const std::vector<FileSys::NupContainer::PackageInfo>& packages) {
    std::optional<size_t> best;
    for (size_t i = 0; i < packages.size(); ++i) {
        if (packages[i].kind != FileSys::NupContainer::PackageKind::Update) {
            continue;
        }
        if (!best || packages[i].version > packages[*best].version) {
            best = i;
        }
    }
    return best;
}

// Base package, or the first package when the container has no base: the same target the
// packer attaches application metadata to.
size_t NupEditTargetIndex(const std::vector<FileSys::NupContainer::PackageInfo>& packages) {
    for (size_t i = 0; i < packages.size(); ++i) {
        if (packages[i].kind == FileSys::NupContainer::PackageKind::Base) {
            return i;
        }
    }
    return 0;
}

} // namespace

Metadata ProbeNupMetadata(const fs::path& archive) {
    Metadata out;
    RealVfsFilesystem vfs;
    const auto file =
        vfs.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read);
    if (!file) {
        return out;
    }
    FileSys::NupContainer nup{file};
    if (nup.GetStatus() != Loader::ResultStatus::Success || nup.GetPackages().empty()) {
        return out;
    }
    const auto& packages = nup.GetPackages();
    const size_t target = NupEditTargetIndex(packages);
    const auto& package = packages[target];
    out.title_name = package.title_name;
    out.icon = package.icon;
    out.publisher = package.publisher;
    out.localized_titles = package.localized_titles;
    // Prefill the version the loader actually displays: the newest update's when one exists.
    if (const auto update = NewestUpdateIndex(packages);
        update && !packages[*update].display_version.empty()) {
        out.display_version = packages[*update].display_version;
    } else {
        out.display_version = package.display_version;
    }
    out.version_source = "NUP";
    return out;
}

EditResult EditNupMetadata(const fs::path& archive, const MetadataEdit& edit,
                           std::string* error_out, const std::atomic_bool* cancel,
                           const std::function<void(const PackProgress&)>& progress_cb) {
    const auto fail = [&](EditResult result, const std::string& message) {
        if (error_out) {
            *error_out = message;
        }
        return result;
    };
    if (!edit.title_name && !edit.icon && !edit.display_version && !edit.publisher &&
        !edit.localized_titles) {
        return EditResult::NoChange;
    }
    std::error_code ec;
    if (!fs::is_regular_file(archive, ec)) {
        return fail(EditResult::InvalidSource, "not a file: " + ToUtf8(archive));
    }
    // The probe objects hold the archive open; release them before rewriting it so the
    // replace below never runs against an open handle.
    size_t target = 0;
    // The newest update when it is not the target itself; NoUpdate otherwise.
    constexpr size_t NoUpdate = std::numeric_limits<size_t>::max();
    size_t update = NoUpdate;
    size_t package_total = 0;
    std::string cur_title;
    std::string cur_version;
    std::string cur_update_version;
    std::string cur_publisher;
    std::vector<u8> cur_icon;
    std::vector<std::pair<std::string, std::string>> cur_localized;
    {
        RealVfsFilesystem vfs;
        const auto file =
            vfs.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read);
        if (!file) {
            return fail(EditResult::IoError, "cannot open " + ToUtf8(archive));
        }
        FileSys::NupContainer nup{file};
        if (nup.GetStatus() != Loader::ResultStatus::Success || nup.GetPackages().empty()) {
            return fail(EditResult::InvalidSource, "not a valid NUP file: " + ToUtf8(archive));
        }
        const auto& packages = nup.GetPackages();
        target = NupEditTargetIndex(packages);
        update = NewestUpdateIndex(packages).value_or(NoUpdate);
        if (update == target) {
            update = NoUpdate;
        }
        package_total = packages.size();
        const auto& current = packages[target];
        cur_title = current.title_name;
        cur_version = current.display_version;
        cur_publisher = current.publisher;
        cur_icon = current.icon;
        cur_localized = current.localized_titles;
        cur_update_version =
            update != NoUpdate ? packages[update].display_version : current.display_version;
    }
    const auto same = [&](const std::optional<std::string>& wanted, const std::string& have) {
        return !wanted || *wanted == have;
    };
    const bool icon_same = !edit.icon || *edit.icon == cur_icon;
    const bool localized_same = !edit.localized_titles || *edit.localized_titles == cur_localized;
    if (same(edit.title_name, cur_title) && icon_same && localized_same &&
        same(edit.display_version, cur_version) && same(edit.display_version, cur_update_version) &&
        same(edit.publisher, cur_publisher)) {
        return EditResult::NoChange;
    }

    Common::FS::IOFile in{archive, Common::FS::FileAccessMode::Read};
    if (!in.IsOpen()) {
        return fail(EditResult::IoError, "cannot open " + ToUtf8(archive));
    }
    const u64 file_size = in.GetSize();
    if (file_size < SuperblockSize) {
        return fail(EditResult::InvalidSource, "file too small");
    }
    std::vector<u8> super(SuperblockSize);
    if (in.ReadSpan(std::span<u8>{super}) != super.size() || !in.Seek(0)) {
        return fail(EditResult::IoError, "cannot read superblock");
    }
    if (super[0] != 'N' || super[1] != 'U' || super[2] != 'P' || super[3] != 0x01) {
        return fail(EditResult::InvalidSource, "not a NUP file");
    }
    const u64 toc_offset = ReadLe(super, 0x08, 8);
    const u64 toc_size = ReadLe(super, 0x10, 8);
    const u64 blob_offset = ReadLe(super, 0x38, 8);
    const u64 super_file_size = ReadLe(super, 0x40, 8);
    const u64 package_count = ReadLe(super, 0x48, 4);
    if (toc_offset != SuperblockSize || toc_size == 0 || blob_offset % ExtentAlignment != 0 ||
        super_file_size != file_size || package_count != package_total ||
        toc_offset + toc_size > file_size || toc_offset + toc_size > blob_offset ||
        blob_offset > file_size) {
        return fail(EditResult::InvalidSource, "NUP superblock mismatch");
    }
    if (!in.Seek(static_cast<s64>(toc_offset))) {
        return fail(EditResult::IoError, "cannot seek to TOC");
    }
    std::vector<u8> toc(static_cast<size_t>(toc_size));
    if (in.ReadSpan(std::span<u8>{toc}) != toc.size()) {
        return fail(EditResult::IoError, "cannot read TOC");
    }

    struct RawPackage {
        u64 meta_offset{};
        u32 meta_size{};
    };
    std::vector<RawPackage> raws;
    size_t pos = TocHeaderSize;
    for (size_t i = 0; i < package_count; ++i) {
        if (pos + PackageRecordSize > toc.size()) {
            return fail(EditResult::InvalidSource, "TOC too small");
        }
        RawPackage raw;
        raw.meta_offset = ReadLe(toc, pos + 0x20, 8);
        raw.meta_size = static_cast<u32>(ReadLe(toc, pos + 0x28, 4));
        if (raw.meta_size > 0 &&
            (raw.meta_offset == 0 || raw.meta_offset > file_size ||
             raw.meta_size > file_size - raw.meta_offset)) {
            return fail(EditResult::InvalidSource, "bad meta extent");
        }
        raws.push_back(raw);
        pos += PackageRecordSize;
    }

    u64 meta_start = file_size;
    for (const auto& raw : raws) {
        if (raw.meta_size > 0) {
            meta_start = std::min(meta_start, raw.meta_offset);
        }
    }
    if (meta_start < blob_offset || meta_start > file_size) {
        return fail(EditResult::InvalidSource, "bad meta start");
    }

    std::vector<std::vector<u8>> new_metas(raws.size());
    for (size_t i = 0; i < raws.size(); ++i) {
        std::vector<u8> block;
        if (raws[i].meta_size > 0) {
            if (!in.Seek(static_cast<s64>(raws[i].meta_offset))) {
                return fail(EditResult::IoError, "cannot seek to meta block");
            }
            block.resize(raws[i].meta_size);
            if (in.ReadSpan(std::span<u8>{block}) != block.size()) {
                return fail(EditResult::IoError, "cannot read meta block");
            }
        }
        if (i == target || (edit.display_version && update != NoUpdate && i == update)) {
            auto tlvs = ParseTlvs(block);
            if (i == target) {
                if (edit.title_name) {
                    SetOrRemoveTlv(tlvs, TlvTitleName, StringBytes(*edit.title_name));
                }
                if (edit.publisher) {
                    SetOrRemoveTlv(tlvs, TlvPublisher, StringBytes(*edit.publisher));
                }
                if (edit.icon) {
                    SetOrRemoveTlv(tlvs, TlvIcon, *edit.icon);
                }
                if (edit.localized_titles) {
                    SetOrRemoveTlv(tlvs, TlvLocalizedTitles,
                                   SerializeLocalizedTitles(*edit.localized_titles));
                }
            }
            if (edit.display_version) {
                SetOrRemoveTlv(tlvs, TlvDisplayVersion, StringBytes(*edit.display_version));
            }
            block = SerializeTlvs(std::move(tlvs));
        }
        new_metas[i] = std::move(block);
    }

    u64 cursor = meta_start;
    std::vector<std::pair<u64, u32>> meta_addrs(raws.size());
    for (size_t i = 0; i < raws.size(); ++i) {
        if (new_metas[i].empty()) {
            meta_addrs[i] = {0, 0};
        } else {
            if (new_metas[i].size() > std::numeric_limits<u32>::max()) {
                return fail(EditResult::IoError, "package metadata is too large");
            }
            meta_addrs[i] = {cursor, static_cast<u32>(new_metas[i].size())};
            cursor += new_metas[i].size();
        }
    }
    const u64 new_file_size = cursor;

    PackProgress eprogress{};
    eprogress.bytes_total = new_file_size;
    eprogress.files_total = 1;
    u64 last_write_report = 0;
    if (progress_cb) {
        progress_cb(eprogress);
    }

    std::vector<u8> new_toc = toc;
    pos = TocHeaderSize;
    for (size_t i = 0; i < raws.size(); ++i) {
        for (size_t b = 0; b < 8; ++b) {
            new_toc[pos + 0x20 + b] = static_cast<u8>(meta_addrs[i].first >> (8 * b));
        }
        for (size_t b = 0; b < 4; ++b) {
            new_toc[pos + 0x28 + b] =
                static_cast<u8>(meta_addrs[i].second >> (8 * b));
        }
        pos += PackageRecordSize;
    }

    std::array<u8, 32> toc_hash{};
    unsigned int hash_len{};
    EVP_Digest(new_toc.data(), new_toc.size(), toc_hash.data(), &hash_len, EVP_sha256(), nullptr);
    if (hash_len != toc_hash.size()) {
        return fail(EditResult::IoError, "cannot hash TOC");
    }

    std::vector<u8> new_super = super;
    std::memcpy(new_super.data() + 0x18, toc_hash.data(), toc_hash.size());
    for (size_t b = 0; b < 8; ++b) {
        new_super[0x40 + b] = static_cast<u8>(new_file_size >> (8 * b));
    }

    const fs::path part = WithSuffix(archive, ".part");
    if (fs::exists(part, ec)) {
        return fail(EditResult::IoError,
                    "temporary file already exists: " + ToUtf8(part));
    }
    Common::FS::IOFile out{part, Common::FS::FileAccessMode::Write};
    if (!out.IsOpen()) {
        return fail(EditResult::IoError, "cannot create " + ToUtf8(part));
    }
    const auto write_all = [&](const u8* data, size_t size) {
        if (out.WriteSpan(std::span<const u8>{data, size}) != size) {
            throw std::runtime_error("write failed, the disk may be full");
        }
        eprogress.bytes_done += size;
        if (eprogress.bytes_done - last_write_report >= ProgressStep && progress_cb) {
            last_write_report = eprogress.bytes_done;
            progress_cb(eprogress);
        }
    };
    try {
        write_all(new_super.data(), new_super.size());
        write_all(new_toc.data(), new_toc.size());
        // Extent offsets are absolute: preserve any padding between the TOC and the blob
        // region instead of shifting the blob down.
        const u64 toc_end = SuperblockSize + new_toc.size();
        if (toc_end > blob_offset) {
            throw std::runtime_error("rewritten TOC overlaps the blob region");
        }
        u64 gap = blob_offset - toc_end;
        static constexpr std::array<u8, 4096> zeros{};
        while (gap > 0) {
            const size_t chunk = static_cast<size_t>(std::min<u64>(gap, zeros.size()));
            write_all(zeros.data(), chunk);
            gap -= chunk;
        }
        std::vector<u8> buffer(ReadBufferSize);
        u64 remaining = meta_start - blob_offset;
        if (!in.Seek(static_cast<s64>(blob_offset))) {
            throw std::runtime_error("cannot seek to blob");
        }
        while (remaining > 0) {
            if (cancel && cancel->load()) {
                out.Close();
                std::error_code rm_ec;
                fs::remove(part, rm_ec);
                return EditResult::Cancelled;
            }
            const size_t chunk = static_cast<size_t>(std::min<u64>(buffer.size(), remaining));
            const size_t got = in.ReadSpan(std::span<u8>{buffer.data(), chunk});
            if (got == 0) {
                throw std::runtime_error("short read of blob");
            }
            write_all(buffer.data(), got);
            remaining -= got;
        }
        for (const auto& block : new_metas) {
            if (!block.empty()) {
                write_all(block.data(), block.size());
            }
        }
        eprogress.files_done = 1;
        if (progress_cb) {
            progress_cb(eprogress);
        }
        if (!out.SetSize(new_file_size) || !out.Commit()) {
            throw std::runtime_error("cannot finish " + ToUtf8(part));
        }
    } catch (const std::exception& e) {
        out.Close();
        std::error_code rm_ec;
        fs::remove(part, rm_ec);
        return fail(EditResult::IoError, e.what());
    }
    out.Close();
    in.Close();

    {
        RealVfsFilesystem check_vfs;
        const auto check_file =
            check_vfs.OpenFile(Common::FS::PathToUTF8String(part), FileSys::OpenMode::Read);
        FileSys::NupContainer check{check_file};
        if (check.GetStatus() != Loader::ResultStatus::Success) {
            std::error_code rm_ec;
            fs::remove(part, rm_ec);
            return fail(EditResult::IoError, "edited file failed validation");
        }
    }

    std::error_code rename_ec;
    fs::rename(part, archive, rename_ec);
    if (rename_ec) {
        std::error_code rm_ec;
        fs::remove(part, rm_ec);
        return fail(EditResult::IoError, "cannot replace archive: " + rename_ec.message());
    }
    return EditResult::Success;
}


} // namespace FileSys::Nup

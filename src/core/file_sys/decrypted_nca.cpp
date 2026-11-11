// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>

#include <openssl/evp.h>

#include "common/alignment.h"
#include "common/logging.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/decrypted_nca.h"
#include "core/file_sys/fssystem/fssystem_aes_ctr_counter_extended_storage.h"
#include "core/file_sys/fssystem/fssystem_aes_ctr_storage.h"
#include "core/file_sys/fssystem/fssystem_alignment_matching_storage.h"
#include "core/file_sys/fssystem/fssystem_compression_configuration.h"
#include "core/file_sys/fssystem/fssystem_crypto_configuration.h"
#include "core/file_sys/fssystem/fssystem_nca_file_system_driver.h"
#include "core/file_sys/fssystem/fssystem_sparse_storage.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_concat.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_static.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

namespace FileSys {

namespace {

using EncryptionType = NcaFsHeader::EncryptionType;

template <typename T>
void Put(std::vector<u8>& out, std::size_t at, T value) {
    std::memcpy(out.data() + at, &value, sizeof(T));
}

constexpr std::size_t HeaderSize = NcaHeader::Size + NcaHeader::FsCountMax * NcaFsHeader::Size;

// The bytes of one section, from several storages over the same section offsets. A later
// Set() wins where ranges overlap; uncovered bytes come from the raw section.
class Pieces {
public:
    void Set(s64 begin, s64 end, VirtualFile source) {
        if (end > begin) {
            list.push_back({begin, end, std::move(source)});
        }
    }

    VirtualFile Build(std::string name, s64 size, const VirtualFile& raw) const {
        // begin -> (end, source), non-overlapping.
        std::map<s64, std::pair<s64, VirtualFile>> cover;
        for (const auto& piece : list) {
            const s64 begin = std::max<s64>(piece.begin, 0);
            const s64 end = std::min(piece.end, size);
            if (end <= begin) {
                continue;
            }
            std::map<s64, std::pair<s64, VirtualFile>> kept;
            for (const auto& [b, range] : cover) {
                if (range.first <= begin || b >= end) {
                    kept.emplace(b, range);
                    continue;
                }
                if (b < begin) {
                    kept.emplace(b, std::make_pair(begin, range.second));
                }
                if (range.first > end) {
                    kept.emplace(end, range);
                }
            }
            kept.emplace(begin, std::make_pair(end, piece.source));
            cover = std::move(kept);
        }
        std::vector<VirtualFile> parts;
        s64 at = 0;
        for (const auto& [begin, range] : cover) {
            if (begin > at) {
                parts.push_back(std::make_shared<OffsetVfsFile>(raw, begin - at, at));
            }
            parts.push_back(
                std::make_shared<OffsetVfsFile>(range.second, range.first - begin, begin));
            at = range.first;
        }
        if (at < size) {
            parts.push_back(std::make_shared<OffsetVfsFile>(raw, size - at, at));
        }
        return ConcatenatedVfsFile::MakeConcatenatedFile(std::move(name), std::move(parts));
    }

private:
    struct Piece {
        s64 begin;
        s64 end;
        VirtualFile source;
    };
    std::vector<Piece> list;
};

bool Sha256(const void* data, std::size_t size, Hash& out) {
    unsigned int length = 0;
    return EVP_Digest(data, size, out.value.data(), &length, EVP_sha256(), nullptr) == 1 &&
           length == out.value.size();
}

using RightsId = std::array<u8, NcaHeader::RightsIdSize>;

bool SetTitleKey(NcaReader& reader, std::string& error) {
    RightsId rights_id{};
    reader.GetRightsId(rights_id.data(), rights_id.size());
    if (rights_id == RightsId{}) {
        return true;
    }
    auto& keys = Core::Crypto::KeyManager::Instance();
    u128 rights_id_u128;
    std::memcpy(rights_id_u128.data(), rights_id.data(), sizeof(rights_id));
    auto titlekey =
        keys.GetKey(Core::Crypto::S128KeyType::Titlekey, rights_id_u128[1], rights_id_u128[0]);
    const u8 master_key_id = std::max<u8>(reader.GetKeyGeneration(), 1) - 1;
    if (titlekey == Core::Crypto::Key128{}) {
        error = "missing title key (ticket)";
        return false;
    }
    if (!keys.HasKey(Core::Crypto::S128KeyType::Titlekek, master_key_id)) {
        error = fmt::format("missing titlekek for key generation {}", reader.GetKeyGeneration());
        return false;
    }
    const auto titlekek = keys.GetKey(Core::Crypto::S128KeyType::Titlekek, master_key_id);
    Core::Crypto::AESCipher<Core::Crypto::Key128> cipher(titlekek, Core::Crypto::Mode::ECB);
    cipher.Transcode(titlekey.data(), titlekey.size(), titlekey.data(), Core::Crypto::Op::Decrypt);
    reader.SetExternalDecryptionKey(titlekey.data(), titlekey.size());
    return true;
}

const void* CtrKey(const NcaReader& reader) {
    return reader.HasExternalDecryptionKey()
               ? reader.GetExternalDecryptionKey()
               : reader.GetDecryptionKey(NcaHeader::DecryptionKey_AesCtr);
}

// The section's base AES-CTR stream, readable at any offset.
VirtualFile BaseCtr(const NcaReader& reader, VirtualFile section, s64 fs_offset,
                    const NcaAesCtrUpperIv& upper_iv) {
    std::array<u8, AesCtrStorage::IvSize> iv{};
    AesCtrStorage::MakeIv(iv.data(), iv.size(), upper_iv.value, fs_offset);
    auto ctr = std::make_shared<AesCtrStorage>(std::move(section), CtrKey(reader),
                                               AesCtrStorage::KeySize, iv.data(), iv.size());
    return std::make_shared<AlignmentMatchingStorage<NcaHeader::CtrBlockSize, 1>>(std::move(ctr));
}

// Marks a flattened sparse section in its reserved FS header bytes, as NXConverter does: its
// holes hold no data (a sparse base is only read through its update), not damage.
constexpr std::size_t SparseMarkerOffset = 0x1D0;
constexpr std::array<u8, 4> SparseMarker{'D', 'S', 'P', 'F'};

// A sparse section as its dense ciphertext: the physical extents where the sparse table maps
// them, zeros elsewhere (the console applies SparseStorage before the section crypto).
// The holes are added to `holes` as (begin, end) section offsets.
VirtualFile DenseSparseSection(const NcaReader& reader, const NcaFsHeaderReader& header,
                               const VirtualFile& nca, s64 fs_size,
                               std::vector<std::pair<s64, s64>>& holes, std::string& error) {
    const auto& info = header.GetSparseInfo();
    BucketTree::Header table{};
    std::memcpy(&table, info.bucket.header.data(), sizeof(table));
    if (info.generation == 0 || R_FAILED(table.Verify())) {
        error = "invalid sparse table";
        return nullptr;
    }
    auto sparse = std::make_shared<SparseStorage>();
    if (table.entry_count == 0) {
        sparse->Initialize(fs_size);
        holes.emplace_back(0, fs_size);
        return sparse;
    }
    const s64 physical_offset = info.physical_offset;
    const s64 physical_size = info.GetPhysicalSize();
    if (physical_offset < 0 || info.bucket.offset < 0 || info.bucket.size <= 0 ||
        physical_offset + physical_size > static_cast<s64>(nca->GetSize())) {
        error = "sparse data is out of range";
        return nullptr;
    }
    auto body = std::make_shared<OffsetVfsFile>(nca, physical_size, physical_offset);

    // The table is AES-CTR encrypted with the sparse generation in its IV.
    std::array<u8, AesCtrStorage::IvSize> iv{};
    AesCtrStorage::MakeIv(iv.data(), iv.size(),
                          info.MakeAesCtrUpperIv(header.GetAesCtrUpperIv()).value,
                          physical_offset + info.bucket.offset);
    auto ctr = std::make_shared<AesCtrStorage>(
        std::make_shared<OffsetVfsFile>(body, info.bucket.size, info.bucket.offset), CtrKey(reader),
        AesCtrStorage::KeySize, iv.data(), iv.size());
    const auto aligned =
        std::make_shared<AlignmentMatchingStorage<NcaHeader::CtrBlockSize, 1>>(std::move(ctr));
    std::vector<u8> meta(static_cast<std::size_t>(info.bucket.size));
    if (aligned->Read(meta.data(), meta.size(), 0) != meta.size()) {
        error = "cannot read the sparse table";
        return nullptr;
    }
    const auto node_size = SparseStorage::QueryNodeStorageSize(table.entry_count);
    const auto entry_size = SparseStorage::QueryEntryStorageSize(table.entry_count);
    if (node_size + entry_size > static_cast<s64>(meta.size())) {
        error = "sparse table is truncated";
        return nullptr;
    }
    auto meta_file = std::make_shared<VectorVfsFile>(std::move(meta));
    if (R_FAILED(
            sparse->Initialize(std::make_shared<OffsetVfsFile>(meta_file, node_size, 0),
                               std::make_shared<OffsetVfsFile>(meta_file, entry_size, node_size),
                               table.entry_count))) {
        error = "invalid sparse table";
        return nullptr;
    }
    sparse->SetDataStorage(std::make_shared<OffsetVfsFile>(body, physical_size, 0));
    if (static_cast<s64>(sparse->GetSize()) != fs_size) {
        error = "sparse table does not cover the section";
        return nullptr;
    }

    std::vector<IndirectStorage::Entry> entries(table.entry_count);
    s32 count = 0;
    if (R_FAILED(sparse->GetEntryList(entries.data(), &count, table.entry_count, 0, fs_size))) {
        error = "cannot list the sparse table";
        return nullptr;
    }
    for (s32 i = 0; i < count; ++i) {
        if (entries[i].storage_index != 1) {
            continue;
        }
        const s64 begin = entries[i].GetVirtualOffset();
        const s64 end = i + 1 < count ? entries[i + 1].GetVirtualOffset() : fs_size;
        holes.emplace_back(begin, end);
    }
    return sparse;
}

// One section, decrypted the way the update and base readers of the console decrypt it.
// A sparse section is given here as its dense ciphertext.
VirtualFile DecryptSection(const NcaReader& reader, const NcaFsHeaderReader& header,
                           VirtualFile raw, s64 fs_offset, std::string name, std::string& error) {
    const s64 size = static_cast<s64>(raw->GetSize());
    const auto type = header.GetEncryptionType();
    if (type == EncryptionType::None) {
        return raw;
    }
    s64 hash_target = 0;
    if (header.IsSkipLayerHashEncryption() && R_FAILED(header.GetHashTargetOffset(&hash_target))) {
        error = "invalid hash layer";
        return nullptr;
    }
    const auto upper_iv = header.GetAesCtrUpperIv();
    Pieces pieces;
    switch (type) {
    case EncryptionType::AesCtr:
        pieces.Set(0, size, BaseCtr(reader, raw, fs_offset, upper_iv));
        break;
    case EncryptionType::AesCtrSkipLayerHash:
        pieces.Set(hash_target, size, BaseCtr(reader, raw, fs_offset, upper_iv));
        break;
    case EncryptionType::AesCtrEx:
    case EncryptionType::AesCtrExSkipLayerHash: {
        const auto& patch = header.GetPatchInfo();
        const auto base_ctr = BaseCtr(reader, raw, fs_offset, upper_iv);
        if (patch.HasAesCtrExTable()) {
            BucketTree::Header table{};
            std::memcpy(&table, patch.aes_ctr_ex_header.data(), sizeof(table));
            if (R_FAILED(table.Verify()) || patch.aes_ctr_ex_offset > size) {
                error = "invalid AES-CTR-EX table";
                return nullptr;
            }
            const auto meta_size =
                Common::AlignUp(static_cast<s64>(patch.aes_ctr_ex_size), NcaHeader::XtsBlockSize);
            std::vector<u8> meta(meta_size);
            if (patch.aes_ctr_ex_offset + meta_size > size ||
                base_ctr->Read(meta.data(), meta.size(), patch.aes_ctr_ex_offset) != meta.size()) {
                error = "AES-CTR-EX table out of range";
                return nullptr;
            }
            auto meta_storage = std::make_shared<VectorVfsFile>(std::move(meta));
            const auto node_size =
                AesCtrCounterExtendedStorage::QueryNodeStorageSize(table.entry_count);
            const auto entry_size =
                AesCtrCounterExtendedStorage::QueryEntryStorageSize(table.entry_count);
            std::unique_ptr<AesCtrCounterExtendedStorage::IDecryptor> decryptor;
            auto ex = std::make_shared<AesCtrCounterExtendedStorage>();
            if (R_FAILED(AesCtrCounterExtendedStorage::CreateSoftwareDecryptor(&decryptor)) ||
                R_FAILED(ex->Initialize(
                    CtrKey(reader), AesCtrStorage::KeySize, upper_iv.part.secure_value, fs_offset,
                    std::make_shared<OffsetVfsFile>(raw, patch.aes_ctr_ex_offset, 0),
                    std::make_shared<OffsetVfsFile>(meta_storage, node_size, 0),
                    std::make_shared<OffsetVfsFile>(meta_storage, entry_size, node_size),
                    table.entry_count, std::move(decryptor)))) {
                error = "cannot read the AES-CTR-EX table";
                return nullptr;
            }
            pieces.Set(0, patch.aes_ctr_ex_offset,
                       std::make_shared<AlignmentMatchingStorage<NcaHeader::CtrBlockSize, 1>>(
                           std::move(ex)));
            pieces.Set(patch.aes_ctr_ex_offset, patch.aes_ctr_ex_offset + patch.aes_ctr_ex_size,
                       base_ctr);
        }
        // Patch metadata is in the base stream: one range through the metadata hash data, or
        // the indirect table alone on older layouts.
        if (header.ExistsPatchMetaHashLayer() && patch.HasIndirectTable()) {
            const auto& meta = header.GetPatchMetaDataHashDataInfo();
            pieces.Set(patch.indirect_offset,
                       Common::AlignUp<s64>(meta.offset + meta.size, NcaHeader::CtrBlockSize),
                       base_ctr);
        } else if (patch.HasIndirectTable()) {
            pieces.Set(patch.indirect_offset, patch.indirect_offset + patch.indirect_size,
                       base_ctr);
        }
        if (!patch.HasAesCtrExTable()) {
            pieces.Set(hash_target, size, base_ctr);
        }
        if (type == EncryptionType::AesCtrExSkipLayerHash) {
            pieces.Set(0, hash_target, raw);
        }
        break;
    }
    default:
        error = fmt::format("unsupported NCA encryption type {}", static_cast<int>(type));
        return nullptr;
    }
    return pieces.Build(std::move(name), size, raw);
}

struct Hfs0Entry {
    s64 offset; // In the gamecard image.
    s64 size;
    std::string name;
};

// The files of the HFS0 at `offset`, which must end at or before `end`.
std::optional<std::vector<Hfs0Entry>> ReadHfs0(const VirtualFile& image, s64 offset, s64 end) {
    std::array<u8, 0x10> head{};
    if (offset < 0 || offset + 0x10 > end ||
        image->ReadBytes(head.data(), head.size(), offset) != head.size() ||
        std::memcmp(head.data(), "HFS0", 4) != 0) {
        return std::nullopt;
    }
    u32 count{};
    u32 strings_size{};
    std::memcpy(&count, head.data() + 4, 4);
    std::memcpy(&strings_size, head.data() + 8, 4);
    constexpr std::size_t EntrySize = 0x40;
    if (count > 0x10000 || strings_size > 0x100000) {
        return std::nullopt;
    }
    const std::size_t table_size = count * EntrySize;
    const s64 data = offset + 0x10 + static_cast<s64>(table_size + strings_size);
    std::vector<u8> table(table_size + strings_size);
    if (data > end || image->ReadBytes(table.data(), table.size(), offset + 0x10) != table.size()) {
        return std::nullopt;
    }
    std::vector<Hfs0Entry> entries;
    for (std::size_t i = 0; i < count; ++i) {
        u64 relative{};
        u64 size{};
        u32 name_offset{};
        std::memcpy(&relative, table.data() + i * EntrySize, 8);
        std::memcpy(&size, table.data() + i * EntrySize + 8, 8);
        std::memcpy(&name_offset, table.data() + i * EntrySize + 16, 4);
        if (name_offset >= strings_size || relative > static_cast<u64>(end - data) ||
            size > static_cast<u64>(end - data) - relative) {
            return std::nullopt;
        }
        const auto* name = reinterpret_cast<const char*>(table.data() + table_size + name_offset);
        const auto length = strnlen(name, strings_size - name_offset);
        if (length == strings_size - name_offset) {
            return std::nullopt;
        }
        entries.push_back(
            {data + static_cast<s64>(relative), static_cast<s64>(size), std::string(name, length)});
    }
    return entries;
}

} // namespace

VirtualFile BuildPartitionFile(std::string name, const std::vector<VirtualFile>& files) {
    std::vector<u8> strings;
    for (const auto& file : files) {
        const auto file_name = file->GetName();
        strings.insert(strings.end(), file_name.begin(), file_name.end());
        strings.push_back(0);
    }
    constexpr std::size_t PfsHeaderSize = 0x10;
    constexpr std::size_t EntrySize = 0x18;
    const std::size_t fixed = PfsHeaderSize + EntrySize * files.size();
    strings.resize(Common::AlignUp(fixed + strings.size(), 0x20) - fixed);

    std::vector<u8> header(fixed + strings.size());
    std::memcpy(header.data(), "PFS0", 4);
    Put<u32>(header, 4, static_cast<u32>(files.size()));
    Put<u32>(header, 8, static_cast<u32>(strings.size()));

    u64 offset = 0;
    u32 name_offset = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto at = PfsHeaderSize + EntrySize * i;
        const u64 size = files[i]->GetSize();
        Put<u64>(header, at, offset);
        Put<u64>(header, at + 8, size);
        Put<u32>(header, at + 16, name_offset);
        offset += size;
        name_offset += static_cast<u32>(files[i]->GetName().size() + 1);
    }
    std::memcpy(header.data() + fixed, strings.data(), strings.size());

    std::vector<VirtualFile> parts;
    parts.reserve(files.size() + 1);
    parts.push_back(std::make_shared<VectorVfsFile>(std::move(header), "header"));
    for (const auto& file : files) {
        if (file->GetSize() != 0) {
            parts.push_back(file);
        }
    }
    return ConcatenatedVfsFile::MakeConcatenatedFile(std::move(name), std::move(parts));
}

VirtualFile BuildAddOnContentNca(u64 title_id, VirtualFile romfs) {
    const u64 romfs_size = romfs->GetSize();
    const u64 section_size = Common::AlignUp(romfs_size, NcaHeader::SectorSize);

    NcaFsHeader fs{};
    fs.version = 2;
    fs.fs_type = NcaFsHeader::FsType::RomFs;
    fs.hash_type = NcaFsHeader::HashType::None;
    fs.encryption_type = NcaFsHeader::EncryptionType::None;

    NcaHeader nca{};
    nca.magic = NcaHeader::MagicDecrypted;
    nca.content_type = NcaHeader::ContentType::PublicData;
    nca.content_size = HeaderSize + section_size;
    nca.program_id = title_id;
    nca.sdk_addon_version = 0x000B0000;
    nca.fs_info[0] = {NcaHeader::ByteToSector(HeaderSize),
                      NcaHeader::ByteToSector(HeaderSize + section_size), 0, 0};
    if (!Sha256(&fs, sizeof(fs), nca.fs_header_hash[0])) {
        return nullptr;
    }

    std::vector<u8> header(HeaderSize);
    std::memcpy(header.data(), &nca, sizeof(nca));
    std::memcpy(header.data() + NcaHeader::Size, &fs, sizeof(fs));
    std::vector<VirtualFile> parts{std::make_shared<VectorVfsFile>(std::move(header), "header"),
                                   std::move(romfs)};
    if (section_size > romfs_size) {
        parts.push_back(std::make_shared<StaticVfsFile>(u8{0}, section_size - romfs_size));
    }
    return ConcatenatedVfsFile::MakeConcatenatedFile(fmt::format("{:016x}.nca", title_id),
                                                     std::move(parts));
}

VirtualFile OpenDecryptedNca(VirtualFile nca, std::string& error) {
    if (nca == nullptr || nca->GetSize() < HeaderSize) {
        error = "not an NCA";
        return nullptr;
    }
    NcaReader reader;
    if (R_FAILED(
            reader.Initialize(nca, GetCryptoConfiguration(), GetNcaCompressionConfiguration()))) {
        error = "cannot read the NCA header (missing or wrong keys?)";
        return nullptr;
    }
    if (reader.GetMagic() == NcaHeader::MagicDecrypted) {
        return nca;
    }
    if (!SetTitleKey(reader, error)) {
        return nullptr;
    }

    std::vector<u8> header(HeaderSize);
    reader.GetRawData(header.data(), NcaHeader::Size);
    NcaHeader fixed{};
    std::memcpy(&fixed, header.data(), sizeof(fixed));
    fixed.magic = NcaHeader::MagicDecrypted;

    std::vector<std::pair<s64, VirtualFile>> sections;
    s64 dense_size = 0;
    bool any_sparse = false;
    for (s32 i = 0; i < NcaHeader::FsCountMax; ++i) {
        if (!reader.HasFsInfo(i)) {
            fixed.fs_header_hash[i] = {};
            continue;
        }
        NcaFsHeaderReader fs_header;
        if (R_FAILED(fs_header.Initialize(reader, i))) {
            error = fmt::format("invalid FS header {}", i);
            return nullptr;
        }
        const auto offset = static_cast<s64>(reader.GetFsOffset(i));
        const auto end = static_cast<s64>(reader.GetFsEndOffset(i));
        // A sparse section is larger than its stored bytes: the update brings what was cut.
        const bool sparse = fs_header.ExistsSparseLayer();
        if (end <= offset || (!sparse && static_cast<u64>(end) > nca->GetSize())) {
            error = fmt::format("FS section {} is out of range", i);
            return nullptr;
        }
        dense_size = std::max(dense_size, end);
        std::vector<std::pair<s64, s64>> holes;
        VirtualFile raw;
        if (sparse) {
            any_sparse = true;
            raw = DenseSparseSection(reader, fs_header, nca, end - offset, holes, error);
            if (raw == nullptr) {
                error = fmt::format("FS section {}: {}", i, error);
                return nullptr;
            }
        } else {
            raw = std::make_shared<OffsetVfsFile>(nca, end - offset, offset);
        }
        auto plain = DecryptSection(reader, fs_header, std::move(raw), offset,
                                    fmt::format("fs{}", i), error);
        if (plain == nullptr) {
            error = fmt::format("FS section {}: {}", i, error);
            return nullptr;
        }
        if (!holes.empty()) {
            // Decryption turned the zero ciphertext of the holes into keystream: put zeros
            // back, as NXConverter does (no data, and they compress to nothing).
            Pieces zeroed;
            const auto zeros = std::make_shared<StaticVfsFile>(u8{0}, end - offset);
            for (const auto& [begin, hole_end] : holes) {
                zeroed.Set(begin, hole_end, zeros);
            }
            plain = zeroed.Build(fmt::format("fs{}", i), end - offset, plain);
        }
        sections.emplace_back(offset, std::move(plain));

        NcaFsHeader fs{};
        fs_header.GetRawData(&fs, sizeof(fs));
        fs.encryption_type = EncryptionType::None;
        if (sparse) {
            auto* bytes = reinterpret_cast<u8*>(&fs);
            // SparseInfo is flattened: drop it, and the sparse-only metadata hash record.
            std::memset(bytes + 0x148, 0, 0x30);
            if (bytes[5] != 0 && !fs_header.GetPatchInfo().HasIndirectTable()) {
                bytes[5] = 0;
                std::memset(bytes + 0x1A0, 0, 0x30);
            }
            if (std::any_of(bytes + SparseMarkerOffset,
                            bytes + SparseMarkerOffset + SparseMarker.size(),
                            [](u8 b) { return b != 0; })) {
                error = fmt::format("FS section {}: reserved bytes are not empty", i);
                return nullptr;
            }
            std::memcpy(bytes + SparseMarkerOffset, SparseMarker.data(), SparseMarker.size());
        }
        std::memcpy(header.data() + NcaHeader::Size + i * NcaFsHeader::Size, &fs, sizeof(fs));
        if (!Sha256(&fs, sizeof(fs), fixed.fs_header_hash[i])) {
            error = "SHA-256 failed";
            return nullptr;
        }
    }
    if (any_sparse) {
        fixed.content_size = static_cast<u64>(dense_size);
    }
    std::memcpy(header.data(), &fixed, sizeof(fixed));

    std::sort(sections.begin(), sections.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<VirtualFile> parts;
    parts.push_back(std::make_shared<VectorVfsFile>(std::move(header), "header"));
    s64 at = static_cast<s64>(HeaderSize);
    for (auto& [offset, plain] : sections) {
        if (offset < at) {
            error = "overlapping NCA sections";
            return nullptr;
        }
        if (offset > at) {
            if (static_cast<u64>(offset) > nca->GetSize()) {
                parts.push_back(std::make_shared<StaticVfsFile>(u8{0}, offset - at));
            } else {
                parts.push_back(std::make_shared<OffsetVfsFile>(nca, offset - at, at));
            }
        }
        at = offset + static_cast<s64>(plain->GetSize());
        parts.push_back(std::move(plain));
    }
    // A sparse NCA's stored bytes are its sections' physical data: nothing follows them.
    if (!any_sparse && static_cast<u64>(at) < nca->GetSize()) {
        parts.push_back(std::make_shared<OffsetVfsFile>(nca, nca->GetSize() - at, at));
    }
    return ConcatenatedVfsFile::MakeConcatenatedFile(nca->GetName(), std::move(parts));
}

VirtualFile OpenDecryptedPackage(VirtualFile package, std::string& error) {
    if (package == nullptr) {
        error = "no package";
        return nullptr;
    }
    std::vector<VirtualFile> files;
    auto pfs = std::make_shared<PartitionFilesystem>(package);
    if (pfs->GetStatus() == Loader::ResultStatus::Success) {
        files = pfs->GetFiles();
    } else {
        XCI xci{package};
        const auto secure = xci.GetSecurePartition();
        if (secure == nullptr) {
            error = "not an NSP or XCI";
            return nullptr;
        }
        files = secure->GetFiles();
    }
    for (const auto& file : files) {
        if (file->GetExtension() == "tik" &&
            !Core::Crypto::KeyManager::Instance().AddTicket(Core::Crypto::Ticket::Read(file))) {
            LOG_WARNING(Common_Filesystem, "Could not load ticket {}", file->GetName());
        }
    }
    for (auto& file : files) {
        const auto name = file->GetName();
        if (name.ends_with(".ncz")) {
            error = "compressed NCZ content is not supported: decompress the package first";
            return nullptr;
        }
        if (name.ends_with(".nca")) {
            auto plain = OpenDecryptedNca(file, error);
            if (plain == nullptr) {
                error = fmt::format("{}: {}", name, error);
                return nullptr;
            }
            file = std::move(plain);
        }
    }
    return BuildPartitionFile(package->GetName(), files);
}

VirtualFile OpenDecryptedGamecard(VirtualFile image, std::string& error) {
    std::array<u8, 0x138> header{};
    if (image == nullptr || image->ReadBytes(header.data(), header.size(), 0) != header.size() ||
        std::memcmp(header.data() + 0x100, "HEAD", 4) != 0) {
        error = "not a gamecard image";
        return nullptr;
    }
    const auto size = static_cast<s64>(image->GetSize());
    u64 root{};
    std::memcpy(&root, header.data() + 0x130, 8);
    const auto partitions = root < static_cast<u64>(size)
                                ? ReadHfs0(image, static_cast<s64>(root), size)
                                : std::nullopt;
    if (!partitions) {
        error = "invalid gamecard partitions";
        return nullptr;
    }

    // Offset in the image -> what replaces the bytes there, of the same size.
    std::map<s64, VirtualFile> replaced;
    replaced.emplace(0x100,
                     std::make_shared<VectorVfsFile>(std::vector<u8>{'D', 'X', 'C', 'I'}, "magic"));
    // The files of every partition but update, which is the console firmware and stays as it is.
    std::vector<Hfs0Entry> files;
    for (const auto& partition : *partitions) {
        if (partition.name == "update") {
            continue;
        }
        const auto entries = ReadHfs0(image, partition.offset, partition.offset + partition.size);
        if (!entries) {
            error = fmt::format("invalid {} partition", partition.name);
            return nullptr;
        }
        files.insert(files.end(), entries->begin(), entries->end());
    }
    // Tickets first: an NCA may come before its ticket.
    for (const auto& entry : files) {
        if (entry.name.ends_with(".tik") &&
            !Core::Crypto::KeyManager::Instance().AddTicket(Core::Crypto::Ticket::Read(
                std::make_shared<OffsetVfsFile>(image, entry.size, entry.offset)))) {
            LOG_WARNING(Common_Filesystem, "Could not load ticket {}", entry.name);
        }
    }
    for (const auto& entry : files) {
        if (entry.name.ends_with(".ncz")) {
            error = "compressed NCZ content is not supported: decompress the package first";
            return nullptr;
        }
        if (!entry.name.ends_with(".nca")) {
            continue;
        }
        auto plain = OpenDecryptedNca(
            std::make_shared<OffsetVfsFile>(image, entry.size, entry.offset, entry.name), error);
        if (plain == nullptr) {
            error = fmt::format("{}: {}", entry.name, error);
            return nullptr;
        }
        // A sparse NCA grows when decrypted; it cannot keep its place in the image.
        if (plain->GetSize() != static_cast<std::size_t>(entry.size)) {
            error = fmt::format("{}: the decrypted NCA has another size (sparse NCA)", entry.name);
            return nullptr;
        }
        replaced.insert_or_assign(entry.offset, std::move(plain));
    }

    std::vector<VirtualFile> parts;
    s64 at = 0;
    for (auto& [offset, file] : replaced) {
        if (offset < at) {
            error = "overlapping gamecard files";
            return nullptr;
        }
        if (offset > at) {
            parts.push_back(std::make_shared<OffsetVfsFile>(image, offset - at, at));
        }
        at = offset + static_cast<s64>(file->GetSize());
        parts.push_back(std::move(file));
    }
    if (at < size) {
        parts.push_back(std::make_shared<OffsetVfsFile>(image, size - at, at));
    }
    return ConcatenatedVfsFile::MakeConcatenatedFile(image->GetName(), std::move(parts));
}

} // namespace FileSys

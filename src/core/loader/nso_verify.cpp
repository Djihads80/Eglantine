// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/loader/nso_verify.h"

#include <algorithm>
#include <span>
#include <vector>

#include <openssl/evp.h>

#include "common/common_funcs.h"
#include "common/lz4_compression.h"
#include "common/zbic_compression.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/loader/nso.h"

namespace Loader {

namespace {

// Larger than any real segment; stops a corrupt header from asking for gigabytes.
constexpr u64 MaxSegmentSize = u64{1} << 30;

bool Sha256(const std::vector<u8>& data, NSOHeader::SHA256Hash& out) {
    unsigned int length = 0;
    return EVP_Digest(data.data(), data.size(), out.data(), &length, EVP_sha256(), nullptr) == 1 &&
           length == out.size();
}

NSOHashStatus CheckSegment(const FileSys::VfsFile& file, const NSOHeader& header, std::size_t index) {
    const auto& segment = header.segments[index];
    const u64 stored_size = header.segments_compressed_size[index];
    const u64 file_size = file.GetSize();
    if (segment.size > MaxSegmentSize || stored_size > MaxSegmentSize || segment.offset > file_size ||
        stored_size > file_size - segment.offset) {
        return NSOHashStatus::Unreadable;
    }
    std::vector<u8> stored(static_cast<std::size_t>(stored_size));
    if (file.Read(stored.data(), stored.size(), segment.offset) != stored.size()) {
        return NSOHashStatus::Unreadable;
    }

    std::vector<u8> contents;
    if (header.IsSegmentCompressed(index)) {
        contents.resize(segment.size);
        if (header.IsZBICCompressed()) {
            if (Common::Compression::DecompressDataZBIC(std::span<u8>{contents}, std::span<const u8>{stored}) <= 0) {
                return NSOHashStatus::Unreadable;
            }
        } else if (Common::Compression::DecompressDataLZ4(contents.data(), contents.size(), stored.data(),
                                                           stored.size()) != static_cast<int>(segment.size)) {
            return NSOHashStatus::Unreadable;
        }
    } else {
        if (stored.size() < segment.size) {
            return NSOHashStatus::Unreadable;
        }
        stored.resize(segment.size);
        contents = std::move(stored);
    }

    NSOHeader::SHA256Hash actual{};
    if (!Sha256(contents, actual)) {
        return NSOHashStatus::Unreadable;
    }
    return actual == header.segment_hashes[index] ? NSOHashStatus::Match : NSOHashStatus::Mismatch;
}

} // namespace

bool NSOHashReport::AllMatch() const {
    return header_valid && std::ranges::all_of(segments, [](const auto& s) { return s.status == NSOHashStatus::Match; });
}

bool NSOHashReport::FlaggedMatch() const {
    return header_valid && std::ranges::all_of(segments, [](const auto& s) {
               return !s.flagged || s.status == NSOHashStatus::Match;
           });
}

NSOHashReport VerifyNSOSegmentHashes(const FileSys::VfsFile& nso_file) {
    NSOHashReport report;
    NSOHeader header{};
    if (nso_file.GetSize() < sizeof(NSOHeader) || nso_file.ReadObject(&header) != sizeof(NSOHeader) ||
        header.magic != Common::MakeMagic('N', 'S', 'O', '0')) {
        return report;
    }
    report.header_valid = true;
    for (std::size_t i = 0; i < report.segments.size(); ++i) {
        report.segments[i].flagged = ((header.flags >> (3 + i)) & 1) != 0;
        report.segments[i].status = CheckSegment(nso_file, header, i);
    }
    return report;
}

} // namespace Loader

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/zstd_compression.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/crypto/ncz_decompressor.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {

using Core::Crypto::Key128;

constexpr std::size_t NcaHeaderSize = 0x4000;
constexpr std::size_t SectionSize = 0x4000;
constexpr std::size_t NcaSize = NcaHeaderSize + 2 * SectionSize;
constexpr Key128 DummyKey{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                          0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
constexpr Key128 DummyCounter{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                              0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7};

void PutLe(std::vector<u8>& out, u64 value, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i) {
        out.push_back(static_cast<u8>(value >> (8 * i)));
    }
}

void PutMagic(std::vector<u8>& out, const char (&magic)[9]) {
    out.insert(out.end(), magic, magic + 8);
}

void PutSection(std::vector<u8>& out, u64 offset, u64 size, u64 crypto_type) {
    PutLe(out, offset, 8);
    PutLe(out, size, 8);
    PutLe(out, crypto_type, 8);
    PutLe(out, 0, 8);
    out.insert(out.end(), DummyKey.begin(), DummyKey.end());
    out.insert(out.end(), DummyCounter.begin(), DummyCounter.end());
}

struct SyntheticNcz {
    std::vector<u8> header;
    std::vector<u8> plain_body;
    std::vector<u8> ncz;
};

SyntheticNcz MakeNcz() {
    SyntheticNcz out;
    out.header.resize(NcaHeaderSize);
    for (std::size_t i = 0; i < out.header.size(); ++i) {
        out.header[i] = static_cast<u8>(i * 7 + 3);
    }
    out.plain_body.resize(2 * SectionSize);
    for (std::size_t i = 0; i < SectionSize; ++i) {
        out.plain_body[i] = static_cast<u8>(i % 16);
        out.plain_body[SectionSize + i] = static_cast<u8>(i * 131 + 7);
    }

    const auto compressed =
        Common::Compression::CompressDataZSTDDefault(out.plain_body.data(), SectionSize);
    REQUIRE(!compressed.empty());
    REQUIRE(compressed.size() < SectionSize);

    out.ncz = out.header;
    PutMagic(out.ncz, "NCZSECTN");
    PutLe(out.ncz, 2, 8);
    PutSection(out.ncz, NcaHeaderSize, SectionSize, 3);
    PutSection(out.ncz, NcaHeaderSize + SectionSize, SectionSize, 1);

    PutMagic(out.ncz, "NCZBLOCK");
    out.ncz.insert(out.ncz.end(), {2, 1, 0, 14});
    PutLe(out.ncz, 2, 4);
    PutLe(out.ncz, out.plain_body.size(), 8);
    PutLe(out.ncz, compressed.size(), 4);
    PutLe(out.ncz, SectionSize, 4);
    out.ncz.insert(out.ncz.end(), compressed.begin(), compressed.end());
    out.ncz.insert(out.ncz.end(), out.plain_body.begin() + SectionSize, out.plain_body.end());
    return out;
}

std::vector<u8> ExpectedCiphertextNca(const SyntheticNcz& ncz) {
    std::vector<u8> nca = ncz.header;
    nca.insert(nca.end(), ncz.plain_body.begin(), ncz.plain_body.end());

    Key128 iv = DummyCounter;
    u64 block = NcaHeaderSize >> 4;
    for (std::size_t i = 0; i < 8; ++i) {
        iv[15 - i] = static_cast<u8>(block & 0xFF);
        block >>= 8;
    }
    Core::Crypto::AESCipher<Key128> cipher(DummyKey, Core::Crypto::Mode::CTR);
    cipher.SetIV(iv);
    cipher.Transcode(nca.data() + NcaHeaderSize, SectionSize, nca.data() + NcaHeaderSize,
                     Core::Crypto::Op::Encrypt);
    return nca;
}

SyntheticNcz MakeRawNcz(std::size_t valid_blocks, u64 crypto_type, bool corrupt_tail) {
    SyntheticNcz out;
    out.header.assign(NcaHeaderSize, 0x5A);
    out.plain_body.resize(valid_blocks * SectionSize);
    for (std::size_t i = 0; i < out.plain_body.size(); ++i) {
        out.plain_body[i] = static_cast<u8>(i * 13 + 1);
    }
    const std::vector<u8> corrupt(corrupt_tail ? 0x10 : 0, 0xFF);
    const std::size_t block_count = valid_blocks + (corrupt_tail ? 1 : 0);

    out.ncz = out.header;
    PutMagic(out.ncz, "NCZSECTN");
    PutLe(out.ncz, 1, 8);
    PutSection(out.ncz, NcaHeaderSize, block_count * SectionSize, crypto_type);

    PutMagic(out.ncz, "NCZBLOCK");
    out.ncz.insert(out.ncz.end(), {2, 1, 0, 14});
    PutLe(out.ncz, block_count, 4);
    PutLe(out.ncz, block_count * SectionSize, 8);
    for (std::size_t i = 0; i < valid_blocks; ++i) {
        PutLe(out.ncz, SectionSize, 4);
    }
    if (corrupt_tail) {
        PutLe(out.ncz, corrupt.size(), 4);
    }
    out.ncz.insert(out.ncz.end(), out.plain_body.begin(), out.plain_body.end());
    out.ncz.insert(out.ncz.end(), corrupt.begin(), corrupt.end());
    return out;
}

std::vector<u8> Slice(const std::vector<u8>& data, std::size_t offset, std::size_t length) {
    const auto end = std::min(offset + length, data.size());
    return std::vector<u8>(data.begin() + offset, data.begin() + end);
}

} // Anonymous namespace

TEST_CASE("NCZ files read as the ciphertext NCA with a decrypted view", "[core][ncz]") {
    const auto synthetic = MakeNcz();
    const auto expected_nca = ExpectedCiphertextNca(synthetic);
    std::vector<u8> plain_nca = synthetic.header;
    plain_nca.insert(plain_nca.end(), synthetic.plain_body.begin(), synthetic.plain_body.end());
    REQUIRE(expected_nca != plain_nca);

    const auto source = std::make_shared<FileSys::VectorVfsFile>(synthetic.ncz, "test.ncz");
    REQUIRE(Core::Crypto::IsBlockCompressedNCZ(source));

    constexpr std::array<std::pair<std::size_t, std::size_t>, 6> reads{{
        {0x3FE8, 0x40},
        {0x4005, 0x7},
        {0x400F, 0x11},
        {0x4123, 0x4321},
        {0x7FF1, 0x20},
        {NcaSize - 5, 0x100},
    }};

    for (const auto& nca : {Core::Crypto::DecompressNCZ(source, "test.nca"),
                            Core::Crypto::DecompressNCZForInstall(source, "test.nca")}) {
        REQUIRE(nca != nullptr);
        REQUIRE(nca->GetSize() == NcaSize);
        REQUIRE(nca->ReadAllBytes() == expected_nca);
        for (const auto& [offset, length] : reads) {
            REQUIRE(nca->ReadBytes(length, offset) == Slice(expected_nca, offset, length));
        }

        const auto view = nca->GetDecryptedView();
        REQUIRE(view != nullptr);
        REQUIRE(view->GetSize() == NcaSize);
        REQUIRE(view->ReadAllBytes() == plain_nca);
        for (const auto& [offset, length] : reads) {
            REQUIRE(view->ReadBytes(length, offset) == Slice(plain_nca, offset, length));
        }
        REQUIRE(view->GetDecryptedView() == nullptr);
        REQUIRE(nca->ReadAllBytes() == expected_nca);

        const auto body =
            std::make_shared<FileSys::OffsetVfsFile>(nca, 2 * SectionSize, NcaHeaderSize);
        REQUIRE(body->ReadAllBytes() == Slice(expected_nca, NcaHeaderSize, 2 * SectionSize));
        const auto body_view = body->GetDecryptedView();
        REQUIRE(body_view != nullptr);
        REQUIRE(body_view->ReadAllBytes() == synthetic.plain_body);
    }
}

TEST_CASE("NCZ block cache recovers after a colliding block fails to decode", "[core][ncz]") {
    constexpr std::size_t CacheSlots = 32;
    const auto synthetic = MakeRawNcz(CacheSlots, 1, true);
    const auto source = std::make_shared<FileSys::VectorVfsFile>(synthetic.ncz, "corrupt.ncz");
    const auto nca = Core::Crypto::DecompressNCZ(source, "corrupt.nca");
    REQUIRE(nca != nullptr);

    const auto block0 = Slice(synthetic.plain_body, 0, SectionSize);
    REQUIRE(nca->ReadBytes(SectionSize, NcaHeaderSize) == block0);
    REQUIRE(nca->ReadBytes(SectionSize, NcaHeaderSize + CacheSlots * SectionSize).empty());
    REQUIRE(nca->ReadBytes(SectionSize, NcaHeaderSize) == block0);

    const auto view = nca->GetDecryptedView();
    REQUIRE(view != nullptr);
    REQUIRE(view->ReadBytes(SectionSize, NcaHeaderSize + CacheSlots * SectionSize).empty());
    REQUIRE(view->ReadBytes(SectionSize, NcaHeaderSize) == block0);
    REQUIRE(nca->ReadBytes(SectionSize, NcaHeaderSize + SectionSize) ==
            Slice(synthetic.plain_body, SectionSize, SectionSize));
}

TEST_CASE("NCZ with XTS or unknown sections offers no decrypted view", "[core][ncz]") {
    for (const u64 crypto_type : {u64{2}, u64{5}}) {
        const auto synthetic = MakeRawNcz(1, crypto_type, false);
        const auto source = std::make_shared<FileSys::VectorVfsFile>(synthetic.ncz, "xts.ncz");
        const auto nca = Core::Crypto::DecompressNCZ(source, "xts.nca");
        REQUIRE(nca != nullptr);
        std::vector<u8> raw_nca = synthetic.header;
        raw_nca.insert(raw_nca.end(), synthetic.plain_body.begin(), synthetic.plain_body.end());
        REQUIRE(nca->ReadAllBytes() == raw_nca);
        REQUIRE(nca->GetDecryptedView() == nullptr);
        const auto body = std::make_shared<FileSys::OffsetVfsFile>(nca, SectionSize, NcaHeaderSize);
        REQUIRE(body->GetDecryptedView() == nullptr);
    }

    const auto plain = MakeRawNcz(1, 1, false);
    const auto plain_source = std::make_shared<FileSys::VectorVfsFile>(plain.ncz, "none.ncz");
    const auto plain_nca = Core::Crypto::DecompressNCZ(plain_source, "none.nca");
    REQUIRE(plain_nca != nullptr);
    REQUIRE(plain_nca->GetDecryptedView() != nullptr);
}

TEST_CASE("Plain files have no decrypted view", "[core][ncz]") {
    const auto plain = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(0x100), "a.nca");
    REQUIRE(plain->GetDecryptedView() == nullptr);
    const auto window = std::make_shared<FileSys::OffsetVfsFile>(plain, 0x80, 0x10);
    REQUIRE(window->GetDecryptedView() == nullptr);
}

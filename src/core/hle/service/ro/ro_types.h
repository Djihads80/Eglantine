// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/assert.h"
#include "common/common_funcs.h"
#include "common/common_types.h"

namespace Service::RO {

enum class NrrKind : u8 {
    User = 0,
    JitPlugin = 1,
    Count,
};

static constexpr size_t ModuleIdSize = 0x20;
struct ModuleId {
    std::array<u8, ModuleIdSize> data;
};
static_assert(sizeof(ModuleId) == ModuleIdSize);

template <size_t KeySize>
struct NrrCertificationLayout {
    static constexpr size_t RsaKeySize = KeySize;
    static constexpr size_t SignedSize = 0x20 + KeySize;

    u64 program_id_mask;
    u64 program_id_pattern;
    std::array<u8, 0x10> reserved_10;
    std::array<u8, RsaKeySize> modulus;
    std::array<u8, RsaKeySize> signature;
};
using NrrCertification = NrrCertificationLayout<0x100>;
static_assert(sizeof(NrrCertification) ==
              NrrCertification::RsaKeySize + NrrCertification::SignedSize);

template <size_t KeySize>
class NrrHeaderLayout {
public:
    static constexpr u32 Magic = Common::MakeMagic('N', 'R', 'R', '0');

public:
    bool IsMagicValid() const {
        return m_magic == Magic;
    }

    bool IsProgramIdValid() const {
        return (m_program_id & m_certification.program_id_mask) ==
               m_certification.program_id_pattern;
    }

    NrrKind GetNrrKind() const {
        const NrrKind kind = static_cast<NrrKind>(m_nrr_kind);
        ASSERT(kind < NrrKind::Count);
        return kind;
    }

    u64 GetProgramId() const {
        return m_program_id;
    }

    u32 GetSize() const {
        return m_size;
    }

    u32 GetNumHashes() const {
        return m_num_hashes;
    }

    size_t GetHashesOffset() const {
        return m_hashes_offset;
    }

    bool IsHashRangeValid(u64 registered_size) const {
        return IsMagicValid() && registered_size >= sizeof(*this) && m_size == registered_size &&
               m_hashes_offset >= sizeof(*this) && m_hashes_offset <= m_size &&
               m_num_hashes <= (m_size - m_hashes_offset) / 0x20;
    }

    u32 GetKeyGeneration() const {
        return m_key_generation;
    }

    const u8* GetCertificationSignature() const {
        return m_certification.signature.data();
    }

    const u8* GetCertificationSignedArea() const {
        return reinterpret_cast<const u8*>(std::addressof(m_certification));
    }

    const u8* GetCertificationModulus() const {
        return m_certification.modulus.data();
    }

    const u8* GetSignature() const {
        return m_signature.data();
    }

    size_t GetSignedAreaSize() const {
        return m_size - GetSignedAreaOffset();
    }

    static constexpr size_t GetSignedAreaOffset() {
        return offsetof(NrrHeaderLayout, m_program_id);
    }

private:
    u32 m_magic;
    u32 m_key_generation;
    INSERT_PADDING_BYTES_NOINIT(8);
    NrrCertificationLayout<KeySize> m_certification;
    std::array<u8, KeySize> m_signature;
    u64 m_program_id;
    u32 m_size;
    u8 m_nrr_kind; // 7.0.0+
    INSERT_PADDING_BYTES_NOINIT(3);
    u32 m_hashes_offset;
    u32 m_num_hashes;
    INSERT_PADDING_BYTES_NOINIT(8);
};
using NrrHeader = NrrHeaderLayout<0x100>;
using NrrHeaderNx2 = NrrHeaderLayout<0x180>;
static_assert(sizeof(NrrHeaderNx2) == 0x4d0);
static_assert(NrrHeaderNx2::GetSignedAreaOffset() == 0x4b0);
static_assert(sizeof(NrrHeader) == 0x350, "NrrHeader has wrong size");

static constexpr u32 NRO_HEADER_MAGIC = Common::MakeMagic('N', 'R', 'O', '0');
struct NroHeader {
    u32 m_entrypoint_insn;
    u32 m_mod_offset;
    INSERT_PADDING_BYTES_NOINIT(0x8);
    u32 m_magic;
    INSERT_PADDING_BYTES_NOINIT(0x4);
    u32 m_size;
    INSERT_PADDING_BYTES_NOINIT(0x4);
    u32 m_text_offset;
    u32 m_text_size;
    u32 m_ro_offset;
    u32 m_ro_size;
    u32 m_rw_offset;
    u32 m_rw_size;
    u32 m_bss_size;
    INSERT_PADDING_BYTES_NOINIT(0x4);
    ModuleId m_module_id;
    INSERT_PADDING_BYTES_NOINIT(0x20);
};
static_assert(sizeof(NroHeader) == 0x80, "NroHeader has wrong size");

} // namespace Service::RO

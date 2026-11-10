// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// BND4 reference: SoulsFormatsNEXT, BinderFileHeader.ReadBinder4FileHeader (MIT).
// BNSH layout reference: MIT-licensed ShaderLibrary, BnshLoader.cs and Structs.cs.

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <zlib.h>
#include <zstd.h>

#include "video_core/shader_warm/shader_archives.h"

namespace VideoCommon::ShaderWarm {
namespace {

using Bytes = std::span<const u8>;

constexpr u64 MaxAllocation = 512ULL * 1024 * 1024;
constexpr std::array<u8, 8> ControlMagic{0x34, 0x12, 0x76, 0x98, 0xc9, 0x00, 0x00, 0x00};

/// The `size` bytes of `data` at `offset`. Throws ArchiveError when they run past the end.
Bytes Extent(Bytes data, u64 offset, u64 size) {
    if (offset > data.size() || size > data.size() - offset) {
        throw ArchiveError(
            fmt::format("extent {:#x}+{:#x} outside {:#x} bytes", offset, size, data.size()));
    }
    return data.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
}

template <typename T>
T Le(Bytes data, u64 offset) {
    const auto bytes = Extent(data, offset, sizeof(T));
    T value;
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

u32 Be32(Bytes data, u64 offset) {
    const auto b = Extent(data, offset, 4);
    return (u32{b[0]} << 24) | (u32{b[1]} << 16) | (u32{b[2]} << 8) | u32{b[3]};
}

/// Python `data[offset:offset + len(text)] == text` (false when short).
bool Is(Bytes data, u64 offset, std::string_view text) {
    return offset <= data.size() && text.size() <= data.size() - offset &&
           std::memcmp(data.data() + offset, text.data(), text.size()) == 0;
}

/// True when the control section starts with the supported native control magic.
bool IsControlMagic(Bytes control) {
    return control.size() >= 8 && std::memcmp(control.data(), ControlMagic.data(), 8) == 0;
}

/// True when `text` ends with `suffix`.
bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

/// ASCII lowercase copy of `text`.
std::string Lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return result;
}

struct Record {
    ProgramStage stage;
    Bytes code;
    Bytes control;
};

// ---------------------------------------------------------------------------------------------
// Decompressors

/// Decompresses one whole zstd frame, with the dictionary it names when it needs one. Throws on an
/// unknown or oversized content size, a missing dictionary or trailing input.
std::vector<u8> ZstdDecompress(Bytes data, const ZstdDictionaries* dictionaries) {
    const unsigned long long size = ZSTD_getFrameContentSize(data.data(), data.size());
    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR ||
        size > MaxAllocation) {
        throw ArchiveError("unknown or excessive zstd decompressed size");
    }
    const unsigned id = ZSTD_getDictID_fromFrame(data.data(), data.size());
    const ZSTD_DDict* dictionary = nullptr;
    if (id != 0) {
        dictionary = dictionaries ? dictionaries->Find(id) : nullptr;
        if (dictionary == nullptr) {
            throw ArchiveError(fmt::format("zstd dictionary {} is required", id));
        }
    }
    const size_t frame = ZSTD_findFrameCompressedSize(data.data(), data.size());
    if (ZSTD_isError(frame) || frame != data.size()) {
        throw ArchiveError("zstd decompressed size mismatch or trailing input");
    }
    struct Context {
        ZSTD_DCtx* ctx = ZSTD_createDCtx();
        ~Context() {
            ZSTD_freeDCtx(ctx);
        }
    };
    thread_local Context context;
    std::vector<u8> output(static_cast<size_t>(size));
    const size_t result =
        dictionary ? ZSTD_decompress_usingDDict(context.ctx, output.data(), output.size(),
                                                data.data(), data.size(), dictionary)
                   : ZSTD_decompressDCtx(context.ctx, output.data(), output.size(), data.data(),
                                         data.size());
    if (ZSTD_isError(result)) {
        throw ArchiveError(
            fmt::format("zstd decompression failed: {}", ZSTD_getErrorName(result)));
    }
    if (result != output.size()) {
        throw ArchiveError("zstd decompressed size mismatch or trailing input");
    }
    return output;
}

/// Decompresses a raw LZ4 block that must produce exactly `expected` bytes. Throws on malformed
/// input.
std::vector<u8> Lz4Block(Bytes data, u64 expected) {
    if (expected > MaxAllocation) {
        throw ArchiveError("excessive LZ4 allocation");
    }
    std::vector<u8> output;
    output.reserve(static_cast<size_t>(expected));
    size_t cursor = 0;
    const auto length = [&](u32 initial) {
        u64 value = initial;
        if (initial == 15) {
            for (;;) {
                if (cursor >= data.size()) {
                    throw ArchiveError("truncated LZ4 length");
                }
                const u8 extra = data[cursor++];
                value += extra;
                if (extra != 255) {
                    break;
                }
            }
        }
        return value;
    };
    while (cursor < data.size()) {
        const u8 token = data[cursor++];
        const u64 literals = length(static_cast<u32>(token >> 4));
        if (output.size() + literals > expected) {
            throw ArchiveError("LZ4 literals exceed allocation");
        }
        const auto literal_bytes = Extent(data, cursor, literals);
        output.insert(output.end(), literal_bytes.begin(), literal_bytes.end());
        cursor += static_cast<size_t>(literals);
        if (cursor == data.size()) {
            break;
        }
        const u16 distance = Le<u16>(data, cursor);
        cursor += 2;
        u64 count = length(static_cast<u32>(token & 15)) + 4;
        if (distance == 0 || distance > output.size() || output.size() + count > expected) {
            throw ArchiveError("invalid LZ4 match");
        }
        while (count != 0) {
            const size_t start = output.size() - distance;
            const size_t chunk = static_cast<size_t>(std::min<u64>(distance, count));
            for (size_t i = 0; i < chunk; ++i) {
                output.push_back(output[start + i]);
            }
            count -= chunk;
        }
    }
    if (output.size() != expected) {
        throw ArchiveError("LZ4 decompressed size mismatch");
    }
    return output;
}

/// Inflates a zlib stream of `size` bytes. Throws when it is incomplete or has trailing input.
std::vector<u8> ZlibDecompress(Bytes payload, u64 size) {
    std::vector<u8> output(static_cast<size_t>(size) + 1);
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        throw ArchiveError("zlib initialization failed");
    }
    stream.next_in = const_cast<Bytef*>(payload.data());
    stream.avail_in = static_cast<uInt>(payload.size());
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());
    const int status = inflate(&stream, Z_FINISH);
    const size_t produced = output.size() - stream.avail_out;
    const bool trailing = stream.avail_in != 0;
    inflateEnd(&stream);
    if (status != Z_STREAM_END || trailing) {
        throw ArchiveError("incomplete or trailing DCX zlib stream");
    }
    output.resize(produced);
    return output;
}

/// Unpacks a DCX file (zstd or zlib payload) and checks the result against the header's size.
std::vector<u8> DcxDecompress(Bytes data) {
    Extent(data, 0, 0x30);
    if (!Is(data, 0, std::string_view{"DCX\0", 4}) || !Is(data, 0x18, std::string_view{"DCS\0", 4}) ||
        !Is(data, 0x24, std::string_view{"DCP\0", 4})) {
        throw ArchiveError("unsupported DCX header");
    }
    const u32 offset = Be32(data, 0x14);
    const u32 size = Be32(data, 0x1c);
    const u32 compressed = Be32(data, 0x20);
    if (size > MaxAllocation) {
        throw ArchiveError("excessive DCX allocation");
    }
    const auto payload = Extent(data, offset, compressed);
    std::vector<u8> result;
    if (Is(data, 0x28, "ZSTD")) {
        result = ZstdDecompress(payload, nullptr);
    } else if (Is(data, 0x28, "DFLT")) {
        result = ZlibDecompress(payload, size);
    } else {
        throw ArchiveError("unsupported DCX compressor");
    }
    if (result.size() != size) {
        throw ArchiveError("DCX decompressed size mismatch");
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// Archives

struct Member {
    std::string name;
    Bytes payload;
};

/// The named members of a SARC archive (either byte order). Throws on any malformed header, name
/// or member range.
std::vector<Member> ParseSarc(Bytes data) {
    Extent(data, 0, 0x20);
    if (!Is(data, 0, "SARC")) {
        throw ArchiveError("not a SARC archive");
    }
    bool little;
    if (data[6] == 0xff && data[7] == 0xfe) {
        little = true;
    } else if (data[6] == 0xfe && data[7] == 0xff) {
        little = false;
    } else {
        throw ArchiveError("invalid SARC byte order");
    }
    const auto u16_at = [&](u64 offset) -> u64 {
        const u16 v = Le<u16>(data, offset);
        return little ? v : static_cast<u16>((v >> 8) | (v << 8));
    };
    const auto u32_at = [&](u64 offset) -> u64 {
        const u32 v = Le<u32>(data, offset);
        return little ? v
                      : ((v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24));
    };
    const u64 header_size = u16_at(4);
    const u64 file_size = u32_at(8);
    const u64 data_offset = u32_at(12);
    if (header_size < 0x14 || file_size != data.size() || data_offset > file_size) {
        throw ArchiveError("invalid SARC header or file size");
    }
    const u64 sfat = header_size;
    if (!Is(Extent(data, sfat, 4), 0, "SFAT") || u16_at(sfat + 4) < 0x0c) {
        throw ArchiveError("invalid SFAT header");
    }
    const u64 count = u16_at(sfat + 6);
    const u64 nodes = sfat + u16_at(sfat + 4);
    Extent(data, nodes, count * 0x10);
    const u64 sfnt = nodes + count * 0x10;
    if (!Is(Extent(data, sfnt, 4), 0, "SFNT") || u16_at(sfnt + 4) < 8) {
        throw ArchiveError("invalid SFNT header");
    }
    const u64 names = sfnt + u16_at(sfnt + 4);
    if (names > data_offset) {
        throw ArchiveError("SARC names overlap file data");
    }
    std::vector<Member> result;
    result.reserve(static_cast<size_t>(count));
    for (u64 i = 0; i < count; ++i) {
        const u64 node = nodes + i * 0x10;
        const u64 flags = u32_at(node + 4);
        const u64 start = u32_at(node + 8);
        const u64 end = u32_at(node + 12);
        if (flags >> 24 != 1) {
            throw ArchiveError("SARC member has no supported name");
        }
        const u64 name_offset = names + (flags & 0xffffff) * 4;
        if (name_offset >= data_offset) {
            throw ArchiveError("SARC name outside string table");
        }
        const auto name_area = data.subspan(static_cast<size_t>(name_offset),
                                            static_cast<size_t>(data_offset - name_offset));
        const auto terminator = std::find(name_area.begin(), name_area.end(), u8{0});
        if (terminator == name_area.end()) {
            throw ArchiveError("unterminated SARC name");
        }
        std::string name(name_area.begin(), terminator);
        if (end < start || end > file_size - data_offset) {
            throw ArchiveError("invalid SARC member range");
        }
        result.push_back({std::move(name), Extent(data, data_offset + start, end - start)});
    }
    return result;
}

struct Bnd4Member {
    std::string name;
    std::vector<u8> owned;
    Bytes payload;
};

/// The members of a BND4 archive, with DCX compressed members unpacked. Throws on unsupported
/// formats or sizes.
std::vector<Bnd4Member> ParseBnd4(Bytes data) {
    Extent(data, 0, 64);
    if (!Is(data, 0, "BND4") || !Is(data, 9, std::string_view{"\0\1\0", 3}) ||
        !Is(data, 48, std::string_view{"\1t\0\0", 4})) {
        throw ArchiveError("unsupported BND4 byte order or file-record format");
    }
    const u32 count = Le<u32>(data, 12);
    const u64 header = Le<u64>(data, 16);
    const u64 record_size = Le<u64>(data, 32);
    if (header != 64 || record_size != 36 || count > 100000) {
        throw ArchiveError("unsupported BND4 record size or count");
    }
    Extent(data, 64, u64{count} * record_size);
    std::vector<Bnd4Member> result;
    for (u32 index = 0; index < count; ++index) {
        const u64 record = 64 + u64{index} * record_size;
        if (!Is(data, record, std::string_view{"\x40\0\0\0\xff\xff\xff\xff", 8}) &&
            !Is(data, record, std::string_view{"\xc0\0\0\0\xff\xff\xff\xff", 8})) {
            throw ArchiveError("unsupported BND4 member flags");
        }
        const u64 size = Le<u64>(data, record + 8);
        const u64 unpacked = Le<u64>(data, record + 16);
        const u32 offset = Le<u32>(data, record + 24);
        const u32 name = Le<u32>(data, record + 32);
        if (size > MaxAllocation || unpacked > MaxAllocation || name % 2) {
            throw ArchiveError("invalid BND4 member size or name");
        }
        u64 end = name;
        std::string filename;
        for (;;) {
            const u16 unit = Le<u16>(data, end);
            if (unit == 0) {
                break;
            }
            // Only used for suffix checks: non-ASCII characters become '?'.
            filename.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
            end += 2;
            if (end - name > 65536) {
                throw ArchiveError("excessive BND4 filename");
            }
        }
        Bnd4Member member;
        member.name = std::move(filename);
        member.payload = Extent(data, offset, size);
        if (data[static_cast<size_t>(record)] == 0xc0) {
            member.owned = DcxDecompress(member.payload);
            member.payload = member.owned;
        }
        if (member.payload.size() != unpacked) {
            throw ArchiveError("BND4 member size mismatch");
        }
        result.push_back(std::move(member));
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// Shader containers

constexpr std::array<ProgramStage, 5> GraphicsStages{
    ProgramStage::Vertex, ProgramStage::TessControl, ProgramStage::TessEval,
    ProgramStage::Geometry, ProgramStage::Fragment};

u32 HeaderType(Bytes code) {
    return (Le<u32>(code, 0) >> 10) & 15;
}

/// A graphics record whose stage comes from the native program header. Throws when the code size,
/// header stage or control version is not supported.
Record GraphicsRecord(Bytes code, Bytes control) {
    if (code.size() <= 128 || (code.size() - 128) % 16) {
        throw ArchiveError("unsupported native graphics allocation");
    }
    const u32 stage = HeaderType(code);
    if (stage < 1 || stage > 5) {
        throw ArchiveError("native envelope is not a verified graphics header");
    }
    if (!IsControlMagic(control)) {
        throw ArchiveError("unsupported native shader control version");
    }
    return {GraphicsStages[stage - 1], code, control};
}

/// Every stage program of every variation in a BNSH file, task and mesh included. Throws when the
/// version, layout or a stage's header does not check out.
std::vector<Record> ParseBnsh(Bytes data) {
    Extent(data, 0, 0x98);
    if (!Is(data, 0, std::string_view{"BNSH\0\0\0\0", 8}) || !Is(data, 0x60, "grsc")) {
        throw ArchiveError("invalid BNSH header");
    }
    if (!Is(data, 0xc, "\xff\xfe")) {
        throw ArchiveError("unsupported BNSH byte order");
    }
    if (Le<u32>(data, 0x1c) != data.size()) {
        throw ArchiveError("invalid BNSH file size");
    }
    const std::array<u8, 4> version{data[8], data[9], data[10], data[11]};
    constexpr std::array<u8, 4> v0420{0, 4, 2, 0}, v1320{1, 3, 2, 0}, v1420{1, 4, 2, 0};
    if (version != v0420 && version != v1320 && version != v1420) {
        throw ArchiveError("unsupported BNSH version");
    }
    if (Le<u16>(data, 0x70) != 4 || Le<u16>(data, 0x72) != 200) {
        throw ArchiveError("unsupported BNSH API");
    }
    const u32 count = Le<u32>(data, 0x7c);
    const u64 variations = Le<u64>(data, 0x80);
    if (count != 0 && variations == 0) {
        throw ArchiveError("missing BNSH variation table");
    }
    Extent(data, variations, u64{count} * 0x40);
    std::vector<Record> result;
    for (u32 variation = 0; variation < count; ++variation) {
        const u64 program = Le<u64>(data, variations + u64{variation} * 0x40 + 0x10);
        if (program == 0) {
            throw ArchiveError("missing BNSH variation program");
        }
        Extent(data, program, 0x38);
        std::vector<std::pair<u64, ProgramStage>> slots{
            {0x08, ProgramStage::Vertex},   {0x10, ProgramStage::TessControl},
            {0x18, ProgramStage::TessEval}, {0x20, ProgramStage::Geometry},
            {0x28, ProgramStage::Fragment}, {0x30, ProgramStage::Compute}};
        if (version == v1420 && Le<u64>(data, program) == 2) {
            Extent(data, program, 0x50);
            slots.emplace_back(0x40, ProgramStage::Task);
            slots.emplace_back(0x48, ProgramStage::Mesh);
        }
        for (size_t index = 0; index < slots.size(); ++index) {
            const auto [slot, stage] = slots[index];
            const u64 record = Le<u64>(data, program + slot);
            if (record == 0) {
                continue;
            }
            Extent(data, record, 0x40);
            const u64 control_offset = Le<u64>(data, record + 8);
            const u64 code_offset = Le<u64>(data, record + 16);
            const u32 code_size = Le<u32>(data, record + 24);
            const u32 control_size = Le<u32>(data, record + 28);
            if (code_offset == 0 || code_size == 0 || (control_size != 0 && control_offset == 0)) {
                throw ArchiveError("invalid BNSH stage data pointer or size");
            }
            const auto code = Extent(data, code_offset, code_size);
            const auto control = Extent(data, control_offset, control_size);
            if (stage != ProgramStage::Compute) {
                if (code.size() <= 128 || (code.size() - 128) % 16) {
                    throw ArchiveError("invalid native graphics code size");
                }
                const u32 header_type = HeaderType(code);
                if (stage == ProgramStage::Task || stage == ProgramStage::Mesh) {
                    const u32 expected = stage == ProgramStage::Task ? 6 : 7;
                    const bool header_ok = stage == ProgramStage::Task
                                               ? header_type == 1
                                               : (header_type == 1 || header_type == 3);
                    if (control.size() < 88 || !IsControlMagic(control) ||
                        Le<u32>(data, control_offset + 84) >> 16 != expected || !header_ok) {
                        throw ArchiveError("invalid BNSH task/mesh stage metadata");
                    }
                } else if (header_type != index + 1) {
                    throw ArchiveError("BNSH graphics stage disagrees with native header");
                }
            }
            result.push_back({stage, code, control});
        }
    }
    return result;
}

/// Every BNSH file embedded in `data` with its offset, each one validated. Throws when there is
/// none.
std::vector<std::pair<size_t, Bytes>> FindEmbeddedBnsh(Bytes data) {
    static constexpr std::array<u8, 8> magic{'B', 'N', 'S', 'H', 0, 0, 0, 0};
    std::vector<std::pair<size_t, Bytes>> result;
    size_t offset = 0;
    for (;;) {
        const auto it = std::search(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                    data.end(), magic.begin(), magic.end());
        if (it == data.end()) {
            break;
        }
        offset = static_cast<size_t>(it - data.begin());
        const auto header = Extent(data, offset, 0x20);
        const u32 size = Le<u32>(header, 0x1c);
        if (size < 0x98) {
            throw ArchiveError("invalid embedded BNSH size");
        }
        const auto bnsh = Extent(data, offset, size);
        ParseBnsh(bnsh);
        result.emplace_back(offset, bnsh);
        offset += size;
    }
    if (result.empty()) {
        throw ArchiveError("container contains no supported BNSH");
    }
    return result;
}

/// The BNSH files of a BFSHA archive, after its header checks.
std::vector<std::pair<size_t, Bytes>> FindBnsh(Bytes data) {
    if (data.size() < 0x20 || !Is(data, 0, "FSHA")) {
        throw ArchiveError("not a BFSHA archive");
    }
    if (!Is(data, 0xc, "\xff\xfe") || Le<u32>(data, 0x1c) != data.size()) {
        throw ArchiveError("invalid BFSHA byte order or size");
    }
    return FindEmbeddedBnsh(data);
}

/// The records of a CGSB container (one stage, up to 16 programs). Throws when the platform or
/// stage is not supported.
std::vector<Record> ParseCgsb(Bytes data) {
    Extent(data, 0, 24);
    if (!Is(data, 0, std::string_view{"CGSB\x33\x22\x11\0", 8})) {
        throw ArchiveError("unsupported CGSB version");
    }
    const u16 count = Le<u16>(data, 8);
    const u16 shader_stage = Le<u16>(data, 10);
    const u64 table = Le<u64>(data, 16);
    if (count < 1 || count > 16) {
        throw ArchiveError("invalid CGSB record count");
    }
    if (shader_stage > 7) {
        throw ArchiveError("unsupported CGSB stage");
    }
    Extent(data, table, u64{count} * 80);
    std::vector<Record> result;
    for (u32 index = 0; index < count; ++index) {
        const u64 record = table + u64{index} * 80;
        if (!Is(data, record, std::string_view{"NVN \1\0\0\0", 8})) {
            throw ArchiveError("unsupported CGSB shader platform");
        }
        const u64 code_offset = Le<u64>(data, record + 16);
        const u64 code_size = Le<u64>(data, record + 24);
        const u64 control_offset = Le<u64>(data, record + 32);
        const u64 control_size = Le<u64>(data, record + 40);
        if (shader_stage >= 5) {
            constexpr std::array<ProgramStage, 3> stages{ProgramStage::Compute, ProgramStage::Task,
                                                         ProgramStage::Mesh};
            const auto code = Extent(data, code_offset, code_size);
            const auto control = Extent(data, control_offset, control_size);
            result.push_back({stages[shader_stage - 5], code, control});
        } else {
            const auto code = Extent(data, code_offset, code_size);
            const auto control = Extent(data, control_offset, control_size);
            const auto item = GraphicsRecord(code, control);
            if (item.stage != GraphicsStages[shader_stage]) {
                throw ArchiveError("CGSB stage disagrees with native header");
            }
            result.push_back(item);
        }
    }
    return result;
}

/// The records of a SHN container, whose two LZ4 blocks are unpacked into `metadata` and `code`
/// (the records point into them). Throws when it holds no supported record.
std::vector<Record> ParseShn(Bytes data, std::vector<u8>& metadata, std::vector<u8>& code) {
    Extent(data, 0, 68);
    if (!Is(data, 0, "SHN!")) {
        throw ArchiveError("not a SHN shader container");
    }
    std::array<std::vector<u8>, 2> blocks;
    for (size_t i = 0; i < 2; ++i) {
        const u64 descriptor = i == 0 ? 4 : 36;
        const u32 offset = Le<u32>(data, descriptor);
        const u32 compressed = Le<u32>(data, descriptor + 4);
        const u32 size = Le<u32>(data, descriptor + 8);
        blocks[i] = Lz4Block(Extent(data, offset, compressed), size);
    }
    metadata = std::move(blocks[0]);
    code = std::move(blocks[1]);
    const Bytes meta{metadata};
    const Bytes pool{code};
    if (!Is(meta, 0, "SHDROUNC") || meta.size() < 64) {
        throw ArchiveError("unsupported SHN metadata format");
    }
    const u32 version = Le<u32>(meta, 8);
    const u32 variations = Le<u32>(meta, 12);
    if (version != 20241128 || variations < 1 || variations > 65536) {
        throw ArchiveError("unsupported SHDROUNC version or variation count");
    }
    Extent(meta, 16, u64{variations} * 80);
    std::vector<Record> result;
    for (u32 variation = 0; variation < variations; ++variation) {
        const u64 table = Le<u64>(meta, 48 + u64{variation} * 80);
        Extent(meta, table, 9 * 80);
        for (u32 index = 0; index < 9; ++index) {
            const u64 record = table + u64{index} * 80;
            const u64 code_offset = Le<u64>(meta, record);
            const u64 control_offset = Le<u64>(meta, record + 8);
            const u32 code_size = Le<u32>(meta, record + 64);
            const u32 control_size = Le<u32>(meta, record + 68);
            if (code_size == 0 && control_size == 0) {
                continue;
            }
            const auto payload = Extent(pool, code_offset, code_size);
            const auto control = Extent(meta, control_offset, control_size);
            if (index == 2) {
                result.push_back({ProgramStage::Compute, payload, control});
            } else if (index < 2) {
                const auto item = GraphicsRecord(payload, control);
                if (item.stage != (index == 0 ? ProgramStage::Vertex : ProgramStage::Fragment)) {
                    throw ArchiveError("SHDROUNC stage disagrees with native header");
                }
                result.push_back(item);
            } else {
                result.push_back({ProgramStage::Other, payload, control});
            }
        }
    }
    if (result.empty()) {
        throw ArchiveError("SHDROUNC contains no supported shader records");
    }
    return result;
}

/// NVN2 AGL BAHS v11 with its separate metadata and code pools.
std::vector<Record> ParseBahs(Bytes data) {
    Extent(data, 0, 32);
    const u32 version = Le<u32>(data, 4);
    const u32 size = Le<u32>(data, 8);
    const u32 platform = Le<u32>(data, 12);
    const u32 flags = Le<u32>(data, 16);
    const u64 metadata_base = Le<u32>(data, 20);
    const u64 pool_size = Le<u32>(data, 24);
    const u64 pool = Le<u32>(data, 28);
    if (!Is(data, 0, "BAHS") || version != 11 || platform != 5 || flags != 0 ||
        size != data.size()) {
        throw ArchiveError("unsupported BAHS version, platform or file size");
    }
    const u64 table = metadata_base + 0x150;
    if (metadata_base < 32 || table + 8 > pool || pool + pool_size != size) {
        throw ArchiveError("invalid BAHS metadata or code pool");
    }
    const u64 table_size = Le<u32>(data, table);
    const u64 count = Le<u32>(data, table + 4);
    const u64 end = table + table_size;
    if (count == 0 || table_size < 8 || end > pool || count > (table_size - 8) / 32) {
        throw ArchiveError("invalid BAHS record table");
    }
    struct SlotInfo {
        ProgramStage stage;
        u32 type;
    };
    const auto slot_info = [](u32 slot) -> std::optional<SlotInfo> {
        switch (slot) {
        case 0:
            return SlotInfo{ProgramStage::Vertex, 0};
        case 1:
            return SlotInfo{ProgramStage::Fragment, 1};
        case 3:
            return SlotInfo{ProgramStage::Compute, 5};
        case 4:
            return SlotInfo{ProgramStage::Task, 6};
        case 5:
            return SlotInfo{ProgramStage::Mesh, 7};
        default:
            return std::nullopt;
        }
    };
    std::vector<Record> records;
    u64 cursor = table + 8;
    for (u64 index = 0; index < count; ++index) {
        if (cursor + 32 > end) {
            throw ArchiveError("truncated BAHS record header");
        }
        const u64 length = Le<u32>(data, cursor);
        const u32 slot = Le<u32>(data, cursor + 4);
        const u64 body_size = Le<u32>(data, cursor + 12);
        const u64 descriptor = cursor + 16;
        const u64 code_relative = Le<u64>(data, descriptor);
        const u64 control_size = Le<u32>(data, descriptor + 8);
        const s64 control_relative = Le<s32>(data, descriptor + 12);
        if (length < 32 || body_size != length - 16 || cursor + length > end) {
            throw ArchiveError("invalid BAHS record size");
        }
        const auto info = slot_info(slot);
        if (!info) {
            throw ArchiveError(fmt::format("unsupported BAHS shader slot {}", slot));
        }
        // A shared control can be referenced backwards from a later descriptor.
        const s64 control_offset = static_cast<s64>(descriptor) + control_relative;
        if (control_size < 104 || control_offset < static_cast<s64>(table + 8) ||
            static_cast<u64>(control_offset) + control_size > end) {
            throw ArchiveError("invalid BAHS control extent");
        }
        const auto control = data.subspan(static_cast<size_t>(control_offset),
                                          static_cast<size_t>(control_size));
        if (!IsControlMagic(control) || Le<u32>(control, 84) >> 16 != info->type) {
            throw ArchiveError("BAHS record disagrees with the NVN2 control stage or version");
        }
        const u64 declared = Le<u32>(control, 56);
        const u64 instruction_allocation = Le<u32>(control, 64);
        const u64 allocation = Le<u32>(control, 68);
        if (declared == 0 || declared % 16 || instruction_allocation % 16 ||
            !(declared <= instruction_allocation && instruction_allocation <= allocation) ||
            code_relative + allocation > pool_size) {
            throw ArchiveError("invalid BAHS code extent");
        }
        const u64 code_offset = pool + code_relative;
        const u64 code_size =
            info->stage == ProgramStage::Compute ? declared : instruction_allocation;
        const auto code = Extent(data, code_offset, code_size);
        if (info->stage == ProgramStage::Task || info->stage == ProgramStage::Mesh) {
            const u32 header_type = HeaderType(code);
            const bool header_ok = info->stage == ProgramStage::Task
                                       ? header_type == 1
                                       : (header_type == 1 || header_type == 3);
            if (code.size() <= 128 || (code.size() - 128) % 16 || !header_ok) {
                throw ArchiveError("invalid BAHS task/mesh graphics envelope");
            }
            records.push_back({info->stage, code, control});
        } else if (info->stage == ProgramStage::Compute) {
            records.push_back({info->stage, code, control});
        } else {
            const auto item = GraphicsRecord(code, control);
            if (item.stage != info->stage) {
                throw ArchiveError("BAHS record disagrees with the graphics header stage");
            }
            records.push_back(item);
        }
        cursor += length;
    }
    if (cursor != end) {
        throw ArchiveError("trailing bytes in BAHS record table");
    }
    return records;
}

// ---------------------------------------------------------------------------------------------
// Walker

struct Walker {
    const ZstdDictionaries* dictionaries;
    const std::function<void(const ShaderRecord&)>& on_record;
    const std::function<void(const std::string&)>& on_member_error;

    /// Passes every record to the caller's record callback.
    void Emit(const std::vector<Record>& records) const {
        for (const auto& record : records) {
            on_record(ShaderRecord{record.stage, record.code, record.control});
        }
    }

    /// Walks one archive member, reporting its error to the caller instead of stopping the walk.
    void WalkMember(Bytes payload, const std::string& name, int depth) const {
        try {
            Walk(payload, name, depth);
        } catch (const ArchiveError& error) {
            on_member_error(name + ": " + error.what());
        }
    }

    /// Detects the container format by its magic and unpacks or parses it, recursing into members.
    /// Known non-shader files are skipped; other unknown data throws.
    void Walk(Bytes data, const std::string& name, int depth) const {
        if (depth > 12) {
            throw ArchiveError("excessive container nesting");
        }
        if (Is(data, 0, "\x28\xb5\x2f\xfd")) {
            const auto inner = ZstdDecompress(data, dictionaries);
            Walk(inner, name + "!zstd", depth + 1);
        } else if (Is(data, 0, std::string_view{"DCX\0", 4})) {
            const auto inner = DcxDecompress(data);
            Walk(inner, name + "!dcx", depth + 1);
        } else if (Is(data, 0, "SARC")) {
            for (const auto& member : ParseSarc(data)) {
                WalkMember(member.payload, name + "!" + member.name, depth + 1);
            }
        } else if (Is(data, 0, "BND4")) {
            for (const auto& member : ParseBnd4(data)) {
                WalkMember(member.payload, name + "!" + member.name, depth + 1);
            }
        } else if (Is(data, 0, "FSHA")) {
            for (const auto& [offset, bnsh] : FindBnsh(data)) {
                Walk(bnsh, name + fmt::format("!BNSH@{:x}", offset), depth + 1);
            }
        } else if (Is(data, 0, std::string_view{"YB\x05\0", 4}) &&
                   (EndsWith(Lower(name), ".vsetb.byml") ||
                    EndsWith(Lower(name), ".vsetb.byml.zs!zstd"))) {
            for (const auto& [offset, bnsh] : FindEmbeddedBnsh(data)) {
                Walk(bnsh, name + fmt::format("!BNSH@{:x}", offset), depth + 1);
            }
        } else if (Is(data, 0, "BNSH")) {
            Emit(ParseBnsh(data));
        } else if (Is(data, 0, "CGSB")) {
            Emit(ParseCgsb(data));
        } else if (Is(data, 0, "SHN!")) {
            std::vector<u8> metadata, code;
            Emit(ParseShn(data, metadata, code));
        } else if (Is(data, 0, "BAHS")) {
            Emit(ParseBahs(data));
        } else {
            const auto lower = Lower(name);
            for (const std::string_view skip :
                 {".json", ".presetb", ".meminfo", ".tex", ".byml", ".yaml", ".raw"}) {
                if (EndsWith(lower, skip)) {
                    return;
                }
            }
            std::string magic;
            for (size_t i = 0; i < std::min<size_t>(4, data.size()); ++i) {
                magic += fmt::format("{:02x}", data[i]);
            }
            throw ArchiveError(fmt::format("unsupported container magic {}", magic));
        }
    }
};

} // Anonymous namespace

ZstdDictionaries::ZstdDictionaries() = default;

/// Frees every loaded dictionary.
ZstdDictionaries::~ZstdDictionaries() {
    for (const auto& [id, dictionary] : dictionaries) {
        ZSTD_freeDDict(dictionary);
    }
}

void ZstdDictionaries::Load(std::span<const u8> pack) {
    std::vector<u8> decompressed;
    Bytes archive = pack;
    if (Is(pack, 0, "\x28\xb5\x2f\xfd")) {
        decompressed = ZstdDecompress(pack, nullptr);
        archive = decompressed;
    }
    for (const auto& member : ParseSarc(archive)) {
        if (!Is(member.payload, 0, "\x37\xa4\x30\xec")) {
            continue;
        }
        const unsigned id = ZSTD_getDictID_fromDict(member.payload.data(), member.payload.size());
        if (dictionaries.contains(id)) {
            throw ArchiveError(fmt::format("duplicate zstd dictionary ID {} in {}", id, member.name));
        }
        ZSTD_DDict* const dictionary =
            ZSTD_createDDict(member.payload.data(), member.payload.size());
        if (dictionary == nullptr) {
            throw ArchiveError("invalid zstd dictionary " + member.name);
        }
        dictionaries.emplace(id, dictionary);
    }
    if (dictionaries.empty()) {
        throw ArchiveError("no zstd dictionaries found in the pack");
    }
}

/// The dictionary with this ID, null when none was loaded.
const ZSTD_DDict_s* ZstdDictionaries::Find(u32 id) const {
    const auto it = dictionaries.find(id);
    return it == dictionaries.end() ? nullptr : it->second;
}

GameFileKind ClassifyGameFile(std::string_view path) {
    if (path == "Pack/ZsDic.pack.zs") {
        return GameFileKind::ZstdDictionaries;
    }
    std::string lower = Lower(path);
    if (EndsWith(lower, ".zst")) {
        lower.resize(lower.size() - 4);
    } else if (EndsWith(lower, ".zs")) {
        lower.resize(lower.size() - 3);
    }
    for (const std::string_view extension :
         {".bfsha", ".sarc", ".bnsh", ".cgsb", ".shn", ".sharcb"}) {
        if (EndsWith(lower, extension)) {
            return GameFileKind::ShaderArchive;
        }
    }
    if (path.starts_with("Effect/") && path.find('/', 7) == std::string_view::npos &&
        EndsWith(path, ".vsetb.byml.zs")) {
        return GameFileKind::Effect;
    }
    return GameFileKind::Other;
}

void WalkArchive(std::span<const u8> data, const std::string& name,
                 const ZstdDictionaries* dictionaries,
                 const std::function<void(const ShaderRecord&)>& on_record,
                 const std::function<void(const std::string&)>& on_member_error) {
    const Walker walker{dictionaries, on_record, on_member_error};
    walker.Walk(data, name, 0);
}

} // namespace VideoCommon::ShaderWarm

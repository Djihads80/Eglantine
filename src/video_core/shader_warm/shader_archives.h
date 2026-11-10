// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

#include "common/common_types.h"

struct ZSTD_DDict_s;

namespace VideoCommon::ShaderWarm {

/// Readers for the NVN2 shader containers found in Switch 2 game files. Port of the PR #214
/// Python extractor (tools/nx2/extract_static_shaders.py, shader_archive.py,
/// shader_containers.py): every check and every error case is kept, so both find the same
/// programs.

class ArchiveError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class ProgramStage : u8 {
    Vertex,
    TessControl,
    TessEval,
    Geometry,
    Fragment,
    Compute,
    Task,
    Mesh,
    Other,
};

/// One shader program inside a container. The spans are only valid during the callback.
struct ShaderRecord {
    ProgramStage stage{};
    std::span<const u8> code;
    std::span<const u8> control;
};

/// The game's Zstandard dictionaries (Pack/ZsDic.pack.zs), by dictionary ID.
class ZstdDictionaries {
public:
    ZstdDictionaries();
    ~ZstdDictionaries();
    ZstdDictionaries(const ZstdDictionaries&) = delete;
    ZstdDictionaries& operator=(const ZstdDictionaries&) = delete;

    /// Reads a dictionary pack (zstd compressed or plain SARC). Throws ArchiveError.
    void Load(std::span<const u8> pack);

    [[nodiscard]] const ZSTD_DDict_s* Find(u32 id) const;
    [[nodiscard]] size_t Count() const {
        return dictionaries.size();
    }

private:
    std::unordered_map<u32, ZSTD_DDict_s*> dictionaries;
};

/// What the precache builder does with a RomFS file.
enum class GameFileKind : u8 {
    Other,            ///< Not read
    ShaderArchive,    ///< *.bfsha, *.sarc, *.bnsh, *.cgsb, *.shn or *.sharcb, raw or zstd (.zs)
    Effect,           ///< Effect/*.vsetb.byml.zs, read after every shader archive
    ZstdDictionaries, ///< Pack/ZsDic.pack.zs
};

/// Kind of the RomFS file at `path` ('/' separated, file extensions in any case).
[[nodiscard]] GameFileKind ClassifyGameFile(std::string_view path);

/// Walks one archive file (zstd, DCX, SARC, BND4, BFSHA, BNSH, effect bundles, CGSB, SHN, BAHS)
/// and calls `on_record` for every shader program found. Errors inside a SARC or BND4 member
/// go to `on_member_error` and the walk goes on; other errors throw ArchiveError (programs
/// already reported stay valid, like the Python generator).
void WalkArchive(std::span<const u8> data, const std::string& name,
                 const ZstdDictionaries* dictionaries,
                 const std::function<void(const ShaderRecord&)>& on_record,
                 const std::function<void(const std::string&)>& on_member_error);

} // namespace VideoCommon::ShaderWarm

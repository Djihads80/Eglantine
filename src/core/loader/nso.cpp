// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_funcs.h"
#include "common/game_presets.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/lz4_compression.h"
#include "common/zbic_compression.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/nso.h"
#include "core/memory.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

namespace Loader {
namespace {

// Mario Kart World: reduced detail (Settings::values.mkw_lod_distance, mkw_shadow_cascades).
// Pattern-matched text and rodata patches to the module before it is mapped, each applied only
// when its pattern occurs exactly once. The settings select two shadow cascades and the LOD
// distance ratio key renamed (the game then keeps the constructor's 1.0 instead of the loaded
// 2.0). EDEN_NX2_MKW_DETAIL, when set, replaces the set: a comma list of "cascades=1|2|3" (3: no
// patch), "lod=1.0|0.75|0.5" (below 1.0 the constructor default is patched too), "elink"
// (vehicle count divisor key), "emit" (single emit key), "traffic" (mob car count and distance
// keys), e.g. "cascades=2,lod=1.0,elink,emit,traffic".
struct MkwDetailOptions {
    u32 cascades{3};
    bool lod_key{};
    u32 lod_default{0}; ///< 0: keep the constructor's 1.0; else the mov w8 immediate to use
    bool elink{};
    bool emit{};
    bool traffic{};
};

MkwDetailOptions ParseMkwDetail(std::string_view text) {
    MkwDetailOptions options{.cascades = 3, .lod_key = false};
    while (!text.empty()) {
        const size_t comma = text.find(',');
        const std::string_view token = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        if (token.empty()) {
            continue;
        }
        if (token == "elink") {
            options.elink = true;
        } else if (token == "emit") {
            options.emit = true;
        } else if (token == "traffic") {
            options.traffic = true;
        } else if (token.starts_with("cascades=")) {
            const std::string_view value = token.substr(9);
            options.cascades = value == "1" ? 1U : value == "2" ? 2U : 3U;
        } else if (token.starts_with("lod=")) {
            const std::string_view value = token.substr(4);
            options.lod_key = true;
            if (value == "1.0" || value == "1") {
                options.lod_default = 0;
            } else if (value == "0.75") {
                options.lod_default = 0x52A7E808; // mov w8, #0x3f40'0000 (0.75f)
            } else if (value == "0.5") {
                options.lod_default = 0x52A7E008; // mov w8, #0x3f00'0000 (0.5f)
            } else {
                LOG_WARNING(Loader, "EDEN_NX2_MKW_DETAIL: unknown LOD ratio '{}' (1.0, 0.75, 0.5)",
                            value);
                options.lod_key = false;
            }
        } else {
            LOG_WARNING(Loader, "EDEN_NX2_MKW_DETAIL: unknown item '{}'", token);
        }
    }
    return options;
}

/// The offset of `needle` in `hay` when it occurs exactly once; `matches` gets the count.
std::optional<size_t> FindOnce(std::span<const u8> hay, std::span<const u8> needle,
                               size_t& matches) {
    matches = 0;
    std::optional<size_t> found;
    auto it = hay.begin();
    while (true) {
        it = std::search(it, hay.end(), needle.begin(), needle.end());
        if (it == hay.end()) {
            break;
        }
        if (++matches == 1) {
            found = static_cast<size_t>(it - hay.begin());
        }
        ++it;
    }
    return matches == 1 ? found : std::nullopt;
}

/// Replaces the first word of the one occurrence of `sequence` in the text segment.
void PatchMkwWords(std::span<u8> text, u64 text_offset, std::span<const u32> sequence,
                   u32 replacement, std::string_view what) {
    std::array<u8, 64> needle{};
    const size_t bytes = sequence.size() * sizeof(u32);
    ASSERT(bytes <= needle.size());
    std::memcpy(needle.data(), sequence.data(), bytes);
    size_t matches = 0;
    const auto at = FindOnce(text, std::span<const u8>{needle.data(), bytes}, matches);
    if (!at) {
        LOG_WARNING(Loader, "Mario Kart World: {} patch skipped ({} matches)", what, matches);
        return;
    }
    std::memcpy(text.data() + *at, &replacement, sizeof(replacement));
    LOG_INFO(Loader, "Mario Kart World: {} (main+{:#x})", what, text_offset + *at);
}

/// Overwrites the one occurrence of the NUL-framed `key` in rodata with `replacement`, of the
/// same length (NUL padding included).
void PatchMkwKey(std::span<u8> rodata, u64 rodata_offset, std::string_view key,
                 std::string_view replacement, std::string_view what) {
    ASSERT(key.size() == replacement.size());
    std::array<u8, 64> needle{};
    ASSERT(key.size() + 2 <= needle.size());
    needle[0] = 0;
    std::memcpy(needle.data() + 1, key.data(), key.size());
    needle[key.size() + 1] = 0;
    size_t matches = 0;
    const auto at = FindOnce(rodata, std::span<const u8>{needle.data(), key.size() + 2}, matches);
    if (!at) {
        LOG_WARNING(Loader, "Mario Kart World: {} patch skipped ({} matches)", what, matches);
        return;
    }
    std::memcpy(rodata.data() + *at + 1, replacement.data(), replacement.size());
    LOG_INFO(Loader, "Mario Kart World: {} (main+{:#x})", what, rodata_offset + *at + 1);
}

struct MODHeader {
    u32_le magic;
    u32_le dynamic_offset;
    u32_le bss_start_offset;
    u32_le bss_end_offset;
    u32_le eh_frame_hdr_start_offset;
    u32_le eh_frame_hdr_end_offset;
    u32_le module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(MODHeader) == 0x1c, "MODHeader has incorrect size.");

constexpr u32 PageAlignSize(u32 size) {
    return static_cast<u32>((size + Core::Memory::YUZU_PAGEMASK) & ~Core::Memory::YUZU_PAGEMASK);
}
} // Anonymous namespace

bool NSOHeader::IsSegmentCompressed(size_t segment_num) const {
    ASSERT_MSG(segment_num < 3, "Invalid segment {}", segment_num);
    return ((flags >> segment_num) & 1) != 0;
}

AppLoader_NSO::AppLoader_NSO(FileSys::VirtualFile file_) : AppLoader(std::move(file_)) {}

FileType AppLoader_NSO::IdentifyType(const FileSys::VirtualFile& in_file) {
    u32 magic = 0;
    if (in_file->ReadObject(&magic) != sizeof(magic)) {
        return FileType::Error;
    }

    if (Common::MakeMagic('N', 'S', 'O', '0') != magic) {
        return FileType::Error;
    }

    return FileType::NSO;
}

std::optional<VAddr> AppLoader_NSO::LoadModule(Kernel::KProcess& process, Core::System& system, const FileSys::VfsFile& nso_file, VAddr load_base, bool should_pass_arguments, bool load_into_process, std::optional<FileSys::PatchManager> pm, std::vector<Core::NCE::Patcher>* patches, s32 patch_index) {
    if (nso_file.GetSize() < sizeof(NSOHeader))
        return std::nullopt;
    NSOHeader nso_header{};
    if (sizeof(NSOHeader) != nso_file.ReadObject(&nso_header))
        return std::nullopt;
    if (nso_header.magic != Common::MakeMagic('N', 'S', 'O', '0'))
        return std::nullopt;
    if (nso_header.segments.empty())
        return std::nullopt;

    // Allocate some space at the beginning if we are patching in PreText mode.
    const size_t module_start = [&]() -> size_t {
#ifdef HAS_NCE
        if (patches && load_into_process) {
            auto* patch = &patches->operator[](patch_index);
            if (patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
                return patch->GetSectionSize();
            } else if (patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
                return patch->GetPreSectionSize();
            }
        }
#endif
        return 0;
    }();

    auto const last_segment_it = &nso_header.segments[nso_header.segments.size() - 1];
    // Build program image directly in codeset memory :)
    Kernel::CodeSet codeset;
    codeset.memory.resize(module_start + last_segment_it->location + last_segment_it->size);
    {
        std::vector<u8> compressed_data(*std::ranges::max_element(nso_header.segments_compressed_size));
        std::vector<u8> decompressed_size(std::ranges::max_element(nso_header.segments, [](auto const& a, auto const& b) {
            return a.size < b.size;
        })->size);
        for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
            nso_file.Read(compressed_data.data(), nso_header.segments_compressed_size[i], nso_header.segments[i].offset);
            if (nso_header.IsSegmentCompressed(i)) {
                if (nso_header.IsZBICCompressed()) {
                    // ZBIC compression
                    const int r = Common::Compression::DecompressDataZBIC(
                        std::span<u8>{decompressed_size}.first(nso_header.segments[i].size),
                        std::span<const u8>{compressed_data}.first(nso_header.segments_compressed_size[i])
                    );
                    ASSERT(r > 0);
                } else {
                    // LZ4 compression
                    int r = Common::Compression::DecompressDataLZ4(
                        decompressed_size.data(),
                        nso_header.segments[i].size,
                        compressed_data.data(),
                        nso_header.segments_compressed_size[i]
                    );
                    ASSERT(r == int(nso_header.segments[i].size));
                }

                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    decompressed_size.data(),
                    nso_header.segments[i].size
                );
            } else {
                // Not compressed
                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    compressed_data.data(),
                    nso_header.segments[i].size
                );
            }
            codeset.segments[i].addr = module_start + nso_header.segments[i].location;
            codeset.segments[i].offset = module_start + nso_header.segments[i].location;
            codeset.segments[i].size = nso_header.segments[i].size;
        }
    }

    if (should_pass_arguments && !Settings::values.program_args.GetValue().empty()) {
        const auto arg_data{Settings::values.program_args.GetValue()};

        codeset.DataSegment().size += NSO_ARGUMENT_DATA_ALLOCATION_SIZE;
        NSOArgumentHeader args_header{NSO_ARGUMENT_DATA_ALLOCATION_SIZE, static_cast<u32_le>(arg_data.size()), {}};
        const auto end_offset = codeset.memory.size();
        codeset.memory.resize(u32(codeset.memory.size()) + NSO_ARGUMENT_DATA_ALLOCATION_SIZE);
        std::memcpy(codeset.memory.data() + end_offset, &args_header, sizeof(NSOArgumentHeader));
        std::memcpy(codeset.memory.data() + end_offset + sizeof(NSOArgumentHeader), arg_data.data(), arg_data.size());
    }

    codeset.DataSegment().size += nso_header.segments[2].bss_size;
    u32 image_size = PageAlignSize(u32(codeset.memory.size()) + nso_header.segments[2].bss_size);
    codeset.memory.resize(image_size);

    for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
        codeset.segments[i].size = PageAlignSize(codeset.segments[i].size);
    }

    // Apply patches if necessary
    const auto name = nso_file.GetName();
    if (pm && (pm->HasNSOPatch(nso_header.build_id, name) || Settings::values.dump_nso)) {
        std::span<u8> patchable_section(codeset.memory.data() + module_start, codeset.memory.size() - module_start);
        std::vector<u8> pi_header(sizeof(NSOHeader) + patchable_section.size());
        std::memcpy(pi_header.data(), &nso_header, sizeof(NSOHeader));
        std::memcpy(pi_header.data() + sizeof(NSOHeader), patchable_section.data(),
                    patchable_section.size());

        pi_header = pm->PatchNSO(pi_header, name);

        std::copy(pi_header.begin() + sizeof(NSOHeader), pi_header.end(), patchable_section.data());
    }

    // Mario Kart World selects a 1- or 2-vblank frame mode in its main loop (mode 0: dynamic, from
    // the CPU frame time at [x19, #0x128]; 1: one vblank; 2: two vblanks). Force its native 30 FPS
    // mode, its 60 FPS mode, or let it choose dynamically using the whole frame time (CPU + GPU
    // wait), in that order of precedence. Match the instruction sequence instead of relying on a
    // title-version-specific address.
    const bool mkw_30 = Settings::values.mkw_30fps.GetValue();
    const bool mkw_60 = !mkw_30 && Settings::values.mkw_60fps.GetValue();
    const bool mkw_dynamic = !mkw_30 && !mkw_60 && Settings::values.mkw_dynamic_fps.GetValue();
    bool mkw_dynamic_fps_patch_applied = false;
    if ((mkw_30 || mkw_60 || mkw_dynamic) && name == "main" &&
        process.GetProgramId() == Settings::MarioKartWorldProgramId) {
        constexpr u32 ModeLoadInsn = 0xB9421268; // ldr w8, [x19, #0x210]
        const u32 mode = mkw_dynamic ? 0U : (mkw_60 ? 1U : 2U);
        const u32 mode_instruction = 0x52800008U | (mode << 5); // mov w8, #mode
        const auto& text = nso_header.segments[0];
        u8* const code = codeset.memory.data() + module_start + text.location;
        const size_t count = text.size / sizeof(u32);
        const auto word = [code](size_t index) {
            u32 value;
            std::memcpy(&value, code + index * sizeof(u32), sizeof(value));
            return value;
        };
        size_t mode_site = 0;
        size_t mode_matches = 0;
        for (size_t i = 2; i + 8 < count; ++i) {
            if (word(i) != ModeLoadInsn || word(i - 2) != 0x6B01013F || // cmp w9, w1
                (word(i - 1) & 0xFF00001F) != 0x54000001 ||             // b.ne
                (word(i + 1) & 0xFF00001F) != 0x35000008) {             // cbnz w8
                continue;
            }
            for (size_t j = i + 2; j < i + 8; ++j) {
                if (word(j) == 0x52A84E08) { // mov w8, #60.0f
                    mode_site = i;
                    ++mode_matches;
                    break;
                }
            }
        }

        size_t frame_time_site = 0;
        size_t frame_time_matches = 0;
        if (mode_matches == 1 && mkw_dynamic) {
            for (size_t i = mode_site + 1; i < mode_site + 32 && i < count; ++i) {
                if ((word(i) & 0xFFFFFFE0) == 0xF9409660) { // ldr xN, [x19, #0x128]
                    frame_time_site = i;
                    ++frame_time_matches;
                }
            }
        }

        if (mode_matches == 1 && (!mkw_dynamic || frame_time_matches == 1)) {
            std::memcpy(code + mode_site * sizeof(u32), &mode_instruction,
                        sizeof(mode_instruction));
            if (mkw_dynamic) {
                const u32 whole_frame_time = 0xF9407660 | (word(frame_time_site) & 0x1F);
                std::memcpy(code + frame_time_site * sizeof(u32), &whole_frame_time,
                            sizeof(whole_frame_time));
                mkw_dynamic_fps_patch_applied = true;
            }
            LOG_INFO(Loader, "Mario Kart World: {} FPS mode enabled (main+{:#x})",
                     mkw_dynamic ? "dynamic" : (mkw_60 ? "60" : "30"),
                     text.location + mode_site * sizeof(u32));
        } else {
            LOG_WARNING(Loader,
                        "Mario Kart World FPS patch skipped (mode matches {}, frame-time matches {})",
                        mode_matches, frame_time_matches);
        }
    }

    // Mario Kart World: reduced detail (see MkwDetailOptions). The environment list, when set,
    // replaces the settings (with both off too, as an opt-in for testing).
    if (name == "main" && process.GetProgramId() == Settings::MarioKartWorldProgramId) {
        const char* const env = std::getenv("EDEN_NX2_MKW_DETAIL");
        const bool lod_setting = Settings::values.mkw_lod_distance.GetValue();
        const bool cascades_setting = Settings::values.mkw_shadow_cascades.GetValue();
        if (lod_setting || cascades_setting || env != nullptr) {
            const MkwDetailOptions options =
                env != nullptr ? ParseMkwDetail(env)
                               : MkwDetailOptions{.cascades = cascades_setting ? 2U : 3U,
                                                  .lod_key = lod_setting};
            const auto& text = nso_header.segments[0];
            const auto& rodata = nso_header.segments[1];
            const std::span<u8> text_bytes{codeset.memory.data() + module_start + text.location,
                                           text.size};
            const std::span<u8> rodata_bytes{
                codeset.memory.data() + module_start + rodata.location, rodata.size};
            if (options.cascades < 3) {
                // DepthShadow init: ldr w8, [x21, #0x1a48] (the cascade count) -> mov w8, #N.
                static constexpr std::array<u32, 7> cascades_sequence{
                    0xB95A4AA8, 0x6B09011F, 0x1A89B108, 0xF941B6C9,
                    0x7100051F, 0x1A9FC508, 0xB9022AC8};
                PatchMkwWords(text_bytes, text.location, cascades_sequence,
                              0x52800008U | (options.cascades << 5),
                              options.cascades == 1 ? "1 shadow cascade" : "2 shadow cascades");
            }
            if (options.lod_key) {
                PatchMkwKey(rodata_bytes, rodata.location, "LODDistanceRatio", "LODDistanceQatio",
                            "LOD distance ratio key renamed (constructor default kept)");
            }
            if (options.lod_default != 0) {
                // The constructor: mov w8, #1.0f stored to the ratio field.
                static constexpr std::array<u32, 8> lod_sequence{
                    0x52A7F008, 0xF9001A7F, 0xF803C27F, 0xF804427F,
                    0xF807C27F, 0xF808427F, 0x79011A7F, 0xB9003A68};
                PatchMkwWords(text_bytes, text.location, lod_sequence, options.lod_default,
                              options.lod_default == 0x52A7E808 ? "LOD distance ratio default 0.75"
                                                                 : "LOD distance ratio default 0.5");
            }
            if (options.elink) {
                PatchMkwKey(rodata_bytes, rodata.location, "VehicleNumNoDiv",
                            std::string_view{"VehicleNumDiv2\0", 15}, "vehicle count divided by 2");
            }
            if (options.emit) {
                PatchMkwKey(rodata_bytes, rodata.location, "NoEmitSingle",
                            std::string_view{"NoEmitDiv2\0\0", 12}, "single emits divided by 2");
            }
            if (options.traffic) {
                PatchMkwKey(rodata_bytes, rodata.location, "MobCarDispNum", "MobCarQispNum",
                            "mob car count key renamed");
                PatchMkwKey(rodata_bytes, rodata.location, "MobCarDispDist", "MobCarQispDist",
                            "mob car distance key renamed");
            }
        }
    }

#ifdef HAS_NCE
    // If we are computing the process code layout and using nce backend, patch.
    const auto& code = codeset.CodeSegment();
    auto* patch = patches ? &patches->operator[](patch_index) : nullptr;
    if (patch && !load_into_process) {
        //Set module ID using build_id from the NSO header
        patch->SetModuleID(nso_header.build_id);
        // Patch SVCs and MRS calls in the guest code
        while (!patch->PatchText(codeset.memory, code)) {
            patch = &patches->emplace_back();
            patch->SetModuleID(nso_header.build_id);  // In case the patcher is changed for big modules, the new patcher should also have the build_id
        }
    } else if (patch) {
        // Relocate code patch and copy to the program image.
        // Save size before RelocateAndCopy (which may resize)
        const size_t size_before_relocate = codeset.memory.size();
        if (patch->RelocateAndCopy(load_base, code, codeset.memory, &process.GetPostHandlers())) {
            // Update patch section.
            auto& patch_segment = codeset.PatchSegment();
            auto& post_patch_segment = codeset.PostPatchSegment();
            const auto patch_mode = patch->GetPatchMode();
            if (patch_mode == Core::NCE::PatchMode::PreText) {
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else if (patch_mode == Core::NCE::PatchMode::Split) {
                // For Split-mode, we are using pre-patch buffer at start, post-patch buffer at end
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetPreSectionSize());
                post_patch_segment.addr = size_before_relocate;
                post_patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else {
                patch_segment.addr = image_size;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            }
        }

        // Refresh image_size to take account the patch section if it was added by RelocateAndCopy
        image_size = static_cast<u32>(codeset.memory.size());
    }
#endif

    // If we aren't actually loading (i.e. just computing the process code layout), we are done
    if (!load_into_process) {
#ifdef HAS_NCE
        // Ok, so for Split mode, we need to account for pre-patch and post-patch space
        // which will be added during RelocateAndCopy in the second pass. Where it crashed
        // in Android Studio at PreText. May be a better way. Works for now.
        if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
            return load_base + patch->GetPreSectionSize() + image_size + patch->GetSectionSize();
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
            return load_base + patch->GetSectionSize() + image_size;
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PostData) {
            return load_base + image_size + patch->GetSectionSize();
        }
#endif
        return load_base + image_size;
    }

    // Apply cheats if they exist and the program has a valid title ID
    if (pm) {
        // TODO(Maufeat): Check if there is a better way to check
        if (name == "main")
            system.SetApplicationProcessBuildID(nso_header.build_id);

        const auto cheats = pm->CreateCheatList(nso_header.build_id);
        if (!cheats.empty()) {
            system.RegisterCheatList(cheats, nso_header.build_id, load_base, image_size);
        }
    }

    // Load codeset for current process
    LOG_INFO(Loader, "Module {} loaded at {:#x} (size {:#x})", nso_file.GetName(), load_base, image_size);
    process.LoadModule(system.Kernel(), std::move(codeset), load_base);
    if (name == "main" && process.GetProgramId() == Settings::MarioKartWorldProgramId) {
        process.SetMkwDynamicFpsPatchApplied(mkw_dynamic_fps_patch_applied);
    }
    return load_base + image_size;
}

AppLoader_NSO::LoadResult AppLoader_NSO::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    modules.clear();

    // Load module
    const VAddr base_address = GetInteger(process.GetEntryPoint());
    if (!LoadModule(process, system, *file, base_address, true, true)) {
        return {ResultStatus::ErrorLoadingNSO, {}};
    }

    modules.insert_or_assign(base_address, file->GetName());
    LOG_DEBUG(Loader, "loaded module {} @ {:#x}", file->GetName(), base_address);

    is_loaded = true;
    return {ResultStatus::Success, LoadParameters{Kernel::KThread::DefaultThreadPriority,
                                                  Core::Memory::DEFAULT_STACK_SIZE}};
}

ResultStatus AppLoader_NSO::ReadNSOModules(Modules& out_modules) {
    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader

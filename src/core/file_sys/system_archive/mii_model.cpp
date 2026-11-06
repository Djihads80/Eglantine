// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <utility>

#include "core/file_sys/romfs.h"
#include "core/file_sys/system_archive/mii_model.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys::SystemArchive {

namespace MiiModelData {

constexpr std::array<u8, 0x10> NFTR_STANDARD{'N',  'F',  'T',  'R',  0x01, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr std::array<u8, 0x10> NFSR_STANDARD{'N',  'F',  'S',  'R',  0x01, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

constexpr auto TEXTURE_LOW_LINEAR = NFTR_STANDARD;
constexpr auto TEXTURE_LOW_SRGB = NFTR_STANDARD;
constexpr auto TEXTURE_MID_LINEAR = NFTR_STANDARD;
constexpr auto TEXTURE_MID_SRGB = NFTR_STANDARD;
constexpr auto SHAPE_HIGH = NFSR_STANDARD;
constexpr auto SHAPE_MID = NFSR_STANDARD;

} // namespace MiiModelData

VirtualDir MiiModel() {
    auto out = std::make_shared<VectorVfsDirectory>(std::vector<VirtualFile>{},
                                                    std::vector<VirtualDir>{}, "data");

    out->AddFile(MakeArrayFile(MiiModelData::TEXTURE_LOW_LINEAR, "NXTextureLowLinear.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::TEXTURE_LOW_SRGB, "NXTextureLowSRGB.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::TEXTURE_MID_LINEAR, "NXTextureMidLinear.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::TEXTURE_MID_SRGB, "NXTextureMidSRGB.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::SHAPE_HIGH, "ShapeHigh.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::SHAPE_MID, "ShapeMid.dat"));

    return out;
}

VirtualFile MiiResource(VirtualFile mii_model_romfs) {
    // The Switch 2 archive has the Switch 1 files plus "Ounce" textures (Low, Mid and High); here
    // those names point at the NX textures, High at Mid.
    constexpr std::array<std::pair<const char*, const char*>, 6> ounce_textures{{
        {"OunceTextureLowLinear.dat", "NXTextureLowLinear.dat"},
        {"OunceTextureLowSRGB.dat", "NXTextureLowSRGB.dat"},
        {"OunceTextureMidLinear.dat", "NXTextureMidLinear.dat"},
        {"OunceTextureMidSRGB.dat", "NXTextureMidSRGB.dat"},
        {"OunceTextureHighLinear.dat", "NXTextureMidLinear.dat"},
        {"OunceTextureHighSRGB.dat", "NXTextureMidSRGB.dat"},
    }};
    VirtualDir model = mii_model_romfs != nullptr ? ExtractRomFS(mii_model_romfs) : nullptr;
    if (model == nullptr) {
        model = MiiModel();
    }
    auto out = std::make_shared<VectorVfsDirectory>(model->GetFiles(), std::vector<VirtualDir>{},
                                                    "data");
    for (const auto& [name, source] : ounce_textures) {
        if (const auto file = model->GetFile(source); file != nullptr) {
            out->AddFile(std::make_shared<OffsetVfsFile>(file, file->GetSize(), 0, name));
        }
    }
    return CreateRomFS(out);
}

} // namespace FileSys::SystemArchive

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>

#include <algorithm>
#include <filesystem>
#include <vector>
#include <cstdlib>
#include "common/dynamic_library.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/vulkan_common/vulkan_library.h"

namespace Vulkan {

std::shared_ptr<Common::DynamicLibrary> OpenLibrary(
    [[maybe_unused]] Core::Frontend::GraphicsContext* context) {
    LOG_DEBUG(Render_Vulkan, "Looking for a Vulkan library");
#if defined(__ANDROID__) && defined(ARCHITECTURE_arm64)
    // Android manages its Vulkan driver from the frontend.
    return context->GetDriverLibrary();
#else
    auto library = std::make_shared<Common::DynamicLibrary>();
#ifdef __APPLE__
    const auto libvulkan_filename =
        Common::FS::GetBundleDirectory() / "Contents/Frameworks/libvulkan.1.dylib";
    const auto libmoltenvk_filename =
        Common::FS::GetBundleDirectory() / "Contents/Frameworks/libMoltenVK.dylib";
    // Outside a bundle that carries its own Vulkan (or when it doesn't), fall back to the LunarG
    // Vulkan SDK's loader, installed system-wide or in the user's home. With the SDK's loader and
    // no driver chosen, prefer KosmicKrisp (Mesa's conformant Vulkan-on-Metal) over MoltenVK:
    // it compiles every shader this emulator emits where MoltenVK rejects several hundred.
    std::vector<std::string> sdk_libraries;
    std::vector<std::string> sdk_kosmickrisp_icds;
    const auto add_sdk = [&](const std::filesystem::path& prefix) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(prefix / "lib/libvulkan.1.dylib", ec)) {
            sdk_libraries.push_back((prefix / "lib/libvulkan.1.dylib").string());
            sdk_kosmickrisp_icds.push_back(
                (prefix / "share/vulkan/icd.d/libkosmickrisp_icd.json").string());
        }
    };
    add_sdk("/usr/local");
    if (const char* home = std::getenv("HOME")) {
        std::error_code ec;
        const std::filesystem::path sdk_root = std::filesystem::path(home) / "VulkanSDK";
        std::vector<std::filesystem::path> versions;
        for (const auto& entry : std::filesystem::directory_iterator(sdk_root, ec)) {
            versions.push_back(entry.path() / "macOS");
        }
        std::sort(versions.rbegin(), versions.rend());
        for (const auto& version : versions) {
            add_sdk(version);
        }
    }
    std::vector<const char*> library_paths{std::getenv("LIBVULKAN_PATH"),
                                           libvulkan_filename.c_str(),
                                           libmoltenvk_filename.c_str()};
    for (const auto& sdk_library : sdk_libraries) {
        library_paths.push_back(sdk_library.c_str());
    }
    // Check if a path to a specific Vulkan library has been specified.
    for (size_t i = 0; i < library_paths.size(); ++i) {
        const char* library_path = library_paths[i];
        if (!library_path || !library->Open(library_path)) {
            continue;
        }
        LOG_INFO(Render_Vulkan, "Using Vulkan library {}", library_path);
        const size_t sdk_index = i >= 3 ? i - 3 : sdk_kosmickrisp_icds.size();
        if (sdk_index < sdk_kosmickrisp_icds.size() && std::getenv("VK_DRIVER_FILES") == nullptr &&
            std::getenv("VK_ICD_FILENAMES") == nullptr) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(sdk_kosmickrisp_icds[sdk_index], ec)) {
                setenv("VK_DRIVER_FILES", sdk_kosmickrisp_icds[sdk_index].c_str(), 0);
                LOG_INFO(Render_Vulkan, "Selecting KosmicKrisp: {}", sdk_kosmickrisp_icds[sdk_index]);
            }
        }
        break;
    }
#else
    std::string filename = Common::DynamicLibrary::GetVersionedFilename("vulkan", 1);
    LOG_DEBUG(Render_Vulkan, "Trying Vulkan library: {}", filename);
    if (!library->Open(filename.c_str())) {
        // Android devices may not have libvulkan.so.1, only libvulkan.so.
        filename = Common::DynamicLibrary::GetVersionedFilename("vulkan");
        LOG_DEBUG(Render_Vulkan, "Trying Vulkan library (second attempt): {}", filename);
        void(library->Open(filename.c_str()));
    }
#endif
    return library;
#endif
}

} // namespace Vulkan

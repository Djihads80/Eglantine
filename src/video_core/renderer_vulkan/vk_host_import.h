// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <bit>
#include <cstdint>
#include <unordered_map>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

/// Guest memory imported as Vulkan buffers (VK_EXT_external_memory_host), so the host GPU reads
/// the guest's own pages with no copy (EDEN_NX2_HOST_IMPORT=1, experimental). A window is the
/// page-aligned span of the range asked for; windows live as long as the device. Any failure turns
/// the importer off and the caller copies as before.
class HostImporter {
public:
    struct Ref {
        VkBuffer buffer{};
        VkDeviceAddress address{};
        u32 offset{}; ///< Of the asked range inside the window's buffer
    };

    explicit HostImporter(const Device& device_) : device{device_} {}

    [[nodiscard]] bool Usable() const noexcept {
        return !failed && device.IsExtExternalMemoryHost();
    }

    /// Imports (once) the pages covering [host_ptr, host_ptr + size) and describes the range.
    /// Returns false when it cannot, which the caller treats as "copy instead".
    [[nodiscard]] bool Get(const u8* host_ptr, u64 size, Ref& out) {
        if (!Usable() || host_ptr == nullptr || size == 0) {
            return false;
        }
        const uintptr_t address = reinterpret_cast<uintptr_t>(host_ptr);
        const uintptr_t first = address & ~uintptr_t{PageMask};
        const uintptr_t last = (address + size + PageMask) & ~uintptr_t{PageMask};
        const Key key{first, last - first};
        auto it = windows.find(key);
        if (it == windows.end()) {
            if (windows.size() >= MaxWindows) {
                return false;
            }
            it = windows.emplace(key, Import(reinterpret_cast<const void*>(first), last - first))
                     .first;
            if (failed) {
                return false;
            }
        }
        const Window& window = it->second;
        if (window.buffer == VK_NULL_HANDLE) {
            return false;
        }
        out = Ref{window.buffer, window.address, static_cast<u32>(address - first)};
        return true;
    }

    [[nodiscard]] size_t WindowCount() const noexcept {
        return windows.size();
    }

private:
    static constexpr uintptr_t PageMask = 0xfff;
    static constexpr size_t MaxWindows = 1024;

    struct Key {
        uintptr_t start;
        uintptr_t size;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            return std::hash<uintptr_t>{}(key.start) ^ (std::hash<uintptr_t>{}(key.size) << 1);
        }
    };
    using SparseBufferHandle = vk::Handle<VkBuffer, VkDevice, vk::DeviceDispatch>;
    struct Window {
        vk::DeviceMemory memory;
        SparseBufferHandle buffer_handle;
        VkBuffer buffer{};
        VkDeviceAddress address{};
    };
    Window Import(const void* start, size_t size) {
        const VkDevice logical = *device.GetLogical();
        const auto& dld = device.GetDispatchLoader();
        VkMemoryHostPointerPropertiesEXT host_props{
            .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
            .pNext = nullptr,
            .memoryTypeBits = 0,
        };
        if (dld.vkGetMemoryHostPointerPropertiesEXT(
                logical, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, start,
                &host_props) != VK_SUCCESS) {
            return Fail("memory host pointer properties");
        }
        const bool bda = device.IsBufferDeviceAddressSupported();
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size,
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     (bda ? static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
                      : VkBufferUsageFlags{0}),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        VkBuffer raw{};
        if (dld.vkCreateBuffer(logical, &buffer_ci, nullptr, &raw) != VK_SUCCESS) {
            return Fail("buffer");
        }
        Window window;
        window.buffer_handle = SparseBufferHandle{raw, logical, dld};
        const VkMemoryRequirements requirements = device.GetLogical().GetBufferMemoryRequirements(raw);
        const u32 bits = host_props.memoryTypeBits & requirements.memoryTypeBits;
        if (bits == 0) {
            return Fail("memory type");
        }
        const VkMemoryAllocateFlagsInfo flags_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .pNext = nullptr,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
            .deviceMask = 0,
        };
        const VkImportMemoryHostPointerInfoEXT import_info{
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
            .pNext = bda ? &flags_info : nullptr,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            .pHostPointer = const_cast<void*>(start),
        };
        const VkMemoryAllocateInfo allocate_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &import_info,
            .allocationSize = size,
            .memoryTypeIndex = static_cast<u32>(std::countr_zero(bits)),
        };
        window.memory = device.GetLogical().TryAllocateMemory(allocate_info);
        if (!window.memory) {
            return Fail("memory import");
        }
        if (dld.vkBindBufferMemory(logical, raw, *window.memory, 0) != VK_SUCCESS) {
            return Fail("bind");
        }
        window.buffer = raw;
        if (bda) {
            const VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = raw,
            };
            window.address = dld.vkGetBufferDeviceAddress(logical, &info);
        }
        if (windows.empty()) {
            LOG_INFO(HW_GPU, "nx2: guest memory import active (first window {} KiB)", size >> 10);
        }
        return window;
    }

    Window Fail(const char* what) {
        LOG_WARNING(HW_GPU, "nx2: guest memory import failed ({}), copying instead", what);
        failed = true;
        return Window{};
    }

    const Device& device;
    bool failed{};
    std::unordered_map<Key, Window, KeyHash> windows;
};

} // namespace Vulkan

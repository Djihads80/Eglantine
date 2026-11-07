// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace {

std::atomic<int> validation_errors{0};

// CreateInstance only enables the layer and the repo callback only logs, so the test installs
// its own minimal messenger and counts error-severity messages.
VkBool32 VKAPI_PTR CountErrors(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                               VkDebugUtilsMessageTypeFlagsEXT,
                               const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validation_errors;
        // CI greps the log for this exact string.
        std::fprintf(stderr, "Validation Error: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

} // Anonymous namespace

// Runs against whatever Vulkan driver the machine has. CI points it at Mesa lavapipe with the
// Khronos validation layer installed; with no loader, driver or layer the test is skipped.
TEST_CASE("A headless Vulkan instance exposes a physical device", "[video_core][vulkan]") {
    const auto library = Vulkan::OpenLibrary();
    if (!library || !library->IsOpen()) {
        SKIP("no Vulkan loader on this machine");
    }
    Vulkan::vk::InstanceDispatch dld;
    Vulkan::vk::Instance instance;
    try {
        // CreateInstance silently drops the layer when it is missing, so check for it below.
        instance = Vulkan::CreateInstance(*library, dld, VK_API_VERSION_1_1,
                                          Core::Frontend::WindowSystemType::Headless, true);
    } catch (const Vulkan::vk::Exception&) {
        SKIP("no Vulkan driver on this machine");
    }
    const auto layers = Vulkan::vk::EnumerateInstanceLayerProperties(dld);
    const bool has_layer = layers && std::ranges::any_of(*layers, [](const VkLayerProperties& l) {
        return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
    });
    if (!has_layer || !dld.vkCreateDebugUtilsMessengerEXT) {
        SKIP("Khronos validation layer or VK_EXT_debug_utils not available");
    }

    validation_errors = 0;
    const Vulkan::vk::DebugUtilsMessenger messenger = instance.CreateDebugUtilsMessenger(
        VkDebugUtilsMessengerCreateInfoEXT{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = 0,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = CountErrors,
            .pUserData = nullptr,
        });

    REQUIRE(!instance.EnumeratePhysicalDevices().empty());
    REQUIRE(validation_errors.load() == 0);
}

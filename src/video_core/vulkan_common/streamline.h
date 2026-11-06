// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS through NVIDIA Streamline. Only Streamline's MIT-licensed
// headers are built in: NVIDIA's DLLs are loaded at run time from the user's PC, and none ship
// with the emulator.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace sl {
struct FrameToken;
enum class PCLMarker : uint32_t;
}

namespace Vulkan::Streamline {

/// NVIDIA's files DLSS needs, as found on this PC. Paths are empty when missing.
struct DlssFiles {
    std::filesystem::path folder;     ///< Where the loader is, or where it should go
    std::filesystem::path interposer; ///< sl.interposer.dll (Streamline's loader)
    std::filesystem::path model;      ///< nvngx_dlss.dll next to the loader
    std::filesystem::path app_model;  ///< Newest DLSS the NVIDIA app downloaded
    std::string interposer_version;
    std::string model_version;
    std::string app_model_version;
};

[[nodiscard]] DlssFiles FindDlssFiles();

/// The user folder's "streamline" folder.
[[nodiscard]] std::filesystem::path UserFolder();

/// Streamline's own modules carry a second, Streamline-only signature.
[[nodiscard]] bool IsStreamlineModule(const std::filesystem::path& path);

/// A valid Authenticode signature from NVIDIA Corporation (DLSS models).
[[nodiscard]] bool IsSignedByNvidia(const std::filesystem::path& path);

/// When the NVIDIA DLSS setting is on, loads the user's sl.interposer.dll (checking NVIDIA's
/// signature) and starts Streamline for DLSS. Call before creating the renderer's instance.
bool Start();

/// Whether Streamline is running and waiting for the renderer's device.
[[nodiscard]] bool WantsDevice();

/// Instance and device extensions DLSS asks for (empty when Streamline isn't running).
[[nodiscard]] std::vector<std::string> InstanceExtensions();
[[nodiscard]] std::vector<std::string> DeviceExtensions();
[[nodiscard]] std::vector<std::string> OptionalDeviceExtensions();

/// Hands the renderer's device to Streamline and routes the swapchain calls through it, as
/// Streamline's manual hooking needs. Stops Streamline and returns false when DLSS can't run.
bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, vk::DeviceDispatch& dld);

/// Whether DLSS can run on the attached device.
[[nodiscard]] bool IsDlssAvailable();

/// Shuts Streamline down; call before the device is destroyed.
void Stop();

/// OptiScaler interop functions
[[nodiscard]] bool IsOptiScalerPresent();
sl::FrameToken* GetOrNewFrameToken();
void AdvanceFrameToken();
bool IsRenderSubmitStarted();
void NotifyRenderSubmitStart(const sl::FrameToken* frame);
void SetReflexMarker(sl::PCLMarker marker, const sl::FrameToken* frame);
void ReflexSleep(const sl::FrameToken* frame);

} // namespace Vulkan::Streamline

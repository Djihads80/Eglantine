// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS through NVIDIA Streamline, with Streamline's manual hooking.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_api.h"

#include <sl_security.h>

#include <softpub.h>
#include <wintrust.h>

namespace Vulkan::Streamline {
namespace {

// A custom engine names itself with its own project id; an application id from NVIDIA is only
// needed without an engine type. Keep it fixed: the driver may key behaviour off it.
constexpr const char* PROJECT_ID = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";
constexpr const wchar_t* INTERPOSER = L"sl.interposer.dll";
constexpr const wchar_t* MODEL = L"nvngx_dlss.dll";
// The first DLSS with preset K.
constexpr u32 MIN_DLSS_MAJOR = 310;

using Version = std::array<u32, 4>;

struct State {
    HMODULE module{};
    Api api{};
    std::wstring plugin_dir;
    std::wstring model_dir;
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    std::vector<std::string> optional_device_extensions;
    bool started{};
    bool attached{};
    sl::FrameToken* current_frame_token{};
    bool render_submit_started{};
    std::mutex token_mutex;
};

State& GetState() {
    static State state;
    return state;
}

int LogLevel() {
    const char* const value = std::getenv("EDEN_NX2_DLSS_LOG");
    return value ? std::atoi(value) : 0;
}

void LogFromStreamline(sl::LogType type, const char* message) {
    std::string_view text{message ? message : ""};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    switch (type) {
    case sl::LogType::eError:
        LOG_ERROR(Render_Vulkan, "Streamline: {}", text);
        break;
    case sl::LogType::eWarn:
        LOG_WARNING(Render_Vulkan, "Streamline: {}", text);
        break;
    default:
        if (LogLevel() > 0) {
            LOG_INFO(Render_Vulkan, "Streamline: {}", text);
        }
        break;
    }
}

/// Where the user may put sl.interposer.dll: EDEN_STREAMLINE_DIR, the user folder's "streamline"
/// folder, then a "streamline" folder next to the executable.
std::vector<std::filesystem::path> SearchDirs() {
    std::vector<std::filesystem::path> dirs;
    if (const char* const dir = std::getenv("EDEN_STREAMLINE_DIR"); dir && *dir) {
        dirs.emplace_back(dir);
    }
    dirs.push_back(UserFolder());
    dirs.push_back(Common::FS::GetExeDirectory() / "streamline");
    return dirs;
}

template <typename T>
bool Resolve(HMODULE module, const char* name, T*& function) {
    function = reinterpret_cast<T*>(GetProcAddress(module, name));
    if (!function) {
        LOG_ERROR(Render_Vulkan, "sl.interposer.dll has no {}", name);
    }
    return function != nullptr;
}

std::optional<Version> ReadVersion(const std::filesystem::path& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) {
        return std::nullopt;
    }
    std::vector<u8> data(size);
    VS_FIXEDFILEINFO* info{};
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
        return std::nullopt;
    }
    return Version{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                   HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
}

std::string VersionText(const std::filesystem::path& path) {
    const std::optional<Version> version = path.empty() ? std::nullopt : ReadVersion(path);
    if (!version) {
        return {};
    }
    const Version& v = *version;
    return v[3] == 0 ? fmt::format("{}.{}.{}", v[0], v[1], v[2])
                     : fmt::format("{}.{}.{}.{}", v[0], v[1], v[2], v[3]);
}

/// The NVIDIA app keeps every DLSS it downloads under ProgramData; pick the newest.
std::filesystem::path FindAppModel() {
    const wchar_t* const program_data = _wgetenv(L"ProgramData");
    if (!program_data) {
        return {};
    }
    const std::filesystem::path root =
        std::filesystem::path{program_data} / L"NVIDIA" / L"NGX" / L"models" / L"dlss" / L"versions";
    std::filesystem::path best_path;
    Version best{};
    std::error_code ec;
    for (const auto& version_dir : std::filesystem::directory_iterator(root, ec)) {
        std::error_code files_ec;
        for (const auto& file :
             std::filesystem::directory_iterator(version_dir.path() / L"files", files_ec)) {
            if (file.path().extension() != L".bin") {
                continue;
            }
            const std::optional<Version> version = ReadVersion(file.path());
            if (version && (*version)[0] >= MIN_DLSS_MAJOR && *version > best) {
                best = *version;
                best_path = file.path();
            }
        }
    }
    return best_path;
}

/// Copies the NVIDIA app's DLSS into our folder as nvngx_dlss.dll, the name Streamline loads.
std::filesystem::path UseAppModel(const DlssFiles& files) {
    const std::filesystem::path dir = UserFolder() / L"nvidia-app";
    const std::filesystem::path target = dir / MODEL;
    std::error_code ec;
    const bool current = std::filesystem::is_regular_file(target, ec) &&
                         std::filesystem::file_size(target, ec) ==
                             std::filesystem::file_size(files.app_model, ec) &&
                         ReadVersion(target) == ReadVersion(files.app_model);
    if (!current) {
        if (!IsSignedByNvidia(files.app_model)) {
            LOG_WARNING(Render_Vulkan, "The NVIDIA app's DLSS {} is not signed by NVIDIA",
                        Common::FS::PathToUTF8String(files.app_model));
            return {};
        }
        std::filesystem::create_directories(dir, ec);
        std::filesystem::copy_file(files.app_model, target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_WARNING(Render_Vulkan, "Could not copy the NVIDIA app's DLSS to {}: {}",
                        Common::FS::PathToUTF8String(target), ec.message());
            return {};
        }
    }
    return dir;
}

bool Load(State& state, const std::filesystem::path& path) {
    if (!IsStreamlineModule(path)) {
        LOG_ERROR(Render_Vulkan, "{} is not signed by NVIDIA; not loading it",
                  Common::FS::PathToUTF8String(path));
        return false;
    }
    state.module = LoadLibraryW(path.c_str());
    if (!state.module) {
        LOG_ERROR(Render_Vulkan, "Could not load {} (error {})", Common::FS::PathToUTF8String(path),
                  GetLastError());
        return false;
    }
    Api& api = state.api;
    const HMODULE m = state.module;
    if (Resolve(m, "slInit", api.init) && Resolve(m, "slShutdown", api.shutdown) &&
        Resolve(m, "slIsFeatureSupported", api.is_feature_supported) &&
        Resolve(m, "slGetFeatureRequirements", api.get_feature_requirements) &&
        Resolve(m, "slGetFeatureVersion", api.get_feature_version) &&
        Resolve(m, "slGetFeatureFunction", api.get_feature_function) &&
        Resolve(m, "slGetNewFrameToken", api.get_new_frame_token) &&
        Resolve(m, "slSetConstants", api.set_constants) &&
        Resolve(m, "slEvaluateFeature", api.evaluate_feature) &&
        Resolve(m, "slFreeResources", api.free_resources) &&
        Resolve(m, "slSetVulkanInfo", api.set_vulkan_info)) {
        return true;
    }
    FreeLibrary(state.module);
    state.module = nullptr;
    state.api = {};
    return false;
}

std::vector<std::string> Names(uint32_t count, const char** names) {
    std::vector<std::string> result;
    for (uint32_t i = 0; i < count; ++i) {
        result.emplace_back(names[i]);
    }
    return result;
}

} // Anonymous namespace

bool IsOptiScalerPresent() {
    static int cached = -1;
    if (cached != -1) {
        return cached == 1;
    }

    // Check common OptiScaler DLL names
    const wchar_t* dll_names[] = {L"winmm.dll", L"version.dll", L"dxgi.dll", L"d3d11.dll"};

    for (const auto* dll_name : dll_names) {
        if (HMODULE module = GetModuleHandleW(dll_name)) {
            wchar_t path[MAX_PATH];
            if (GetModuleFileNameW(module, path, MAX_PATH)) {
                DWORD handle = 0;
                DWORD size = GetFileVersionInfoSizeW(path, &handle);
                if (size > 0) {
                    std::vector<BYTE> buffer(size);
                    if (GetFileVersionInfoW(path, handle, size, buffer.data())) {
                        struct LANGANDCODEPAGE {
                            WORD language;
                            WORD codePage;
                        } *translate;
                        UINT len;
                        if (VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                                          reinterpret_cast<LPVOID*>(&translate), &len)) {
                            wchar_t subBlock[50];
                            swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\ProductName",
                                      translate[0].language, translate[0].codePage);
                            wchar_t* productName = nullptr;
                            if (VerQueryValueW(buffer.data(), subBlock,
                                              reinterpret_cast<LPVOID*>(&productName), &len)) {
                                if (productName && wcsstr(productName, L"OptiScaler")) {
                                    LOG_INFO(Render_Vulkan, "OptiScaler detected via {} (ProductName: {})",
                                            Common::FS::PathToUTF8String(path),
                                            Common::UTF16ToUTF8(productName));
                                    cached = 1;
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    cached = 0;
    return false;
}

std::filesystem::path UserFolder() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::StreamlineDir);
}

bool IsStreamlineModule(const std::filesystem::path& path) {
    return sl::security::verifyEmbeddedSignature(path.c_str());
}

bool IsSignedByNvidia(const std::filesystem::path& path) {
    const std::wstring file_path = std::filesystem::absolute(path).wstring();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = file_path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool valid = WinVerifyTrust(nullptr, &action, &data) == ERROR_SUCCESS;
    if (valid) {
        CRYPT_PROVIDER_DATA* const provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* const signer =
            provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* const cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        wchar_t name[128]{};
        valid = cert &&
                CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name,
                                   static_cast<DWORD>(std::size(name))) > 1 &&
                std::wstring_view{name} == L"NVIDIA Corporation";
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return valid;
}

DlssFiles FindDlssFiles() {
    DlssFiles files;
    files.folder = UserFolder();
    std::error_code ec;
    for (const std::filesystem::path& dir : SearchDirs()) {
        if (std::filesystem::is_regular_file(dir / INTERPOSER, ec)) {
            files.folder = dir;
            files.interposer = dir / INTERPOSER;
            break;
        }
    }
    if (std::filesystem::is_regular_file(files.folder / MODEL, ec)) {
        files.model = files.folder / MODEL;
    }
    files.app_model = FindAppModel();
    files.interposer_version = VersionText(files.interposer);
    files.model_version = VersionText(files.model);
    files.app_model_version = VersionText(files.app_model);
    return files;
}

bool Start() {
    State& state = GetState();
    if (state.started) {
        return true;
    }
    // Enable if: user setting is ON, OR OptiScaler is detected (auto-enable)
    if (!Settings::values.nvidia_dlss.GetValue() && !IsOptiScalerPresent()) {
        return false;
    }
    if (!state.module) {
        const DlssFiles files = FindDlssFiles();
        if (files.interposer.empty()) {
            LOG_WARNING(Render_Vulkan,
                        "NVIDIA DLSS is on, but sl.interposer.dll (NVIDIA Streamline) is not in "
                        "{} (the Graphics settings can download it); using FSR 3.1",
                        Common::FS::PathToUTF8String(files.folder));
            return false;
        }
        if (!Load(state, files.interposer)) {
            return false;
        }
        state.plugin_dir = files.folder.wstring();
        state.model_dir.clear();
        if (files.model.empty() && !files.app_model.empty()) {
            if (const std::filesystem::path dir = UseAppModel(files); !dir.empty()) {
                state.model_dir = dir.wstring();
                LOG_INFO(Render_Vulkan, "Using the NVIDIA app's DLSS {}", files.app_model_version);
            }
        } else if (files.model.empty()) {
            LOG_WARNING(Render_Vulkan, "No nvngx_dlss.dll in {} and no DLSS from the NVIDIA app",
                        Common::FS::PathToUTF8String(files.folder));
        }
    }

    // Streamline's plugins come from the loader's folder or the NVIDIA app's downloads (OTA); the
    // DLSS model from the loader's folder, else the NVIDIA app's copy.
    const wchar_t* plugin_paths[] = {state.plugin_dir.c_str(), state.model_dir.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences preferences{};
    preferences.logLevel = LogLevel() >= 2 ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    preferences.logMessageCallback = LogFromStreamline;
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = state.model_dir.empty() ? 1 : 2;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                        sl::PreferenceFlags::eUseManualHooking |
                        sl::PreferenceFlags::eAllowOTA |
                        sl::PreferenceFlags::eLoadDownloadedPlugins;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 3;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1.0";
    preferences.projectId = PROJECT_ID;
    preferences.renderAPI = sl::RenderAPI::eVulkan;

    LOG_INFO(Render_Vulkan,
             "Requesting Streamline features: DLSS (id={}), Reflex (id={}) and PCL (id={})",
             static_cast<int>(sl::kFeatureDLSS), static_cast<int>(sl::kFeatureReflex),
             static_cast<int>(sl::kFeaturePCL));

    const Api& api = state.api;
    if (const sl::Result result = api.init(preferences, sl::kSDKVersion);
        result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "NVIDIA Streamline failed to start (result {}); using FSR 3.1",
                  static_cast<int>(result));
        return false;
    }
    sl::FeatureRequirements requirements{};
    if (const sl::Result result = api.get_feature_requirements(sl::kFeatureDLSS, requirements);
        result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan,
                    "Streamline found no DLSS plugin (result {}); put sl.dlss.dll next to "
                    "sl.interposer.dll or install the NVIDIA app. Using FSR 3.1",
                    static_cast<int>(result));
        api.shutdown();
        return false;
    }
    if (requirements.vkNumComputeQueuesRequired != 0 ||
        requirements.vkNumGraphicsQueuesRequired != 0) {
        LOG_WARNING(Render_Vulkan, "This DLSS wants extra Vulkan queues; using FSR 3.1");
        api.shutdown();
        return false;
    }
    state.instance_extensions =
        Names(requirements.vkNumInstanceExtensions, requirements.vkInstanceExtensions);
    state.device_extensions =
        Names(requirements.vkNumDeviceExtensions, requirements.vkDeviceExtensions);
    // The KHR and EXT buffer device address extensions can't both be enabled.
    const auto& device_extensions = state.device_extensions;
    if (std::ranges::find(device_extensions, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) !=
        device_extensions.end()) {
        std::erase(state.device_extensions, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    }
    // Check if Reflex was loaded successfully
    sl::FeatureRequirements reflex_requirements{};
    if (const sl::Result result = api.get_feature_requirements(sl::kFeatureReflex, reflex_requirements);
        result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan,
                    "Streamline Reflex plugin not available (result {}). Low latency features "
                    "may not work with OptiScaler. Ensure sl.reflex.dll is in the Streamline folder.",
                    static_cast<int>(result));
    } else {
        LOG_INFO(Render_Vulkan, "Streamline Reflex plugin loaded successfully");
        if (reflex_requirements.vkNumComputeQueuesRequired != 0 ||
            reflex_requirements.vkNumGraphicsQueuesRequired != 0) {
            LOG_WARNING(Render_Vulkan, "Reflex wants extra Vulkan queues (compute: {}, graphics: {})",
                       reflex_requirements.vkNumComputeQueuesRequired,
                       reflex_requirements.vkNumGraphicsQueuesRequired);
        }
        // Merge Reflex extension requirements into our lists
        const auto reflex_inst_exts = Names(reflex_requirements.vkNumInstanceExtensions,
                                            reflex_requirements.vkInstanceExtensions);
        const auto reflex_dev_exts = Names(reflex_requirements.vkNumDeviceExtensions,
                                          reflex_requirements.vkDeviceExtensions);
        for (const auto& ext : reflex_inst_exts) {
            if (std::ranges::find(state.instance_extensions, ext) == state.instance_extensions.end()) {
                state.instance_extensions.push_back(ext);
                LOG_INFO(Render_Vulkan, "Adding Reflex instance extension: {}", ext);
            }
        }
        for (const auto& ext : reflex_dev_exts) {
            if (std::ranges::find(state.optional_device_extensions, ext) == state.optional_device_extensions.end() &&
                std::ranges::find(state.device_extensions, ext) == state.device_extensions.end()) {
                state.optional_device_extensions.push_back(ext);
                LOG_INFO(Render_Vulkan, "Adding Reflex optional device extension: {}", ext);
            }
        }
    }

    // Check if PCL was loaded successfully
    sl::FeatureRequirements pcl_requirements{};
    if (const sl::Result result = api.get_feature_requirements(sl::kFeaturePCL, pcl_requirements);
        result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan, "Streamline PCL plugin not available (result {})",
                    static_cast<int>(result));
    } else {
        LOG_INFO(Render_Vulkan, "Streamline PCL plugin loaded successfully");
        const auto pcl_inst_exts = Names(pcl_requirements.vkNumInstanceExtensions,
                                         pcl_requirements.vkInstanceExtensions);
        const auto pcl_dev_exts = Names(pcl_requirements.vkNumDeviceExtensions,
                                        pcl_requirements.vkDeviceExtensions);
        for (const auto& ext : pcl_inst_exts) {
            if (std::ranges::find(state.instance_extensions, ext) == state.instance_extensions.end()) {
                state.instance_extensions.push_back(ext);
                LOG_INFO(Render_Vulkan, "Adding PCL instance extension: {}", ext);
            }
        }
        for (const auto& ext : pcl_dev_exts) {
            if (std::ranges::find(state.optional_device_extensions, ext) == state.optional_device_extensions.end() &&
                std::ranges::find(state.device_extensions, ext) == state.device_extensions.end()) {
                state.optional_device_extensions.push_back(ext);
                LOG_INFO(Render_Vulkan, "Adding PCL optional device extension: {}", ext);
            }
        }
    }

    sl::FeatureVersion version{};
    api.get_feature_version(sl::kFeatureDLSS, version);
    LOG_INFO(Render_Vulkan, "NVIDIA Streamline {} started from {} (DLSS {})",
             version.versionSL.toStr(),
             Common::FS::PathToUTF8String(std::filesystem::path{state.plugin_dir}),
             version.versionNGX.toStr());
    state.started = true;
    return true;
}

bool WantsDevice() {
    const State& state = GetState();
    return state.started && !state.attached;
}

std::vector<std::string> InstanceExtensions() {
    const State& state = GetState();
    return state.started ? state.instance_extensions : std::vector<std::string>{};
}

std::vector<std::string> DeviceExtensions() {
    const State& state = GetState();
    return state.started ? state.device_extensions : std::vector<std::string>{};
}

std::vector<std::string> OptionalDeviceExtensions() {
    const State& state = GetState();
    return state.started ? state.optional_device_extensions : std::vector<std::string>{};
}

bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, vk::DeviceDispatch& dld) {
    State& state = GetState();
    if (!WantsDevice()) {
        return false;
    }
    sl::VulkanInfo info{};
    info.device = device;
    info.instance = instance;
    info.physicalDevice = physical;
    info.computeQueueFamily = graphics_family;
    info.graphicsQueueFamily = graphics_family;
    if (const sl::Result result = state.api.set_vulkan_info(info); result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "Streamline did not take the Vulkan device (result {})",
                  static_cast<int>(result));
        Stop();
        return false;
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical;
    if (const sl::Result result = state.api.is_feature_supported(sl::kFeatureDLSS, adapter);
        result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan,
                    "NVIDIA DLSS is not supported on this GPU or driver (Streamline result {}); "
                    "using FSR 3.1",
                    static_cast<int>(result));
        Stop();
        return false;
    }

    // Check Reflex support (non-fatal - we can continue without it)
    if (const sl::Result reflex_result =
            state.api.is_feature_supported(sl::kFeatureReflex, adapter);
        reflex_result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan,
                    "NVIDIA Reflex is not supported on this GPU/driver (Streamline result {}). "
                    "OptiScaler low latency detection will use fallback mode.",
                    static_cast<int>(reflex_result));
    } else {
        LOG_INFO(Render_Vulkan, "NVIDIA Reflex is supported and available through Streamline");

        void* func_ptr = nullptr;
        if (state.api.get_feature_function(sl::kFeatureReflex, "slReflexSleep", func_ptr) ==
            sl::Result::eOk) {
            state.api.reflex_sleep = reinterpret_cast<PFun_slReflexSleep*>(func_ptr);
        } else {
            LOG_WARNING(Render_Vulkan, "Failed to get slReflexSleep function");
        }

        func_ptr = nullptr;
        if (state.api.get_feature_function(sl::kFeatureReflex, "slReflexSetMarker", func_ptr) ==
            sl::Result::eOk) {
            state.api.reflex_set_marker = reinterpret_cast<PFun_slReflexSetMarker*>(func_ptr);
            LOG_INFO(Render_Vulkan, "Streamline Reflex marker function loaded successfully");
        } else {
            LOG_WARNING(Render_Vulkan, "Failed to get slReflexSetMarker function");
        }

        func_ptr = nullptr;
        if (state.api.get_feature_function(sl::kFeatureReflex, "slReflexGetState", func_ptr) ==
            sl::Result::eOk) {
            state.api.reflex_get_state = reinterpret_cast<PFun_slReflexGetState*>(func_ptr);
        }

        func_ptr = nullptr;
        if (state.api.get_feature_function(sl::kFeatureReflex, "slReflexSetOptions", func_ptr) ==
            sl::Result::eOk) {
            state.api.reflex_set_options = reinterpret_cast<PFun_slReflexSetOptions*>(func_ptr);

            sl::ReflexOptions reflex_options{};
            reflex_options.mode = sl::ReflexMode::eLowLatency;
            if (const sl::Result reflex_set_result = state.api.reflex_set_options(reflex_options);
                reflex_set_result != sl::Result::eOk) {
                LOG_WARNING(Render_Vulkan, "Failed to enable Reflex low latency mode (result {})",
                            static_cast<int>(reflex_set_result));
            } else {
                LOG_INFO(Render_Vulkan, "NVIDIA Reflex low latency mode enabled");
            }
        } else {
            LOG_WARNING(Render_Vulkan, "Failed to get slReflexSetOptions function");
        }
    }

    // Check PCL support and get marker function (used by OptiScaler / Streamline PCL)
    if (const sl::Result pcl_result = state.api.is_feature_supported(sl::kFeaturePCL, adapter);
        pcl_result == sl::Result::eOk) {
        void* pcl_func_ptr = nullptr;
        if (state.api.get_feature_function(sl::kFeaturePCL, "slPCLSetMarker", pcl_func_ptr) ==
            sl::Result::eOk) {
            state.api.pcl_set_marker = reinterpret_cast<PFun_slPCLSetMarker*>(pcl_func_ptr);
            LOG_INFO(Render_Vulkan, "Streamline PCL marker function loaded successfully");
        } else {
            LOG_WARNING(Render_Vulkan, "Failed to get slPCLSetMarker function");
        }
    }

    // Manual hooking: Streamline interposer handles per-frame bookkeeping (presentCommon())
    // and resource garbage collection in these swapchain functions before forwarding to Vulkan/OptiScaler.
    // NOTE: DO NOT disable manual hooking even if OptiScaler is present. OptiScaler intercepts
    // the downstream Vulkan calls, but Streamline MUST process the calls first to update its internal state.
    const auto hook = [&](auto& function, const char* name) {
        if (const FARPROC proxy = GetProcAddress(state.module, name)) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(proxy);
        } else {
            LOG_WARNING(Render_Vulkan, "sl.interposer.dll has no {}", name);
        }
    };
    hook(dld.vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
    hook(dld.vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
    hook(dld.vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
    hook(dld.vkAcquireNextImageKHR, "vkAcquireNextImageKHR");
    hook(dld.vkQueuePresentKHR, "vkQueuePresentKHR");
    hook(dld.vkDeviceWaitIdle, "vkDeviceWaitIdle");
    state.attached = true;
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS is available through Streamline");
    return true;
}

bool IsDlssAvailable() {
    const State& state = GetState();
    return state.started && state.attached;
}

void Stop() {
    State& state = GetState();
    if (!state.started) {
        return;
    }
    {
        std::lock_guard lock(state.token_mutex);
        state.current_frame_token = nullptr;
        state.render_submit_started = false;
    }
    const sl::Result result = state.api.shutdown();
    state.api = {};
    state.instance_extensions.clear();
    state.device_extensions.clear();
    state.optional_device_extensions.clear();
    state.started = false;
    state.attached = false;
    LOG_INFO(Render_Vulkan, "NVIDIA Streamline stopped (result {})", static_cast<int>(result));
}

sl::FrameToken* GetOrNewFrameToken() {
    State& state = GetState();
    if (!state.started || !state.attached || !state.api.get_new_frame_token) {
        return nullptr;
    }
    std::lock_guard lock(state.token_mutex);
    if (!state.current_frame_token) {
        if (state.api.get_new_frame_token(state.current_frame_token, nullptr) == sl::Result::eOk) {
            // Automatically mark the start of simulation when a new frame begins
            if (state.api.pcl_set_marker) {
                state.api.pcl_set_marker(sl::PCLMarker::eSimulationStart, *state.current_frame_token);
            }
            if (state.api.reflex_set_marker) {
                state.api.reflex_set_marker(sl::PCLMarker::eSimulationStart, *state.current_frame_token);
            }
        } else {
            state.current_frame_token = nullptr;
        }
    }
    return state.current_frame_token;
}

void AdvanceFrameToken() {
    State& state = GetState();
    std::lock_guard lock(state.token_mutex);
    state.current_frame_token = nullptr;
    state.render_submit_started = false;
}

bool IsRenderSubmitStarted() {
    State& state = GetState();
    std::lock_guard lock(state.token_mutex);
    return state.render_submit_started;
}

void NotifyRenderSubmitStart(const sl::FrameToken* frame) {
    if (!frame) {
        return;
    }
    State& state = GetState();
    std::lock_guard lock(state.token_mutex);
    if (state.render_submit_started) {
        return;
    }
    // Sleep to pace the frame just before GPU submission
    ReflexSleep(frame);
    SetReflexMarker(sl::PCLMarker::eSimulationEnd, frame);
    SetReflexMarker(sl::PCLMarker::eRenderSubmitStart, frame);
    state.render_submit_started = true;
}

void SetReflexMarker(sl::PCLMarker marker, const sl::FrameToken* frame) {
    if (!frame) {
        return;
    }
    const State& state = GetState();
    if (state.api.pcl_set_marker) {
        state.api.pcl_set_marker(marker, *frame);
    }
    if (state.api.reflex_set_marker) {
        state.api.reflex_set_marker(marker, *frame);
    }
}

void ReflexSleep(const sl::FrameToken* frame) {
    if (!frame) {
        return;
    }
    const State& state = GetState();
    if (state.api.reflex_sleep) {
        state.api.reflex_sleep(*frame);
    }
}

const Api& GetApi() {
    return GetState().api;
}

} // namespace Vulkan::Streamline

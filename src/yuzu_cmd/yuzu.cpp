// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <iterator>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include "common/settings_enums.h"
#include "core/launch_params.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#include <fmt/ostream.h>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "frontend_common/config.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/main.h"
#include "network/network.h"
#include "sdl_config.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3.h"
#ifdef HAS_OPENGL
#include "yuzu_cmd/emu_window/emu_window_sdl3_gl.h"
#endif
#include "yuzu_cmd/emu_window/emu_window_sdl3_null.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3_vk.h"
#include "yuzu_cmd/nx2_sampler.h"

#ifdef _WIN32
// windows.h needs to be included before shellapi.h
#include <windows.h>
#include <shellapi.h>
#include "common/windows/timer_resolution.h"
#endif

#ifdef _WIN32
extern "C" {
// tells Nvidia and AMD drivers to use the dedicated GPU by default on laptops with switchable
// graphics
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// EDEN_NX2_CRASH_DUMP=<dir>: on an unhandled exception (a real crash; fastmem faults are handled
// before this), write <dir>/eden-crash-<pid>.dmp. A debugger changes the timing enough to hide
// some crashes, so this catches them at full speed. dbghelp.dll is loaded only then.
// Prints a code address as module+offset (resolve with llvm-symbolizer and the module's PDB).
static void PrintCodeAddress(const char* label, const void* address) {
    HMODULE module{};
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module)) {
        GetModuleFileNameA(module, name, sizeof(name));
    }
    const char* base_name = std::strrchr(name, '\\');
    std::fprintf(stderr, "  %s %p %s+%#llx\n", label, address, base_name ? base_name + 1 : name,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                 reinterpret_cast<uintptr_t>(module)));
}

static LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* exception) {
    static std::atomic<bool> written{};
    const char* directory = std::getenv("EDEN_NX2_CRASH_DUMP");
    if (directory == nullptr || written.exchange(true)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // The faulting address and stack first: they survive even if the dump fails.
    const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
    std::fprintf(stderr, "Unhandled exception %#lx on thread %lu, info %#llx %#llx\n",
                 record.ExceptionCode, GetCurrentThreadId(),
                 static_cast<unsigned long long>(record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0),
                 static_cast<unsigned long long>(record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0));
    PrintCodeAddress("at", record.ExceptionAddress);
    void* frames[48];
    const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        PrintCodeAddress("frame", frames[i]);
    }
    std::fflush(stderr);
    struct ExceptionInformation {
        DWORD thread_id;
        EXCEPTION_POINTERS* exception_pointers;
        BOOL client_pointers;
    };
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, const ExceptionInformation*,
                                              const void*, const void*);
    const HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    const auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(
                                     GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                               : nullptr;
    if (write == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\eden-crash-%lu.dmp", directory, GetCurrentProcessId());
    const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const ExceptionInformation information{GetCurrentThreadId(), exception, FALSE};
        // MiniDumpWithThreadInfo; the larger MiniDumpWithIndirectlyReferencedMemory failed on
        // Eden's address space and left an empty file.
        const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, 0x1000,
                              &information, nullptr, nullptr);
        const DWORD error = ok ? 0 : GetLastError();
        CloseHandle(file);
        std::fprintf(stderr, ok ? "Crash dump written to %s\n" : "Crash dump to %s failed (%#lx)\n",
                     path, error);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void OnStateChanged(const Network::RoomMember::State& state) {
    switch (state) {
    case Network::RoomMember::State::Idle:
        LOG_DEBUG(Network, "Network is idle");
        break;
    case Network::RoomMember::State::Joining:
        LOG_DEBUG(Network, "Connection sequence to room started");
        break;
    case Network::RoomMember::State::Joined:
        LOG_DEBUG(Network, "Successfully joined to the room");
        break;
    case Network::RoomMember::State::Moderator:
        LOG_DEBUG(Network, "Successfully joined the room as a moderator");
        break;
    default:
        break;
    }
}

static void OnNetworkError(const Network::RoomMember::Error& error) {
    switch (error) {
    case Network::RoomMember::Error::LostConnection:
        LOG_DEBUG(Network, "Lost connection to the room");
        break;
    case Network::RoomMember::Error::CouldNotConnect:
        LOG_ERROR(Network, "Error: Could not connect");
        exit(1);
        break;
    case Network::RoomMember::Error::NameCollision:
        LOG_ERROR(
            Network,
            "You tried to use the same nickname as another user that is connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::IpCollision:
        LOG_ERROR(Network, "You tried to use the same fake IP-Address as another user that is "
                           "connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongPassword:
        LOG_ERROR(Network, "Room replied with: Wrong password");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongVersion:
        LOG_ERROR(Network,
                  "You are using a different version than the room you are trying to connect to");
        exit(1);
        break;
    case Network::RoomMember::Error::RoomIsFull:
        LOG_ERROR(Network, "The room is full");
        exit(1);
        break;
    case Network::RoomMember::Error::HostKicked:
        LOG_ERROR(Network, "You have been kicked by the host");
        break;
    case Network::RoomMember::Error::HostBanned:
        LOG_ERROR(Network, "You have been banned by the host");
        break;
    case Network::RoomMember::Error::UnknownError:
        LOG_ERROR(Network, "UnknownError");
        break;
    case Network::RoomMember::Error::PermissionDenied:
        LOG_ERROR(Network, "PermissionDenied");
        break;
    case Network::RoomMember::Error::NoSuchUser:
        LOG_ERROR(Network, "NoSuchUser");
        break;
    }
}

static void OnMessageReceived(const Network::ChatEntry& msg) {
    std::cout << std::endl << msg.nickname << ": " << msg.message << std::endl << std::endl;
}

static void OnStatusMessageReceived(const Network::StatusMessageEntry& msg) {
    std::string message = [&]() {
        switch (msg.type) {
        case Network::IdMemberJoin:
            return fmt::format("{} has joined", msg.nickname);
        case Network::IdMemberLeave:
            return fmt::format("{} has left", msg.nickname);
        case Network::IdMemberKicked:
            return fmt::format("{} has been kicked", msg.nickname);
        case Network::IdMemberBanned:
            return fmt::format("{} has been banned", msg.nickname);
        case Network::IdAddressUnbanned:
            return fmt::format("{} has been unbanned", msg.nickname);
        default:
            return std::string{};
        }
    }();
    if (!message.empty())
        std::cout << std::endl << "* " << message << std::endl << std::endl;
}

struct SdlState {
    Core::System system{};
    // Must outlive emu_window, which keeps a pointer to it.
    InputCommon::InputSubsystem input_subsystem{};
    std::unique_ptr<EmuWindow_SDL3> emu_window;
};

extern "C" SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(WriteCrashDump);
#endif
    Nx2Sampler::StartFromEnvironment();
    // SDL calls SDL_AppIterate back to back by default, which kept a host core at 100% polling
    // input; 240 Hz is plenty for input and the key script.
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "240");
    SdlState* state = new SdlState();
    *appstate = state;

#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "wb", stdout);
        freopen("CONOUT$", "wb", stderr);
    }
    // Raise the CRT's open-file limit like the Qt frontend: loose RomFS folders keep thousands of
    // files open through the VFS cache.
    _setmaxstdio(8192);
#endif

    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();

    int option_index = 0;
#ifdef _WIN32
    int argc_w;
    auto argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w);
    if (argv_w == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to get command line arguments");
        return SDL_APP_FAILURE;
    }
#else
    wchar_t **argv_w = nullptr;
#endif
    Core::LaunchParams lp = Core::ParseLaunchParams(state->system, argc, argv, argv_w);
#ifdef _WIN32
    LocalFree(argv_w);
#endif
    SdlConfig config{lp.config_path};

    Core::ApplyLaunchParams(lp);

    if (lp.filepath.empty()) {
        LOG_CRITICAL(Frontend, "Failed to load ROM: No ROM specified");
        return SDL_APP_FAILURE;
    }

    state->system.Initialize();


    // Apply the command line arguments
    state->system.ApplySettings();

    switch (Settings::values.renderer_backend.GetValue()) {
#ifdef HAS_OPENGL
    case Settings::RendererBackend::OpenGL_GLSL:
    case Settings::RendererBackend::OpenGL_GLASM:
    case Settings::RendererBackend::OpenGL_SPIRV:
        state->emu_window = std::make_unique<EmuWindow_SDL3_GL>(&state->input_subsystem, state->system, lp.fullscreen);
        break;
#endif
    case Settings::RendererBackend::Vulkan:
        state->emu_window = std::make_unique<EmuWindow_SDL3_VK>(&state->input_subsystem, state->system, lp.fullscreen);
        break;
    case Settings::RendererBackend::Null:
        state->emu_window = std::make_unique<EmuWindow_SDL3_Null>(&state->input_subsystem, state->system, lp.fullscreen);
        break;
    default:
        LOG_CRITICAL(Frontend, "Invalid renderer backend");
        return SDL_APP_FAILURE;
    }

#ifdef _WIN32
    Common::Windows::SetCurrentTimerResolutionToMaximum();
    state->system.CoreTiming().SetTimerResolutionNs(Common::Windows::GetCurrentTimerResolution());
#endif

    state->system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    state->system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    state->system.GetFileSystemController().CreateFactories(*state->system.GetFilesystem());
    state->system.GetUserChannel().clear();

    Service::AM::FrontendAppletParameters load_parameters{
        .applet_id = Service::AM::AppletId::Application,
    };
    const Core::SystemResultStatus load_result = state->system.Load(*state->emu_window, lp.filepath, load_parameters);
    switch (load_result) {
    case Core::SystemResultStatus::Success:
        break; // Expected case
    case Core::SystemResultStatus::ErrorGetLoader:
        LOG_CRITICAL(Frontend, "Failed to obtain loader for {}!", lp.filepath);
        return SDL_APP_FAILURE;
    case Core::SystemResultStatus::ErrorLoader:
        LOG_CRITICAL(Frontend, "Failed to load ROM!");
        return SDL_APP_FAILURE;
    case Core::SystemResultStatus::ErrorNotInitialized:
        LOG_CRITICAL(Frontend, "CPUCore not initialized");
        return SDL_APP_FAILURE;
    case Core::SystemResultStatus::ErrorVideoCore:
        LOG_CRITICAL(Frontend, "Failed to initialize VideoCore!");
        return SDL_APP_FAILURE;
    default:
        const u16 loader_id = u16(Core::SystemResultStatus::ErrorLoader);
        const u16 error_id = u16(load_result) - loader_id;
        LOG_CRITICAL(Frontend,
            "While attempting to load the ROM requested, an error occurred. Please "
            "refer to the Eden wiki for more information or the Eden discord for "
            "additional help.\n\nError Code: {:04X}-{:04X}\nError Description: {}",
            loader_id, error_id, Loader::ResultStatus(error_id));
        return SDL_APP_FAILURE;
    }

    if (lp.use_multiplayer) {
        if (auto member = Network::GetRoomMember().lock()) {
            member->BindOnChatMessageReceived(OnMessageReceived);
            member->BindOnStatusMessageReceived(OnStatusMessageReceived);
            member->BindOnStateChanged(OnStateChanged);
            member->BindOnError(OnNetworkError);
            LOG_DEBUG(Network, "Start connection to {}:{} with nickname {}", lp.address, lp.port, lp.nickname);
            member->Join(lp.nickname, lp.address.c_str(), lp.port, 0, Network::NoPreferredIP, lp.password);
        } else {
            LOG_ERROR(Network, "Could not access RoomMember");
            return SDL_APP_FAILURE;
        }
    }

    // Core is loaded, start the GPU (makes the GPU contexts current to this thread)
    state->system.GPU().Start();
    state->system.GetCpuManager().OnGpuReady();

    if (Settings::values.use_disk_shader_cache.GetValue()) {
        state->system.Renderer().ReadRasterizer()->LoadDiskResources(
            state->system.GetApplicationProcessProgramID(), std::stop_token{},
            [](VideoCore::LoadCallbackStage, size_t value, size_t total) {});
    }

    // don't do anything, SDL3 already exists for us :D
    state->system.RegisterExitCallback([] {});

    // QLaunch launched applications (should) now reload shader cache.
    static std::mutex shader_cache_reload_mutex;
    state->system.RegisterApplicationChangedCallback([&state](u64 changed_program_id) {
        if (!Settings::values.use_disk_shader_cache.GetValue()) {
            return;
        }

        std::scoped_lock lk{shader_cache_reload_mutex};

        LOG_INFO(Frontend, "Reloading disk shader cache for {:016X}", changed_program_id);

        state->system.Pause();
        state->system.GPU().WaitForIdle();
        state->system.GPU().ObtainContext();

        state->system.Renderer().ReadRasterizer()->LoadDiskResources(
            changed_program_id, std::stop_token{},
            [](VideoCore::LoadCallbackStage, size_t, size_t) {});

        state->system.GPU().ReleaseContext();
        state->system.Run();
    });

    void(state->system.Run());
    if (state->system.DebuggerEnabled())
        state->system.InitializeDebugger();
    return SDL_APP_CONTINUE;
}
// EDEN_NX2_KEY_SCRIPT="seconds:code,seconds:-code,..." (or "@<file>"): press (code) or release (-code) keyboard
// keys, by SDL scancode, that many seconds after start. Lets hidden test runs drive menus.
double Nx2KeyClockSeconds(); // emu_window_sdl3.cpp

static void RunKeyScript(InputCommon::InputSubsystem& input_subsystem) {
    struct KeyEvent {
        double seconds; // when frame is 0
        u64 frame;      // "f<frame>:code": after that many presents
        int code;
    };
    static const std::vector<KeyEvent> script = [] {
        std::vector<KeyEvent> events;
        if (const char* text = std::getenv("EDEN_NX2_KEY_SCRIPT")) {
            // "@<file>" reads the list from a file: long scripts in the environment slow down
            // every getenv in the process.
            std::string list{text};
            if (!list.empty() && list[0] == '@') {
                std::ifstream file{list.substr(1)};
                list.assign(std::istreambuf_iterator<char>{file}, {});
            }
            size_t start = 0;
            while (start < list.size()) {
                const size_t end = (std::min)(list.find(',', start), list.size());
                const std::string item = list.substr(start, end - start);
                if (const size_t colon = item.find(':'); colon != std::string::npos) {
                    const std::string when = item.substr(0, colon);
                    const int code = std::stoi(item.substr(colon + 1));
                    if (!when.empty() && when[0] == 'f') {
                        events.push_back({0.0, std::stoull(when.substr(1)), code});
                    } else {
                        events.push_back({std::stod(when), 0, code});
                    }
                }
                start = end + 1;
            }
        }
        return events;
    }();
    static size_t next = 0;
    const double elapsed = Nx2KeyClockSeconds();
    const u64 frame = Core::g_nx2_present_count.load();
    while (next < script.size() && (script[next].frame != 0 ? script[next].frame <= frame
                                                            : script[next].seconds <= elapsed)) {
        const int code = script[next].code;
        LOG_INFO(Frontend, "Key script: {} key {} at {:.1f} s (frame {})",
                 code < 0 ? "release" : "press", code < 0 ? -code : code, elapsed, frame);
        if (code < 0) {
            input_subsystem.GetKeyboard()->ReleaseKey(static_cast<std::size_t>(-code));
        } else {
            input_subsystem.GetKeyboard()->PressKey(static_cast<std::size_t>(code));
        }
        ++next;
    }
}

extern "C" SDL_AppResult SDL_AppIterate(void *appstate) {
    SdlState *state = (SdlState *)appstate;
    RunKeyScript(state->input_subsystem);
    return state->emu_window && state->emu_window->IsOpen() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}
extern "C" SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    SdlState *state = (SdlState *)appstate;
    if (state->emu_window)
        state->emu_window->OnEvent(*event);
    return SDL_APP_CONTINUE;
}
extern "C" void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    SdlState *state = (SdlState *)appstate;
    state->system.DetachDebugger();
    void(state->system.Pause());
    state->system.ShutdownMainProcess();
    delete state;
}

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"

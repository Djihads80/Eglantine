// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

#include <tlhelp32.h>

#include "common/cpu_layout.h"
#include "common/logging.h"
#include "common/windows/cpu_layout.h"

namespace Common::Windows {
namespace {

std::optional<CpuThreadLayout> ReadLayout() {
    if (GetActiveProcessorGroupCount() != 1) {
        return std::nullopt;
    }
    DWORD_PTR allowed{}, system{};
    if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) {
        return std::nullopt;
    }
    DWORD size{};
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &size);
    if (size == 0) {
        return std::nullopt;
    }
    std::vector<u8> buffer(size);
    if (!GetLogicalProcessorInformationEx(
            RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
            &size)) {
        return std::nullopt;
    }
    std::vector<CpuCacheDomain> caches;
    std::vector<u64> cores;
    for (size_t offset = 0; offset < size;) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            buffer.data() + offset);
        if (size - offset < 8 || info->Size < 8 || info->Size > size - offset) {
            return std::nullopt;
        }
        if (info->Relationship == RelationCache && info->Cache.Level == 3) {
            if (info->Cache.GroupMask.Group != 0) {
                return std::nullopt;
            }
            caches.push_back({static_cast<u64>(info->Cache.GroupMask.Mask), info->Cache.CacheSize});
        } else if (info->Relationship == RelationProcessorCore) {
            if (info->Processor.GroupCount != 1 || info->Processor.GroupMask[0].Group != 0) {
                return std::nullopt;
            }
            cores.push_back(static_cast<u64>(info->Processor.GroupMask[0].Mask));
        }
        offset += info->Size;
    }
    return MakeCpuThreadLayout(static_cast<u64>(allowed), caches, cores);
}

u64 ThreadMask(std::wstring_view name, const CpuThreadLayout& layout) {
    if (name == L"GPUBack") {
        return layout.backend;
    }
    if (name == L"GPU" && layout.front != 0) {
        return layout.front;
    }
    if (name == L"HostTiming" || name == L"DSP_AudioRenderer_Main" || name == L"audio" ||
        name == L"AudioRenderSystemManager" || name == L"CPUPlacement") {
        return layout.support;
    }
    return layout.foreground;
}

void ApplyLayout(const CpuThreadLayout& layout) {
    DWORD_PTR allowed{}, system{};
    if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) ||
        static_cast<u64>(allowed) !=
            (layout.foreground | layout.backend | layout.support | layout.front)) {
        return;
    }
    const DWORD owner = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL found = Thread32First(snapshot, &entry); found;
         found = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != owner) {
            continue;
        }
        const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_INFORMATION,
                                         FALSE, entry.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        if (GetProcessIdOfThread(thread) == owner) {
            PWSTR text{};
            GetThreadDescription(thread, &text);
            const u64 mask =
                ThreadMask(text != nullptr ? std::wstring_view{text} : std::wstring_view{}, layout);
            GROUP_AFFINITY current{};
            if (GetThreadGroupAffinity(thread, &current) && current.Group == 0 &&
                static_cast<u64>(current.Mask) != mask) {
                SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(mask));
            }
            if (text != nullptr) {
                LocalFree(text);
            }
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}

class CpuLayoutPolicy {
public:
    CpuLayoutPolicy() {
        const char* flag = std::getenv("EDEN_NX2_CPU_LAYOUT");
        if (flag == nullptr || (flag[0] != '1' && flag[0] != '2')) {
            return;
        }
        auto selected = ReadLayout();
        if (selected) {
            if (flag[0] == '2') {
                // The front parses every command list and feeds the back: on a core of its own
                // it no longer shares one with busy guest cores (Ocarina's audio threads).
                selected->foreground &= ~selected->front;
            } else {
                selected->front = 0;
            }
        }
        if (!selected) {
            LOG_INFO(Common, "NX2 CPU layout: unsupported topology or restricted process affinity");
            return;
        }
        LOG_INFO(Common,
                 "NX2 CPU layout: foreground {:#x}, backend {:#x}, front {:#x}, support {:#x}",
                 selected->foreground, selected->backend, selected->front, selected->support);
        worker = std::jthread([this, layout = *selected](std::stop_token stop) {
            SetThreadDescription(GetCurrentThread(), L"CPUPlacement");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            while (!stop.stop_requested()) {
                ApplyLayout(layout);
                std::unique_lock lock{mutex};
                condition.wait_for(lock, stop, std::chrono::seconds{1}, [] { return false; });
            }
        });
    }

private:
    std::mutex mutex;
    std::condition_variable_any condition;
    std::jthread worker;
};

} // namespace

void StartNx2CpuLayout() {
    static CpuLayoutPolicy policy;
}

} // namespace Common::Windows

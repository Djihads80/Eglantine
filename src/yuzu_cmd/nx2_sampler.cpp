// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/nx2_sampler.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

namespace Nx2Sampler {
namespace {

struct Sample {
    unsigned thread;
    DWORD64 instruction_pointer;
};

struct Target {
    HANDLE handle;
    std::string name;
};

double EnvSeconds(const char* name, double fallback) {
    const char* text = std::getenv(name);
    return text != nullptr ? std::strtod(text, nullptr) : fallback;
}

std::vector<std::string> SplitNames(const char* text) {
    std::vector<std::string> names;
    std::string current;
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c == ',') {
            names.push_back(current);
            current.clear();
        } else {
            current += *c;
        }
    }
    if (!current.empty()) {
        names.push_back(current);
    }
    return names;
}

bool Matches(const std::string& thread_name, const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        if (!pattern.empty() && pattern.back() == '*') {
            if (thread_name.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0) {
                return true;
            }
        } else if (thread_name == pattern) {
            return true;
        }
    }
    return false;
}

std::vector<Target> FindThreads(const std::vector<std::string>& patterns) {
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleA("KernelBase.dll"), "GetThreadDescription"));
    std::vector<Target> targets;
    if (get_description == nullptr) {
        return targets;
    }
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return targets;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self) {
            continue;
        }
        const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                             THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ThreadID);
        if (handle == nullptr) {
            continue;
        }
        PWSTR wide{};
        std::string name;
        if (SUCCEEDED(get_description(handle, &wide)) && wide != nullptr) {
            char buffer[256]{};
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, sizeof(buffer) - 1, nullptr,
                                nullptr);
            name = buffer;
            LocalFree(wide);
        }
        if (Matches(name, patterns)) {
            targets.push_back({handle, name});
        } else {
            CloseHandle(handle);
        }
    }
    CloseHandle(snapshot);
    return targets;
}

// Resolve only loaded-image addresses. Never inspect JIT unwind metadata or guess a
// symbol from the nearest export; resolve executable RVAs with the matching linker map.
struct Symbolizer {
    const std::string& Name(DWORD64 address) {
        const auto [it, is_new] = cache.try_emplace(address);
        if (!is_new) {
            return it->second;
        }
        HMODULE module{};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCSTR>(address), &module)) {
            char path[MAX_PATH] = "?";
            GetModuleFileNameA(module, path, sizeof(path));
            const char* base = std::strrchr(path, '\\');
            char relative[24]{};
            std::snprintf(relative, sizeof(relative), "+0x%llx",
                          static_cast<unsigned long long>(
                              address - reinterpret_cast<DWORD64>(module)));
            FreeLibrary(module);
            it->second = std::string{base != nullptr ? base + 1 : path} + relative;
        } else {
            it->second = "(jit/unmapped)";
        }
        return it->second;
    }

    std::unordered_map<DWORD64, std::string> cache;
};

void Report(const std::vector<Target>& targets, const std::vector<Sample>& samples,
            const char* path) {
    Symbolizer symbolizer;
    std::FILE* const file = std::fopen(path, "w");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file, "# mode=rip-only; self samples only; no call-stack attribution\n");
    for (unsigned thread = 0; thread < targets.size(); ++thread) {
        std::map<std::string, unsigned> self;
        unsigned total = 0;
        for (const Sample& sample : samples) {
            if (sample.thread != thread) {
                continue;
            }
            ++total;
            ++self[symbolizer.Name(sample.instruction_pointer)];
        }
        const auto print = [&](const char* title, const std::map<std::string, unsigned>& counts,
                               size_t limit) {
            std::vector<std::pair<unsigned, std::string>> sorted;
            for (const auto& [name, count] : counts) {
                sorted.emplace_back(count, name);
            }
            std::sort(sorted.rbegin(), sorted.rend());
            std::fprintf(file, "-- %s\n", title);
            for (size_t i = 0; i < (std::min)(limit, sorted.size()); ++i) {
                std::fprintf(file, "%6u %5.1f%% %s\n", sorted[i].first,
                             100.0 * sorted[i].first / (std::max)(total, 1U),
                             sorted[i].second.c_str());
            }
        };
        std::fprintf(file, "=== thread \"%s\": %u samples\n", targets[thread].name.c_str(), total);
        print("self (where the thread is)", self, 60);
        std::fprintf(file, "\n");
    }
    std::fclose(file);

    // <out>.folded preserves the format, with exactly one sampled PC after the thread name.
    std::map<std::string, unsigned> folded;
    for (const Sample& sample : samples) {
        std::string line = targets[sample.thread].name;
        line += ';';
        line += symbolizer.Name(sample.instruction_pointer);
        ++folded[line];
    }
    if (std::FILE* const out = std::fopen((std::string{path} + ".folded").c_str(), "w")) {
        for (const auto& [line, count] : folded) {
            std::fprintf(out, "%s %u\n", line.c_str(), count);
        }
        std::fclose(out);
    }
}

void Run(std::vector<std::string> patterns, double start, double seconds, std::string out) {
    std::this_thread::sleep_for(std::chrono::duration<double>(start));
    const std::vector<Target> targets = FindThreads(patterns);
    std::fprintf(stderr, "nx2 sampler: %zu threads match, sampling %.0f s (RIP only, no unwinding)\n", targets.size(),
                 seconds);
    if (targets.empty()) {
        return;
    }
    std::vector<Sample> samples;
    samples.reserve(static_cast<size_t>(seconds * 1100.0) * targets.size() + 16);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline &&
           samples.size() + targets.size() <= samples.capacity()) {
        for (unsigned i = 0; i < targets.size(); ++i) {
            const HANDLE handle = targets[i].handle;
            if (SuspendThread(handle) == static_cast<DWORD>(-1)) {
                continue;
            }
            // While suspended, call only the context/resume APIs. The target may hold
            // heap or loader locks; storing and symbolizing the PC happens after resuming.
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            const bool captured = GetThreadContext(handle, &context) != 0;
            ResumeThread(handle);
            if (captured && context.Rip != 0) {
                samples.push_back({i, context.Rip});
            }
        }
        Sleep(1);
    }
    Report(targets, samples, out.c_str());
    std::fprintf(stderr, "nx2 sampler: %zu samples written to %s\n", samples.size(), out.c_str());
    for (const auto& target : targets) {
        CloseHandle(target.handle);
    }
}

} // Anonymous namespace

void StartFromEnvironment() {
    const char* names = std::getenv("EDEN_NX2_SAMPLE");
    if (names == nullptr || *names == '\0') {
        return;
    }
    const char* out = std::getenv("EDEN_NX2_SAMPLE_OUT");
    std::thread{Run, SplitNames(names), EnvSeconds("EDEN_NX2_SAMPLE_START", 60.0),
                EnvSeconds("EDEN_NX2_SAMPLE_SECONDS", 30.0),
                std::string{out != nullptr ? out : "nx2_samples.txt"}}
        .detach();
}

} // namespace Nx2Sampler

#else

namespace Nx2Sampler {
void StartFromEnvironment() {}
} // namespace Nx2Sampler

#endif

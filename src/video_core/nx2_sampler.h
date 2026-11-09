// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __linux__

#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::Nx2Sampler {

// EDEN_NX2_SAMPLER=1 (Linux, diagnostic): signals the GPU front ("GPU") and back ("GPUBack")
// threads 500 times a second, records where they were, and every 20 s logs the most frequent
// leaf functions and call chains as "module+offset", for `addr2line -f -C -e <binary>`. A thread
// sleeping in a wait shows up in the wait function, so the shares are of wall time.
class Sampler {
public:
    static void MaybeStart() {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_SAMPLER");
            return value != nullptr && value[0] == '1';
        }();
        static std::once_flag once;
        if (enabled) {
            std::call_once(once, [] { Get().Start(); });
        }
    }

private:
    static constexpr int Depth = 12;
    static constexpr size_t RingSize = size_t{1} << 17;

    struct Sample {
        std::array<void*, Depth> pc;
        s32 depth;
        s32 who;
    };

    static Sampler& Get() {
        static Sampler instance;
        return instance;
    }

    static void Handler(int, siginfo_t*, void*) {
        Sampler& self = Get();
        const s32 who = self.who_of_tid.load(std::memory_order_relaxed);
        (void)who;
        void* frames[Depth + 2];
        const int n = backtrace(frames, Depth + 2);
        const size_t slot = self.head.fetch_add(1, std::memory_order_relaxed) % RingSize;
        Sample& sample = self.ring[slot];
        sample.depth = n > 2 ? n - 2 : 0; // The handler and the signal trampoline
        for (int i = 0; i < sample.depth; ++i) {
            sample.pc[i] = frames[i + 2];
        }
        sample.who = static_cast<s32>(syscall(SYS_gettid));
    }

    void Start() {
        void* warm[4];
        backtrace(warm, 4); // Loads libgcc outside a signal handler
        struct sigaction action{};
        action.sa_sigaction = &Handler;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        sigaction(SIGPROF, &action, nullptr);
        ring.resize(RingSize);
        thread = std::thread([this] { Run(); });
        thread.detach();
        LOG_INFO(HW_GPU, "nx2 sampler: sampling the GPU front and back threads at 500 Hz");
    }

    static std::string Where(void* pc) {
        Dl_info info{};
        if (dladdr(pc, &info) == 0 || info.dli_fbase == nullptr) {
            return "??";
        }
        const char* slash = info.dli_fname != nullptr ? std::strrchr(info.dli_fname, '/') : nullptr;
        const char* module = slash != nullptr ? slash + 1 : (info.dli_fname != nullptr ? info.dli_fname : "?");
        char text[160];
        if (info.dli_sname != nullptr && std::strstr(module, "eden") == nullptr &&
            std::strstr(module, "eglantine") == nullptr) {
            std::snprintf(text, sizeof(text), "%s:%s", module, info.dli_sname);
        } else {
            std::snprintf(text, sizeof(text), "%s+%#lx", module,
                          static_cast<unsigned long>(reinterpret_cast<char*>(pc) -
                                                     static_cast<char*>(info.dli_fbase)));
        }
        return text;
    }

    std::vector<std::pair<s32, std::string>> Targets() const {
        std::vector<std::pair<s32, std::string>> found;
        const pid_t pid = getpid();
        char path[96];
        std::snprintf(path, sizeof(path), "/proc/%d/task", pid);
        // Scan the task directory for the thread names.
        FILE* list = popen(("ls " + std::string(path)).c_str(), "r");
        if (list == nullptr) {
            return found;
        }
        char line[64];
        while (std::fgets(line, sizeof(line), list) != nullptr) {
            const int tid = std::atoi(line);
            std::snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", pid, tid);
            if (FILE* comm = std::fopen(path, "r")) {
                char name[32]{};
                if (std::fgets(name, sizeof(name), comm) != nullptr) {
                    name[std::strcspn(name, "\n")] = 0;
                    if (std::strcmp(name, "GPU") == 0 || std::strcmp(name, "GPUBack") == 0) {
                        found.emplace_back(tid, name);
                    }
                }
                std::fclose(comm);
            }
        }
        pclose(list);
        return found;
    }

    void Run() {
        prctl(PR_SET_NAME, "nx2sampler", 0, 0, 0); // It inherited "GPU" and would sample itself
        using clock = std::chrono::steady_clock;
        std::vector<std::pair<s32, std::string>> targets;
        std::map<s32, std::string> names;
        auto next_scan = clock::now();
        auto next_report = clock::now() + std::chrono::seconds(20);
        size_t consumed = 0;
        std::map<std::string, u64> leaf[2];
        std::map<std::string, u64> chain[2];
        u64 total[2]{};
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (clock::now() >= next_scan) {
                targets = Targets();
                for (const auto& [tid, name] : targets) {
                    names[tid] = name;
                }
                next_scan = clock::now() + std::chrono::seconds(2);
            }
            for (const auto& [tid, name] : targets) {
                syscall(SYS_tgkill, getpid(), tid, SIGPROF);
            }
            if (clock::now() < next_report) {
                continue;
            }
            next_report = clock::now() + std::chrono::seconds(20);
            const size_t head = this->head.load(std::memory_order_relaxed);
            for (; consumed < head; ++consumed) {
                const Sample& sample = ring[consumed % RingSize];
                if (sample.depth == 0 || head - consumed > RingSize) {
                    continue;
                }
                const auto it = names.find(sample.who);
                const int side = it != names.end() && it->second == "GPUBack" ? 1 : 0;
                ++total[side];
                ++leaf[side][Where(sample.pc[0])];
                std::string text;
                for (int i = 0; i < std::min<int>(sample.depth, 9); ++i) {
                    text += (i != 0 ? " < " : "") + Where(sample.pc[i]);
                }
                ++chain[side][text];
            }
            for (int side = 0; side < 2; ++side) {
                std::vector<std::pair<u64, std::string>> top;
                for (const auto& [where, count] : leaf[side]) {
                    top.emplace_back(count, where);
                }
                std::sort(top.rbegin(), top.rend());
                LOG_INFO(HW_GPU, "nx2 sampler {} thread: {} samples in the last 20 s", side ? "back" : "front",
                         total[side]);
                for (size_t i = 0; i < std::min<size_t>(top.size(), 25); ++i) {
                    LOG_INFO(HW_GPU, "nx2 sampler {} leaf {:5.1f}% {}", side ? "back" : "front",
                             100.0 * top[i].first / static_cast<double>(std::max<u64>(total[side], 1)),
                             top[i].second);
                }
                std::vector<std::pair<u64, std::string>> chains;
                for (const auto& [where, count] : chain[side]) {
                    chains.emplace_back(count, where);
                }
                std::sort(chains.rbegin(), chains.rend());
                for (size_t i = 0; i < std::min<size_t>(chains.size(), 15); ++i) {
                    LOG_INFO(HW_GPU, "nx2 sampler {} chain {:5.1f}% {}", side ? "back" : "front",
                             100.0 * chains[i].first / static_cast<double>(std::max<u64>(total[side], 1)),
                             chains[i].second);
                }
            }
            // Each report covers its own 20 s window.
            for (int side = 0; side < 2; ++side) {
                leaf[side].clear();
                chain[side].clear();
                total[side] = 0;
            }
        }
    }

    std::vector<Sample> ring;
    std::atomic<size_t> head{0};
    std::atomic<s32> who_of_tid{0};
    std::thread thread;
};

} // namespace VideoCommon::Nx2Sampler

#else
namespace VideoCommon::Nx2Sampler {
struct Sampler {
    static void MaybeStart() {}
};
} // namespace VideoCommon::Nx2Sampler
#endif

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/nx2_sampler.h"

#if defined(__linux__) && defined(__aarch64__)
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/signal_chain.h"
#include "common/thread.h"
#endif

namespace VideoCommon::Nx2Sampler {

#if defined(__linux__) && defined(__aarch64__)

namespace {

constexpr size_t MaxDepth = 40;
constexpr size_t RoleCount = static_cast<size_t>(Role::Count);
/// Threads whose state each record holds after the roles', found by the start of their name.
constexpr std::array<const char*, 9> WatchedNames{
    "VulkanWorker", "GPUFencing", "HostTiming", "CPUCore_0", "CPUCore_1",
    "CPUCore_2",    "CPUCore_3",  "CPUCore_4",  "CPUCore_5",
};
constexpr size_t WatchedCount = RoleCount + WatchedNames.size();

struct Capture {
    u32 depth{};
    /// The pc, the link register, then the return address of each frame record.
    std::array<u64, MaxDepth> pcs{};
};

struct RoleThread {
    std::atomic<pid_t> tid{};
    u64 stack_low{};
    u64 stack_high{};
    Capture capture;             ///< Written by the signal handler
    std::atomic<u32> asked{};    ///< Stacks the sampler asked for
    std::atomic<u32> answered{}; ///< The request `capture` answers
};

std::array<RoleThread, RoleCount> g_roles;
int g_signal = -1;
long g_period_us = 1000;

#pragma pack(push, 1)
struct Tick {
    u64 time_ns;
    u32 frames;
    std::array<u16, RoleCount> marks;
    std::array<u8, RoleCount> asked; ///< A stack was asked for in this record
    std::array<u8, 6> padding;
    /// 1 + the stack that arrived in this record (asked for in an earlier one); 0: none.
    std::array<u32, RoleCount> stacks;
    std::array<char, 12> states; ///< 'R', 'S', 'D'... of the roles, then of WatchedNames; 0: none
    std::array<u8, 12> cpus;     ///< The CPU each last ran on
};
#pragma pack(pop)
static_assert(sizeof(Tick) == 56);
static_assert(WatchedCount <= 12);

// Runs on the sampled thread: where it is (walks the frame records, no locks, no allocation).
void SignalHandler(int, siginfo_t*, void* raw_context) {
    const int saved_errno = errno;
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    for (RoleThread& thread : g_roles) {
        if (thread.tid.load(std::memory_order_relaxed) != tid) {
            continue;
        }
        const mcontext_t& context = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
        Capture& capture = thread.capture;
        u32 depth = 0;
        capture.pcs[depth++] = context.pc;
        capture.pcs[depth++] = context.regs[30];
        u64 floor = context.sp;
        u64 frame = context.regs[29];
        // Only on the thread's own stack, and only upwards: code without frame records (or a
        // stale x29) ends the walk, it can't lead it out of mapped memory.
        if (floor >= thread.stack_low && floor < thread.stack_high) {
            while (depth < MaxDepth && frame >= floor && frame + 16 <= thread.stack_high &&
                   (frame & 7) == 0) {
                const u64* const record = reinterpret_cast<const u64*>(frame);
                capture.pcs[depth++] = record[1];
                floor = frame + 16;
                frame = record[0];
            }
        }
        capture.depth = depth;
        thread.answered.store(thread.asked.load(std::memory_order_relaxed),
                              std::memory_order_release);
        break;
    }
    errno = saved_errno;
}

// Return addresses saved by system libraries carry a pointer authentication code.
u64 StripPac(u64 address) {
    u64 stripped;
    // xpaclri (a no-op without pointer authentication): strips x30.
    asm volatile("mov x30, %1\n hint #7\n mov %0, x30" : "=&r"(stripped) : "r"(address) : "x30");
    return stripped;
}

struct StackTable {
    std::unordered_map<u64, u32> stacks; ///< By the hash of their addresses
    std::unordered_map<u64, u32> frames; ///< By address
    std::FILE* file{};

    /// "F <id> <module> <offset> [<symbol>+<offset>]" the first time an address is seen.
    u32 FrameId(u64 address) {
        const auto [it, added] = frames.try_emplace(address, static_cast<u32>(frames.size()));
        if (added) {
            Dl_info info{};
            std::string text;
            if (dladdr(reinterpret_cast<void*>(address), &info) != 0 &&
                info.dli_fname != nullptr) {
                const char* const name = std::strrchr(info.dli_fname, '/');
                text = fmt::format("{} {:#x}", name != nullptr ? name + 1 : info.dli_fname,
                                   address - reinterpret_cast<u64>(info.dli_fbase));
                if (info.dli_sname != nullptr) {
                    text += fmt::format(" {}+{:#x}", info.dli_sname,
                                        address - reinterpret_cast<u64>(info.dli_saddr));
                }
            } else {
                text = fmt::format("? {:#x}", address);
            }
            std::fprintf(file, "F %u %s\n", it->second, text.c_str());
        }
        return it->second;
    }

    /// "S <id> <frame id>..." the first time a stack is seen.
    u32 Intern(const Capture& capture) {
        std::array<u64, MaxDepth> pcs;
        const u32 depth = (std::min)(capture.depth, static_cast<u32>(MaxDepth));
        u64 hash = 0xcbf29ce484222325ULL;
        for (u32 i = 0; i < depth; ++i) {
            pcs[i] = StripPac(capture.pcs[i]);
            hash = (hash ^ pcs[i]) * 0x100000001b3ULL;
        }
        const auto [it, added] = stacks.try_emplace(hash, static_cast<u32>(stacks.size()));
        if (added) {
            std::string line = fmt::format("S {}", it->second);
            for (u32 i = 0; i < depth; ++i) {
                line += fmt::format(" {}", FrameId(pcs[i]));
            }
            line += '\n';
            std::fputs(line.c_str(), file);
        }
        return it->second;
    }
};

struct Watched {
    pid_t tid{};
    int fd{-1}; ///< /proc/self/task/<tid>/stat
};

void Watch(Watched& watched, pid_t tid) {
    if (watched.fd >= 0) {
        close(watched.fd);
    }
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    watched.tid = tid;
    watched.fd = open(path, O_RDONLY | O_CLOEXEC);
}

/// The state letter of the thread and the CPU it last ran on; false when it is gone.
bool ReadState(Watched& watched, char& state, u8& cpu) {
    if (watched.fd < 0) {
        return false;
    }
    char text[512];
    const ssize_t size = pread(watched.fd, text, sizeof(text) - 1, 0);
    if (size <= 0) {
        close(watched.fd);
        watched = {};
        return false;
    }
    text[size] = '\0';
    // "<tid> (<name>) <state> ...": the state is field 3, the CPU field 39.
    const char* cursor = std::strrchr(text, ')');
    if (cursor == nullptr || cursor[1] != ' ') {
        return false;
    }
    cursor += 2;
    state = *cursor;
    for (int field = 3; field < 39 && cursor != nullptr; ++field) {
        cursor = std::strchr(cursor + 1, ' ');
    }
    cpu = cursor != nullptr ? static_cast<u8>(std::strtol(cursor + 1, nullptr, 10)) : u8{0xff};
    return true;
}

void FindThreads(std::array<Watched, WatchedCount>& watched) {
    for (size_t role = 0; role < RoleCount; ++role) {
        const pid_t tid = g_roles[role].tid.load(std::memory_order_acquire);
        if (tid != 0 && (watched[role].tid != tid || watched[role].fd < 0)) {
            Watch(watched[role], tid);
        }
    }
    bool missing = false;
    for (size_t i = RoleCount; i < WatchedCount; ++i) {
        missing |= watched[i].fd < 0;
    }
    if (!missing) {
        return;
    }
    DIR* const directory = opendir("/proc/self/task");
    if (directory == nullptr) {
        return;
    }
    while (const dirent* const entry = readdir(directory)) {
        const pid_t tid = static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
        if (tid <= 0) {
            continue;
        }
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        char name[32]{};
        const ssize_t size = read(fd, name, sizeof(name) - 1);
        close(fd);
        if (size <= 0) {
            continue;
        }
        for (size_t i = 0; i < WatchedNames.size(); ++i) {
            Watched& slot = watched[RoleCount + i];
            if (slot.fd < 0 &&
                std::strncmp(name, WatchedNames[i], std::strlen(WatchedNames[i])) == 0) {
                Watch(slot, tid);
            }
        }
    }
    closedir(directory);
}

void SamplerLoop() {
    Common::SetCurrentThreadName("Nx2Sampler");
    // EDEN_NX2_SAMPLE_CPUS: keeps the sampler off the CPUs of the threads it looks at.
    Common::PinCurrentThreadFromEnvironment("EDEN_NX2_SAMPLE_CPUS");
    const std::string directory = Common::FS::GetEdenPathString(Common::FS::EdenPath::LogDir);
    std::FILE* const ticks = std::fopen((directory + "/nx2_sample_ticks.bin").c_str(), "wb");
    StackTable table;
    table.file = std::fopen((directory + "/nx2_sample_stacks.txt").c_str(), "wb");
    if (ticks == nullptr || table.file == nullptr) {
        LOG_ERROR(HW_GPU, "GPU thread sampling: no output files in {}", directory);
        g_enabled.store(false, std::memory_order_relaxed);
        return;
    }
    std::array<Watched, WatchedCount> watched;
    std::array<pid_t, RoleCount> pending{}; ///< The thread a stack is awaited from, or 0
    const long period_ns = g_period_us * 1000;
    timespec next{};
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (u64 tick = 0;; ++tick) {
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1'000'000'000) {
            next.tv_nsec -= 1'000'000'000;
            ++next.tv_sec;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - next.tv_sec > 1) {
            // Far behind (the process was stopped): no burst of records to catch up.
            next = now;
        }
        if (tick % 1024 == 0) {
            FindThreads(watched);
            std::fflush(ticks);
            std::fflush(table.file);
        }
        Tick record{};
        record.time_ns = static_cast<u64>(now.tv_sec) * 1'000'000'000ULL +
                         static_cast<u64>(now.tv_nsec);
        record.frames = g_frames.load(std::memory_order_relaxed);
        for (size_t i = 0; i < WatchedCount; ++i) {
            char state{};
            u8 cpu = 0xff;
            if (ReadState(watched[i], state, cpu)) {
                record.states[i] = state;
            }
            record.cpus[i] = cpu;
        }
        for (size_t role = 0; role < RoleCount; ++role) {
            RoleThread& thread = g_roles[role];
            const u32 mark = g_marks[role].load(std::memory_order_relaxed);
            record.marks[role] = static_cast<u16>(mark);
            const pid_t tid = thread.tid.load(std::memory_order_acquire);
            if (pending[role] != 0) {
                if (thread.answered.load(std::memory_order_acquire) ==
                    thread.asked.load(std::memory_order_relaxed)) {
                    record.stacks[role] = table.Intern(thread.capture) + 1;
                    pending[role] = 0;
                } else if (pending[role] != tid) {
                    // The thread ended before it answered.
                    pending[role] = 0;
                }
            }
            if (tid == 0 || pending[role] != 0 || (mark != Work && mark != BackHook)) {
                continue;
            }
            thread.asked.fetch_add(1, std::memory_order_relaxed);
            if (syscall(SYS_tgkill, getpid(), tid, g_signal) == 0) {
                pending[role] = tid;
                record.asked[role] = 1;
            }
        }
        std::fwrite(&record, sizeof(record), 1, ticks);
    }
}

void Start() {
    const char* const value = std::getenv("EDEN_NX2_SAMPLE");
    if (value == nullptr || *value == '\0' || *value == '0') {
        return;
    }
    const long period = std::strtol(value, nullptr, 10);
    g_period_us = period >= 100 ? period : 1000;
    g_signal = SIGRTMAX - 2;
    struct sigaction action {};
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    action.sa_sigaction = &SignalHandler;
    sigemptyset(&action.sa_mask);
    if (Common::SigAction(g_signal, &action, nullptr) != 0) {
        LOG_ERROR(HW_GPU, "GPU thread sampling: signal {} not available", g_signal);
        return;
    }
    g_enabled.store(true, std::memory_order_release);
    std::thread{SamplerLoop}.detach();
    LOG_WARNING(HW_GPU, "GPU thread sampling every {} us on signal {} (EDEN_NX2_SAMPLE)",
                g_period_us, g_signal);
}

} // Anonymous namespace

void Register(Role role) {
    static std::once_flag once;
    std::call_once(once, Start);
    if (!g_enabled.load(std::memory_order_relaxed) || role == Role::None) {
        return;
    }
    RoleThread& thread = g_roles[static_cast<size_t>(role)];
    pthread_attr_t attributes;
    void* stack_address{};
    size_t stack_size{};
    if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
        pthread_attr_getstack(&attributes, &stack_address, &stack_size);
        pthread_attr_destroy(&attributes);
    }
    thread.stack_low = reinterpret_cast<u64>(stack_address);
    thread.stack_high = thread.stack_low + stack_size;
    g_marks[static_cast<size_t>(role)].store(Work, std::memory_order_relaxed);
    t_role = role;
    thread.tid.store(static_cast<pid_t>(syscall(SYS_gettid)), std::memory_order_release);
}

void Unregister() {
    const Role role = t_role;
    if (role == Role::None) {
        return;
    }
    g_roles[static_cast<size_t>(role)].tid.store(0, std::memory_order_release);
    t_role = Role::None;
}

#else

void Register(Role) {}

void Unregister() {}

#endif

} // namespace VideoCommon::Nx2Sampler

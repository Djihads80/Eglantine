// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
//
// LD_PRELOAD helper for debug sessions: writes a backtrace of the faulting thread when the
// process dies on SIGABRT, SIGSEGV, SIGBUS, SIGFPE or SIGILL, then lets the signal take its
// normal course. Needs no extra libraries and no core dump.
//
//   gcc -shared -fPIC -O1 -o crashtrace.so crashtrace.c
//   CRASHTRACE_FILE=trace.txt LD_PRELOAD=./crashtrace.so ./program
//
// Installed before the emulator's own handlers, so faults the JIT handles never reach it.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static int trace_fd = -1;

static void emit(const char* text, size_t size) {
    if (trace_fd >= 0) {
        (void)!write(trace_fd, text, size);
    }
    (void)!write(STDERR_FILENO, text, size);
}

static void on_fatal_signal(int sig, siginfo_t* info, void* context) {
    (void)context;
    static volatile sig_atomic_t entered;
    if (entered++) {
        _exit(128 + sig);
    }
    char name[32] = {0};
    prctl(PR_GET_NAME, name);
    char head[256];
    const int n = snprintf(head, sizeof head,
                           "\n=== crashtrace: %s (signal %d) si_code=%d addr=%p tid=%ld thread=%s ===\n",
                           strsignal(sig), sig, info->si_code, info->si_addr,
                           (long)syscall(SYS_gettid), name);
    emit(head, (size_t)n);

    // One line per frame: module, module-relative address (what addr2line wants), symbol if known.
    void* frames[64];
    const int count = backtrace(frames, 64);
    for (int i = 0; i < count; ++i) {
        char line[512];
        Dl_info dl;
        int len;
        if (dladdr(frames[i], &dl) && dl.dli_fname) {
            len = snprintf(line, sizeof line, "#%02d %s+0x%lx %s [%p]\n", i, dl.dli_fname,
                           (unsigned long)((char*)frames[i] - (char*)dl.dli_fbase),
                           dl.dli_sname ? dl.dli_sname : "", frames[i]);
        } else {
            len = snprintf(line, sizeof line, "#%02d ?? [%p]\n", i, frames[i]);
        }
        emit(line, (size_t)len);
    }
    emit("=== end crashtrace ===\n", 24);

    // Default action: a fault re-executes and dies again, an abort is re-raised.
    signal(sig, SIG_DFL);
    if (sig == SIGABRT) {
        raise(sig);
    }
}

__attribute__((constructor)) static void install(void) {
    void* warm[1];
    backtrace(warm, 1); // load libgcc now: the handler must not allocate

    const char* path = getenv("CRASHTRACE_FILE");
    if (path) {
        trace_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_fatal_signal;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL};
    for (unsigned i = 0; i < sizeof signals / sizeof *signals; ++i) {
        sigaction(signals[i], &action, NULL);
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later

// Drives Common::ThreadWorker::WaitForRequests through the interleaving where its waiter has
// checked the predicate under the queue mutex but is not yet asleep. Built and run in its own
// process by thread_worker_wake.py; Linux with a dynamically linked glibc only.
//
// The waiter is held at that point (inside pthread_cond_wait, before the real call) until every
// worker has taken its last step: notify_all returned, or it is trying to take the queue mutex.
// Then the waiter is let into the real wait. It returns only if that last step still wakes it.
//
// The main thread's waits for each handshake have deadlines. The held waiter and the worker tasks
// spin until the main thread releases them, so if the harness stalls there, the runner's 60 s
// process timeout ends it.
//
// Exit codes: 0 the waiter returned, 1 it never woke, 2 harness failure, 77 not supported.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stop_token>
#include <thread>

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include "common/thread_worker.h"

namespace Common {
void SetCurrentThreadPriority(ThreadPriority) {}
void SetCurrentThreadName(const char*) {}
void SetCurrentThreadToPerformanceCores() {}
void SetCurrentThreadToEfficiencyCores() {}
void SetCurrentThreadToBackgroundWork() {}
void SetCurrentThreadToAllCores() {}
} // namespace Common

namespace {

constexpr size_t NumWorkers = 2;

using CondWait = int (*)(pthread_cond_t*, pthread_mutex_t*);
using CondBroadcast = int (*)(pthread_cond_t*);
using MutexLock = int (*)(pthread_mutex_t*);

template <typename Fn>
Fn Real(std::atomic<Fn>& cache, const char* name) {
    Fn fn = cache.load(std::memory_order_acquire);
    if (fn == nullptr) {
        fn = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
        cache.store(fn, std::memory_order_release);
    }
    return fn;
}

std::atomic<CondWait> real_cond_wait;
std::atomic<CondBroadcast> real_cond_broadcast;
std::atomic<MutexLock> real_mutex_lock;

thread_local bool is_waiter{};
thread_local bool waiter_held{};
thread_local bool is_worker{};
thread_local bool worker_counted{};

std::atomic<pthread_mutex_t*> queue_mutex{};
std::atomic<bool> waiter_in_window{};
std::atomic<bool> release_waiter{};
std::atomic<bool> last_steps{};
std::atomic<size_t> workers_at_last_step{};
std::atomic<size_t> workers_started{};
std::atomic<bool> release_tasks{};
std::atomic<bool> waiter_returned{};

void CountLastStep() {
    if (is_worker && !worker_counted && last_steps.load()) {
        worker_counted = true;
        ++workers_at_last_step;
    }
}

template <typename Predicate>
bool WaitUntil(Predicate&& predicate, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

[[noreturn]] void Finish(int code, const char* message) {
    std::printf("%s\n", message);
    std::fflush(stdout);
    _exit(code);
}

} // Anonymous namespace

extern "C" int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if (is_waiter && !waiter_held) {
        waiter_held = true;
        queue_mutex.store(mutex);
        waiter_in_window.store(true);
        while (!release_waiter.load()) {
            std::this_thread::yield();
        }
    }
    return Real(real_cond_wait, "pthread_cond_wait")(cond, mutex);
}

extern "C" int pthread_cond_broadcast(pthread_cond_t* cond) {
    const int result = Real(real_cond_broadcast, "pthread_cond_broadcast")(cond);
    CountLastStep();
    return result;
}

extern "C" int pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (mutex == queue_mutex.load()) {
        CountLastStep();
    }
    return Real(real_mutex_lock, "pthread_mutex_lock")(mutex);
}

int main(int argc, char** argv) {
    if (argc != 2 || (std::strcmp(argv[1], "cancel") != 0 && std::strcmp(argv[1], "complete") != 0)) {
        Finish(2, "usage: thread_worker_wake cancel|complete");
    }
    const bool cancel = std::strcmp(argv[1], "cancel") == 0;
    if (!Real(real_cond_wait, "pthread_cond_wait") ||
        !Real(real_cond_broadcast, "pthread_cond_broadcast") ||
        !Real(real_mutex_lock, "pthread_mutex_lock")) {
        Finish(77, "unsupported: the pthread functions cannot be interposed");
    }

    auto* const worker = new Common::ThreadWorker(NumWorkers, "WakeRepro");
    // Cancelling leaves queued tasks behind, so only the stopped-worker count can end the wait.
    const size_t num_tasks = cancel ? NumWorkers + 2 : NumWorkers;
    for (size_t i = 0; i < num_tasks; ++i) {
        worker->QueueWork([] {
            is_worker = true;
            ++workers_started;
            while (!release_tasks.load()) {
                std::this_thread::yield();
            }
        });
    }
    if (!WaitUntil([] { return workers_started.load() == NumWorkers; }, std::chrono::seconds{10})) {
        Finish(2, "harness: the workers did not start their tasks");
    }

    std::stop_source stop_source;
    std::thread waiter([worker, cancel, token = stop_source.get_token()] {
        is_waiter = true;
        worker->WaitForRequests(cancel ? token : std::stop_token{});
        waiter_returned.store(true);
    });
    if (!WaitUntil([] { return waiter_in_window.load(); }, std::chrono::seconds{10})) {
        Finish(2, "harness: the waiter did not reach its wait");
    }

    last_steps.store(true);
    if (cancel) {
        stop_source.request_stop();
    }
    release_tasks.store(true);
    if (!WaitUntil([] { return workers_at_last_step.load() == NumWorkers; },
                   std::chrono::seconds{10})) {
        Finish(2, "harness: the workers did not reach their last step");
    }
    release_waiter.store(true);

    if (!WaitUntil([] { return waiter_returned.load(); }, std::chrono::seconds{3})) {
        Finish(1, "lost wake: WaitForRequests never returned");
    }
    waiter.join();
    delete worker;
    Finish(0, "woke: WaitForRequests returned");
}

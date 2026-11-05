// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "video_core/query_cache/query_read_sync.h"

namespace {

// Controlled backend contract: fast SignalFence callbacks run at issue time, whereas queued
// operations run after PopAsyncFlushes. No GPU or worker is needed to test the production
// protocol's choice of callback queue and its unconditional submission.
struct QueryFenceBackend {
    bool fast{};
    bool submitted{};
    int host_value{};
    int guest_value{};
    bool committed_cleanup{};
    std::deque<std::function<void()>> uncommitted;
    std::deque<std::function<void()>> pending;
    std::vector<std::string> events;

    void SyncOperation(std::function<void()> operation) {
        events.emplace_back("queue");
        uncommitted.emplace_back(std::move(operation));
    }

    void SignalFence(std::function<void()> callback, bool force_delayed = false) {
        events.emplace_back("fence");
        // CommitAsyncFlushes adds operations after any marker previously queued with
        // SyncOperation. Completion must also follow these late operations.
        events.emplace_back("commit");
        uncommitted.emplace_back([this] {
            events.emplace_back("cleanup");
            committed_cleanup = true;
        });
        const bool delayed = !fast || force_delayed;
        if (delayed) uncommitted.emplace_back(std::move(callback));
        pending = std::move(uncommitted);
        uncommitted.clear();
        if (!delayed) callback();
    }

    void SignalReference() {
        SignalFence([] {});
    }

    void Submit() {
        events.emplace_back("submit");
        submitted = true;
    }

    void Retire() {
        REQUIRE(submitted);
        events.emplace_back("pop_queries");
        host_value = 37;
        while (!pending.empty()) {
            auto operation = std::move(pending.front());
            pending.pop_front();
            operation();
        }
    }
};

TEST_CASE("Query read completion follows host and guest query results in either fence mode",
          "[video_core][query_read_sync]") {
    const bool fast = GENERATE(false, true);
    QueryFenceBackend backend;
    backend.fast = fast;
    backend.SyncOperation([&] {
        backend.events.emplace_back("write_guest");
        backend.guest_value = backend.host_value;
    });
    bool complete{};
    int observed{};
    bool observed_cleanup{};
    VideoCommon::SubmitQueryReadFence(backend, [&] {
        backend.events.emplace_back("complete");
        observed = backend.guest_value;
        observed_cleanup = backend.committed_cleanup;
        complete = true;
    }, [&] { backend.Submit(); });
    CHECK_FALSE(complete);
    CHECK(backend.guest_value == 0);
    CHECK(backend.submitted);
    CHECK(backend.events == std::vector<std::string>{"queue", "fence", "commit", "submit"});
    backend.Retire();
    CHECK(complete);
    CHECK(observed == 37);
    CHECK(observed_cleanup);
    CHECK(backend.events == std::vector<std::string>{"queue", "fence", "commit", "submit",
                                                   "pop_queries", "write_guest", "cleanup", "complete"});
}

TEST_CASE("Query read fence submits without draws and never uses the fast callback for completion",
          "[video_core][query_read_sync]") {
    QueryFenceBackend backend;
    backend.fast = true;
    bool ordinary_callback{};
    backend.SignalFence([&] { ordinary_callback = true; });
    REQUIRE(ordinary_callback); // Reproduce the immediate-callback hazard.
    backend.Submit();
    backend.Retire();
    backend.events.clear();
    backend.submitted = false;
    backend.committed_cleanup = false;
    bool complete{};
    VideoCommon::SubmitQueryReadFence(backend, [&] {
                                         CHECK(backend.committed_cleanup);
                                         complete = true;
                                     },
                                     [&] { backend.Submit(); });
    CHECK_FALSE(complete);
    REQUIRE(backend.pending.size() == 2);
    CHECK(backend.submitted);
    backend.Retire();
    CHECK(complete);
}

} // namespace

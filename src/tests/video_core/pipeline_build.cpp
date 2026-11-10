// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "common/thread_worker.h"
#include "video_core/renderer_vulkan/pipeline_build.h"

TEST_CASE("Vulkan emission budget bounds native worker jobs", "[video_core][vulkan]") {
    Vulkan::EmissionBudget budget{2};

    REQUIRE_FALSE(budget.TryAcquire(false));
    auto first = budget.TryAcquire(true);
    auto second = budget.TryAcquire(true);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE_FALSE(budget.TryAcquire(true));

    first.reset();
    REQUIRE(budget.TryAcquire(true));
}

TEST_CASE("Vulkan pipeline build runs and reports worker failures", "[video_core][vulkan]") {
    std::atomic_bool ran{};
    Common::ThreadWorker worker{1, "PipelineBuildTest"};
    Vulkan::PipelineBuild build{&worker, [&] { ran.store(true, std::memory_order_release); }};

    build.Wait();
    REQUIRE(build.IsBuilt());
    REQUIRE(ran.load(std::memory_order_acquire));

    Vulkan::PipelineBuild failed{nullptr, [] { throw std::runtime_error{"expected"}; }};
    REQUIRE(failed.IsBuilt());
    REQUIRE(failed.Failed());
}

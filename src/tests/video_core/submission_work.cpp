// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/submission_work.h"

TEST_CASE("Automatic flush does not resubmit a context retired by a fence", "[submission_work]") {
    Vulkan::SubmissionWork work;
    unsigned requests = 0;
    auto request = [&] { ++requests; };
    REQUIRE_FALSE(work.FlushIfPending(false, request));
    REQUIRE(requests == 0);

    work.Record();
    // QueueFence submits the existing draw/query/upload commands before FlushCommands runs.
    // The submit's own worker command is recorded too, before the work is retired.
    work.Record();
    work.Submitted();
    REQUIRE_FALSE(work.FlushIfPending(false, request));
    REQUIRE(requests == 0);

    // If new work is recorded after that fence, the automatic flush must still run.
    work.Record();
    REQUIRE(work.FlushIfPending(false, request));
    REQUIRE(requests == 1);
}

TEST_CASE("Automatic flush preserves dispatched and deferred work", "[submission_work]") {
    Vulkan::SubmissionWork work;
    unsigned requests = 0;
    auto defer = [&] { ++requests; };
    work.Record();
    // Moving a full chunk to the worker does not call Submitted: it is still in the same
    // unsubmitted Vulkan command buffer, even though the replacement chunk is empty.
    REQUIRE(work.FlushIfPending(false, defer));
    // A deferred request does not retire work either. Only the eventual submission does.
    REQUIRE(work.FlushIfPending(false, defer));
    REQUIRE(requests == 2);
    work.Submitted();
    REQUIRE_FALSE(work.FlushIfPending(false, defer));

    // A full-attachment clear has no Record call until EndPendingOperations realizes it.
    REQUIRE(work.FlushIfPending(true, [&] {
        ++requests;
        work.Record(); // RealizeDeferredClear / BeginRenderPass / EndRenderPass.
        work.Record(); // Submission epilogue.
        work.Submitted();
    }));
    REQUIRE(requests == 3);
    REQUIRE_FALSE(work.FlushIfPending(false, defer));
}

TEST_CASE("Synchronous automatic submission retires its epilogue", "[submission_work]") {
    Vulkan::SubmissionWork work;
    unsigned submissions = 0;
    auto submit = [&] {
        work.Record(); // EndPendingOperations may close queries or realize a clear.
        work.Record(); // RecordWithUploadBuffer queues the actual submission.
        ++submissions;
        work.Submitted();
    };
    work.Record();
    REQUIRE(work.FlushIfPending(false, submit));
    REQUIRE(submissions == 1);
    REQUIRE_FALSE(work.FlushIfPending(false, submit));

    // An explicit empty fence or Finish calls the submit path directly, bypassing this
    // automatic-only guard, and therefore still advances its required timeline tick.
    submit();
    REQUIRE(submissions == 2);
    REQUIRE_FALSE(work.FlushIfPending(false, submit));
}

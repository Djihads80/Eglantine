// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

// Optional, bounded submission timing. Mutable state belongs to the Vulkan worker.
// Timestamp scope: https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp.html
// Results are read without WAIT, only after the existing master timeline passed the submission.
class SubmissionTiming {
public:
    static std::unique_ptr<SubmissionTiming> Create(const Device& device) {
        const char* enabled = std::getenv("EDEN_NX2_GPU_TIMING");
        if (enabled == nullptr || std::string_view{enabled} != "1") {
            return {};
        }
        try {
            auto timing = std::make_unique<SubmissionTiming>(device);
            if (!timing->pool) {
                return {};
            }
            return timing;
        } catch (const std::exception& exception) {
            LOG_WARNING(Render_Vulkan, "Submission timing unavailable: {}", exception.what());
            return {};
        }
    }

    explicit SubmissionTiming(const Device& device_) : device{device_} {
        const auto families = device.GetPhysical().GetQueueFamilyProperties();
        const auto family = device.GetGraphicsFamily();
        period_ns = device.GetPhysical().GetProperties().limits.timestampPeriod;
        if (family >= families.size() || families[family].timestampValidBits == 0 ||
            !std::isfinite(period_ns) || period_ns <= 0.0) {
            LOG_WARNING(Render_Vulkan, "Submission timing unsupported by this graphics queue");
            return;
        }
        valid_bits = families[family].timestampValidBits;
        mask = valid_bits >= 64 ? ~u64{} : (u64{1} << valid_bits) - 1;
        write_timestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
            device.GetDispatchLoader().vkGetDeviceProcAddr(*device.GetLogical(),
                                                           "vkCmdWriteTimestamp"));
        if (write_timestamp == nullptr) {
            LOG_WARNING(Render_Vulkan, "Submission timing: vkCmdWriteTimestamp unavailable");
            return;
        }
        pool = device.GetLogical().CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = MaxPairs * 2,
            .pipelineStatistics = 0,
        });
        const char* output = std::getenv("EDEN_NX2_GPU_TIMING_OUT");
        path = output != nullptr && *output != '\0' ? output : "nx2_gpu_timing.tsv";
        begin_ns = Now() + Seconds("EDEN_NX2_GPU_TIMING_START", 120.0, 0.0, 3600.0);
        end_ns = begin_ns + Seconds("EDEN_NX2_GPU_TIMING_SECONDS", 5.0, 0.1, 30.0);
    }

    ~SubmissionTiming() {
        // The scheduler worker has joined. Protect query-pool lifetime even during partial
        // renderer construction; this diagnostic-only wait never runs during measurement.
        if (pool) {
            void(device.GetLogical().WaitIdle());
        }
        if (pool && !reported) {
            Report();
        }
    }

    static u64 Now() {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
    }

    void Begin(vk::CommandBuffer upload_cmdbuf, u64 known_gpu_tick) {
        if (reported) {
            return;
        }
        if ((++poll_counter & 63U) == 0) {
            Poll(known_gpu_tick);
        }
        active = NoPair;
        const u64 now = Now();
        if (now < begin_ns || now >= end_ns) {
            return;
        }
        if (count == MaxPairs) {
            ++capacity_skips;
            return;
        }
        active = count++;
        samples[active].record_begin_ns = now;
        // Each pair is used once: no reset/reuse can race an asynchronous host read.
        upload_cmdbuf.ResetQueryPool(*pool, active * 2, 2);
        write_timestamp(*upload_cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *pool, active * 2);
    }

    void End(vk::CommandBuffer cmdbuf) {
        if (active == NoPair) {
            return;
        }
        write_timestamp(*cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *pool, active * 2 + 1);
        samples[active].record_end_ns = Now();
    }

    void Submitted(u64 tick, u64 enqueue_ns, u64 submit_begin_ns, u64 submit_end_ns,
                   VkResult result) {
        if (active == NoPair) {
            return;
        }
        auto& sample = samples[active];
        sample.tick = tick;
        sample.enqueue_ns = enqueue_ns;
        sample.submit_begin_ns = submit_begin_ns;
        sample.submit_end_ns = submit_end_ns;
        sample.status = result == VK_SUCCESS ? Status::Pending : Status::SubmitFailed;
        active = NoPair;
    }

private:
    enum class Status { NotSubmitted, Pending, Ready, SubmitFailed, QueryFailed };
    struct Sample {
        u64 tick{};
        u64 record_begin_ns{};
        u64 record_end_ns{};
        u64 enqueue_ns{};
        u64 submit_begin_ns{};
        u64 submit_end_ns{};
        u64 gpu_begin{};
        u64 gpu_end{};
        Status status{Status::NotSubmitted};
    };
    static constexpr u32 MaxPairs = 16'384;
    static constexpr u32 NoPair = ~u32{};

    static u64 Seconds(const char* name, double fallback, double minimum, double maximum) {
        const char* value = std::getenv(name);
        char* end{};
        const double seconds = value != nullptr ? std::strtod(value, &end) : fallback;
        const bool valid = std::isfinite(seconds) && seconds >= minimum && seconds <= maximum &&
                           (value == nullptr || (end != value && *end == '\0'));
        return static_cast<u64>((valid ? seconds : fallback) * 1'000'000'000.0);
    }

    void Poll(u64 known_gpu_tick) {
        if (reported) {
            return;
        }
        // At most 64 pairs per poll: bounded host work, even after a long stalled submission.
        u32 remaining = 64;
        while (remaining != 0 && next_read < count) {
            auto& sample = samples[next_read];
            if (sample.status == Status::NotSubmitted ||
                (sample.status == Status::Pending && sample.tick > known_gpu_tick)) {
                break;
            }
            if (sample.status == Status::Pending) {
                u32 batch = 1;
                while (batch < remaining && next_read + batch < count) {
                    const auto& following = samples[next_read + batch];
                    if (following.status != Status::Pending || following.tick > known_gpu_tick) {
                        break;
                    }
                    ++batch;
                }
                std::array<u64, 128> values{};
                const VkResult result = device.GetLogical().GetQueryResults(
                    *pool, next_read * 2, batch * 2, sizeof(u64) * batch * 2, values.data(), sizeof(u64),
                    VK_QUERY_RESULT_64_BIT);
                if (result == VK_NOT_READY) {
                    ++not_ready_reads;
                    break;
                }
                for (u32 i = 0; i < batch; ++i) {
                    auto& completed = samples[next_read + i];
                    if (result == VK_SUCCESS) {
                        completed.gpu_begin = values[i * 2] & mask;
                        completed.gpu_end = values[i * 2 + 1] & mask;
                        completed.status = Status::Ready;
                    } else {
                        completed.status = Status::QueryFailed;
                    }
                }
                next_read += batch;
                remaining -= batch;
                continue;
            }
            ++next_read;
            --remaining;
        }
        const u64 now = Now();
        if (now >= end_ns && (next_read == count || now - end_ns >= 2'000'000'000ULL)) {
            Report();
        }
    }

    void Report() {
        reported = true;
        std::FILE* const file = std::fopen(path.c_str(), "w");
        if (file == nullptr) {
            LOG_WARNING(Render_Vulkan, "Could not write submission timing to {}", path);
            return;
        }
        std::fprintf(file,
                     "# submission spans, not GPU utilization; CPU/GPU clocks are uncalibrated\n"
                     "# timestamp_period_ns=%.9g valid_bits=%u max_pairs=%u capacity_skips=%llu "
                     "not_ready_reads=%llu\n"
                     "# window_cpu_begin_ns=%llu window_cpu_end_ns=%llu\n"
                     "# statuses: 0=not_submitted 1=pending 2=ready 3=submit_failed 4=query_failed\n",
                     period_ns, valid_bits, MaxPairs,
                     static_cast<unsigned long long>(capacity_skips),
                     static_cast<unsigned long long>(not_ready_reads),
                     static_cast<unsigned long long>(begin_ns),
                     static_cast<unsigned long long>(end_ns));
        std::fprintf(file,
                     "index\ttick\tstatus\tcpu_record_begin_ns\tcpu_record_end_ns\t"
                     "cpu_end_marker_enqueue_ns\tcpu_submit_begin_ns\tcpu_submit_end_ns\t"
                     "cpu_enqueue_to_submit_ns\tcpu_queue_submit_ns\tgpu_begin_raw\t"
                     "gpu_end_raw\tgpu_span_ns\n");
        for (u32 i = 0; i < count; ++i) {
            const auto& sample = samples[i];
            std::fprintf(file, "%u\t%llu\t%u\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t",
                         i, static_cast<unsigned long long>(sample.tick),
                         static_cast<unsigned>(sample.status),
                         static_cast<unsigned long long>(sample.record_begin_ns),
                         static_cast<unsigned long long>(sample.record_end_ns),
                         static_cast<unsigned long long>(sample.enqueue_ns),
                         static_cast<unsigned long long>(sample.submit_begin_ns),
                         static_cast<unsigned long long>(sample.submit_end_ns),
                         static_cast<unsigned long long>(sample.submit_begin_ns >= sample.enqueue_ns
                                                             ? sample.submit_begin_ns - sample.enqueue_ns
                                                             : 0),
                         static_cast<unsigned long long>(sample.submit_end_ns >= sample.submit_begin_ns
                                                             ? sample.submit_end_ns - sample.submit_begin_ns
                                                             : 0),
                         static_cast<unsigned long long>(sample.gpu_begin),
                         static_cast<unsigned long long>(sample.gpu_end));
            // Reject wrapped/non-monotonic pairs instead of interpreting an unbounded interval.
            if (sample.status == Status::Ready && sample.gpu_end >= sample.gpu_begin) {
                std::fprintf(file, "%.3f\n", double(sample.gpu_end - sample.gpu_begin) * period_ns);
            } else {
                std::fprintf(file, "NA\n");
            }
        }
        std::fclose(file);
        LOG_INFO(Render_Vulkan, "Submission timing: {} bounded samples written to {}", count, path);
    }

    const Device& device;
    vk::QueryPool pool;
    PFN_vkCmdWriteTimestamp write_timestamp{};
    std::string path;
    std::array<Sample, MaxPairs> samples{};
    double period_ns{};
    u64 mask{};
    u64 begin_ns{};
    u64 end_ns{};
    u64 capacity_skips{};
    u64 not_ready_reads{};
    u32 valid_bits{};
    u32 count{};
    u32 next_read{};
    u32 active{NoPair};
    u32 poll_counter{};
    bool reported{};
};

} // namespace Vulkan

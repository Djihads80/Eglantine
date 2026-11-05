// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "common/common_types.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class ImageView;
class MemoryAllocator;
class Scheduler;

/// Reads back one whole frame (present to present): the colour and depth targets of every pass,
/// the images written by every compute dispatch, the textures each pass and compute shader
/// samples (3D and depth included) and the presented image, as raw.zst plus PNG previews, with a
/// manifest.jsonl of passes, dispatches, skipped draws, programs, formats, sizes, addresses and
/// per-channel stats. Readbacks are encoded and written on a worker thread.
/// Triggers: the file <user dir>/dump_frame (checked every 30 frames, deleted once the capture is
/// written; output in <user dir>/dump/frame_<time>/), or EDEN_NX2_FRAME_DUMP=<dir> with
/// EDEN_NX2_FRAME_DUMP_AT (or EDEN_NX2_DUMP_FRAME_AT) = seconds after boot ("45"), a frame
/// number ("2700f") or "file" (<dir>/capture.now). EDEN_NX2_FRAME_DUMP_SAMPLED=0 skips the
/// sampled textures.
class FrameDump {
public:
    struct Sampled {
        u32 stage;
        u32 tic;
        ImageView* view;
    };

    explicit FrameDump(const Device& device, MemoryAllocator& memory_allocator,
                       Scheduler& scheduler);
    ~FrameDump();

    /// Whether pipelines keep the views of their last configure for the capture.
    [[nodiscard]] static bool RecordingResources() noexcept {
        return recording_resources.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool Capturing() const noexcept {
        return capturing;
    }

    [[nodiscard]] bool CapturingSampled() const noexcept {
        return capturing && sampled;
    }

    /// Reads back the textures of the current draw not yet seen in this pass. Returns true when
    /// copies were recorded, which ends the current render pass.
    bool NoteSampled(std::span<const Sampled> textures);
    /// Reads back the textures of the dispatch about to run not yet seen for this shader.
    void NoteDispatchSampled(u64 shader_hash, std::span<const Sampled> textures);

    /// Starts a pass when the framebuffer changes and captures the previous one.
    /// Returns true when copies were recorded, which ends the current render pass.
    bool BeginPass(const void* framebuffer, std::span<ImageView* const> colors,
                   ImageView* depth);
    /// programs: the draw's program addresses (0 for unused stages); written: storage images.
    void NoteDraw(std::span<const u64> shader_hashes, std::span<const u64> programs,
                  std::span<ImageView* const> written);
    /// A draw or dispatch that was not executed, and why.
    void NoteSkipped(std::string_view kind, std::string_view reason, std::span<const u64> programs,
                     GPUVAddr target);
    /// Captures the current pass; returns true when copies were recorded.
    bool EndPass();
    void CaptureDispatch(u64 shader_hash, u64 program, std::span<const u32, 3> groups,
                         std::span<ImageView* const> written);
    void CapturePresent(ImageView& view);
    /// Waits for pending readbacks and writes them once they pass the staging budget.
    void DrainIfFull();
    void TickFrame();

private:
    enum class DepthEncoding { None, Unorm16, Unorm24, Float32 };

    struct Target {
        u32 slot;
        VkImage image;
        VideoCore::Surface::PixelFormat format;
        u32 width;
        u32 height;
        u32 depth;
        s32 level;
        s32 layer;
        GPUVAddr gpu_addr;
        bool rescaled;
        bool msaa;
        DepthEncoding depth_encoding;
    };

    struct Readback {
        vk::Buffer buffer;
        Target target;
        std::string name;
        std::string tags;
    };

    /// Work for the writer thread: a readback (`name` set), a finished manifest line, or the summary.
    struct Job {
        Target target{};
        std::string name;
        std::string tags;
        std::vector<u8> data;
        bool summary{};
        bool truncated{};
    };

    std::optional<Target> MakeTarget(ImageView& view, u32 slot) const;
    static u32 TexelBytes(const Target& target);
    static int Decode(const Target& target, const u8* pixel, std::array<float, 4>& out);
    bool Record(const Target& target, std::string name, std::string tags);
    bool RecordSampled(const Sampled& texture, u32 index, std::string name, std::string tags);
    /// An image was written: its next sampling is read back again.
    void Written(GPUVAddr gpu_addr);
    bool TriggerPending();
    void Start();
    void Drain();
    void Enqueue(Job job);
    void WriterLoop();
    void Write(const Job& job);
    void WriteFile(const std::string& name, std::span<const u8> data);

    static inline std::atomic<bool> recording_resources{};

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;

    bool sampled{true};
    bool capturing{};
    bool done{};
    bool truncated{};
    std::filesystem::path base_directory;
    std::filesystem::path directory;
    std::filesystem::path trigger_file;
    bool remove_trigger{};
    u64 trigger_frame{~0ULL};
    double trigger_seconds{};
    std::chrono::steady_clock::time_point start;
    u64 frame{};

    std::ofstream manifest;
    std::vector<Readback> readbacks;
    size_t pending_bytes{};
    size_t recorded_bytes{};
    size_t written_bytes{};
    u32 sequence{};
    u32 images{};

    const void* pass_framebuffer{};
    std::vector<Target> pass_targets;
    std::vector<u64> pass_hashes;
    std::vector<u64> pass_programs;
    std::vector<std::pair<GPUVAddr, u32>> pass_sampled;
    /// Sampled images (address, format/level/layer) read back this frame and not written since.
    struct FrameReadback {
        std::pair<GPUVAddr, u32> key;
        std::string name;
    };
    std::vector<FrameReadback> frame_readbacks;
    u32 pass_index{};
    u32 pass_first_draw{};
    u32 draws{};
    u32 dispatches{};
    u32 skipped{};

    std::thread writer;
    std::mutex queue_mutex;
    std::condition_variable queue_ready;
    std::condition_variable queue_drained;
    std::deque<Job> queue;
    size_t queued_bytes{};
    bool stop_writer{};
};

} // namespace Vulkan

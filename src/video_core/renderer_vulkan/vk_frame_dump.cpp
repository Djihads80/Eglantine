// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Ported from zortos293/Lichii (546cb67825, 3df1a0e6aa, abe66f465c).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <string_view>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/stb.h"
#include "common/zstd_compression.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_frame_dump.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/texture_cache/formatter.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {

using VideoCore::Surface::PixelFormat;

#ifdef __ANDROID__
constexpr size_t SIZE_CAP = size_t{1} << 30;
constexpr size_t QUEUE_BUDGET = size_t{192} << 20;
#else
constexpr size_t SIZE_CAP = size_t{2} << 30;
constexpr size_t QUEUE_BUDGET = size_t{512} << 20;
#endif
constexpr size_t STAGING_BUDGET = size_t{256} << 20;
/// Larger images (a 12288x12288 shadow map) are listed without their data.
constexpr size_t MAX_READBACK = size_t{64} << 20;
constexpr u32 MAX_SKIPPED_LINES = 512;
constexpr u32 MAX_DRAW_STATE_LINES = 8192;

template <typename T>
T Load(const u8* pixel, int index) {
    T value;
    std::memcpy(&value, pixel + index * sizeof(T), sizeof(T));
    return value;
}

template <typename T, typename Convert>
int Channels(const u8* pixel, int count, std::array<float, 4>& out, Convert&& convert) {
    for (int channel = 0; channel < count; ++channel) {
        out[channel] = convert(Load<T>(pixel, channel));
    }
    return count;
}

/// Unsigned float with a 5-bit exponent, as in half, B10G11R11 and friends.
float PackedFloat(u32 bits, int mantissa_bits) {
    const u32 exponent = bits >> mantissa_bits;
    const u32 mantissa = bits & ((1U << mantissa_bits) - 1);
    if (exponent == 0) {
        return std::ldexp(static_cast<float>(mantissa), -14 - mantissa_bits);
    }
    if (exponent == 31) {
        return mantissa != 0 ? std::numeric_limits<float>::quiet_NaN()
                             : std::numeric_limits<float>::infinity();
    }
    return std::ldexp(static_cast<float>(mantissa | (1U << mantissa_bits)),
                      static_cast<int>(exponent) - 15 - mantissa_bits);
}

float Half(u16 bits) {
    const float magnitude = PackedFloat(bits & 0x7fff, 10);
    return (bits & 0x8000) != 0 ? -magnitude : magnitude;
}

float Unorm8(u8 value) {
    return value / 255.0f;
}

float Snorm8(u8 value) {
    return std::max(static_cast<s8>(value) / 127.0f, -1.0f);
}

float Unorm16(u16 value) {
    return value / 65535.0f;
}

float Snorm16(u16 value) {
    return std::max(static_cast<s16>(value) / 32767.0f, -1.0f);
}

float Identity(float value) {
    return value;
}

float Integer(u32 value) {
    return static_cast<float>(value);
}

/// Returns the number of channels, or 0 when the format has no preview.
int DecodePixel(PixelFormat format, const u8* pixel, std::array<float, 4>& out) {
    out = {0.0f, 0.0f, 0.0f, 1.0f};
    switch (format) {
    case PixelFormat::A8B8G8R8_UNORM:
    case PixelFormat::A8B8G8R8_SRGB:
        return Channels<u8>(pixel, 4, out, Unorm8);
    case PixelFormat::A8B8G8R8_SNORM:
        return Channels<u8>(pixel, 4, out, Snorm8);
    case PixelFormat::B8G8R8A8_UNORM:
    case PixelFormat::B8G8R8A8_SRGB:
        out = {Unorm8(pixel[2]), Unorm8(pixel[1]), Unorm8(pixel[0]), Unorm8(pixel[3])};
        return 4;
    case PixelFormat::R8_UNORM:
        return Channels<u8>(pixel, 1, out, Unorm8);
    case PixelFormat::R8G8_UNORM:
        return Channels<u8>(pixel, 2, out, Unorm8);
    case PixelFormat::R16_UNORM:
        return Channels<u16>(pixel, 1, out, Unorm16);
    case PixelFormat::R16G16_UNORM:
        return Channels<u16>(pixel, 2, out, Unorm16);
    case PixelFormat::R16G16_SNORM:
        return Channels<u16>(pixel, 2, out, Snorm16);
    case PixelFormat::R16G16B16A16_UNORM:
        return Channels<u16>(pixel, 4, out, Unorm16);
    case PixelFormat::R16G16B16A16_SNORM:
        return Channels<u16>(pixel, 4, out, Snorm16);
    case PixelFormat::R16_FLOAT:
        return Channels<u16>(pixel, 1, out, Half);
    case PixelFormat::R16G16_FLOAT:
        return Channels<u16>(pixel, 2, out, Half);
    case PixelFormat::R16G16B16A16_FLOAT:
    case PixelFormat::R16G16B16X16_FLOAT:
        return Channels<u16>(pixel, 4, out, Half);
    case PixelFormat::R32_FLOAT:
        return Channels<float>(pixel, 1, out, Identity);
    case PixelFormat::R32G32_FLOAT:
        return Channels<float>(pixel, 2, out, Identity);
    case PixelFormat::R32G32B32_FLOAT:
        return Channels<float>(pixel, 3, out, Identity);
    case PixelFormat::R32G32B32A32_FLOAT:
        return Channels<float>(pixel, 4, out, Identity);
    case PixelFormat::R32_UINT:
        return Channels<u32>(pixel, 1, out, Integer);
    case PixelFormat::R32G32_UINT:
        return Channels<u32>(pixel, 2, out, Integer);
    case PixelFormat::B10G11R11_FLOAT: {
        const u32 value = Load<u32>(pixel, 0);
        out = {PackedFloat(value & 0x7ff, 6), PackedFloat((value >> 11) & 0x7ff, 6),
               PackedFloat(value >> 22, 5), 1.0f};
        return 3;
    }
    case PixelFormat::E5B9G9R9_FLOAT: {
        const u32 value = Load<u32>(pixel, 0);
        const float scale = std::ldexp(1.0f, static_cast<int>(value >> 27) - 15 - 9);
        out = {static_cast<float>(value & 0x1ff) * scale,
               static_cast<float>((value >> 9) & 0x1ff) * scale,
               static_cast<float>((value >> 18) & 0x1ff) * scale, 1.0f};
        return 3;
    }
    case PixelFormat::A2B10G10R10_UNORM: {
        const u32 value = Load<u32>(pixel, 0);
        out = {(value & 0x3ff) / 1023.0f, ((value >> 10) & 0x3ff) / 1023.0f,
               ((value >> 20) & 0x3ff) / 1023.0f, (value >> 30) / 3.0f};
        return 4;
    }
    default:
        return 0;
    }
}

bool IsFloat(PixelFormat format) {
    switch (format) {
    case PixelFormat::R16_FLOAT:
    case PixelFormat::R16G16_FLOAT:
    case PixelFormat::R16G16B16A16_FLOAT:
    case PixelFormat::R16G16B16X16_FLOAT:
    case PixelFormat::R32_FLOAT:
    case PixelFormat::R32G32_FLOAT:
    case PixelFormat::R32G32B32_FLOAT:
    case PixelFormat::R32G32B32A32_FLOAT:
    case PixelFormat::B10G11R11_FLOAT:
    case PixelFormat::E5B9G9R9_FLOAT:
        return true;
    default:
        return false;
    }
}

/// Float targets hold linear values, so the preview encodes them to sRGB; UNORM bytes are kept.
u8 PreviewByte(float value, bool encode) {
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value >= 1.0f) {
        return 255;
    }
    if (encode) {
        value = value <= 0.0031308f ? value * 12.92f
                                    : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    }
    return static_cast<u8>(value * 255.0f + 0.5f);
}

std::string JsonNumber(double value) {
    return std::isfinite(value) ? fmt::format("{:.6g}", value) : "null";
}

template <typename T>
std::string JsonArray(std::span<T> values) {
    std::string out{"["};
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        if constexpr (std::is_floating_point_v<T>) {
            out += JsonNumber(values[i]);
        } else {
            out += fmt::format("{}", values[i]);
        }
    }
    return out + "]";
}

std::string JsonAddresses(std::span<const u64> values) {
    std::string out{"["};
    for (const u64 value : values) {
        out += fmt::format("{}\"{:#x}\"", out.size() > 1 ? "," : "", value);
    }
    return out + "]";
}

std::string_view Getenv(const char* name) {
    const char* const value = std::getenv(name);
    return value ? value : "";
}

} // Anonymous namespace

FrameDump::FrameDump(const Device& device_, MemoryAllocator& memory_allocator_,
                     Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_} {
    using Common::FS::EdenPath;
    start = std::chrono::steady_clock::now();
    trigger_seconds = std::numeric_limits<double>::infinity();
    sampled = Getenv("EDEN_NX2_FRAME_DUMP_SAMPLED") != "0";
    const std::string_view dir = Getenv("EDEN_NX2_FRAME_DUMP");
    base_directory = dir.empty() ? Common::FS::GetEdenPath(EdenPath::EdenDir) / "dump"
                                 : std::filesystem::path{dir};
    trigger_file = Common::FS::GetEdenPath(EdenPath::EdenDir) / "dump_frame";
    std::string_view at = Getenv("EDEN_NX2_DUMP_FRAME_AT");
    if (at.empty()) {
        at = Getenv("EDEN_NX2_FRAME_DUMP_AT");
    }
    if (at.empty() && !dir.empty()) {
        at = "60";
    }
    if (at == "file") {
        // Captures once <dir>/capture.now exists, so a frame can be picked while playing.
        trigger_file = base_directory / "capture.now";
    } else if (at.ends_with('f')) {
        trigger_frame = std::strtoull(std::string{at}.c_str(), nullptr, 10);
    } else if (!at.empty()) {
        trigger_seconds = std::strtod(std::string{at}.c_str(), nullptr);
    }
    if (!at.empty()) {
        LOG_INFO(Render_Vulkan, "Frame dump armed: dir={} at={}", base_directory.string(), at);
    }
}

FrameDump::~FrameDump() {
    recording_resources = false;
    if (!writer.joinable()) {
        return;
    }
    {
        std::scoped_lock lock{queue_mutex};
        stop_writer = true;
    }
    queue_ready.notify_one();
    writer.join();
}

std::optional<FrameDump::Target> FrameDump::MakeTarget(ImageView& view, u32 slot) const {
    using namespace VideoCore::Surface;
    const Image* const image = view.BackingImage();
    if (!image) {
        return std::nullopt;
    }
    const auto& info = image->info;
    const SurfaceType type = GetFormatType(info.format);
    if (info.type == VideoCommon::ImageType::Buffer ||
        (type != SurfaceType::ColorTexture && type != SurfaceType::Depth &&
         type != SurfaceType::DepthStencil) ||
        DefaultBlockWidth(info.format) != 1 || DefaultBlockHeight(info.format) != 1) {
        return std::nullopt;
    }
    DepthEncoding depth_encoding = DepthEncoding::None;
    if (type != SurfaceType::ColorTexture) {
        // The depth aspect copies in the host format's layout, which may differ from the guest's.
        switch (MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, false, info.format).format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D16_UNORM_S8_UINT:
            depth_encoding = DepthEncoding::Unorm16;
            break;
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D24_UNORM_S8_UINT:
            depth_encoding = DepthEncoding::Unorm24;
            break;
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            depth_encoding = DepthEncoding::Float32;
            break;
        default:
            return std::nullopt;
        }
    }
    const bool rescaled = image->IsRescaled();
    const auto& resolution = Settings::values.resolution_info;
    const u32 width = rescaled ? resolution.ScaleUp(info.size.width) : info.size.width;
    const u32 height = rescaled ? resolution.ScaleUp(info.size.height) : info.size.height;
    const u32 level = static_cast<u32>(view.range.base.level);
    return Target{
        .slot = slot,
        .image = image->Handle(),
        .format = BytesPerBlock(view.format) == BytesPerBlock(info.format) ? view.format
                                                                           : info.format,
        .width = std::max(1U, width >> level),
        .height = std::max(1U, height >> level),
        .depth = info.type == VideoCommon::ImageType::e3D ? std::max(1U, info.size.depth >> level)
                                                          : 1U,
        .level = view.range.base.level,
        .layer = info.type == VideoCommon::ImageType::e3D ? 0 : view.range.base.layer,
        .gpu_addr = image->gpu_addr,
        .rescaled = rescaled,
        .msaa = info.num_samples > 1,
        .depth_encoding = depth_encoding,
    };
}

u32 FrameDump::TexelBytes(const Target& target) {
    switch (target.depth_encoding) {
    case DepthEncoding::None:
        return VideoCore::Surface::BytesPerBlock(target.format);
    case DepthEncoding::Unorm16:
        return 2;
    default:
        return 4;
    }
}

int FrameDump::Decode(const Target& target, const u8* pixel, std::array<float, 4>& out) {
    switch (target.depth_encoding) {
    case DepthEncoding::None:
        return DecodePixel(target.format, pixel, out);
    case DepthEncoding::Unorm16:
        out = {Unorm16(Load<u16>(pixel, 0)), 0.0f, 0.0f, 1.0f};
        return 1;
    case DepthEncoding::Unorm24:
        out = {static_cast<float>(Load<u32>(pixel, 0) & 0xffffff) / 16777215.0f, 0.0f, 0.0f, 1.0f};
        return 1;
    case DepthEncoding::Float32:
        out = {Load<float>(pixel, 0), 0.0f, 0.0f, 1.0f};
        return 1;
    }
    return 0;
}

bool FrameDump::BeginPass(const void* framebuffer, std::span<ImageView* const> colors,
                          ImageView* depth) {
    if (framebuffer == pass_framebuffer) {
        return false;
    }
    const bool recorded = EndPass();
    pass_framebuffer = framebuffer;
    pass_targets.clear();
    for (u32 slot = 0; slot < colors.size(); ++slot) {
        if (!colors[slot]) {
            continue;
        }
        if (const auto target = MakeTarget(*colors[slot], slot)) {
            pass_targets.push_back(*target);
        }
    }
    // Depth is slot 8 ("rt8"): shadow maps are depth-only passes.
    if (depth) {
        if (const auto target = MakeTarget(*depth, 8)) {
            pass_targets.push_back(*target);
        }
    }
    pass_hashes.clear();
    pass_programs.clear();
    pass_sampled.clear();
    pass_first_draw = draws;
    return recorded;
}

namespace {
std::optional<std::pair<GPUVAddr, u32>> SampledKey(const FrameDump::Sampled& texture) {
    const Image* const image = texture.view ? texture.view->BackingImage() : nullptr;
    if (!image) {
        return std::nullopt;
    }
    return std::pair{image->gpu_addr, (static_cast<u32>(texture.view->format) << 16) |
                                          (static_cast<u32>(texture.view->range.base.level) << 8) |
                                          static_cast<u32>(texture.view->range.base.layer)};
}
} // Anonymous namespace

bool FrameDump::RecordSampled(const Sampled& texture, u32 index, std::string name,
                              std::string tags) {
    if (const auto target = MakeTarget(*texture.view, index)) {
        return Record(*target, std::move(name), std::move(tags));
    }
    const Image& image = *texture.view->BackingImage();
    const auto& info = image.info;
    Enqueue({.tags = fmt::format(
                 R"({{"name":"{}",{},"format":"{}","width":{},"height":{},"depth":{},"type":{},"gpu_addr":"{:#x}","skipped":"format"}})",
                 name, tags, info.format, info.size.width, info.size.height, info.size.depth,
                 static_cast<u32>(info.type), image.gpu_addr)});
    return false;
}

bool FrameDump::NoteSampled(std::span<const Sampled> textures) {
    bool recorded = false;
    for (const Sampled& texture : textures) {
        const auto key = SampledKey(texture);
        if (!key) {
            // A handle naming no image: the pass samples zeros.
            if (texture.view) {
                Enqueue({.tags = fmt::format(
                             R"({{"kind":"sampled","pass":{},"draw":{},"stage":{},"tic":{},"skipped":"null image"}})",
                             pass_index, draws - 1, texture.stage, texture.tic)});
            }
            continue;
        }
        if (std::ranges::find(pass_sampled, *key) != pass_sampled.end()) {
            continue;
        }
        const u32 index = static_cast<u32>(pass_sampled.size());
        pass_sampled.push_back(*key);
        const std::string tags =
            fmt::format(R"("kind":"sampled","pass":{},"draw":{},"stage":{},"tic":{},"index":{})",
                        pass_index, draws - 1, texture.stage, texture.tic, index);
        // Read back once per frame until something writes the image again.
        if (const auto it = std::ranges::find(frame_readbacks, *key, &FrameReadback::key);
            it != frame_readbacks.end()) {
            Enqueue({.tags = fmt::format(R"({{{},"same_as":"{}"}})", tags, it->name)});
            continue;
        }
        std::string name = fmt::format("{:04}_pass{:03}_tex{:02}_s{}_tic{}", sequence++,
                                       pass_index, index, texture.stage, texture.tic);
        frame_readbacks.push_back({*key, name});
        recorded |= RecordSampled(texture, index, std::move(name), tags);
    }
    return recorded;
}

void FrameDump::NoteDispatchSampled(u64 shader_hash, std::span<const Sampled> textures) {
    for (u32 index = 0; index < textures.size(); ++index) {
        const Sampled& texture = textures[index];
        const auto key = SampledKey(texture);
        if (!key) {
            continue;
        }
        const std::string tags = fmt::format(
            R"("kind":"sampled","dispatch":{},"after_draws":{},"stage":{},"tic":{},"index":{},"shaders":["{:016x}"])",
            dispatches, draws, texture.stage, texture.tic, index, shader_hash);
        if (const auto it = std::ranges::find(frame_readbacks, *key, &FrameReadback::key);
            it != frame_readbacks.end()) {
            Enqueue({.tags = fmt::format(R"({{{},"same_as":"{}"}})", tags, it->name)});
            continue;
        }
        std::string name = fmt::format("{:04}_dispatch{:04}_{:016x}_tex{:02}_tic{}", sequence++,
                                       dispatches, shader_hash, index, texture.tic);
        frame_readbacks.push_back({*key, name});
        RecordSampled(texture, index, std::move(name), tags);
    }
}

void FrameDump::NoteDraw(std::span<const u64> shader_hashes, std::span<const u64> programs,
                         std::span<ImageView* const> written) {
    ++draws;
    for (const u64 hash : shader_hashes) {
        if (hash != 0 && pass_hashes.size() < 32 &&
            std::ranges::find(pass_hashes, hash) == pass_hashes.end()) {
            pass_hashes.push_back(hash);
        }
    }
    for (const u64 program : programs) {
        if (program != 0 && pass_programs.size() < 64 &&
            std::ranges::find(pass_programs, program) == pass_programs.end()) {
            pass_programs.push_back(program);
        }
    }
    // Storage images the draw writes are captured with the pass's targets (slots 16 and up).
    for (ImageView* const view : written) {
        const auto target = view ? MakeTarget(*view, static_cast<u32>(16 + pass_targets.size()))
                                 : std::nullopt;
        if (!target || std::ranges::any_of(pass_targets, [&](const Target& other) {
                return other.image == target->image && other.level == target->level &&
                       other.layer == target->layer;
            })) {
            continue;
        }
        pass_targets.push_back(*target);
    }
}

void FrameDump::NoteSkipped(std::string_view kind, std::string_view reason,
                            std::span<const u64> programs, GPUVAddr target) {
    if (skipped++ >= MAX_SKIPPED_LINES) {
        return;
    }
    Enqueue({.tags = fmt::format(
                 R"({{"kind":"skipped","what":"{}","reason":"{}","pass":{},"after_draws":{},"after_dispatches":{},"programs":{},"target":"{:#x}"}})",
                 kind, reason, pass_index, draws, dispatches, JsonAddresses(programs), target)});
}

void FrameDump::NoteDrawState(std::string_view fields) {
    if (draw_states++ >= MAX_DRAW_STATE_LINES) {
        return;
    }
    Enqueue({.tags = fmt::format(R"({{"kind":"draw","draw":{},"pass":{},{}}})", draws - 1,
                                 pass_index, fields)});
}

bool FrameDump::EndPass() {
    if (!pass_framebuffer) {
        return false;
    }
    pass_framebuffer = nullptr;
    std::string shaders;
    for (const u64 hash : pass_hashes) {
        shaders += fmt::format("{}\"{:016x}\"", shaders.empty() ? "" : ",", hash);
    }
    const std::string programs = JsonAddresses(pass_programs);
    bool recorded = false;
    for (const Target& target : pass_targets) {
        Written(target.gpu_addr);
        recorded |= Record(
            target,
            fmt::format("{:04}_pass{:03}_d{}-{}_rt{}", sequence++, pass_index, pass_first_draw,
                        draws, target.slot),
            fmt::format(R"("kind":"pass","pass":{},"first_draw":{},"draw_count":{},"slot":{},"shaders":[{}],"programs":{})",
                        pass_index, pass_first_draw, draws - pass_first_draw, target.slot,
                        shaders, programs));
    }
    if (pass_targets.empty() && draws != pass_first_draw) {
        Enqueue({.tags = fmt::format(
                     R"({{"kind":"pass","pass":{},"first_draw":{},"draw_count":{},"shaders":[{}],"programs":{},"skipped":"no target to read back"}})",
                     pass_index, pass_first_draw, draws - pass_first_draw, shaders, programs)});
    }
    ++pass_index;
    return recorded;
}

void FrameDump::CaptureDispatch(u64 shader_hash, u64 program, std::span<const u32, 3> groups,
                                std::span<ImageView* const> written) {
    const u32 dispatch = dispatches++;
    bool any = false;
    for (u32 slot = 0; slot < written.size(); ++slot) {
        const auto target = written[slot] ? MakeTarget(*written[slot], slot) : std::nullopt;
        if (!target) {
            continue;
        }
        Written(target->gpu_addr);
        any = true;
        Record(*target,
               fmt::format("{:04}_dispatch{:04}_{:016x}_img{}", sequence++, dispatch, shader_hash,
                           slot),
               fmt::format(R"("kind":"dispatch","dispatch":{},"after_draws":{},"slot":{},"shaders":["{:016x}"],"program":"{:#x}","groups":[{},{},{}])",
                           dispatch, draws, slot, shader_hash, program, groups[0], groups[1],
                           groups[2]));
    }
    if (!any) {
        // Buffers only (or nothing readable): still listed so the order of passes is complete.
        Enqueue({.tags = fmt::format(
                     R"({{"kind":"dispatch","dispatch":{},"after_draws":{},"shaders":["{:016x}"],"program":"{:#x}","groups":[{},{},{}],"written_images":{}}})",
                     dispatch, draws, shader_hash, program, groups[0], groups[1], groups[2],
                     written.size())});
    }
}

void FrameDump::Written(GPUVAddr gpu_addr) {
    std::erase_if(frame_readbacks,
                  [gpu_addr](const FrameReadback& readback) { return readback.key.first == gpu_addr; });
}

void FrameDump::CapturePresent(ImageView& view) {
    if (const auto target = MakeTarget(view, 0)) {
        Record(*target, fmt::format("{:04}_present", sequence++),
               fmt::format(R"("kind":"present","after_draws":{})", draws));
    }
}

bool FrameDump::Record(const Target& target, std::string name, std::string tags) {
    name += target.depth > 1 ? fmt::format("_{}_{}x{}x{}_{:x}", target.format, target.width,
                                           target.height, target.depth, target.gpu_addr)
                             : fmt::format("_{}_{}x{}_{:x}", target.format, target.width,
                                           target.height, target.gpu_addr);
    if (target.msaa) {
        Enqueue({.tags = fmt::format(R"({{"name":"{}",{},"skipped":"msaa"}})", name, tags)});
        return false;
    }
    const size_t size = size_t{target.width} * target.height * target.depth * TexelBytes(target);
    if (size > MAX_READBACK) {
        Enqueue({.tags = fmt::format(R"({{"name":"{}",{},"format":"{}","width":{},"height":{},"depth":{},"gpu_addr":"{:#x}","skipped":"over {} MiB"}})",
                                     name, tags, target.format, target.width, target.height,
                                     target.depth, target.gpu_addr, MAX_READBACK >> 20)});
        return false;
    }
    if (recorded_bytes + size > SIZE_CAP) {
        truncated = true;
        return false;
    }
    recorded_bytes += size;
    vk::Buffer buffer = memory_allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        },
        MemoryUsage::Download);
    const VkBufferImageCopy copy{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource{
            .aspectMask = target.depth_encoding == DepthEncoding::None
                              ? VkImageAspectFlags{VK_IMAGE_ASPECT_COLOR_BIT}
                              : VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT},
            .mipLevel = static_cast<u32>(target.level),
            .baseArrayLayer = static_cast<u32>(target.layer),
            .layerCount = 1,
        },
        .imageOffset{0, 0, 0},
        .imageExtent{target.width, target.height, target.depth},
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = target.image, destination = *buffer, copy](vk::CommandBuffer cmdbuf) {
        const VkMemoryBarrier before{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, before);
        cmdbuf.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_GENERAL, destination, copy);
        const VkMemoryBarrier after{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT |
                             VK_ACCESS_MEMORY_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                               after);
    });
    pending_bytes += size;
    readbacks.push_back({std::move(buffer), target, std::move(name), std::move(tags)});
    return true;
}

void FrameDump::DrainIfFull() {
    if (pending_bytes >= STAGING_BUDGET) {
        Drain();
    }
}

void FrameDump::Drain() {
    if (readbacks.empty()) {
        return;
    }
    scheduler.Finish();
    for (Readback& readback : readbacks) {
        readback.buffer.Invalidate();
        const Target& target = readback.target;
        const auto mapped = readback.buffer.Mapped().first(size_t{target.width} * target.height *
                                                           target.depth * TexelBytes(target));
        Enqueue({.target = target,
                 .name = std::move(readback.name),
                 .tags = std::move(readback.tags),
                 .data = {mapped.begin(), mapped.end()}});
    }
    readbacks.clear();
    pending_bytes = 0;
}

void FrameDump::Enqueue(Job job) {
    {
        std::unique_lock lock{queue_mutex};
        // Bounded memory: wait for the writer when it falls behind.
        queue_drained.wait(lock, [this] { return queued_bytes < QUEUE_BUDGET; });
        queued_bytes += job.data.size();
        queue.push_back(std::move(job));
    }
    queue_ready.notify_one();
}

void FrameDump::WriterLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock{queue_mutex};
            queue_ready.wait(lock, [this] { return !queue.empty() || stop_writer; });
            if (queue.empty()) {
                return;
            }
            job = std::move(queue.front());
            queue.pop_front();
        }
        if (job.summary) {
            manifest << fmt::format(
                            R"({{"kind":"summary","images":{},{},"bytes":{},"truncated":{}}})",
                            images, job.tags, written_bytes, job.truncated)
                     << '\n';
            manifest.close();
            std::error_code error;
            if (remove_trigger && !std::filesystem::remove(trigger_file, error) && error) {
                // A trigger that cannot be removed would capture every 30 frames.
                LOG_ERROR(Render_Vulkan, "Frame dump: cannot remove {}; trigger disabled",
                          trigger_file.string());
                std::scoped_lock lock{queue_mutex};
                trigger_file.clear();
            }
            LOG_INFO(Render_Vulkan, "Frame dump done: {} images, {}, {} MiB in {}{}", images,
                     job.tags, written_bytes >> 20, directory.string(),
                     job.truncated ? fmt::format(" (stopped at the {} MiB cap)", SIZE_CAP >> 20)
                                   : "");
            std::scoped_lock lock{queue_mutex};
            done = false;
            continue;
        }
        if (job.name.empty()) {
            manifest << job.tags << '\n';
        } else {
            Write(job);
        }
        manifest.flush();
        {
            std::scoped_lock lock{queue_mutex};
            queued_bytes -= job.data.size();
        }
        queue_drained.notify_one();
    }
}

void FrameDump::Write(const Job& job) {
    const Target& target = job.target;
    const u32 bytes_per_pixel = TexelBytes(target);
    const size_t pixels = size_t{target.width} * target.height * target.depth;
    const std::span<const u8> data = job.data;
    WriteFile(job.name + ".raw.zst",
              Common::Compression::CompressDataZSTDDefault(data.data(), data.size()));
    ++images;

    std::array<float, 4> pixel;
    const int channels = Decode(target, data.data(), pixel);
    std::string stats;
    std::string png{"null"};
    if (channels != 0) {
        constexpr double inf = std::numeric_limits<double>::infinity();
        std::array<double, 4> lo{inf, inf, inf, inf};
        std::array<double, 4> hi{-inf, -inf, -inf, -inf};
        std::array<double, 4> sum{};
        std::array<u64, 4> nans{};
        std::array<u64, 4> infs{};
        const bool encode = target.depth_encoding == DepthEncoding::None && IsFloat(target.format);
        // Depth is shown normalized to its range (reverse Z keeps most values near 0).
        const bool normalize = target.depth_encoding != DepthEncoding::None;
        std::vector<float> values(normalize ? pixels : 0);
        std::vector<u8> rgb(pixels * 3);
        for (size_t index = 0; index < pixels; ++index) {
            Decode(target, data.data() + index * bytes_per_pixel, pixel);
            bool finite = true;
            for (int c = 0; c < channels; ++c) {
                const double value = pixel[c];
                if (std::isnan(value)) {
                    ++nans[c];
                    finite = false;
                } else if (std::isinf(value)) {
                    ++infs[c];
                    finite = false;
                } else {
                    lo[c] = std::min(lo[c], value);
                    hi[c] = std::max(hi[c], value);
                    sum[c] += value;
                }
            }
            u8* const out = &rgb[index * 3];
            if (!finite) {
                out[0] = 255;
                out[1] = 0;
                out[2] = 255;
                continue;
            }
            if (normalize) {
                values[index] = pixel[0];
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                out[c] = PreviewByte(pixel[channels == 1 ? 0 : c], encode);
            }
        }
        if (normalize && hi[0] > lo[0]) {
            const double scale = 1.0 / (hi[0] - lo[0]);
            for (size_t index = 0; index < pixels; ++index) {
                u8* const out = &rgb[index * 3];
                if (out[1] == 0 && out[0] == 255 && out[2] == 255) {
                    continue;
                }
                const u8 value = PreviewByte(static_cast<float>((values[index] - lo[0]) * scale), false);
                out[0] = out[1] = out[2] = value;
            }
        }
        std::array<double, 4> mean{};
        for (int c = 0; c < channels; ++c) {
            const u64 finite_count = pixels - nans[c] - infs[c];
            mean[c] = finite_count != 0 ? sum[c] / static_cast<double>(finite_count) : inf;
        }
        const auto first = [channels](const auto& values_) {
            return std::span(values_.data(), static_cast<size_t>(channels));
        };
        stats = fmt::format(R"(,"min":{},"max":{},"mean":{},"nan":{},"inf":{})",
                            JsonArray(first(lo)), JsonArray(first(hi)), JsonArray(first(mean)),
                            JsonArray(first(nans)), JsonArray(first(infs)));
        std::vector<u8> encoded;
        const int ok = stbi_write_png_to_func(
            [](void* context, void* bytes, int size) {
                auto* const out = static_cast<std::vector<u8>*>(context);
                const auto* const begin = static_cast<const u8*>(bytes);
                out->insert(out->end(), begin, begin + size);
            },
            &encoded, static_cast<int>(target.width), static_cast<int>(target.height * target.depth), 3,
            rgb.data(), static_cast<int>(target.width * 3));
        if (ok != 0) {
            WriteFile(job.name + ".png", encoded);
            png = fmt::format("\"{}.png\"", job.name);
        }
    }
    manifest << fmt::format(
                    R"({{"name":"{}",{},"format":"{}","width":{},"height":{},"depth":{},"level":{},"layer":{},"gpu_addr":"{:#x}","rescaled":{},"depth_encoding":{},"raw":"{}.raw.zst","png":{}{}}})",
                    job.name, job.tags, target.format, target.width, target.height,
                    target.depth, target.level, target.layer, target.gpu_addr, target.rescaled,
                    static_cast<u32>(target.depth_encoding), job.name, png, stats)
             << '\n';
}

void FrameDump::WriteFile(const std::string& name, std::span<const u8> data) {
    std::ofstream file{directory / name, std::ios::binary};
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    written_bytes += data.size();
}

bool FrameDump::TriggerPending() {
    remove_trigger = false;
    if (frame >= trigger_frame) {
        trigger_frame = ~0ULL;
        return true;
    }
    if (std::isfinite(trigger_seconds) &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >=
            trigger_seconds) {
        trigger_seconds = std::numeric_limits<double>::infinity();
        return true;
    }
    if (frame % 30 != 0 || trigger_file.empty()) {
        return false;
    }
    std::error_code error;
    if (!std::filesystem::exists(trigger_file, error)) {
        return false;
    }
    remove_trigger = true;
    return true;
}

void FrameDump::Start() {
    std::error_code error;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    directory = base_directory / fmt::format("frame_{:04}{:02}{:02}_{:02}{:02}{:02}",
                                             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                                             local.tm_hour, local.tm_min, local.tm_sec);
    std::filesystem::create_directories(directory, error);
    manifest.open(directory / "manifest.jsonl", std::ios::trunc);
    if (!manifest) {
        LOG_ERROR(Render_Vulkan, "Frame dump: cannot write {}", directory.string());
        if (remove_trigger) {
            std::filesystem::remove(trigger_file, error);
        }
        return;
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    manifest << fmt::format(R"({{"kind":"frame","frame":{},"seconds":{:.3f},"sampled":{}}})",
                            frame, elapsed, sampled)
             << '\n';
    images = 0;
    sequence = 0;
    written_bytes = 0;
    recorded_bytes = 0;
    truncated = false;
    pass_index = 0;
    pass_first_draw = 0;
    draws = 0;
    dispatches = 0;
    skipped = 0;
    draw_states = 0;
    pass_framebuffer = nullptr;
    frame_readbacks.clear();
    if (!writer.joinable()) {
        writer = std::thread{[this] { WriterLoop(); }};
    }
    {
        std::scoped_lock lock{queue_mutex};
        done = true; // until the writer has finished this capture
    }
    capturing = true;
    recording_resources = true;
    LOG_INFO(Render_Vulkan, "Frame dump: capturing frame {} ({:.1f} s after boot) into {}", frame,
             elapsed, directory.string());
}

void FrameDump::TickFrame() {
    ++frame;
    if (capturing) {
        EndPass();
        Drain();
        Enqueue({.tags = fmt::format(R"("passes":{},"draws":{},"dispatches":{},"skipped":{})",
                                     pass_index, draws, dispatches, skipped),
                 .summary = true,
                 .truncated = truncated});
        capturing = false;
        recording_resources = false;
        LOG_INFO(Render_Vulkan, "Frame dump: frame captured ({} passes, {} draws, {} dispatches), "
                 "writing {}", pass_index, draws, dispatches, directory.string());
        return;
    }
    {
        // The previous capture is still being written.
        std::scoped_lock lock{queue_mutex};
        if (done) {
            return;
        }
    }
    if (TriggerPending()) {
        Start();
    }
}

} // namespace Vulkan

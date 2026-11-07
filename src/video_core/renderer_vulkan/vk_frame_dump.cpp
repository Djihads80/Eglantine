// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/stb.h"
#include "common/zstd_compression.h"
#include "video_core/nx2_debug_control.h"
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

constexpr size_t SIZE_CAP = size_t{2} << 30;
/// RecordingSampledTextures: a sampled capture is running.
std::atomic<bool> recording_sampled{};
/// RecordingWrittenImages: a capture is running.
std::atomic<bool> recording_capture{};
constexpr size_t STAGING_BUDGET = size_t{256} << 20;

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

float Unorm16(u16 value) {
    return value / 65535.0f;
}

float Identity(float value) {
    return value;
}

/// Returns the number of channels, or 0 when the format has no preview.
int DecodePixel(PixelFormat format, const u8* pixel, std::array<float, 4>& out) {
    out = {0.0f, 0.0f, 0.0f, 1.0f};
    switch (format) {
    case PixelFormat::A8B8G8R8_UNORM:
    case PixelFormat::A8B8G8R8_SRGB:
        return Channels<u8>(pixel, 4, out, Unorm8);
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
    case PixelFormat::R16G16B16A16_UNORM:
        return Channels<u16>(pixel, 4, out, Unorm16);
    case PixelFormat::R16_FLOAT:
        return Channels<u16>(pixel, 1, out, Half);
    case PixelFormat::R16G16_FLOAT:
        return Channels<u16>(pixel, 2, out, Half);
    case PixelFormat::R16G16B16A16_FLOAT:
        return Channels<u16>(pixel, 4, out, Half);
    case PixelFormat::R32_FLOAT:
        return Channels<float>(pixel, 1, out, Identity);
    case PixelFormat::R32G32_FLOAT:
        return Channels<float>(pixel, 2, out, Identity);
    case PixelFormat::R32G32B32A32_FLOAT:
        return Channels<float>(pixel, 4, out, Identity);
    case PixelFormat::B10G11R11_FLOAT: {
        const u32 value = Load<u32>(pixel, 0);
        out = {PackedFloat(value & 0x7ff, 6), PackedFloat((value >> 11) & 0x7ff, 6),
               PackedFloat(value >> 22, 5), 1.0f};
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
    case PixelFormat::R32_FLOAT:
    case PixelFormat::R32G32_FLOAT:
    case PixelFormat::R32G32B32A32_FLOAT:
    case PixelFormat::B10G11R11_FLOAT:
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
        value =
            value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
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

std::string_view FilterName(VkFilter filter) {
    return filter == VK_FILTER_NEAREST  ? "nearest"
           : filter == VK_FILTER_LINEAR ? "linear"
                                        : "other";
}

std::string_view WrapName(VkSamplerAddressMode mode) {
    switch (mode) {
    case VK_SAMPLER_ADDRESS_MODE_REPEAT:
        return "repeat";
    case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
        return "mirrored_repeat";
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
        return "clamp_to_edge";
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
        return "clamp_to_border";
    case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
        return "mirror_clamp_to_edge";
    default:
        return "other";
    }
}

/// The host sampler state and the raw guest TSC words it came from.
std::string SamplerJson(const Sampler* sampler) {
    if (!sampler) {
        return {};
    }
    const VkSamplerCreateInfo& ci = sampler->CreateInfo();
    std::array<u32, 8> words;
    static_assert(sizeof(words) == sizeof(Tegra::Texture::TSCEntry));
    std::memcpy(words.data(), &sampler->Descriptor(), sizeof(words));
    std::string tsc;
    for (const u32 word : words) {
        tsc += fmt::format("{}\"{:08x}\"", tsc.empty() ? "" : ",", word);
    }
    return fmt::format(
        R"(,"sampler":{{"mag":"{}","min":"{}","mip":"{}","wrap":["{}","{}","{}"],)"
        R"("unnormalized":{},"compare":{},"compare_op":{},"lod_bias":{},"min_lod":{},)"
        R"("max_lod":{},"anisotropy":{},"tsc":[{}]}})",
        FilterName(ci.magFilter), FilterName(ci.minFilter),
        ci.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? "linear" : "nearest",
        WrapName(ci.addressModeU), WrapName(ci.addressModeV), WrapName(ci.addressModeW),
        ci.unnormalizedCoordinates != VK_FALSE, ci.compareEnable != VK_FALSE,
        static_cast<u32>(ci.compareOp), JsonNumber(ci.mipLodBias), JsonNumber(ci.minLod),
        JsonNumber(ci.maxLod), ci.anisotropyEnable != VK_FALSE ? JsonNumber(ci.maxAnisotropy) : "1",
        tsc);
}

} // Anonymous namespace

FrameDump::FrameDump(const Device& device_, MemoryAllocator& memory_allocator_,
                     Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_} {
    const char* const dir = std::getenv("EDEN_NX2_FRAME_DUMP");
    if (!dir || *dir == '\0') {
        return;
    }
    const char* const sampled_value = std::getenv("EDEN_NX2_FRAME_DUMP_SAMPLED");
    directory = dir;
    start = std::chrono::steady_clock::now();
    const char* const at = std::getenv("EDEN_NX2_FRAME_DUMP_AT");
    const std::string_view value = at ? at : "60";
    // "file" captures once <dir>/capture.now exists, so a frame can be picked while playing.
    schedule.Configure(value, directory / "capture.now",
                       sampled_value && *sampled_value != '\0' && *sampled_value != '0');
    LOG_INFO(Render_Vulkan, "Frame dump armed: dir={} at={}",
             Common::FS::PathToUTF8String(directory), value);
}

FrameDump::~FrameDump() {
    StopWriter();
}

void FrameDump::StopWriter() {
    if (!writer.joinable()) {
        return;
    }
    {
        std::scoped_lock lock{queue_mutex};
        stop_writer = true;
    }
    queue_ready.notify_one();
    writer.join();
    stop_writer = false;
}

void FrameDump::Arm(std::filesystem::path directory_, bool sampled, bool buffers, u64 id) {
    if (!schedule.Arm(sampled, buffers, id)) {
        VideoCore::Nx2Debug::FinishCapture(id, {.error = "another capture is in progress"});
        return;
    }
    StopWriter();
    directory = std::move(directory_);
    truncated = false;
    recorded_bytes = 0;
    written_bytes = 0;
    sequence = 0;
    images = 0;
    pass_framebuffer = nullptr;
    dispatch_sampled.clear();
    seen_buffers.clear();
    written_buffers.clear();
    shader_keys.clear();
    pass_index = 0;
    draws = 0;
    dispatches = 0;
    if (start == std::chrono::steady_clock::time_point{}) {
        start = std::chrono::steady_clock::now();
    }
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
        switch (
            MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, false, info.format).format) {
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
        .format =
            BytesPerBlock(view.format) == BytesPerBlock(info.format) ? view.format : info.format,
        .width = std::max(1U, width >> level),
        .height = std::max(1U, height >> level),
        .depth =
            info.type == VideoCommon::ImageType::e3D ? std::max(1U, info.size.depth >> level) : 1U,
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

bool FrameDump::BeginPass(const void* framebuffer, std::span<ImageView* const> colors) {
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
    pass_hashes.clear();
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
    tags += SamplerJson(texture.sampler);
    if (const auto target = MakeTarget(*texture.view, index)) {
        return Record(*target, std::move(name), std::move(tags));
    }
    const Image& image = *texture.view->BackingImage();
    const auto& info = image.info;
    Enqueue({.tags = fmt::format(
                 R"({{"name":"{}",{},"format":"{}","width":{},"height":{},"depth":{},"type":{},)"
                 R"("gpu_addr":"{:#x}","skipped":"format"}})",
                 name, tags, info.format, info.size.width, info.size.height, info.size.depth,
                 static_cast<u32>(info.type), image.gpu_addr)});
    return false;
}

bool FrameDump::NoteSampled(std::span<const Sampled> textures) {
    bool recorded = false;
    for (const Sampled& texture : textures) {
        const auto key = SampledKey(texture);
        if (!key || std::ranges::find(pass_sampled, *key) != pass_sampled.end()) {
            continue;
        }
        const u32 index = static_cast<u32>(pass_sampled.size());
        pass_sampled.push_back(*key);
        recorded |= RecordSampled(
            texture, index,
            fmt::format("{:04}_pass{:03}_tex{:02}_s{}_tic{}", sequence++, pass_index, index,
                        texture.stage, texture.tic),
            fmt::format(R"("kind":"sampled","pass":{},"draw":{},"stage":{},"tic":{},"index":{})",
                        pass_index, draws - 1, texture.stage, texture.tic, index));
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
        const std::tuple<u64, GPUVAddr, u32> seen{shader_hash, key->first, key->second};
        if (std::ranges::find(dispatch_sampled, seen) != dispatch_sampled.end()) {
            continue;
        }
        dispatch_sampled.push_back(seen);
        RecordSampled(
            texture, index,
            fmt::format("{:04}_dispatch{:04}_{:016x}_tex{:02}_tic{}", sequence++, dispatches,
                        shader_hash, index, texture.tic),
            fmt::format(R"("kind":"sampled","dispatch":{},"after_draws":{},"stage":{},"tic":{},)"
                        R"("index":{},{})",
                        dispatches, draws, texture.stage, texture.tic, index,
                        ShaderList(std::array{shader_hash})));
    }
}

bool FrameDump::NoteBuffers(bool compute, std::span<const u64> shader_hashes,
                            std::span<BoundBuffer> buffers) {
    bool recorded = false;
    for (BoundBuffer& buffer : buffers) {
        const size_t program = compute ? 0 : buffer.stage + 1;
        const u64 shader = program < shader_hashes.size() ? shader_hashes[program] : 0;
        const std::tuple<u64, bool, u32, u64, u32> key{shader, buffer.storage, buffer.index,
                                                       buffer.device_addr, buffer.size};
        if (std::ranges::find(seen_buffers, key) != seen_buffers.end()) {
            continue;
        }
        seen_buffers.push_back(key);
        const std::string owner =
            compute ? fmt::format(R"("dispatch":{},"after_draws":{})", dispatches, draws)
                    : fmt::format(R"("pass":{},"draw":{})", pass_index, draws - 1);
        const bool from_host = buffer.guest.empty();
        std::string tags = fmt::format(
            R"("kind":"buffer",{},"stage":{},"shader":"{:016x}","key":"{}","type":"{}",)"
            R"("index":{},"written":{},"gpu_addr":"{:#x}","device_addr":"{:#x}",)"
            R"("bound_size":{},"size":{},"gpu_modified":{},"source":"{}")",
            owner, buffer.stage, shader, KeyOf(shader), buffer.storage ? "storage" : "uniform",
            buffer.index, buffer.written, buffer.gpu_addr, buffer.device_addr, buffer.bound_size,
            buffer.size, buffer.gpu_modified, from_host ? "host" : "guest");
        if (!from_host) {
            Enqueue({.tags = std::move(tags), .data = std::move(buffer.guest), .raw = true});
            continue;
        }
        recorded |= RecordBuffer(buffer.host, buffer.host_offset, buffer.size, std::move(tags));
    }
    return recorded;
}

void FrameDump::NoteLaunch(u64 shader_hash, const std::array<u32, 3>& grid,
                           const std::array<u32, 3>& block, std::optional<GPUVAddr> indirect,
                           std::optional<std::array<u32, 3>> indirect_grid) {
    std::string tags =
        fmt::format(R"({{"kind":"launch","dispatch":{},"after_draws":{},{},"grid":[{},{},{}],)"
                    R"("block":[{},{},{}])",
                    dispatches, draws, ShaderList(std::array{shader_hash}), grid[0], grid[1],
                    grid[2], block[0], block[1], block[2]);
    if (indirect) {
        tags += fmt::format(R"(,"indirect":"{:#x}")", *indirect);
    }
    if (indirect_grid) {
        tags += fmt::format(R"(,"indirect_grid_guest":[{},{},{}])", (*indirect_grid)[0],
                            (*indirect_grid)[1], (*indirect_grid)[2]);
    }
    Enqueue({.tags = tags + "}"});
}

void FrameDump::NoteKeys(std::span<const u64> shader_hashes, std::span<const u64> keys) {
    for (size_t index = 0; index < shader_hashes.size() && index < keys.size(); ++index) {
        const u64 hash = shader_hashes[index];
        if (hash == 0 || keys[index] == 0 ||
            std::ranges::find(shader_keys, hash, &std::pair<u64, u64>::first) !=
                shader_keys.end()) {
            continue;
        }
        shader_keys.emplace_back(hash, keys[index]);
    }
}

std::string FrameDump::KeyOf(u64 shader_hash) const {
    const auto it = std::ranges::find(shader_keys, shader_hash, &std::pair<u64, u64>::first);
    return it != shader_keys.end() ? fmt::format("{:016x}", it->second) : std::string{};
}

std::string FrameDump::ShaderList(std::span<const u64> hashes) const {
    std::string shaders;
    std::string keys;
    for (const u64 hash : hashes) {
        const char* const separator = shaders.empty() ? "" : ",";
        shaders += fmt::format("{}\"{:016x}\"", separator, hash);
        keys += fmt::format("{}\"{}\"", separator, KeyOf(hash));
    }
    return fmt::format(R"("shaders":[{}],"keys":[{}])", shaders, keys);
}

void FrameDump::NoteDraw(std::span<const u64> shader_hashes) {
    ++draws;
    for (const u64 hash : shader_hashes) {
        if (hash != 0 && pass_hashes.size() < 32 &&
            std::ranges::find(pass_hashes, hash) == pass_hashes.end()) {
            pass_hashes.push_back(hash);
        }
    }
}

bool FrameDump::EndPass() {
    if (!pass_framebuffer) {
        return false;
    }
    pass_framebuffer = nullptr;
    const std::string shaders = ShaderList(pass_hashes);
    bool recorded = false;
    for (const Target& target : pass_targets) {
        recorded |=
            Record(target,
                   fmt::format("{:04}_pass{:03}_d{}-{}_rt{}", sequence++, pass_index,
                               pass_first_draw, draws, target.slot),
                   fmt::format(
                       R"("kind":"pass","pass":{},"first_draw":{},"draw_count":{},"slot":{},{})",
                       pass_index, pass_first_draw, draws - pass_first_draw, target.slot, shaders));
    }
    ++pass_index;
    return recorded;
}

void FrameDump::CaptureDispatch(u64 shader_hash, std::span<ImageView* const> written) {
    const u32 dispatch = dispatches++;
    for (u32 slot = 0; slot < written.size(); ++slot) {
        const auto target = written[slot] ? MakeTarget(*written[slot], slot) : std::nullopt;
        if (!target) {
            continue;
        }
        Record(*target,
               fmt::format("{:04}_dispatch{:04}_{:016x}_img{}", sequence++, dispatch, shader_hash,
                           slot),
               fmt::format(R"("kind":"dispatch","dispatch":{},"after_draws":{},"slot":{},{})",
                           dispatch, draws, slot, ShaderList(std::array{shader_hash})));
    }
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
            .dstAccessMask =
                VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                               after);
    });
    pending_bytes += size;
    readbacks.push_back({std::move(buffer), target, std::move(name), std::move(tags)});
    return true;
}

bool FrameDump::RecordBuffer(VkBuffer source, u32 offset, u32 size, std::string tags) {
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
    const VkBufferCopy copy{.srcOffset = offset, .dstOffset = 0, .size = size};
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([source, destination = *buffer, copy](vk::CommandBuffer cmdbuf) {
        const VkMemoryBarrier before{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, before);
        cmdbuf.CopyBuffer(source, destination, copy);
        const VkMemoryBarrier after{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask =
                VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                               after);
    });
    pending_bytes += size;
    readbacks.push_back({.buffer = std::move(buffer),
                         .target = {},
                         .name = {},
                         .tags = std::move(tags),
                         .raw = true,
                         .bytes = size});
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
        const auto mapped =
            readback.buffer.Mapped().first(readback.raw ? readback.bytes
                                                        : size_t{target.width} * target.height *
                                                              target.depth * TexelBytes(target));
        Enqueue({.target = target,
                 .name = std::move(readback.name),
                 .tags = std::move(readback.tags),
                 .data = {mapped.begin(), mapped.end()},
                 .raw = readback.raw});
    }
    readbacks.clear();
    pending_bytes = 0;
}

void FrameDump::Enqueue(Job job) {
    {
        std::scoped_lock lock{queue_mutex};
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
            LOG_INFO(Render_Vulkan, "Frame dump done: {} images, {}, {} MiB in {}{}", images,
                     job.tags, written_bytes >> 20, Common::FS::PathToUTF8String(directory),
                     job.truncated ? fmt::format(" (stopped at the {} GiB cap)", SIZE_CAP >> 30)
                                   : "");
            if (const u64 id = schedule.CaptureId(); id != 0) {
                VideoCore::Nx2Debug::FinishCapture(id, {});
            }
            continue;
        }
        if (job.raw) {
            WriteBuffer(job);
        } else if (job.name.empty()) {
            manifest << job.tags << '\n';
        } else {
            Write(job);
        }
        manifest.flush();
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
            for (int c = 0; c < 3; ++c) {
                out[c] = PreviewByte(pixel[channels == 1 ? 0 : c], encode);
            }
        }
        std::array<double, 4> mean{};
        for (int c = 0; c < channels; ++c) {
            const u64 finite_count = pixels - nans[c] - infs[c];
            mean[c] = finite_count != 0 ? sum[c] / static_cast<double>(finite_count) : inf;
        }
        const auto first = [channels](const auto& values) {
            return std::span(values.data(), static_cast<size_t>(channels));
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
            &encoded, static_cast<int>(target.width),
            static_cast<int>(target.height * target.depth), 3, rgb.data(),
            static_cast<int>(target.width * 3));
        if (ok != 0) {
            WriteFile(job.name + ".png", encoded);
            png = fmt::format("\"{}.png\"", job.name);
        }
    }
    manifest
        << fmt::format(
               R"({{"name":"{}",{},"format":"{}","width":{},"height":{},"depth":{},"level":{},)"
               R"("layer":{},"gpu_addr":"{:#x}","rescaled":{},"depth_encoding":{},)"
               R"("raw":"{}.raw.zst","png":{}{}}})",
               job.name, job.tags, target.format, target.width, target.height, target.depth,
               target.level, target.layer, target.gpu_addr, target.rescaled,
               static_cast<u32>(target.depth_encoding), job.name, png, stats)
        << '\n';
}

void FrameDump::WriteBuffer(const Job& job) {
    const u64 hash =
        Common::CityHash64(reinterpret_cast<const char*>(job.data.data()), job.data.size());
    const std::string name = fmt::format("buffer_{:016x}.bin.zst", hash);
    if (std::ranges::find(written_buffers, hash) == written_buffers.end()) {
        written_buffers.push_back(hash);
        WriteFile(name,
                  Common::Compression::CompressDataZSTDDefault(job.data.data(), job.data.size()));
    }
    manifest << fmt::format(R"({{{},"hash":"{:016x}","raw":"{}"}})", job.tags, hash, name) << '\n';
}

void FrameDump::WriteFile(const std::string& name, std::span<const u8> data) {
    std::ofstream file{directory / name, std::ios::binary};
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    written_bytes += data.size();
}

void FrameDump::TickFrame() {
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    switch (schedule.Tick(elapsed)) {
    case FrameDumpSchedule::Step::None:
        return;
    case FrameDumpSchedule::Step::Finish:
        EndPass();
        Drain();
        Enqueue({.tags = fmt::format(R"("passes":{},"draws":{},"dispatches":{})", pass_index, draws,
                                     dispatches),
                 .summary = true,
                 .truncated = truncated});
        return;
    case FrameDumpSchedule::Step::Start:
        break;
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    manifest.open(directory / "manifest.jsonl", std::ios::trunc);
    if (!manifest) {
        LOG_ERROR(Render_Vulkan, "Frame dump: cannot write {}",
                  Common::FS::PathToUTF8String(directory));
        if (const u64 id = schedule.CaptureId(); id != 0) {
            VideoCore::Nx2Debug::FinishCapture(
                id, {.error = fmt::format("cannot write {}", directory.generic_string())});
        }
        schedule.Abort();
        return;
    }
    const u64 frame = schedule.Frame();
    manifest << fmt::format(R"({{"kind":"frame","frame":{},"seconds":{:.3f}}})", frame, elapsed)
             << '\n';
    shader_keys.clear();
    writer = std::thread{[this] { WriterLoop(); }};
    const char* const extras =
        schedule.Sampled()
            ? (schedule.Buffers() ? " with sampled textures and buffers" : " with sampled textures")
            : (schedule.Buffers() ? " with buffers" : "");
    LOG_INFO(Render_Vulkan, "Frame dump: capturing frame {} ({:.1f} s after boot){} into {}", frame,
             elapsed, extras, Common::FS::PathToUTF8String(directory));
}

void FrameDumpSchedule::Configure(std::string_view at, std::filesystem::path trigger_file_,
                                  bool sampled_) {
    enabled = true;
    sampled = sampled_;
    buffers = sampled_;
    if (at == "file") {
        trigger_file = std::move(trigger_file_);
    } else if (at.ends_with('f')) {
        trigger_frame = std::strtoull(std::string{at}.c_str(), nullptr, 10);
    } else {
        trigger_seconds = std::strtod(std::string{at}.c_str(), nullptr);
    }
}

bool FrameDumpSchedule::Arm(bool sampled_, bool buffers_, u64 id) {
    if (capturing) {
        return false;
    }
    enabled = true;
    done = false;
    sampled = sampled_;
    buffers = buffers_;
    trigger_frame = 0;
    capture_id = id;
    return true;
}

FrameDumpSchedule::Step FrameDumpSchedule::Tick(double elapsed_seconds) {
    if (!enabled || done) {
        return Step::None;
    }
    ++frame;
    if (capturing) {
        SetCapturing(false);
        done = true;
        return Step::Finish;
    }
    std::error_code error;
    if (frame < trigger_frame && elapsed_seconds < trigger_seconds &&
        (trigger_file.empty() || frame % 30 != 0 ||
         !std::filesystem::exists(trigger_file, error))) {
        return Step::None;
    }
    SetCapturing(true);
    return Step::Start;
}

void FrameDumpSchedule::Abort() {
    SetCapturing(false);
    done = true;
}

void FrameDumpSchedule::SetCapturing(bool value) {
    capturing = value;
    recording_sampled.store(value && sampled, std::memory_order_relaxed);
    recording_capture.store(value, std::memory_order_relaxed);
}

bool RecordingSampledTextures() noexcept {
    return recording_sampled.load(std::memory_order_relaxed);
}

bool RecordingWrittenImages() noexcept {
    return recording_capture.load(std::memory_order_relaxed);
}

} // namespace Vulkan

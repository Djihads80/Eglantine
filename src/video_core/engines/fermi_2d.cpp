// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <set>
#include <cstdlib>
#include "common/assert.h"
#include "common/logging.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/sw_blitter/blitter.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/surface.h"
#include "video_core/textures/decoders.h"

using VideoCore::Surface::BytesPerBlock;
using VideoCore::Surface::PixelFormatFromRenderTargetFormat;

namespace Tegra::Engines {

using namespace Texture;

Fermi2D::Fermi2D(MemoryManager& memory_manager_) : memory_manager{memory_manager_} {
    sw_blitter = std::make_unique<Blitter::SoftwareBlitEngine>(memory_manager);
    // Nvidia's OpenGL driver seems to assume these values
    regs.src.depth = 1;
    regs.dst.depth = 1;

    execution_mask.reset();
    execution_mask[FERMI2D_REG_INDEX(pixels_from_memory.src_y0) + 1] = true;
}

Fermi2D::~Fermi2D() = default;

void Fermi2D::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

void Fermi2D::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    ASSERT_MSG(method < Regs::NUM_REGS, "Invalid Fermi2D register, increase the size of the Regs structure");
    regs.reg_array[method] = method_argument;

    if (method == FERMI2D_REG_INDEX(pixels_from_memory.src_y0) + 1) {
        Blit();
    }
}

void Fermi2D::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    for (u32 i = 0; i < amount; ++i) {
        CallMethod(system, method, base_start[i], methods_pending - i <= 1);
    }
}

void Fermi2D::ConsumeSinkImpl(Core::System& system) {
    for (auto [method, value] : method_sink) {
        regs.reg_array[method] = value;
    }
    method_sink.clear();
}

void Fermi2D::Blit() {
    if (static const u64 trace_destination = [] {
            const char* value = std::getenv("EDEN_NX2_TRACE_DST");
            return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ULL;
        }();
        trace_destination != 0 && regs.dst.Address() >= trace_destination &&
        regs.dst.Address() < trace_destination + (32ULL << 20)) {
        static u32 traced_destination{};
        if (traced_destination++ < 40) {
            LOG_INFO(HW_GPU, "nx2 2D blit to traced dst: {:#x} {}x{} -> {:#x} {}x{}",
                     regs.src.Address(), regs.src.width, regs.src.height, regs.dst.Address(),
                     regs.dst.width, regs.dst.height);
        }
    }
    if (static const bool trace = std::getenv("EDEN_NX2_RT_TRACE") != nullptr; trace) {
        static u32 traced{};
        if (traced++ < 64) {
            LOG_INFO(HW_GPU, "nx2 2D blit {:#x} {}x{} -> {:#x} {}x{}", regs.src.Address(),
                     regs.src.width, regs.src.height, regs.dst.Address(), regs.dst.width,
                     regs.dst.height);
        }
    }
    LOG_DEBUG(HW_GPU, "called. source address={:#x}, destination address={:#x}", regs.src.Address(), regs.dst.Address());

    UNIMPLEMENTED_IF_MSG(regs.operation != Operation::SrcCopy, "Operation is not copy");
    UNIMPLEMENTED_IF_MSG(regs.src.layer != 0, "Source layer is not zero");
    UNIMPLEMENTED_IF_MSG(regs.dst.layer != 0, "Destination layer is not zero");
    UNIMPLEMENTED_IF_MSG(regs.src.depth != 1, "Source depth is not one");
    UNIMPLEMENTED_IF_MSG(regs.clip_enable != 0, "Clipped blit enabled");

    const auto& args = regs.pixels_from_memory;
    constexpr s64 null_derivative = 1ULL << 32;
    Surface src = regs.src;
    // The byte size below drives both the delegation decision and the pitch alignment arithmetic,
    // and the software blitter copies rows using it. Guessing four bytes for an undecodable
    // format makes a pitch linear copy read past the end of the source, so refuse the blit
    // instead of guessing.
    const auto src_pixel_format = VideoCore::Surface::TryPixelFormatFromRenderTargetFormat(src.format);
    if (!src_pixel_format) {
        static std::mutex mutex;
        static std::set<u32> logged;
        bool should_log = false;
        {
            const std::scoped_lock lock{mutex};
            should_log = logged.size() < 8 && logged.emplace(static_cast<u32>(src.format)).second;
        }
        if (should_log) {
            LOG_CRITICAL(HW_GPU,
                         "Fermi2D: undecodable source format {:#x}, skipping the blit instead of "
                         "guessing a pixel size",
                         static_cast<u32>(src.format));
        }
        return;
    }
    const auto bytes_per_pixel = BytesPerBlock(*src_pixel_format);
    const bool delegate_to_gpu = src.width > 512 && src.height > 512 && bytes_per_pixel <= 8 &&
                                 src.format != regs.dst.format;

    auto srcX = args.src_x0;
    auto srcY = args.src_y0;
    if (args.sample_mode.origin == Origin::Corner) {
        srcX -= (args.du_dx >> 33) << 32;
        srcY -= (args.dv_dy >> 33) << 32;
    }

    Config config{
        .operation = regs.operation,
        .filter = args.sample_mode.filter,
        .must_accelerate =
            args.du_dx != null_derivative || args.dv_dy != null_derivative || delegate_to_gpu,
        .dst_x0 = args.dst_x0,
        .dst_y0 = args.dst_y0,
        .dst_x1 = args.dst_x0 + args.dst_width,
        .dst_y1 = args.dst_y0 + args.dst_height,
        .src_x0 = static_cast<s32>(srcX >> 32),
        .src_y0 = static_cast<s32>(srcY >> 32),
        .src_x1 = static_cast<s32>((srcX + args.du_dx * args.dst_width) >> 32),
        .src_y1 = static_cast<s32>((srcY + args.dv_dy * args.dst_height) >> 32),
    };

    const auto need_align_to_pitch =
        src.linear == Tegra::Engines::Fermi2D::MemoryLayout::Pitch &&
        static_cast<s32>(src.width) == config.src_x1 &&
        config.src_x1 > static_cast<s32>(src.pitch / bytes_per_pixel) && config.src_x0 > 0;
    if (need_align_to_pitch) {
        auto address = src.Address() + config.src_x0 * bytes_per_pixel;
        src.addr_upper = static_cast<u32>(address >> 32);
        src.addr_lower = static_cast<u32>(address);
        src.width -= config.src_x0;
        config.src_x1 -= config.src_x0;
        config.src_x0 = 0;
    }

    memory_manager.FlushCaching();
    if (!rasterizer->AccelerateSurfaceCopy(src, regs.dst, config)) {
        sw_blitter->Blit(src, regs.dst, config);
    }
}

} // namespace Tegra::Engines

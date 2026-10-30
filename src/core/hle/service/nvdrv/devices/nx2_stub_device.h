// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "core/hle/service/nvdrv/devices/nvdevice.h"

namespace Service::Nvidia::Devices {

// Set once a Switch 2 NvRm has identified itself (0xD0-byte GetCharacteristics). Devices then
// answer ioctls they don't know with a logged, input-echoing success instead of NotImplemented.
inline std::atomic<bool> g_nx2_driver{false};

inline NvResult Nx2UnknownIoctl(const char* device, Ioctl command, std::span<const u8> input,
                                std::span<u8> output) {
    LOG_WARNING(Service_NVDRV, "(STUBBED) Switch 2 {} ioctl={:08X} group={:01X} command={:02X} in={:#x}",
                device, command.raw, command.group, command.cmd, input.size());
    std::fill(output.begin(), output.end(), u8{0});
    std::copy_n(input.begin(), std::min(input.size(), output.size()), output.begin());
    return NvResult::Success;
}

// GPU mappings a Switch 2 process made through nx2:gpu: CreateGpuAccessibleMemory (sub 101) maps
// the memory at GPU address == CPU address, and NormalMapGpuMemory4K (211) maps a memory handle at
// a chosen GPU address (also in sparse regions). Every nvhost-as-gpu address space applies each
// mapping, including ones made before the address space existed.
class Nx2GpuMemoryRegistry {
public:
    using Listener = std::function<void(u64 gpu_address, u64 cpu_address, u64 size)>;
    using UnmapListener = std::function<void(u64 gpu_address, u64 size)>;
    using ReleaseListener = std::function<void(u64 cpu_address, u64 size)>;
    using KindListener = std::function<bool(u64 gpu_address, u64 size, u8 kind)>;

    static Nx2GpuMemoryRegistry& Get() {
        static Nx2GpuMemoryRegistry registry;
        return registry;
    }

    void Register(u64 address, u64 size) {
        MapAt(address, address, size);
    }

    void MapAt(u64 gpu_address, u64 cpu_address, u64 size) {
        std::scoped_lock lk{mutex};
        TrimMappings(gpu_address, size);
        mappings.push_back({gpu_address, cpu_address, size});
        for (auto& [owner, listener] : listeners) {
            listener.map(gpu_address, cpu_address, size);
        }
    }

    // NormalUnmapGpuMemory4K, or destroying the memory: the GPU range stops reaching it.
    u64 Unmap(u64 gpu_address, u64 size) {
        std::scoped_lock lk{mutex};
        const u64 unmapped = TrimMappings(gpu_address, size);
        for (auto& [owner, listener] : listeners) {
            listener.unmap(gpu_address, size);
        }
        return unmapped;
    }

    // Backing memory [cpu_address, +size) is about to be freed (its GPU mappings are gone): each
    // address space drops its SMMU mapping of it, in GPU-thread order.
    void ReleaseBacking(u64 cpu_address, u64 size) {
        {
            std::scoped_lock lk{device_mutex};
            device_ranges.erase(cpu_address);
        }
        std::scoped_lock lk{mutex};
        for (auto& [owner, listener] : listeners) {
            if (listener.release) {
                listener.release(cpu_address, size);
            }
        }
    }

    void AddListener(const void* owner, Listener listener, UnmapListener unmap_listener,
                      ReleaseListener release_listener = {}, KindListener kind_listener = {}) {
        std::scoped_lock lk{mutex};
        for (const auto& m : mappings) {
            listener(m.gpu_address, m.cpu_address, m.size);
        }
        listeners.insert_or_assign(owner, Listeners{std::move(listener), std::move(unmap_listener),
                                                    std::move(release_listener), std::move(kind_listener)});
    }

    bool ModifyKind(u64 address, u64 size, u8 kind) {
        if (size == 0 || address + size < address || ((address | size) & 0xFFF) != 0 ||
            (kind != 0 && kind != 1 && kind != 3 && kind != 6 && kind != 8)) {
            return false;
        }
        std::scoped_lock lk{mutex};
        const bool mapped = std::any_of(mappings.begin(), mappings.end(), [&](const auto& mapping) {
            return address >= mapping.gpu_address && address - mapping.gpu_address <= mapping.size &&
                   size <= mapping.size - (address - mapping.gpu_address);
        });
        if (!mapped || listeners.empty()) {
            return false;
        }
        for (const auto& [owner, listener] : listeners) {
            if (!listener.kind || !listener.kind(address, size, kind)) {
                return false;
            }
        }
        return true;
    }

    bool ContainsMappingRange(u64 base, u64 offset, u64 size) {
        std::scoped_lock lk{mutex};
        for (const auto& mapping : mappings) {
            if (mapping.gpu_address == base && offset <= mapping.size &&
                size <= mapping.size - offset) {
                return true;
            }
        }
        return false;
    }

    void RemoveListener(const void* owner) {
        std::scoped_lock lk{mutex};
        listeners.erase(owner);
    }

    // Device (SMMU) address of a CPU range an address space mapped; used to present images.
    void RecordDeviceRange(u64 cpu_address, u64 size, u64 device_address) {
        std::scoped_lock lk{device_mutex};
        device_ranges[cpu_address] = {size, device_address};
    }

    // Device address for a CPU range already backed by an address space mapping, if any. Used to
    // keep one identity per backing across the process's several GPU address spaces.
    std::optional<u64> DeviceAddressOfRange(u64 cpu_address, u64 size) {
        std::scoped_lock lk{device_mutex};
        auto it = device_ranges.upper_bound(cpu_address);
        if (it == device_ranges.begin()) {
            return std::nullopt;
        }
        --it;
        const auto& [start, range] = *it;
        const u64 offset = cpu_address - start;
        if (offset > range.first || size > range.first - offset) {
            return std::nullopt;
        }
        return range.second + offset;
    }

    std::optional<u64> DeviceAddressOf(u64 cpu_address) {
        std::scoped_lock lk{device_mutex};
        auto it = device_ranges.upper_bound(cpu_address);
        if (it == device_ranges.begin()) {
            return std::nullopt;
        }
        --it;
        const auto& [start, range] = *it;
        if (cpu_address - start >= range.first) {
            return std::nullopt;
        }
        return range.second + (cpu_address - start);
    }

private:
    // Separate lock: ranges are recorded from listeners that run under `mutex`.
    std::mutex device_mutex;
    std::map<u64, std::pair<u64, u64>> device_ranges; // cpu start -> (size, device address)

    struct Mapping {
        u64 gpu_address;
        u64 cpu_address;
        u64 size;
    };

    struct Listeners {
        Listener map;
        UnmapListener unmap;
        ReleaseListener release;
        KindListener kind;
    };

    // Drops [gpu_address, +size) from the recorded mappings, keeping the parts outside it.
    u64 TrimMappings(u64 gpu_address, u64 size) {
        const u64 end = gpu_address + size;
        u64 unmapped{};
        std::vector<Mapping> kept;
        kept.reserve(mappings.size() + 1);
        for (const auto& m : mappings) {
            const u64 m_end = m.gpu_address + m.size;
            if (m_end <= gpu_address || m.gpu_address >= end) {
                kept.push_back(m);
                continue;
            }
            unmapped += std::min(m_end, end) - std::max(m.gpu_address, gpu_address);
            if (m.gpu_address < gpu_address) {
                kept.push_back({m.gpu_address, m.cpu_address, gpu_address - m.gpu_address});
            }
            if (m_end > end) {
                kept.push_back({end, m.cpu_address + (end - m.gpu_address), m_end - end});
            }
        }
        mappings = std::move(kept);
        return unmapped;
    }

    std::mutex mutex;
    std::vector<Mapping> mappings;
    std::map<const void*, Listeners> listeners;
};

// Device node that Switch 2 nvgpu drivers open but Switch 1 never had (e.g. /dev/nvhost-tsg-gpu).
// Every ioctl succeeds with a zeroed reply and is logged, so the driver's use of it shows up.
class nx2_stub_device final : public nvdevice {
public:
    explicit nx2_stub_device(Core::System& system_, std::string name_)
        : nvdevice{system_}, name{std::move(name_)} {}

    NvResult Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                    std::span<u8> output) override {
        return Stub(fd, command, input, output, {});
    }
    NvResult Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                    std::span<const u8> inline_input, std::span<u8> output) override {
        return Stub(fd, command, input, output, {});
    }
    NvResult Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                    std::span<u8> inline_output) override {
        return Stub(fd, command, input, output, inline_output);
    }

    void OnOpen(NvCore::SessionId session_id, DeviceFD fd) override {
        LOG_WARNING(Service_NVDRV, "(STUBBED) {} opened as fd {}", name, fd);
    }
    void OnClose(DeviceFD fd) override {}

private:
    NvResult Stub(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                  std::span<u8> inline_output) {
        LOG_WARNING(Service_NVDRV, "(STUBBED) {} fd={} ioctl={:08X} group={:01X} command={:02X} in={:#x}",
                    name, fd, command.raw, command.group, command.cmd, input.size());
        // Echo the input (in/out ioctls keep their fields), zero the rest.
        std::fill(output.begin(), output.end(), u8{0});
        std::copy_n(input.begin(), std::min(input.size(), output.size()), output.begin());
        std::fill(inline_output.begin(), inline_output.end(), u8{0});
        return NvResult::Success;
    }

    std::string name;
};

} // namespace Service::Nvidia::Devices

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "core/core.h"
#include "core/file_sys/errors.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"

namespace Service::FileSystem {

IStorage::IStorage(Core::System& system_, FileSys::VirtualFile backend_)
    : ServiceFramework{system_, "IStorage"}, backend(std::move(backend_)) {
    static const FunctionInfo functions[] = {
        {0, D<&IStorage::Read>, "Read"},
        {1, nullptr, "Write"},
        {2, nullptr, "Flush"},
        {3, nullptr, "SetSize"},
        {4, D<&IStorage::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
    };
    RegisterHandlers(functions);
}

Result IStorage::Read(
    OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
    s64 offset, s64 length) {
    LOG_DEBUG(Service_FS, "called, offset={:#x}, length={}", offset, length);

    R_UNLESS(length >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(offset >= 0, FileSys::ResultInvalidOffset);

    // Custom (EDEN_NX2_FS_READ_MBPS=<n>): the emulated storage speed in MB/s for this read
    // throttle. 0 turns it off. Default: 40 MB/s (a Switch 1 card), none for Switch 2 titles.
    // Ocarina of Time streams its opening video with hundreds of 64 KiB reads a second on the
    // thread that also returns decoder images, so at 40 MB/s that thread sleeps most of the time
    // and the video falls seconds behind its audio.
    static const s64 forced_bytes_per_second = [] {
        const char* value = std::getenv("EDEN_NX2_FS_READ_MBPS");
        return value != nullptr ? std::strtoll(value, nullptr, 10) * 1'000'000 : s64{-1};
    }();
    const Kernel::KProcess* const process = system.ApplicationProcess();
    const s64 bytes_per_second =
        forced_bytes_per_second >= 0 ? forced_bytes_per_second
        : process != nullptr && process->IsSwitch2() ? s64{0}
                                                     : ReadBytesPerSecond;
    if (bytes_per_second > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_read_tick) {
            std::this_thread::sleep_for(next_read_tick - now);
        }
        next_read_tick = std::chrono::steady_clock::now() +
                         std::chrono::nanoseconds{std::min(length, MaxChargedReadSize) *
                                                  1'000'000'000 / bytes_per_second};
    }

    // Read the data from the Storage backend
    backend->Read(out_bytes.data(), length, offset);

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem

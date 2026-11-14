// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/nvn_storage_descriptor.h"
#include "video_core/buffer_cache/storage_buffer_size.h"

TEST_CASE("Global fallback identifies NVN buffers by slot", "[shader_recompiler][global_memory]") {
    Shader::Info info{};
    info.nvn_buffer_base = 0x310;
    info.nvn_buffer_used.set(3);
    info.nvn_buffer_used.set(15);
    const Shader::StorageBufferDescriptor slot3{0, 0x340, 1, false};
    const Shader::StorageBufferDescriptor slot15{0, 0x400, 1, false};
    const Shader::StorageBufferDescriptor unused{0, 0x330, 1, false};
    const Shader::StorageBufferDescriptor other_bank{1, 0x340, 1, false};
    const Shader::StorageBufferDescriptor misaligned{0, 0x341, 1, false};
    const Shader::StorageBufferDescriptor out_of_range{0, 0x410, 1, false};
    const Shader::StorageBufferDescriptor before_base{0, 0x300, 1, false};
    REQUIRE(Shader::Backend::UsesNvnStorageDescriptor(info, slot3));
    REQUIRE(Shader::Backend::UsesNvnStorageDescriptor(info, slot15));
    REQUIRE_FALSE(Shader::Backend::UsesNvnStorageDescriptor(info, unused));
    REQUIRE_FALSE(Shader::Backend::UsesNvnStorageDescriptor(info, other_bank));
    REQUIRE_FALSE(Shader::Backend::UsesNvnStorageDescriptor(info, misaligned));
    REQUIRE_FALSE(Shader::Backend::UsesNvnStorageDescriptor(info, out_of_range));
    REQUIRE_FALSE(Shader::Backend::UsesNvnStorageDescriptor(info, before_base));
}

TEST_CASE("Native graphics preserve the declared light-probe buffer size", "[nvn][global_memory]") {
    using VideoCommon::IsNvnGraphicsStorageDescriptor;
    using VideoCommon::StorageBufferSize;
    constexpr u32 probe_size = 0x102edc0;
    constexpr u32 mapping_size = 0x1030000;
    const bool bare = !IsNvnGraphicsStorageDescriptor(0, 0, 0x190, ~0U);
    REQUIRE_FALSE(bare);
    REQUIRE(StorageBufferSize(mapping_size, probe_size, bare, false, false) == probe_size);
    REQUIRE(StorageBufferSize(mapping_size, probe_size, bare, false, true) == probe_size);
    REQUIRE(IsNvnGraphicsStorageDescriptor(4, 0, 0x520, ~0U));
    REQUIRE(IsNvnGraphicsStorageDescriptor(0, 0, 0x200, ~0U));
    REQUIRE_FALSE(IsNvnGraphicsStorageDescriptor(0, 0, 0x100, ~0U));
    REQUIRE_FALSE(IsNvnGraphicsStorageDescriptor(0, 0, 0x210, ~0U));
    REQUIRE_FALSE(IsNvnGraphicsStorageDescriptor(0, 0, 0x191, ~0U));
    REQUIRE_FALSE(IsNvnGraphicsStorageDescriptor(0, 1, 0x190, ~0U));
    REQUIRE_FALSE(IsNvnGraphicsStorageDescriptor(0, 0, 0x190, 0));
    REQUIRE_FALSE(IsNvnGraphicsStorageDescriptor(5, 0, 0x610, ~0U));
}

TEST_CASE("Bare pointers and invalid NVN sizes retain bounded ranges", "[nvn][global_memory]") {
    using VideoCommon::StorageBufferSize;
    constexpr u32 mapping_size = 32 * 1024 * 1024;
    constexpr u32 fallback = 8 * 1024 * 1024;
    REQUIRE(StorageBufferSize(mapping_size, 266, true, false, false) == fallback);
    REQUIRE(StorageBufferSize(mapping_size, 0, false, false, false) == fallback);
    REQUIRE(StorageBufferSize(mapping_size, mapping_size + 1, false, false, false) == fallback);
    REQUIRE(StorageBufferSize(mapping_size, 0x2100000100ULL, false, false, false) == fallback);
    REQUIRE(StorageBufferSize(mapping_size, 256, false, false, false) == 256);
    REQUIRE(StorageBufferSize(mapping_size, 0, true, true, false) == mapping_size);
    REQUIRE(StorageBufferSize(mapping_size, 0, true, true, true) == fallback);
    REQUIRE(StorageBufferSize(4096, 0, true, false, false) == 4096);
}

TEST_CASE("Read-only pointer tables retain the current device range limit", "[nvn][global_memory]") {
    using VideoCommon::StorageBufferSize;
    constexpr u32 MiB = 1024 * 1024;
    CHECK(StorageBufferSize(512 * MiB, 0, true, true, false, 256 * MiB) == 256 * MiB);
    CHECK(StorageBufferSize(512 * MiB, 0, true, true, false, 128 * MiB) == 128 * MiB);
    CHECK(StorageBufferSize(512 * MiB, 0, true, true, true, 256 * MiB) == 8 * MiB);
}

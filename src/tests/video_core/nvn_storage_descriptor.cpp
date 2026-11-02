// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/nvn_storage_descriptor.h"

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

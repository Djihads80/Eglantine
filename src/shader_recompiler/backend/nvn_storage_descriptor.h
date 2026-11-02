// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "shader_recompiler/shader_info.h"

namespace Shader::Backend {

// nvn_buffer_used is indexed by the guest slot, not the position in the compact descriptor list.
[[nodiscard]] inline bool UsesNvnStorageDescriptor(const Info& info,
                                                    const StorageBufferDescriptor& descriptor) {
    constexpr u32 descriptor_size = 0x10;
    if (descriptor.cbuf_index != 0 || descriptor.cbuf_offset < info.nvn_buffer_base) {
        return false;
    }
    const u32 offset = descriptor.cbuf_offset - info.nvn_buffer_base;
    if (offset % descriptor_size != 0) {
        return false;
    }
    const u32 slot = offset / descriptor_size;
    return slot < info.nvn_buffer_used.size() && info.nvn_buffer_used.test(slot);
}

} // namespace Shader::Backend

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include "common/common_types.h"
#include "common/literals.h"

namespace VideoCommon {

[[nodiscard]] constexpr bool IsNvnGraphicsStorageDescriptor(size_t stage, u32 cbuf_index,
                                                            u32 cbuf_offset, u32 indirect_offset) {
    const u32 base = 0x110 + static_cast<u32>(stage) * 0x100;
    return stage < 5 && cbuf_index == 0 && indirect_offset == ~0U && cbuf_offset >= base &&
           cbuf_offset < base + 0x100 && (cbuf_offset - base) % 0x10 == 0;
}

[[nodiscard]] constexpr u32 StorageBufferSize(u32 memory_layout_size, u64 size_word,
                                              bool native_pointer, bool table_pointer,
                                              bool is_written,
                                              u32 read_only_table_limit = 64 * 1024 * 1024) {
    using namespace Common::Literals;
    if (table_pointer && !is_written) {
        return (std::min)(memory_layout_size, read_only_table_limit);
    }
    if (!native_pointer && static_cast<u32>(size_word >> 32) == 0 && size_word != 0 &&
        size_word <= memory_layout_size) {
        return static_cast<u32>(size_word);
    }
    return (std::min)(memory_layout_size, static_cast<u32>(8_MiB));
}

} // namespace VideoCommon

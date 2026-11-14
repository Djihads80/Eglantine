// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/vfs/vfs_vector.h"

namespace {
TEST_CASE("VectorVfsFile reads stop at the end of the data", "[core][vfs]") {
    const FileSys::VectorVfsFile file(std::vector<u8>{1, 2, 3, 4});
    std::array<u8, 8> out{};

    REQUIRE(file.Read(out.data(), out.size(), 2) == 2);
    REQUIRE(out[0] == 3);
    REQUIRE(out[1] == 4);
    REQUIRE(file.Read(out.data(), out.size(), 4) == 0);
    REQUIRE(file.Read(out.data(), out.size(), 100) == 0);

    const FileSys::VectorVfsFile empty;
    REQUIRE(empty.Read(out.data(), out.size(), 0) == 0);
}
} // namespace

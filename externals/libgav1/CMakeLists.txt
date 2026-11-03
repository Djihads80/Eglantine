# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# libgav1 decodes the AV1 movies of Switch 2 titles (nx2 video decoder) where the bundled FFmpeg
# has no libdav1d (Android's package); with libdav1d, FFmpeg decodes them.

include(FetchContent)

set(LIBGAV1_ENABLE_TESTS OFF)
set(LIBGAV1_ENABLE_EXAMPLES OFF)
# std::mutex in its thread pool: no Abseil (it is only needed for absl::Mutex, tests and examples)
set(LIBGAV1_THREADPOOL_USE_STD_MUTEX 1)
# 8 and 10-bit streams only, which leaves the 12-bit DSP code out
set(LIBGAV1_MAX_BITDEPTH 10)
FetchContent_Declare(libgav1
    GIT_REPOSITORY https://chromium.googlesource.com/codecs/libgav1
    GIT_TAG c1deec657b32b911920c78e078cfd089faa77200
)
FetchContent_MakeAvailable(libgav1)

set_property(GLOBAL APPEND PROPERTY CPM_PACKAGE_NAMES libgav1)
set_property(GLOBAL APPEND PROPERTY CPM_PACKAGE_URLS https://chromium.googlesource.com/codecs/libgav1)

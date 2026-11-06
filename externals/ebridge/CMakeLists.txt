# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Eglantine Bridge host library: discovery, pairing, controls in; H.264 video,
# Opus audio and rumble out to the Nintendo Switch 1 app (switch-client).
# Video: FFmpeg's H.264 encoders (x264, NVENC, QSV, AMF) when the FFmpeg has them, and on
# Windows Media Foundation, which also covers the decode-only bundled FFmpeg.

add_library(ebridge STATIC
    src/host.cpp
    src/video_encoder.cpp
    src/video_encoder.h
    include/ebridge/host.h
    common/ebridge_protocol.h
)
target_include_directories(ebridge PUBLIC include common)
target_compile_features(ebridge PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(ebridge PUBLIC Threads::Threads)
if (WIN32)
    target_link_libraries(ebridge PUBLIC ws2_32)
    target_sources(ebridge PRIVATE src/video_encoder_mf.cpp src/video_encoder_mf.h src/nv12.cpp src/nv12.h)
    target_link_libraries(ebridge PRIVATE mfplat mfuuid ole32 oleaut32)
    target_compile_definitions(ebridge PRIVATE EBRIDGE_HAVE_VIDEO)
endif()

# Inside the emulator, use its FFmpeg and Opus; otherwise look for system ones.
find_package(PkgConfig QUIET)

if (DEFINED FFmpeg_LIBRARIES)
    target_include_directories(ebridge PRIVATE ${FFmpeg_INCLUDE_DIR})
    target_link_libraries(ebridge PRIVATE ${FFmpeg_LIBRARIES})
    target_link_options(ebridge PRIVATE ${FFmpeg_LDFLAGS})
    if (TARGET ffmpeg-build)
        add_dependencies(ebridge ffmpeg-build)
    endif()
    target_compile_definitions(ebridge PRIVATE EBRIDGE_HAVE_FFMPEG EBRIDGE_HAVE_VIDEO)
elseif (PKG_CONFIG_FOUND)
    pkg_check_modules(EBRIDGE_FFMPEG IMPORTED_TARGET libavcodec libavutil libswscale)
    if (EBRIDGE_FFMPEG_FOUND)
        target_link_libraries(ebridge PRIVATE PkgConfig::EBRIDGE_FFMPEG)
        target_compile_definitions(ebridge PRIVATE EBRIDGE_HAVE_FFMPEG EBRIDGE_HAVE_VIDEO)
    endif()
endif()
if (NOT WIN32 AND NOT DEFINED FFmpeg_LIBRARIES AND NOT EBRIDGE_FFMPEG_FOUND)
    message(WARNING "Eglantine Bridge: FFmpeg not found, streaming without video")
endif()

if (TARGET Opus::opus)
    target_link_libraries(ebridge PRIVATE Opus::opus)
    target_compile_definitions(ebridge PRIVATE EBRIDGE_HAVE_OPUS)
elseif (PKG_CONFIG_FOUND)
    pkg_check_modules(EBRIDGE_OPUS IMPORTED_TARGET opus)
    if (EBRIDGE_OPUS_FOUND)
        target_link_libraries(ebridge PRIVATE PkgConfig::EBRIDGE_OPUS)
        target_compile_definitions(ebridge PRIVATE EBRIDGE_HAVE_OPUS)
    endif()
endif()
if (NOT TARGET Opus::opus AND NOT EBRIDGE_OPUS_FOUND)
    message(WARNING "Eglantine Bridge: Opus not found, streaming without audio")
endif()

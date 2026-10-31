# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/fibers.cpp
    common/host_memory.cpp
    common/host_memory_fallback.cpp
    common/param_package.cpp
    common/range_map.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/undefined_fix.cpp
    common/unique_function.cpp
    core/core_timing.cpp
    core/address_space.cpp
    core/page_heap.cpp
    core/read_traps.cpp
    core/nso.cpp
    core/nrr.cpp
    core/ncz_decompressor.cpp
    core/internal_network/network.cpp
    video_core/memory_tracker.cpp
    video_core/memory_budget.cpp
    video_core/astc_recompression_override.cpp
    video_core/nx2_video_configuration.cpp
    video_core/nx2_decoder_image.cpp
    video_core/nx2_present.cpp
    video_core/nx2_gpu_mappings.cpp
    video_core/nx2_sparse_mappings.cpp
    video_core/nx2_sparse_region.cpp
    video_core/nx2_command_sync.cpp
    video_core/physical_alias.cpp
    video_core/ampere_returns.cpp
    video_core/ampere_f2i.cpp
    video_core/compute_storage_buffers.cpp
    video_core/ampere_kirby.cpp
    video_core/ampere_geometry.cpp
    video_core/ampere_geometry_lowering.cpp
    video_core/ampere_geometry_exit.cpp
    video_core/ampere_uniform_lea.cpp
    video_core/ampere_bindless_tex.cpp
    video_core/ampere_release_guards.cpp
    video_core/ampere_release_f2i.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

if (WIN32)
    target_sources(tests PRIVATE common/host_memory_commit.cpp)
endif()

target_link_libraries(tests PRIVATE common core input_common video_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

add_test(NAME tests COMMAND tests)

# needed for vma
if (NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()

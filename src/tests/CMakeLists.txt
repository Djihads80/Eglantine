# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    core/nx2_oot_time.cpp
    video_core/unbacked_image_policy.cpp
    core/builtin_patches.cpp
    core/gpu_dirty_memory_manager.cpp
    video_core/macro_dma_forward.cpp
    video_core/gpu_timestamp.cpp
    video_core/maxwell_fast_method.cpp
    video_core/split_write_tracker.cpp
    video_core/nx2_written_pages.cpp
    video_core/frame_dump_schedule.cpp
    core/nx2_debug_server.cpp
    core/system_test_context.h
    video_core/ampere_logged_numeric.cpp
    video_core/pipeline_optimization.cpp
    video_core/spirv_atomic_retry.cpp
    video_core/spirv_storage_alignment.cpp
    video_core/spirv_fragment_shuffle.cpp
    video_core/mesh_fallback.cpp
    video_core/spirv_software_matrix.cpp
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/event.cpp
    common/fibers.cpp
    common/game_presets.cpp
    common/host_memory.cpp
    common/page_table.cpp
    common/param_package.cpp
    common/range_map.cpp
    common/range_set_subtract.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/slot_vector.cpp
    common/undefined_fix.cpp
    common/unique_function.cpp
    common/zstd_compression.cpp
    core/core_timing.cpp
    core/ldn.cpp
    core/internal_network/socket_proxy.cpp
    core/nup_writer.cpp
    core/nx2_debug_server.cpp
    core/nx2_movie_pacing.cpp
    core/nx2_gpu_memory_registry.cpp
    core/priority_queue.cpp
    core/internal_network/network.cpp
    video_core/frame_dump_schedule.cpp
    video_core/memory_tracker.cpp
    video_core/nx2_perf.cpp
    video_core/nx2_perf_draw_binds.cpp
    video_core/nx2_perf_texture_writes.cpp
    video_core/nx2_perf_native_qmd.cpp
    video_core/nx2_perf_query_callback.cpp
    video_core/nx2_perf_switches.cpp
    video_core/memory_manager_runs.cpp
    video_core/nx2_sparse_residency.cpp
    video_core/spirv_storage_alignment.cpp
    video_core/pipeline_optimization.cpp
    video_core/spirv_atomic_retry.cpp
    video_core/spirv_fragment_shuffle.cpp
    video_core/ampere_depth_compare.cpp
    video_core/h264_packet.cpp
    video_core/vertex_repack.cpp
    video_core/virtual_range_cache.cpp
    video_core/ampere_lowering.cpp
    video_core/spirv_software_matrix.cpp
    video_core/ampere_geometry.cpp
    video_core/ampere_i2f.cpp
    video_core/ampere_gradient.cpp
    video_core/ampere_returns.cpp
    video_core/ampere_guarded_ulea.cpp
    video_core/ampere_compute_lowering.cpp
    video_core/native_macro_fifo.cpp
    video_core/uniform_stream_reuse.cpp
    video_core/shader_invalidation.cpp
    video_core/descriptor_table.cpp
    video_core/global_address_tracking.cpp
    video_core/global_atomic_routing.cpp
    video_core/nvn_storage_descriptor.cpp
    video_core/ampere_f2i.cpp
    video_core/ampere_ldc.cpp
    video_core/ampere_vote.cpp
    video_core/shader_attributes.cpp
    video_core/indirect_reset.cpp
    video_core/ocarina_upscale.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

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

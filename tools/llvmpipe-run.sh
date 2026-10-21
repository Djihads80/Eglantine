#!/bin/sh -e

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# This script basically allows you to "dirtily" use llvmpipe in any configuration
# llvmpipe will of course, run at negative mach-1 speeds. However this is mainly useful to test that mesa
# is properly working on extraneous systems (such as Managarm). This mainly only works with MESA >22.0.0
# Use as follows: ./llvmpipe-run.sh <command>

export MESA_GL_VERSION_OVERRIDE=4.6
export MESA_GLSL_VERSION_OVERRIDE=460
export MESA_EXTENSION_MAX_YEAR=2025
export MESA_DEBUG=1
export MESA_VK_VERSION_OVERRIDE=1.3
export LIBGL_ALWAYS_SOFTWARE=1

# This was automated via a script so I might have missed some
ext="GL_AMD_seamless_cubemap_per_texture"
ext="$ext GL_ARB_bindless_texture GL_ARB_derivative_control GL_ARB_direct_state_access GL_ARB_draw_buffers"
ext="$ext GL_ARB_draw_buffers_blend GL_ARB_draw_instanced GL_ARB_fragment_layer_viewport GL_ARB_fragment_program"
ext="$ext GL_ARB_geometry_shader4 GL_ARB_gl_spirv"
ext="$ext GL_ARB_gpu_shader_fp64 GL_ARB_gpu_shader_int64 GL_ARB_imaging"
ext="$ext GL_ARB_indirect_parameters GL_ARB_instanced_arrays GL_ARB_multisample"
ext="$ext GL_ARB_parallel_shader_compile GL_ARB_post_depth_coverage"
ext="$ext GL_ARB_robustness GL_ARB_sample_locations"
ext="$ext GL_ARB_sample_shading GL_ARB_seamless_cube_map GL_ARB_seamless_cubemap_per_texture GL_ARB_separate_shader_objects"
ext="$ext GL_ARB_shader_ballot GL_ARB_shader_group_vote GL_ARB_shader_objects"
ext="$ext GL_ARB_shader_viewport_layer_array GL_ARB_shading_language_420pack"
ext="$ext GL_ARB_shading_language_include GL_ARB_sparse_buffer GL_ARB_sparse_texture"
ext="$ext GL_ARB_sparse_texture2 GL_ARB_spirv_extensions GL_ARB_tessellation_shader"
ext="$ext GL_ARB_texture_compression GL_ARB_texture_compression_bptc GL_ARB_texture_compression_rgtc"
ext="$ext GL_ARB_texture_filter_anisotropic GL_ARB_texture_filter_minmax"
ext="$ext GL_ARB_texture_swizzle GL_ARB_transform_feedback3"
ext="$ext GL_EXT_direct_state_access GL_EXT_framebuffer_sRGB"
ext="$ext GL_EXT_multiview_texture_multisample GL_EXT_multiview_timer_query"
ext="$ext GL_EXT_shader_image_load_formatted GL_EXT_sparse_texture2"
ext="$ext GL_EXT_texture_array GL_EXT_texture_compression_s3tc"
ext="$ext GL_EXT_texture_filter_anisotropic GL_EXT_texture_filter_minmax GL_EXT_texture_mirror_clamp"
ext="$ext GL_KHR_blend_equation_advanced GL_KHR_blend_equation_advanced_coherent"
ext="$ext GL_KHR_debug GL_KHR_no_error GL_KHR_parallel_shader_compile"
ext="$ext GL_KHR_robustness GL_KHR_shader_subgroup GL_NVX_gpu_memory_info"
ext="$ext GL_NV_bindless_multi_draw_indirect GL_NV_bindless_multi_draw_indirect_count GL_NV_bindless_texture"
ext="$ext GL_NV_blend_equation_advanced GL_NV_blend_minmax_factor"
ext="$ext GL_NV_blend_square GL_NV_command_list GL_NV_compute_program5"
ext="$ext GL_NV_conservative_raster GL_NV_conservative_raster_dilate"
ext="$ext GL_NV_conservative_raster_pre_snap_triangles GL_NV_copy_depth_to_color"
ext="$ext GL_NV_depth_buffer_float GL_NV_depth_clamp GL_NV_draw_texture"
ext="$ext GL_NV_explicit_multisample GL_NV_feature_query GL_NV_fill_rectangle"
ext="$ext GL_NV_fragment_coverage_to_color GL_NV_fragment_program"
ext="$ext GL_NV_fragment_program2 GL_NV_fragment_program_option GL_NV_fragment_shader_interlock"
ext="$ext GL_NV_framebuffer_multisample_coverage GL_NV_geometry_program4 GL_NV_geometry_shader4"
ext="$ext GL_NV_geometry_shader_passthrough GL_NV_gpu_program4 GL_NV_gpu_program4_1"
ext="$ext GL_NV_gpu_program5 GL_NV_gpu_program5_mem_extended GL_NV_gpu_program_fp64 GL_NV_gpu_program_multiview"
ext="$ext GL_NV_gpu_shader5 GL_NV_half_float GL_NV_internalformat_sample_query"
ext="$ext GL_NV_memory_attachment GL_NV_multisample_coverage GL_NV_multisample_filter_hint"
ext="$ext GL_NV_packed_depth_stencil GL_NV_parameter_buffer_object GL_NV_sample_locations"
ext="$ext GL_NV_sample_mask_override_coverage GL_NV_shader_atomic_counters GL_NV_shader_atomic_float"
ext="$ext GL_NV_shader_atomic_float64 GL_NV_shader_atomic_fp16_vector GL_NV_shader_atomic_int64"
ext="$ext GL_NV_shader_buffer_load GL_NV_shader_storage_buffer_object"
ext="$ext GL_NV_shader_thread_group GL_NV_shader_thread_shuffle"
ext="$ext GL_NV_texture_barrier GL_NV_texture_compression_vtc GL_NV_texture_env_combine4 GL_NV_texture_multisample"
ext="$ext GL_NV_texture_rectangle GL_NV_texture_rectangle_compressed GL_NV_texture_shader GL_NV_texture_shader2"
ext="$ext GL_NV_texture_shader3 GL_NV_timeline_semaphore GL_NV_transform_feedback GL_NV_transform_feedback2"
ext="$ext GL_NV_uniform_buffer_unified_memory"
ext="$ext GL_NV_vertex_array_range GL_NV_vertex_array_range2 GL_NV_vertex_attrib_integer_64bit"
ext="$ext GL_NV_vertex_buffer_unified_memory GL_NV_vertex_program GL_NV_vertex_program1_1 GL_NV_vertex_program2"
ext="$ext GL_NV_vertex_program2_option GL_NV_vertex_program3 GL_NV_viewport_array2 GL_NV_viewport_swizzle"
ext="$ext GL_AMD_gpu_shader_half_float GL_AMD_gpu_shader_int64 GL_AMD_seamless_cubemap_per_texture"
ext="$ext GL_NV_copy_buffer GL_NV_draw_buffers GL_NV_draw_instanced"
ext="$ext GL_NV_framebuffer_blit GL_NV_framebuffer_multisample"
ext="$ext GL_NV_geometry_shader_passthrough GL_NV_gpu_shader5 GL_NV_image_formats GL_NV_instanced_arrays"
ext="$ext GL_NV_internalformat_sample_query GL_NV_memory_attachment GL_NV_texture_array"
ext="$ext GL_NV_viewport_array GL_NV_viewport_array2 GL_NV_viewport_swizzle"

export MESA_EXTENSION_OVERRIDE="$ext"
"$@"
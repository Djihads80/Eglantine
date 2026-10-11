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
ext="$ext GL_ARB_arrays_of_arrays"
ext="$ext GL_ARB_bindless_texture GL_ARB_buffer_storage GL_ARB_clear_texture"
ext="$ext GL_ARB_color_buffer_float GL_ARB_compute_variable_group_size"
ext="$ext GL_ARB_copy_buffer GL_ARB_cull_distance GL_ARB_debug_output"
ext="$ext GL_ARB_depth_buffer_float GL_ARB_depth_clamp GL_ARB_depth_texture GL_ARB_derivative_control GL_ARB_draw_buffers"
ext="$ext GL_ARB_draw_buffers_blend GL_ARB_draw_instanced GL_ARB_fragment_layer_viewport"
ext="$ext GL_ARB_fragment_program GL_ARB_fragment_program_shadow GL_ARB_fragment_shader GL_ARB_fragment_shader_interlock"
ext="$ext GL_ARB_geometry_shader4 GL_ARB_gl_spirv"
ext="$ext GL_ARB_gpu_shader5 GL_ARB_gpu_shader_fp64 GL_ARB_gpu_shader_int64 GL_ARB_half_float_pixel GL_ARB_half_float_vertex"
ext="$ext GL_ARB_indirect_parameters GL_ARB_instanced_arrays GL_ARB_internalformat_query GL_ARB_internalformat_query2"
ext="$ext GL_ARB_map_buffer_alignment GL_ARB_multi_bind GL_ARB_multi_draw_indirect GL_ARB_multisample GL_ARB_multitexture"
ext="$ext GL_ARB_occlusion_query GL_ARB_occlusion_query2 GL_ARB_parallel_shader_compile"
ext="$ext GL_ARB_point_parameters GL_ARB_post_depth_coverage GL_ARB_program_interface_query"
ext="$ext GL_ARB_provoking_vertex GL_ARB_query_buffer_object GL_ARB_sample_locations"
ext="$ext GL_ARB_sample_shading GL_ARB_seamless_cube_map GL_ARB_seamless_cubemap_per_texture GL_ARB_separate_shader_objects"
ext="$ext GL_ARB_shader_atomic_counters GL_ARB_shader_ballot GL_ARB_shader_clock"
ext="$ext GL_ARB_shader_draw_parameters GL_ARB_shader_group_vote GL_ARB_shader_image_load_store GL_ARB_shader_objects"
ext="$ext GL_ARB_shader_precision GL_ARB_shader_storage_buffer_object GL_ARB_shader_subroutine GL_ARB_shader_texture_image_samples"
ext="$ext GL_ARB_shader_texture_lod GL_ARB_shader_viewport_layer_array GL_ARB_shading_language_420pack"
ext="$ext GL_ARB_shading_language_include GL_ARB_shading_language_packing GL_ARB_sparse_buffer GL_ARB_sparse_texture GL_ARB_sparse_texture2"
ext="$ext GL_ARB_spirv_extensions GL_ARB_stencil_texturing GL_ARB_tessellation_shader GL_ARB_texture_barrier"
ext="$ext GL_ARB_texture_border_clamp GL_ARB_texture_compression"
ext="$ext GL_ARB_texture_compression_bptc GL_ARB_texture_compression_rgtc GL_ARB_texture_cube_map GL_ARB_texture_cube_map_array GL_ARB_texture_env_add"
ext="$ext GL_ARB_texture_env_combine GL_ARB_texture_env_crossbar GL_ARB_texture_env_dot3 GL_ARB_texture_filter_anisotropic GL_ARB_texture_filter_minmax"
ext="$ext GL_ARB_texture_float GL_ARB_texture_mirrored_repeat GL_ARB_texture_multisample"
ext="$ext GL_ARB_texture_rectangle"
ext="$ext GL_ARB_texture_stencil8 GL_ARB_texture_storage GL_ARB_texture_swizzle"
ext="$ext GL_ARB_transform_feedback2 GL_ARB_transform_feedback3 GL_ARB_transform_feedback_instanced"
ext="$ext GL_ARB_uniform_buffer_object GL_ARB_vertex_array_bgra"
ext="$ext GL_ARB_vertex_array_object GL_ARB_vertex_buffer_object"
ext="$ext GL_ARB_vertex_program GL_ARB_vertex_shader"
ext="$ext GL_ARB_viewport_array GL_ARB_window_pos GL_EXT_blend_minmax"
ext="$ext GL_EXT_geometry_shader4 GL_EXT_gpu_shader4"
ext="$ext GL_EXT_multiview_texture_multisample GL_EXT_multiview_timer_query"
ext="$ext GL_EXT_shader_image_load_formatted"
ext="$ext GL_EXT_shader_image_load_store GL_EXT_sparse_texture2 GL_EXT_texture_buffer_object"
ext="$ext GL_EXT_texture_compression_rgtc GL_EXT_texture_compression_s3tc GL_EXT_texture_edge_clamp"
ext="$ext GL_EXT_texture_filter_anisotropic GL_EXT_texture_filter_minmax"
ext="$ext GL_EXT_texture_lod GL_EXT_texture_mirror_clamp"
ext="$ext GL_EXT_texture_storage GL_EXT_transform_feedback2"
ext="$ext GL_KHR_blend_equation_advanced GL_KHR_blend_equation_advanced_coherent"
ext="$ext GL_KHR_debug GL_KHR_no_error GL_KHR_parallel_shader_compile"
ext="$ext GL_KHR_robustness GL_KHR_shader_subgroup GL_KTX_buffer_region GL_NVX_gpu_memory_info"
ext="$ext GL_NV_compute_program5 GL_NV_conservative_raster_dilate GL_NV_copy_depth_to_color"
ext="$ext GL_NV_depth_buffer_float GL_NV_fragment_program"
ext="$ext GL_NV_fragment_program2 GL_NV_fragment_program_option GL_NV_fragment_shader_interlock"
ext="$ext GL_NV_geometry_shader4"
ext="$ext GL_NV_geometry_shader_passthrough GL_NV_gpu_program4 GL_NV_gpu_program4_1"
ext="$ext GL_NV_gpu_program5 GL_NV_gpu_program5_mem_extended GL_NV_gpu_program_fp64 GL_NV_gpu_program_multiview"
ext="$ext GL_NV_gpu_shader5 GL_NV_half_float GL_NV_parameter_buffer_object"
ext="$ext GL_NV_shader_atomic_float"
ext="$ext GL_NV_shader_atomic_float64 GL_NV_shader_atomic_fp16_vector GL_NV_shader_atomic_int64"
ext="$ext GL_NV_shader_buffer_load GL_NV_shader_storage_buffer_object"
ext="$ext GL_NV_shader_thread_group GL_NV_shader_thread_shuffle GL_NV_texture_multisample"
ext="$ext GL_NV_transform_feedback GL_NV_transform_feedback2"
ext="$ext GL_NV_vertex_buffer_unified_memory GL_NV_vertex_program1_1 GL_NV_vertex_program2"
ext="$ext GL_NV_vertex_program2_option GL_NV_vertex_program3 GL_NV_viewport_array2 GL_NV_viewport_swizzle"
ext="$ext GL_AMD_seamless_cubemap_per_texture"
ext="$ext GL_EXT_blend_minmax GL_EXT_buffer_storage GL_EXT_clear_texture"
ext="$ext GL_EXT_clip_control GL_EXT_clip_cull_distance GL_EXT_color_buffer_float"
ext="$ext GL_EXT_conservative_depth GL_EXT_copy_image GL_EXT_debug_label"
ext="$ext GL_EXT_geometry_shader GL_EXT_multi_draw_indirect"
ext="$ext GL_EXT_shader_io_blocks GL_EXT_shader_non_constant_global_initializers"
ext="$ext GL_EXT_shader_texture_lod GL_EXT_shadow_samplers GL_EXT_sparse_texture"
ext="$ext GL_EXT_tessellation_shader GL_EXT_texture_buffer"
ext="$ext GL_EXT_texture_filter_minmax GL_EXT_texture_storage"
ext="$ext GL_KHR_robust_buffer_access_behavior GL_KHR_shader_subgroup"
ext="$ext GL_NV_bindless_texture GL_NV_blend_equation_advanced GL_NV_blend_equation_advanced_coherent"
ext="$ext GL_NV_blend_minmax_factor GL_NV_conditional_render GL_NV_conservative_raster"
ext="$ext GL_NV_conservative_raster_pre_snap_triangles GL_NV_copy_buffer GL_NV_draw_buffers"
ext="$ext GL_NV_draw_texture GL_NV_fill_rectangle"
ext="$ext GL_NV_framebuffer_blit GL_NV_image_formats GL_NV_instanced_arrays"
ext="$ext GL_NV_packed_float GL_NV_packed_float_linear GL_NV_texture_array GL_NV_viewport_array"

export MESA_EXTENSION_OVERRIDE="$ext"
"$@"
d3d9_shaders = files([
  'shaders/d3d9_convert_yuy2_uyvy.comp',
  'shaders/d3d9_convert_l6v5u5.comp',
  'shaders/d3d9_convert_x8l8v8u8.comp',
  'shaders/d3d9_convert_a2w10v10u10.comp',
  'shaders/d3d9_convert_w11v11u10.comp',
  'shaders/d3d9_convert_nv12.comp',
  'shaders/d3d9_convert_yv12.comp',
])

# WC3 Android/Adreno: the precompiled fixed-function shaders must be SPIR-V
# 1.3.  The Adreno 725 driver 0.786.0 rejects the SPIR-V 1.6 variants at
# pipeline creation, while the 1.10.3 Vulkan 1.1 shaders work on the same
# device.  Use a dedicated generator so only the fixed-function shaders are
# lowered to vulkan1.1; the conversion shaders keep the global target env.
# The WC3 Android build consumes pre-generated headers that add the
# SPIRV_VERSION macro asserted in d3d9_fixed_function.cpp.  A plain
# glslangValidator regeneration lacks that macro and fails the assertion on
# purpose.
glsl_args_ff = []
foreach arg : glsl_args
  if arg == 'vulkan1.3'
    glsl_args_ff += 'vulkan1.1'
  else
    glsl_args_ff += arg
  endif
endforeach

glsl_generator_ff = generator(
  glsl_compiler,
  output    : [ '@BASENAME@.h' ],
  depfile   : '@BASENAME@.h.d',
  arguments : glsl_args_ff,
)

d3d9_ff_shaders = files([
  'shaders/d3d9_fixed_function_vert.vert',
  'shaders/d3d9_fixed_function_frag.frag',
  'shaders/d3d9_fixed_function_frag_sample.frag',
])

d3d9_src = [
  'd3d9_main.cpp',
  'd3d9_interface.cpp',
  'd3d9_adapter.cpp',
  'd3d9_monitor.cpp',
  'd3d9_device.cpp',
  'd3d9_state.cpp',
  'd3d9_cursor.cpp',
  'd3d9_swapchain.cpp',
  'd3d9_format.cpp',
  'd3d9_common_texture.cpp',
  'd3d9_constant_buffer.cpp',
  'd3d9_constant_copy.cpp',
  'd3d9_texture.cpp',
  'd3d9_surface.cpp',
  'd3d9_volume.cpp',
  'd3d9_common_buffer.cpp',
  'd3d9_buffer.cpp',
  'd3d9_shader.cpp',
  'd3d9_vertex_declaration.cpp',
  'd3d9_query.cpp',
  'd3d9_shader_validator.cpp',
  'd3d9_options.cpp',
  'd3d9_stateblock.cpp',
  'd3d9_util.cpp',
  'd3d9_initializer.cpp',
  'd3d9_fixed_function.cpp',
  'd3d9_names.cpp',
  'd3d9_swvp_emu.cpp',
  'd3d9_format_helpers.cpp',
  'd3d9_hud.cpp',
  'd3d9_annotation.cpp',
  'd3d9_window.cpp',
  'd3d9_interop.cpp',
  'd3d9_on_12.cpp',
  'd3d9_bridge.cpp',
  'd3d9_shader_analysis.cpp',
]

# WC3 recompilation: build d3d9 as a static archive.  The host links this
# archive and the DXVK core archives directly into wc3_host27.exe, so no
# d3d9.dll / DXVK DLL is loaded at run time.
d3d9_lib = static_library(dxvk_name_prefix+'d3d9', d3d9_src,
  glsl_generator.process(d3d9_shaders),
  glsl_generator_ff.process(d3d9_ff_shaders),
  dependencies        : [ dxbc_spirv_dep, dxvk_dep ],
  include_directories : dxvk_include_path,
  install             : false,
)

d3d9_dep = declare_dependency(
  link_with           : [ d3d9_lib ],
  include_directories : [ dxvk_include_path ],
)

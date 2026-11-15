# Compiles GLSL to SPIR-V with glslc (shaderc) or glslangValidator.

find_program(HAILSTONE_GLSLC glslc)
find_program(HAILSTONE_GLSLANG NAMES glslangValidator glslang)
if(NOT HAILSTONE_GLSLC AND NOT HAILSTONE_GLSLANG)
  message(FATAL_ERROR "Need glslc (shaderc) or glslangValidator to compile shaders")
endif()

set(HAILSTONE_SHADER_OUT ${PROJECT_BINARY_DIR}/shaders)
# The run folder's copy (build/hailstone/shaders, where the executable looks): each shader is copied there when it
# is compiled, so a change to a shader alone reaches it without the executable being relinked.
set(HAILSTONE_SHADER_RUN ${PROJECT_BINARY_DIR}/hailstone/shaders)
# Files the shaders #include (GL_GOOGLE_include_directive): a change rebuilds every shader.
set(HAILSTONE_SHADER_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/lighting.glsl
                          ${CMAKE_CURRENT_SOURCE_DIR}/shaders/zone_surface.glsl
                          ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ao_kernel.glsl
                          ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ao_blur.glsl
                          ${CMAKE_CURRENT_SOURCE_DIR}/shaders/motion.glsl
                          ${CMAKE_CURRENT_SOURCE_DIR}/shaders/reactive.glsl
                          ${CMAKE_CURRENT_SOURCE_DIR}/shaders/coverage.glsl
                          ${CMAKE_CURRENT_SOURCE_DIR}/shaders/weather.glsl
                          ${CMAKE_CURRENT_SOURCE_DIR}/shaders/puddle_cover.glsl
                          ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr1/ffx_a.h
                          ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr1/ffx_fsr1.h)
# AMD's FSR 1 headers (fsr_easu.frag, fsr_rcas.frag).
set(HAILSTONE_SHADER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr1)

function(hailstone_add_shaders target)
  set(outputs)
  foreach(src ${ARGN})
    get_filename_component(name ${src} NAME)
    set(out ${HAILSTONE_SHADER_OUT}/${name}.spv)
    if(HAILSTONE_GLSLC)
      set(cmd ${HAILSTONE_GLSLC} --target-env=vulkan1.3 -O -I ${HAILSTONE_SHADER_INCLUDE_DIR} -o ${out} ${CMAKE_CURRENT_SOURCE_DIR}/${src})
    else()
      set(cmd ${HAILSTONE_GLSLANG} -V --target-env vulkan1.3 -I${HAILSTONE_SHADER_INCLUDE_DIR} -o ${out} ${CMAKE_CURRENT_SOURCE_DIR}/${src})
    endif()
    add_custom_command(OUTPUT ${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${HAILSTONE_SHADER_OUT}
      COMMAND ${cmd}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${HAILSTONE_SHADER_RUN}
      COMMAND ${CMAKE_COMMAND} -E copy ${out} ${HAILSTONE_SHADER_RUN}/${name}.spv
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src} ${HAILSTONE_SHADER_INCLUDES}
      COMMENT "Compiling shader ${name}"
      VERBATIM)
    list(APPEND outputs ${out})
  endforeach()
  add_custom_target(${target} DEPENDS ${outputs})
endfunction()

# AMD FSR 3.1's upscaler passes (third_party/fidelityfx, compute): each pass compiled once in the permutation
# Hailstone uses (render/fsr3_upscaler.cpp): HDR colour, reverse depth, motion vectors at the render size and
# without jitter, the reference Lanczos, 32-bit maths; the accumulate pass twice, with and without sharpening.
# Output as build/hailstone/shaders/fsr3_<pass>.spv.
set(HAILSTONE_FSR3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx)
file(GLOB HAILSTONE_FSR3_HEADERS ${HAILSTONE_FSR3_DIR}/gpu/*.h ${HAILSTONE_FSR3_DIR}/gpu/*/*.h)
function(hailstone_add_fsr3_shaders target)
  set(defines -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_HALF=0
              -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
              -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
              -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=0
              -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
              -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
              -DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0
              -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=1
              -DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1
              -DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0
              -DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=1)
  set(outputs)
  # pass name, then the sharpening option
  set(passes prepare_inputs:0 luma_pyramid:0 shading_change_pyramid:0 shading_change:0 prepare_reactivity:0
             luma_instability:0 accumulate:0 accumulate:1 rcas:0)
  foreach(entry ${passes})
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 pass)
    list(GET parts 1 sharpen)
    set(name fsr3_${pass})
    if(sharpen)
      set(name fsr3_${pass}_sharpen)
    endif()
    set(src ${HAILSTONE_FSR3_DIR}/shaders/ffx_fsr3upscaler_${pass}_pass.glsl)
    set(out ${HAILSTONE_SHADER_OUT}/${name}.spv)
    if(HAILSTONE_GLSLC)
      set(cmd ${HAILSTONE_GLSLC} -fshader-stage=comp --target-env=vulkan1.3 -O ${defines}
              -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=${sharpen}
              -I ${HAILSTONE_FSR3_DIR}/gpu -I ${HAILSTONE_FSR3_DIR}/gpu/fsr3upscaler -o ${out} ${src})
    else()
      set(cmd ${HAILSTONE_GLSLANG} -V -S comp --target-env vulkan1.3 ${defines}
              -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=${sharpen}
              -I${HAILSTONE_FSR3_DIR}/gpu -I${HAILSTONE_FSR3_DIR}/gpu/fsr3upscaler -o ${out} ${src})
    endif()
    add_custom_command(OUTPUT ${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${HAILSTONE_SHADER_OUT}
      COMMAND ${cmd}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${HAILSTONE_SHADER_RUN}
      COMMAND ${CMAKE_COMMAND} -E copy ${out} ${HAILSTONE_SHADER_RUN}/${name}.spv
      DEPENDS ${src} ${HAILSTONE_FSR3_HEADERS}
      COMMENT "Compiling shader ${name}"
      VERBATIM)
    list(APPEND outputs ${out})
  endforeach()
  add_custom_target(${target} DEPENDS ${outputs})
endfunction()

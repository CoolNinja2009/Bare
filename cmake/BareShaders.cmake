# bare_add_shaders(): compiles GLSL fragment shaders for canvas passes
# (bare_pass), and programs for draws (bare_draw), into a C header, at build time.
#
#   bare_add_shaders(myapp OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/generated/my_shaders.h
#                    SOURCES shaders/blur.glsl shaders/glow.glsl)
#
# The header defines `static const BareShaderCode blur_shader, glow_shader;`
# (file name + _shader), each holding SPIR-V, DXIL and MSL plus how many inputs
# and params it takes. Use them with bare_shader(&blur_shader).
#
# Writing a shader (GLSL 450, fragment stage):
#
#   #version 450
#   layout(location = 0) in vec2 v_uv;            // 0..1 across the target, (0,0) top-left
#   layout(location = 0) out vec4 o_color;
#   layout(set = 2, binding = 0) uniform sampler2D u_source;    // .inputs[0], [1], ...
#   layout(set = 3, binding = 0) uniform Params {               // .params (std140 layout)
#       vec2 texel; float strength;
#   };
#   void main() { o_color = texture(u_source, v_uv) * strength; }
#
# Matching C struct for std140: floats and vec2s pack as in C, but a vec3 or
# vec4 starts on a 16-byte boundary; use float[4] for them and pad by hand.
#
# A program (for bare_draw) is one file with both halves, each compiled with
# BARE_VERTEX or BARE_FRAGMENT defined; BARE_PARAMS_SET is the params' set in
# either. Vertex attributes are vec4 slots (declare float..vec4). The vertex
# half may read .inputs too, at set 0 (binding = the input's index).
#
# Shared code: `#extension GL_GOOGLE_include_directive : require` and
# `#include "common.glsl"` (beside the shader; don't list it in SOURCES).
#
#   #version 450
#   layout(set = BARE_PARAMS_SET, binding = 0) uniform Params { vec4 tint; };
#   #ifdef BARE_VERTEX
#   layout(location = 0) in vec2 a_pos;              // per vertex
#   layout(location = 1) in vec4 a_offset;           // per instance (after the vertex's)
#   layout(location = 0) out vec2 v_local;
#   void main() { v_local = a_pos; gl_Position = vec4(a_pos * 0.1 + a_offset.xy, 0.0, 1.0); }
#   #else
#   layout(location = 0) in vec2 v_local;
#   layout(location = 0) out vec4 o_color;
#   void main() { o_color = tint * (1.0 - length(v_local)); }
#   #endif
#
# Needs glslc (Vulkan SDK) and SDL_shadercross. Without them the header that's
# already there is used, so commit it and others can build without the tools.

set(BARE_SHADER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/bare_shader_header.cmake CACHE INTERNAL "")

function(bare_add_shaders target)
    cmake_parse_arguments(ARG "" "OUTPUT" "SOURCES" ${ARGN})
    if(NOT ARG_OUTPUT OR NOT ARG_SOURCES)
        message(FATAL_ERROR "bare_add_shaders(${target} OUTPUT <header> SOURCES <files...>)")
    endif()
    get_filename_component(out_dir ${ARG_OUTPUT} DIRECTORY)
    target_include_directories(${target} PRIVATE ${out_dir})

    set(sources "")
    set(deps "")
    foreach(src ${ARG_SOURCES})
        get_filename_component(abs ${src} ABSOLUTE)
        list(APPEND sources ${abs})
        get_filename_component(dir ${abs} DIRECTORY)
        file(GLOB near ${dir}/*.glsl)   # files they #include
        list(APPEND deps ${near})
    endforeach()
    list(REMOVE_DUPLICATES deps)

    if(BARE_GLSLC AND BARE_SHADERCROSS)
        string(REPLACE ";" "|" packed "${sources}")
        add_custom_command(
            OUTPUT ${ARG_OUTPUT}
            COMMAND ${CMAKE_COMMAND} -DGLSLC=${BARE_GLSLC} -DSHADERCROSS=${BARE_SHADERCROSS}
                    "-DSOURCES=${packed}" -DOUT=${ARG_OUTPUT} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/${target}_shaders
                    -P ${BARE_SHADER_SCRIPT}
            DEPENDS ${sources} ${deps} ${BARE_SHADER_SCRIPT}
            COMMENT "Compiling ${target} shaders"
            VERBATIM)
        add_custom_target(${target}_shaders DEPENDS ${ARG_OUTPUT})
        add_dependencies(${target} ${target}_shaders)
    elseif(EXISTS ${ARG_OUTPUT})
        message(STATUS "${target}: glslc or shadercross not found; using the committed ${ARG_OUTPUT}")
    else()
        message(FATAL_ERROR "${target}: compiling shaders needs glslc and SDL_shadercross "
                            "(set BARE_GLSLC / BARE_SHADERCROSS), or a pre-generated ${ARG_OUTPUT}")
    endif()
endfunction()

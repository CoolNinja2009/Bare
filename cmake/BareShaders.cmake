# bare_add_shaders(): compiles GLSL fragment shaders for canvas passes
# (bare_pass) into a C header, at build time.
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
    foreach(src ${ARG_SOURCES})
        get_filename_component(abs ${src} ABSOLUTE)
        list(APPEND sources ${abs})
    endforeach()

    if(BARE_GLSLC AND BARE_SHADERCROSS)
        string(REPLACE ";" "|" packed "${sources}")
        add_custom_command(
            OUTPUT ${ARG_OUTPUT}
            COMMAND ${CMAKE_COMMAND} -DGLSLC=${BARE_GLSLC} -DSHADERCROSS=${BARE_SHADERCROSS}
                    "-DSOURCES=${packed}" -DOUT=${ARG_OUTPUT} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/${target}_shaders
                    -P ${BARE_SHADER_SCRIPT}
            DEPENDS ${sources} ${BARE_SHADER_SCRIPT}
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

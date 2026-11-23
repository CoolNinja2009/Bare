foreach(name hello shapes glass layout buttons cards)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE bare)
    target_compile_options(${name} PRIVATE ${BARE_WARNINGS})
endforeach()

# Fluids: a GPU fluid simulation on canvases, with its own shaders.
add_executable(fluid fluid/fluid.c)
target_link_libraries(fluid PRIVATE bare)
target_compile_options(fluid PRIVATE ${BARE_WARNINGS})
bare_add_shaders(fluid OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/fluid/generated/fluid_shaders.h SOURCES
    fluid/shaders/curl.glsl fluid/shaders/vorticity.glsl fluid/shaders/divergence.glsl fluid/shaders/fade.glsl
    fluid/shaders/pressure.glsl fluid/shaders/gradient.glsl fluid/shaders/advect.glsl fluid/shaders/splat.glsl
    fluid/shaders/bloom_prefilter.glsl fluid/shaders/bloom_blur.glsl fluid/shaders/sunrays_mask.glsl
    fluid/shaders/sunrays.glsl fluid/shaders/blur.glsl fluid/shaders/display.glsl)

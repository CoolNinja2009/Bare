foreach(name hello shapes glass layout buttons cards text)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE bare)
    target_compile_options(${name} PRIVATE ${BARE_WARNINGS})
endforeach()
target_compile_definitions(text PRIVATE ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets/")

# Fluids: a GPU fluid simulation on canvases, with its own shaders.
add_executable(fluid fluid/fluid.c)
target_link_libraries(fluid PRIVATE bare)
target_compile_options(fluid PRIVATE ${BARE_WARNINGS})
bare_add_shaders(fluid OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/fluid/generated/fluid_shaders.h SOURCES
    fluid/shaders/curl.glsl fluid/shaders/vorticity.glsl fluid/shaders/divergence.glsl fluid/shaders/fade.glsl
    fluid/shaders/pressure.glsl fluid/shaders/gradient.glsl fluid/shaders/advect.glsl fluid/shaders/splat.glsl
    fluid/shaders/bloom_prefilter.glsl fluid/shaders/bloom_blur.glsl fluid/shaders/sunrays_mask.glsl
    fluid/shaders/sunrays.glsl fluid/shaders/blur.glsl fluid/shaders/display.glsl)

# Liquid glass in motion: glass controls animated with springs and eases.
add_executable(glass_motion glass_motion/glass_motion.c)
target_link_libraries(glass_motion PRIVATE bare)
target_compile_options(glass_motion PRIVATE ${BARE_WARNINGS})
bare_add_shaders(glass_motion OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/glass_motion/generated/glass_motion_shaders.h SOURCES
    glass_motion/shaders/dunes.glsl glass_motion/shaders/petals.glsl glass_motion/shaders/wheel.glsl
    glass_motion/shaders/icons.glsl)

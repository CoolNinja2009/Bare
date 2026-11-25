foreach(name hello shapes glass layout buttons cards text scroll styles editor widgets code)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE bare)
    target_compile_options(${name} PRIVATE ${BARE_WARNINGS})
endforeach()
target_compile_definitions(text PRIVATE ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets/")

# Write: a Markdown and plain text editor on liquid glass.
add_executable(write write/write.c write/md.c)
target_link_libraries(write PRIVATE bare)
target_compile_options(write PRIVATE ${BARE_WARNINGS})

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

# Draw: your own triangles (bare_draw): a strip and instanced dots.
add_executable(draw draw/draw.c)
target_link_libraries(draw PRIVATE bare)
target_compile_options(draw PRIVATE ${BARE_WARNINGS})
bare_add_shaders(draw OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/draw/generated/draw_shaders.h SOURCES
    draw/shaders/ribbon.glsl draw/shaders/dots.glsl)

# Singularity: a black hole ray traced in real time; a pixel-exact port of a
# Screen+ web wallpaper (two render targets, a 32-bit float buffer, bloom).
add_executable(singularity singularity/singularity.c)
target_link_libraries(singularity PRIVATE bare)
target_compile_options(singularity PRIVATE ${BARE_WARNINGS})
bare_add_shaders(singularity OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/singularity/generated/singularity_shaders.h SOURCES
    singularity/shaders/scene.glsl singularity/shaders/down.glsl singularity/shaders/up.glsl
    singularity/shaders/comp.glsl singularity/shaders/probe.glsl)

# Aurora: Screen+'s "Aurora" wallpaper as a program (soft light, stars, the
# cursor gathers the light); a Screen+ app wallpaper with a Customise panel.
add_executable(aurora aurora/aurora.c)
target_link_libraries(aurora PRIVATE bare)
target_compile_options(aurora PRIVATE ${BARE_WARNINGS})
if(WIN32)
    set_target_properties(aurora PROPERTIES WIN32_EXECUTABLE TRUE)   # no console window behind the wallpaper
    if(MSVC)
        target_link_options(aurora PRIVATE /ENTRY:mainCRTStartup)    # still main(), not WinMain
    endif()
endif()
bare_add_shaders(aurora OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/aurora/generated/aurora_shaders.h SOURCES
    aurora/shaders/aurora.glsl)

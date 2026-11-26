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

# Draw: your own triangles (bare_draw): a strip, instanced dots and depth-tested cubes.
add_executable(draw draw/draw.c)
target_link_libraries(draw PRIVATE bare)
target_compile_options(draw PRIVATE ${BARE_WARNINGS})
bare_add_shaders(draw OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/draw/generated/draw_shaders.h SOURCES
    draw/shaders/ribbon.glsl draw/shaders/dots.glsl draw/shaders/cubes.glsl)

# Singularity: a black hole ray traced in real time; a pixel-exact port of a
# Screen+ web wallpaper (two render targets, a 32-bit float buffer, bloom).
add_executable(singularity singularity/singularity.c)
target_link_libraries(singularity PRIVATE bare)
target_compile_options(singularity PRIVATE ${BARE_WARNINGS})
bare_add_shaders(singularity OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/singularity/generated/singularity_shaders.h SOURCES
    singularity/shaders/scene.glsl singularity/shaders/down.glsl singularity/shaders/up.glsl
    singularity/shaders/comp.glsl singularity/shaders/probe.glsl)

# Koi Pond: a garden pond seen from above (Screen+'s sample): passes and bare_draw.
add_executable(koi koi/koi.c)
target_link_libraries(koi PRIVATE bare)
target_compile_options(koi PRIVATE ${BARE_WARNINGS})
bare_add_shaders(koi OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/koi/generated/koi_shaders.h SOURCES
    koi/shaders/sim.glsl koi/shaders/base.glsl koi/shaders/floor.glsl koi/shaders/water.glsl
    koi/shaders/koi.glsl koi/shaders/float.glsl)

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

# Ocean: the open sea simulated on the GPU (Tessendorf FFT waves in three
# cascades, foam, spray), drawn as a mesh with a depth buffer; fly above and under it.
add_executable(ocean ocean/ocean.c)
target_link_libraries(ocean PRIVATE bare)
target_compile_options(ocean PRIVATE ${BARE_WARNINGS})
bare_add_shaders(ocean OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/ocean/generated/ocean_shaders.h SOURCES
    ocean/shaders/spectrum.glsl ocean/shaders/evolve.glsl ocean/shaders/fft.glsl ocean/shaders/assemble.glsl
    ocean/shaders/probe.glsl ocean/shaders/skymap.glsl ocean/shaders/sky.glsl ocean/shaders/ocean.glsl
    ocean/shaders/spray.glsl ocean/shaders/water.glsl ocean/shaders/dof.glsl ocean/shaders/motes.glsl
    ocean/shaders/detail.glsl ocean/shaders/down.glsl ocean/shaders/up.glsl
    ocean/shaders/final.glsl)

# Water: a copy of the Ocean example to build on.
add_executable(water water/water.c)
target_link_libraries(water PRIVATE bare)
target_compile_options(water PRIVATE ${BARE_WARNINGS})
bare_add_shaders(water OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/water/generated/water_shaders.h SOURCES
    water/shaders/spectrum.glsl water/shaders/evolve.glsl water/shaders/fft.glsl water/shaders/assemble.glsl
    water/shaders/probe.glsl water/shaders/skymap.glsl water/shaders/sky.glsl water/shaders/ocean.glsl
    water/shaders/spray.glsl water/shaders/water.glsl water/shaders/dof.glsl water/shaders/motes.glsl
    water/shaders/detail.glsl water/shaders/down.glsl water/shaders/up.glsl
    water/shaders/final.glsl)

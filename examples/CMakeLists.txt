foreach(name hello shapes glass layout buttons cards text scroll styles editor widgets code voice ascii)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE bare)
    target_compile_options(${name} PRIVATE ${BARE_WARNINGS})
endforeach()
# Where text.c finds avatar.png: the source tree, or "assets/" beside the exe for packaged builds.
set(BARE_EXAMPLE_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/assets/" CACHE STRING "Folder the examples load their files from")
target_compile_definitions(text PRIVATE ASSETS="${BARE_EXAMPLE_ASSETS}")

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

# Matrix Rain: Screen+'s sample (falling katakana on a canvas never cleared); bare_draw and bare_text_pixels.
add_executable(matrix matrix/matrix.c)
target_link_libraries(matrix PRIVATE bare)
target_compile_options(matrix PRIVATE ${BARE_WARNINGS})
bare_add_shaders(matrix OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/matrix/generated/matrix_shaders.h SOURCES
    matrix/shaders/rain.glsl)

# Music Tunnel: Screen+'s sample (rocksdanister's tunnel shader, blurred; album art while a song plays).
add_executable(tunnel tunnel/tunnel.c)
target_link_libraries(tunnel PRIVATE bare)
target_compile_options(tunnel PRIVATE ${BARE_WARNINGS})
target_compile_definitions(tunnel PRIVATE ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tunnel/assets")
bare_add_shaders(tunnel OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/tunnel/generated/tunnel_shaders.h SOURCES
    tunnel/shaders/tunnel.glsl tunnel/shaders/blur.glsl)

# Simple System: Screen+'s sample (rocksdanister's Chart.js system monitor; the card repainted as an RTX 5090).
add_executable(system system/system.c)
target_link_libraries(system PRIVATE bare)
target_compile_options(system PRIVATE ${BARE_WARNINGS})
target_compile_definitions(system PRIVATE ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/system/assets")
bare_add_shaders(system OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/system/generated/system_shaders.h SOURCES
    screenplus/shaders/canvas2d.glsl)

# Rain: Screen+'s sample (rocksdanister's Rainy Window, modified: rain or snow in a picture in 3D).
add_executable(rain rain/rain.c)
target_link_libraries(rain PRIVATE bare)
target_compile_options(rain PRIVATE ${BARE_WARNINGS})
target_compile_definitions(rain PRIVATE ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/rain/assets")
bare_add_shaders(rain OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/rain/generated/rain_shaders.h SOURCES rain/shaders/rain.glsl)

# Triangles & Light: Screen+'s sample (Maksim Surguy's triangle pattern on Flat Surface Shader).
add_executable(triangles triangles/triangles.c)
target_link_libraries(triangles PRIVATE bare)
target_compile_options(triangles PRIVATE ${BARE_WARNINGS})
bare_add_shaders(triangles OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/triangles/generated/triangles_shaders.h SOURCES
    screenplus/shaders/canvas2d.glsl)

# The Hill: Screen+'s sample (ykob's GLSL Hills under the time and date).
add_executable(hill hill/hill.c)
target_link_libraries(hill PRIVATE bare)
target_compile_options(hill PRIVATE ${BARE_WARNINGS})
target_compile_definitions(hill PRIVATE ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/hill/assets")
bare_add_shaders(hill OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/hill/generated/hill_shaders.h SOURCES
    hill/shaders/hill.glsl screenplus/shaders/canvas2d.glsl)

# Parallax.js: Screen+'s sample (the parallax.js demo scene, its CSS and animations).
add_executable(parallax parallax/parallax.c)
target_link_libraries(parallax PRIVATE bare)
target_compile_options(parallax PRIVATE ${BARE_WARNINGS})
target_compile_definitions(parallax PRIVATE ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/parallax/assets")
bare_add_shaders(parallax OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/parallax/generated/parallax_shaders.h SOURCES parallax/shaders/sprite.glsl)

# Periodic Table: Screen+'s sample (Mike Golus's CodePen, its CSS: glowing tiles, hover, the model, the key).
add_executable(periodic periodic/periodic.c)
target_link_libraries(periodic PRIVATE bare)
target_compile_options(periodic PRIVATE ${BARE_WARNINGS})
bare_add_shaders(periodic OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/periodic/generated/periodic_shaders.h SOURCES
    periodic/shaders/blur.glsl screenplus/shaders/canvas2d.glsl)

add_executable(medusae medusae/medusae.c)
target_link_libraries(medusae PRIVATE bare)
target_compile_options(medusae PRIVATE ${BARE_WARNINGS})
target_compile_definitions(medusae PRIVATE ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/medusae/assets")
bare_add_shaders(medusae OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/medusae/generated/medusae_shaders.h SOURCES
    medusae/shaders/mesh.glsl medusae/shaders/line.glsl medusae/shaders/dust.glsl medusae/shaders/post.glsl medusae/shaders/dirt.glsl)

add_executable(livingroom livingroom/livingroom.c)
target_link_libraries(livingroom PRIVATE bare)
target_compile_options(livingroom PRIVATE ${BARE_WARNINGS})
target_compile_definitions(livingroom PRIVATE ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/livingroom/assets")
bare_add_shaders(livingroom OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/livingroom/generated/livingroom_shaders.h SOURCES
    screenplus/shaders/three.glsl screenplus/shaders/three_page.glsl)

add_executable(musictv musictv/musictv.c)
target_link_libraries(musictv PRIVATE bare)
target_compile_options(musictv PRIVATE ${BARE_WARNINGS})
target_compile_definitions(musictv PRIVATE ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/musictv/assets")
bare_add_shaders(musictv OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/musictv/generated/musictv_shaders.h SOURCES
    screenplus/shaders/three.glsl screenplus/shaders/three_page.glsl screenplus/shaders/pmrem.glsl)

add_executable(nullsaint nullsaint/nullsaint.c)
target_link_libraries(nullsaint PRIVATE bare)
target_compile_options(nullsaint PRIVATE ${BARE_WARNINGS})
target_include_directories(nullsaint SYSTEM PRIVATE ${stb_SOURCE_DIR})
target_compile_definitions(nullsaint PRIVATE ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/nullsaint/assets")
bare_add_shaders(nullsaint OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/nullsaint/generated/nullsaint_shaders.h SOURCES
    nullsaint/shaders/ns_copy.glsl nullsaint/shaders/ns_mosh.glsl nullsaint/shaders/ns_bright.glsl nullsaint/shaders/ns_blur.glsl
    nullsaint/shaders/ns_final.glsl nullsaint/shaders/ns_present.glsl screenplus/shaders/ctx2d.glsl)

# Screen+ app wallpapers answer --screenplus-properties with their settings (see screenplus/embed_properties.cmake)
include(screenplus/embed_properties.cmake)
foreach(sp_app aurora koi singularity fluid matrix tunnel system rain triangles hill parallax periodic medusae livingroom musictv nullsaint)
    if(TARGET ${sp_app})
        screenplus_embed_properties(${sp_app} "${CMAKE_CURRENT_SOURCE_DIR}/${sp_app}/LivelyProperties.json")
        if(MSVC)
            target_compile_definitions(${sp_app} PRIVATE _CRT_SECURE_NO_WARNINGS)   # screenplus_wallpaper.h: fopen, sscanf
        endif()
    endif()
endforeach()

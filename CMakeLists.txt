cmake_minimum_required(VERSION 3.24)
project(Bare VERSION 0.1.0 LANGUAGES C)

set(BARE_BACKEND "SDL3" CACHE STRING "Backend pair: SDL3 (SOKOL comes in M9)")
set_property(CACHE BARE_BACKEND PROPERTY STRINGS SDL3)
# Tests and examples build by default only when Bare is the top-level project,
# not when an app pulls it in with add_subdirectory or FetchContent.
option(BARE_BUILD_TESTS "Build the headless unit tests" ${PROJECT_IS_TOP_LEVEL})
option(BARE_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(BARE_BUILD_BENCH "Build the benchmarks (bench/; needs the examples)" OFF)
option(BARE_DEFAULT_FONT "Embed the default fonts (Inter in five faces and JetBrains Mono in two, Latin subsets, ~220 KB) so text works with no setup" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Put every executable in build/bin (only when Bare is the top-level project:
# an app including Bare decides where its own files go).
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

if(MSVC)
    set(BARE_WARNINGS /W4)
    set(BARE_STRICT /W4 /WX)
else()
    set(BARE_WARNINGS -Wall -Wextra -Wpedantic)
    set(BARE_STRICT -Wall -Wextra -Wpedantic -Werror)
endif()

include(FetchContent)

# stb_truetype and stb_image (header-only), pinned to a commit.
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/6b5799bae8a1fe3792691ef4b72ae26434c3e799.tar.gz
    URL_HASH SHA256=87e9ecd2d2e0cb8fa35bf5a93ea6ca3999595ea4f0a4738ed1aac0a34cbd4da9)
FetchContent_MakeAvailable(stb)

# ---------------------------------------------------------------------------
# Core: no platform headers, no dependencies beyond libc/libm (and stb).
# ---------------------------------------------------------------------------
add_library(bare_core STATIC
    src/core/arena.c
    src/core/id.c
    src/core/store.c
    src/core/context.c
    src/core/tree.c
    src/core/layout.c
    src/core/easing.c
    src/core/spring.c
    src/core/anim.c
    src/core/emit.c
    src/core/hit.c
    src/core/input.c
    src/core/scroll.c
    src/core/style.c
    src/core/gpu.c
    src/core/glass.c
    src/core/font.c
    src/core/text.c
    src/core/caret.c
    src/core/focus.c
    src/core/edit.c
    src/core/field.c
    src/core/widgets.c
    src/core/doc.c
    src/core/doc_edit.c
    src/core/doc_view.c
    src/core/font_stb.c
    src/core/image.c
)
target_include_directories(bare_core PUBLIC include PRIVATE src/core)
target_include_directories(bare_core SYSTEM PRIVATE ${stb_SOURCE_DIR})
if(BARE_DEFAULT_FONT)
    # file:symbol, in the order of the built-in font ids (src/core/font.c).
    foreach(entry Inter-Regular:default_font JetBrainsMono-Regular:font_mono Inter-SemiBold:font_sans_semibold
                  Inter-Bold:font_sans_bold Inter-Italic:font_sans_italic Inter-BoldItalic:font_sans_bold_italic
                  JetBrainsMono-Bold:font_mono_bold)
        string(REPLACE ":" ";" parts ${entry})
        list(GET parts 0 file)
        list(GET parts 1 symbol)
        set(in ${PROJECT_SOURCE_DIR}/assets/fonts/${file}.ttf)
        set(out ${PROJECT_BINARY_DIR}/generated/bare_${symbol}.c)
        add_custom_command(OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -DIN=${in} -DOUT=${out} -DNAME=bare__${symbol}
                -P ${PROJECT_SOURCE_DIR}/cmake/embed_file.cmake
            DEPENDS ${in} ${PROJECT_SOURCE_DIR}/cmake/embed_file.cmake
            COMMENT "Embedding ${file}.ttf"
            VERBATIM)
        target_sources(bare_core PRIVATE ${out})
    endforeach()
    target_compile_definitions(bare_core PRIVATE BARE_HAS_DEFAULT_FONT)
endif()
target_compile_options(bare_core PRIVATE ${BARE_STRICT})
if(NOT MSVC)
    target_link_libraries(bare_core PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# Backend + simple API
# ---------------------------------------------------------------------------
if(BARE_BACKEND STREQUAL "SDL3")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.20
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL3)

    add_library(bare STATIC
        src/bare.c
        src/backend/sdl3/platform_sdl3.c
        src/backend/sdl3/renderer_sdl3gpu.c
        src/backend/dxbc.c
        src/backend/media_win32.c
        src/picture.c
    )
    target_include_directories(bare PRIVATE src shaders/generated)
    target_include_directories(bare SYSTEM PRIVATE ${stb_SOURCE_DIR})
    target_link_libraries(bare PUBLIC bare_core PRIVATE SDL3::SDL3-static)
    target_compile_options(bare PRIVATE ${BARE_WARNINGS})
    if(MSVC)
        target_compile_definitions(bare PRIVATE _CRT_SECURE_NO_WARNINGS)   # getenv, sscanf
    endif()
    if(WIN32)
        # bare_window_backdrop (desktop duplication) and frameless windows (DWM corners)
        target_sources(bare PRIVATE src/backend/backdrop_win32.c)
        target_link_libraries(bare PRIVATE d3d11 dxgi dxguid dwmapi)
        # bare_picture: WIC still pictures, Media Foundation video
        target_link_libraries(bare PRIVATE windowscodecs mfplat mfreadwrite mfuuid ole32 uuid)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(bare PRIVATE Threads::Threads)
    endif()
else()
    message(FATAL_ERROR "Unknown BARE_BACKEND '${BARE_BACKEND}'")
endif()

# ---------------------------------------------------------------------------
# Shaders: compiled blobs are committed in shaders/generated/. This target
# regenerates them when glslc and shadercross are available.
# ---------------------------------------------------------------------------
find_program(BARE_GLSLC glslc)
find_program(BARE_SHADERCROSS shadercross
    HINTS ${PROJECT_SOURCE_DIR}/tools/shadercross/SDL3_shadercross-3.0.0-windows-VC-x64/bin)
if(BARE_GLSLC AND BARE_SHADERCROSS)
    add_custom_target(bare_shaders
        COMMAND ${CMAKE_COMMAND}
            -DGLSLC=${BARE_GLSLC} -DSHADERCROSS=${BARE_SHADERCROSS}
            -DSRC=${PROJECT_SOURCE_DIR}/shaders
            -DOUT=${PROJECT_SOURCE_DIR}/shaders/generated
            -DWORK=${PROJECT_BINARY_DIR}/shaders
            -P ${PROJECT_SOURCE_DIR}/cmake/compile_shaders.cmake
        COMMENT "Compiling shaders/*.glsl to SPIR-V, DXIL and MSL"
        VERBATIM)
endif()

# Apps' own shaders for canvas passes: bare_add_shaders() (cmake/BareShaders.cmake).
include(${PROJECT_SOURCE_DIR}/cmake/BareShaders.cmake)

# ---------------------------------------------------------------------------
if(BARE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(BARE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(BARE_BUILD_BENCH AND BARE_BUILD_EXAMPLES)
    add_subdirectory(bench)
endif()

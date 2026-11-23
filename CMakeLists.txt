cmake_minimum_required(VERSION 3.24)
project(Bare VERSION 0.1.0 LANGUAGES C)

set(BARE_BACKEND "SDL3" CACHE STRING "Backend pair: SDL3 (SOKOL comes in M9)")
set_property(CACHE BARE_BACKEND PROPERTY STRINGS SDL3)
option(BARE_BUILD_TESTS "Build the headless unit tests" ON)
option(BARE_BUILD_EXAMPLES "Build the examples" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Put every executable next to the others so examples find nothing missing.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    set(BARE_WARNINGS /W4)
    set(BARE_STRICT /W4 /WX)
else()
    set(BARE_WARNINGS -Wall -Wextra -Wpedantic)
    set(BARE_STRICT -Wall -Wextra -Wpedantic -Werror)
endif()

# ---------------------------------------------------------------------------
# Core: no platform headers, no dependencies beyond libc/libm.
# ---------------------------------------------------------------------------
add_library(bare_core STATIC
    src/core/arena.c
    src/core/id.c
    src/core/context.c
    src/core/tree.c
    src/core/layout.c
    src/core/emit.c
)
target_include_directories(bare_core PUBLIC include PRIVATE src/core)
target_compile_options(bare_core PRIVATE ${BARE_STRICT})
if(NOT MSVC)
    target_link_libraries(bare_core PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# Backend + simple API
# ---------------------------------------------------------------------------
if(BARE_BACKEND STREQUAL "SDL3")
    include(FetchContent)
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
    )
    target_include_directories(bare PRIVATE src shaders/generated)
    target_link_libraries(bare PUBLIC bare_core PRIVATE SDL3::SDL3-static)
    target_compile_options(bare PRIVATE ${BARE_WARNINGS})
else()
    message(FATAL_ERROR "Unknown BARE_BACKEND '${BARE_BACKEND}'")
endif()

# ---------------------------------------------------------------------------
# Shaders: compiled blobs are committed in shaders/generated/. This target
# regenerates them when glslc and shadercross are available.
# ---------------------------------------------------------------------------
find_program(BARE_GLSLC glslc)
find_program(BARE_SHADERCROSS shadercross
    HINTS ${CMAKE_SOURCE_DIR}/tools/shadercross/SDL3_shadercross-3.0.0-windows-VC-x64/bin)
if(BARE_GLSLC AND BARE_SHADERCROSS)
    add_custom_target(bare_shaders
        COMMAND ${CMAKE_COMMAND}
            -DGLSLC=${BARE_GLSLC} -DSHADERCROSS=${BARE_SHADERCROSS}
            -DSRC=${CMAKE_SOURCE_DIR}/shaders
            -DOUT=${CMAKE_SOURCE_DIR}/shaders/generated
            -DWORK=${CMAKE_BINARY_DIR}/shaders
            -P ${CMAKE_SOURCE_DIR}/cmake/compile_shaders.cmake
        COMMENT "Compiling shaders/*.glsl to SPIR-V, DXIL and MSL"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
if(BARE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(BARE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

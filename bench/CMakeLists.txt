# Benchmarks (BARE_BUILD_BENCH=ON; build Release). See bench/README.md.

# The Fluids example, driven: stirred every frame, panel hidden, fps in the title.
add_executable(fluid_bench ${PROJECT_SOURCE_DIR}/examples/fluid/fluid.c)
target_link_libraries(fluid_bench PRIVATE bare SDL3::SDL3-static)
target_include_directories(fluid_bench PRIVATE ${PROJECT_SOURCE_DIR}/examples/fluid/generated)
if(MSVC)
    target_compile_options(fluid_bench PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/fluid_driver.h)
else()
    target_compile_options(fluid_bench PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/fluid_driver.h)
endif()
if(TARGET fluid_shaders)
    add_dependencies(fluid_bench fluid_shaders)
endif()

# Where the time to the first frame goes, with SDL alone.
add_executable(startup startup.c)
target_link_libraries(startup PRIVATE SDL3::SDL3-static)

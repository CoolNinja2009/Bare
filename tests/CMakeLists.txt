foreach(name arena id emit layout input gpu anim text scroll style edit doc widgets)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} PRIVATE bare_core)
    target_include_directories(test_${name} PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
    target_compile_options(test_${name} PRIVATE ${BARE_WARNINGS})
    add_test(NAME ${name} COMMAND test_${name})
endforeach()
target_compile_definitions(test_text PRIVATE BARE_TEST_ASSETS="${PROJECT_SOURCE_DIR}/examples/assets/")

# The backend's DXBC edits are plain C, tested on the shader blobs FXC made.
add_executable(test_dxbc test_dxbc.c ${PROJECT_SOURCE_DIR}/src/backend/dxbc.c)
target_include_directories(test_dxbc PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/shaders/generated)
target_compile_options(test_dxbc PRIVATE ${BARE_WARNINGS})
add_test(NAME dxbc COMMAND test_dxbc)

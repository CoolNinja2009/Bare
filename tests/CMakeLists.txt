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

# BARE_RECORD's video encoder (Media Foundation on Windows), without a window or GPU.
add_executable(test_record test_record.c ${PROJECT_SOURCE_DIR}/src/backend/record_win32.c)
target_include_directories(test_record PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(test_record PRIVATE ${BARE_WARNINGS})
if(MSVC)
    target_compile_definitions(test_record PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(WIN32)
    target_link_libraries(test_record PRIVATE mfplat mfreadwrite mfuuid ole32)
endif()
add_test(NAME record COMMAND test_record)

# The system's decoders behind bare_sound_load and bare_video_load, on files written here (no window or GPU).
add_executable(test_media test_media.c ${PROJECT_SOURCE_DIR}/src/backend/media_win32.c
               ${PROJECT_SOURCE_DIR}/src/backend/record_win32.c)
target_include_directories(test_media PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(test_media PRIVATE ${BARE_WARNINGS})
if(MSVC)
    target_compile_definitions(test_media PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(WIN32)
    target_link_libraries(test_media PRIVATE d3d11 dxgi dxguid windowscodecs mfplat mfreadwrite mfuuid ole32 uuid shell32)
else()
    target_link_libraries(test_media PRIVATE m)
endif()
add_test(NAME media COMMAND test_media)

# bare_sound_loop / bare_sound_volume on SDL's dummy audio driver (nothing is heard).
add_executable(test_sound test_sound.c)
target_link_libraries(test_sound PRIVATE bare ${BARE_SDL3_TARGET})
target_compile_options(test_sound PRIVATE ${BARE_WARNINGS})
add_test(NAME sound COMMAND test_sound)

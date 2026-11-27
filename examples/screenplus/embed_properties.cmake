# screenplus_embed_properties(target json): builds the wallpaper's settings
# (LivelyProperties.json) into the program, so `program --screenplus-properties`
# prints them and Screen+'s Customise finds them with no file beside the exe.
function(screenplus_embed_properties target json)
    if(NOT EXISTS "${json}")
        return()
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${json}")
    file(READ "${json}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sp_properties/${target}_properties.h")
    file(WRITE "${out}.new" "/* generated from ${json} */\nstatic const char sp_properties_json[] = { ${bytes}0 };\n")
    file(COPY_FILE "${out}.new" "${out}" ONLY_IF_DIFFERENT)
    file(REMOVE "${out}.new")
    target_compile_definitions(${target} PRIVATE SP_PROPERTIES_H="${out}")
endfunction()

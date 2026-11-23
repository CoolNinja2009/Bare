# embed_file.cmake: turns a file into a C array (cmake -P).
#   -DIN=file -DOUT=out.c -DNAME=symbol  ->  const unsigned char NAME_data[]; const int NAME_size;
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR size "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,))" "\\1\n" bytes "${bytes}")
get_filename_component(name "${IN}" NAME)
file(WRITE "${OUT}"
    "/* Generated from ${name} by cmake/embed_file.cmake: don't edit. */\n"
    "const unsigned char ${NAME}_data[] = {\n${bytes}\n};\n"
    "const int ${NAME}_size = ${size};\n")

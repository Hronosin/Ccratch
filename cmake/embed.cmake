# Turns a binary file into a C source with a byte array, so assets live
# inside libccratch.so instead of next to it.
#
# Script mode:  cmake -DINPUT=icon.png -DOUTPUT=icon.c -DNAME=ccr_icon_png -P embed.cmake
# Produces:     const unsigned char ccr_icon_png[];  const unsigned int ccr_icon_png_size;

file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _hex_len)
math(EXPR _size "${_hex_len} / 2")

string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
# 16 bytes per line keeps diffs and editors sane
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " _bytes "${_bytes}")

file(WRITE "${OUTPUT}"
"/* Generated from ${INPUT} by cmake/embed.cmake. Do not edit. */\n"
"const unsigned char ${NAME}[] = {\n    ${_bytes}\n};\n"
"const unsigned int ${NAME}_size = ${_size}u;\n")

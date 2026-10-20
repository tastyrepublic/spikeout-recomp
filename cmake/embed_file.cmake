# Writes a C++ source holding one file's bytes (a font, a menu page), with a
# 0 after them so text can be read as a string.
#   cmake -DNAME=menu_rml -DIN=menu.rml -DOUT=menu_rml.cpp -P embed_file.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR size "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n    " bytes "${bytes}")
file(WRITE "${OUT}" "// Generated from ${IN} by embed_file.cmake: do not edit.
#include \"ui/assets.hpp\"

namespace ui::assets {

namespace {
const unsigned char kData[${size} + 1] = {
    ${bytes}0
};
} // namespace

const Asset ${NAME} = {kData, ${size}};

} // namespace ui::assets
")

# Writes a C++ source holding one compiled shader: its SPIR-V (Vulkan) as
# bytes and its Metal source as text.
#   cmake -DNAME=present_vert -DSPV=x.spv -DMSL=x.msl -DOUT=x.cpp -P embed_shader.cmake
file(READ "${SPV}" spv HEX)
string(LENGTH "${spv}" hex_length)
math(EXPR size "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${spv}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " bytes "${bytes}")
file(READ "${MSL}" msl)
file(WRITE "${OUT}" "// Generated from src/gpu/shaders by embed_shader.cmake: do not edit.
#include \"gpu/shaders.hpp\"

namespace gpu::shaders {

namespace {
const unsigned char kSpirv[${size}] = {
    ${bytes}
};
const char kMsl[] = R\"msl(${msl})msl\";
} // namespace

const ShaderCode ${NAME} = {kSpirv, sizeof kSpirv, kMsl};

} // namespace gpu::shaders
")

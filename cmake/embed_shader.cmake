# Writes a C++ source holding one compiled shader: its SPIR-V (Vulkan) and
# its Metal source, both as byte arrays (MSVC limits a string literal to
# about 16 KB), the Metal source ending in a 0.
#   cmake -DNAME=present_vert -DSPV=x.spv -DMSL=x.msl -DOUT=x.cpp -P embed_shader.cmake
function(to_bytes path out_var size_var)
    file(READ "${path}" hex HEX)
    string(LENGTH "${hex}" hex_length)
    math(EXPR size "${hex_length} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n    " bytes "${bytes}")
    set(${out_var} "${bytes}" PARENT_SCOPE)
    set(${size_var} ${size} PARENT_SCOPE)
endfunction()

to_bytes("${SPV}" spv spv_size)
to_bytes("${MSL}" msl msl_size)
math(EXPR msl_size "${msl_size} + 1")
file(WRITE "${OUT}" "// Generated from src/gpu/shaders by embed_shader.cmake: do not edit.
#include \"gpu/shaders.hpp\"

namespace gpu::shaders {

namespace {
const unsigned char kSpirv[${spv_size}] = {
    ${spv}
};
const unsigned char kMsl[${msl_size}] = {
    ${msl}0
};
} // namespace

const ShaderCode ${NAME} = {kSpirv, sizeof kSpirv, reinterpret_cast<const char*>(kMsl)};

} // namespace gpu::shaders
")

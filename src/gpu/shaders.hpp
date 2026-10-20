// The GPU renderer's shaders, compiled at build time from src/gpu/shaders
// (GLSL): SPIR-V for Vulkan (glslang) and Metal source (SPIRV-Cross, entry
// point "main0"). Resource bindings follow SDL_CreateGPUShader's rules:
// fragment textures in set 2, fragment uniforms in set 3, vertex ones in
// sets 0 and 1.
#pragma once

#include <cstddef>

namespace gpu {

struct ShaderCode {
    const unsigned char* spirv;
    std::size_t spirv_size;
    const char* msl;
};

namespace shaders {
extern const ShaderCode present_vert, present_frag;
} // namespace shaders

} // namespace gpu

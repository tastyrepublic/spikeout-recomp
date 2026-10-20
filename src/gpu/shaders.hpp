// The GPU renderer's shaders, compiled at build time from src/gpu/shaders
// (GLSL): SPIR-V for Vulkan (glslang) and Metal source (SPIRV-Cross, entry
// point "main0"). Resource bindings follow SDL_CreateGPUShader's rules:
// textures and then storage buffers in set 0 (vertex) or 2 (fragment),
// uniform buffers in set 1 or 3. Metal takes the binding numbers as they
// are (--msl-decoration-binding), so in each stage number them as SDL's
// Metal order wants: textures from 0, uniform buffers from 0, storage
// buffers after the uniform buffers (a fragment shader with one uniform
// buffer and two storage buffers: bindings 0, then 1 and 2).
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
extern const ShaderCode scene_vert, scene_frag, scene_opaque_frag, scene_quads_frag, scene_opaque_quads_frag;
extern const ShaderCode background_fog_frag, depth_copy_frag, composite_frag, screen_frag, premultiply_frag;
} // namespace shaders

} // namespace gpu

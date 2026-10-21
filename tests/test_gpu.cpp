#include "test.hpp"

#include "gpu/shaders.hpp"

#include <cstdint>
#include <cstring>

namespace {

uint32_t word(const gpu::ShaderCode& s, std::size_t i) {
    uint32_t v = 0;
    std::memcpy(&v, s.spirv + 4 * i, 4);
    return v;
}

} // namespace

TEST_CASE("gpu: the shaders compiled to SPIR-V and Metal source") {
    for (const gpu::ShaderCode* s :
         {&gpu::shaders::present_vert, &gpu::shaders::present_frag, &gpu::shaders::scene_vert, &gpu::shaders::scene_frag,
          &gpu::shaders::scene_opaque_frag, &gpu::shaders::background_fog_frag,
          &gpu::shaders::composite_frag, &gpu::shaders::screen_frag, &gpu::shaders::scene_quads_frag,
          &gpu::shaders::scene_opaque_quads_frag, &gpu::shaders::scene_quads_bounded_frag,
          &gpu::shaders::scene_opaque_quads_bounded_frag}) {
        CHECK(s->spirv_size > 20);
        CHECK_EQ(s->spirv_size % 4, 0u);
        CHECK_EQ(word(*s, 0), 0x07230203u); // the SPIR-V magic number
        CHECK(std::strstr(s->msl, "main0") != nullptr);
        CHECK(std::strstr(s->msl, "#include <metal_stdlib>") != nullptr);
    }
}

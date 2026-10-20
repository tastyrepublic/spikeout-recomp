// The screen: black, the 2D layers below the 3D, the 3D picture (opaque, or
// translucent over what is below), then the 2D layers above (as
// m3::TilegenRenderer::composite). The 2D layers are 496x384, sampled
// sharp; the 3D (premultiplied alpha, gpu::Real3dGpu::display_image) may be
// drawn larger and is filtered, averaged down with its mipmaps where the
// window is smaller than it: supersampling.
#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

layout(set = 2, binding = 0) uniform sampler2D bottom;
layout(set = 2, binding = 1) uniform sampler2D three_d;
layout(set = 2, binding = 2) uniform sampler2D top;

void main() {
    const vec4 b = texture(bottom, uv), q = texture(three_d, uv), t = texture(top, uv);
    vec3 p = b != vec4(0.0) ? b.rgb : vec3(0.0); // nothing drawn below: black
    p = q.rgb + p * (1.0 - q.a); // over what is below
    if (t != vec4(0.0)) p = t.rgb;
    colour = vec4(p, 1.0);
}

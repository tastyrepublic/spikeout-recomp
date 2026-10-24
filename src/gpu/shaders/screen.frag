// The screen: black, the 2D layers below the 3D, the 3D picture (opaque, or
// translucent over what is below), then the 2D layers above (as
// m3::TilegenRenderer::composite). The 2D layers are sampled sharp; the 3D
// (premultiplied alpha, gpu::Real3dGpu::display_image) may be drawn larger
// and is filtered, averaged down with its mipmaps where the window is
// smaller than it: supersampling. Then the looks (look.glsl).
//
// Widescreen (wide.x > 0): the 3D is that many screen pixels wider each side
// (its full-screen viewports opened out), and so are the 2D layers (their
// sides filled in by m3::WideLayer). The sides show the 3D only where it
// reaches the edge of the arcade's screen somewhere on that side: on the 2D
// screens (the title, the character select, the map) the 3D is a picture in
// the middle, and what it has past the arcade's screen (a streak waiting to
// cross the title) the arcade never showed.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "look.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

layout(set = 2, binding = 0) uniform sampler2D bottom;
layout(set = 2, binding = 1) uniform sampler2D three_d;
layout(set = 2, binding = 2) uniform sampler2D top;

// Whether the 3D shows anywhere along the arcade screen's edge column at u.
bool reaches(float u) {
    for (int i = 0; i < 48; ++i)
        if (textureLod(three_d, vec2(u, (float(i) + 0.5) / 48.0), 0.0).a > 0.0) return true;
    return false;
}

void main() {
    vec4 q = texture(three_d, uv);
    const float extend = wide.x, width = wide.y, full = width + 2.0 * extend;
    const ivec2 size = textureSize(top, 0);
    const ivec2 at = min(ivec2(uv * vec2(size)), size - 1);
    const float x = uv.x * full - extend; // where in the arcade's 496 pixels this is
    if (x < 0.0 && !reaches((extend + 0.5) / full)) q = vec4(0.0);
    if (x >= width && !reaches((extend + width - 0.5) / full)) q = vec4(0.0);
    const vec4 b = texelFetch(bottom, at, 0), t = texelFetch(top, at, 0);
    vec3 p = b != vec4(0.0) ? b.rgb : vec3(0.0); // nothing drawn below: black
    p = q.rgb + p * (1.0 - q.a);                 // over what is below
    if (t != vec4(0.0)) p = t.rgb;
    colour = vec4(look(p), 1.0);
}

// The 3D picture with its colour premultiplied by alpha, for showing: then
// averaging it down (mipmaps, a window smaller than the picture) doesn't
// darken edges where the 3D meets the 2D layers behind it.
#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

layout(set = 2, binding = 0) uniform sampler2D picture;

void main() {
    const vec4 c = texelFetch(picture, ivec2(gl_FragCoord.xy), 0);
    colour = vec4(c.rgb * c.a, c.a);
}

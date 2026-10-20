// The translucency layers over the opaque picture (the software renderer's
// last step): one layer alone by its alpha, straight (not premultiplied) as
// the 2D compositor expects; both together mixed into an opaque colour.
#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

layout(set = 2, binding = 0) uniform sampler2D opaque;
layout(set = 2, binding = 1) uniform sampler2D layer0;
layout(set = 2, binding = 2) uniform sampler2D layer1;

void main() {
    const ivec2 at = ivec2(gl_FragCoord.xy);
    const vec4 c = texelFetch(opaque, at, 0), t0 = texelFetch(layer0, at, 0), t1 = texelFetch(layer1, at, 0);
    if (t0.a == 0.0 && t1.a == 0.0) {
        colour = c;
    } else if (t0.a != 0.0 && t1.a != 0.0) {
        const float f = (t1.a + (1.0 - t0.a)) / 2.0;
        colour = vec4(t0.rgb + (t1.rgb - t0.rgb) * f, 1.0);
    } else {
        const vec4 t = t0.a != 0.0 ? t0 : t1;
        const float below = c.a * (1.0 - t.a), oa = t.a + below;
        colour = vec4((t.rgb * t.a + c.rgb * below) / oa, oa);
    }
}

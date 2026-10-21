// The translucency layers over the opaque picture (the software renderer's
// last step): one layer alone by its alpha, straight (not premultiplied) as
// the 2D compositor expects; both together mixed into an opaque colour.
// Also the same picture premultiplied by alpha, for showing: then averaging
// it down (mipmaps, a window smaller than the picture) doesn't darken edges
// where the 3D meets the 2D layers behind it. It is premultiplied from the
// straight colour as stored (8 bits), as reading the picture back would give.
#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;
layout(location = 1) out vec4 shown;

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
    const vec4 stored = floor(clamp(colour, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
    shown = vec4(stored.rgb * stored.a, stored.a);
}

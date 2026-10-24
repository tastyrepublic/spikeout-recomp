// A scene pixel: texture, lighting and fog, after the software renderer
// (src/runtime/real3d_render.cpp: sample, light_and_fog, raster), which
// follows Supermodel's R3DShaderCommon.h and R3DShaderTriangles.h. Texture
// RAM is sampled as the software renderer does it, in 8-bit integer steps,
// so the two pictures can be compared closely.
//
// Built in variants: with OPAQUE, the opaque pass (colour, clearing both
// translucency layers, and depth for the translucent passes), without, a
// translucency layer; each with QUADS too, for quads drawn whole (their
// attributes and depth from all four corners; writing the depth costs the
// GPU its early depth test, so only those draws use it). QUADS with BOUNDED
// too: quads whose depth is never nearer than the one they are drawn at
// (scene.vert gives each its nearest corner's), which lets the GPU test
// depth early again, against that bound.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_common.glsl"

#ifdef BOUNDED
layout(depth_less) out float gl_FragDepth;
#endif

layout(location = 0) in vec2 uv;
layout(location = 1) in float shade;
layout(location = 2) in vec3 normal;
layout(location = 3) flat in uint polygon;
layout(location = 4) flat in uint quad;

// (SDL's fragment set 2: the textures, then the storage buffers.)
layout(set = 2, binding = 0) uniform usampler2D texture_ram; // 2048 x 2048, 16 bits a texel
#ifdef OPAQUE
#define kStorage 1
#else
// The opaque pass's depth: a translucent pixel only in front of it.
layout(set = 2, binding = 1) uniform sampler2D opaque_depth;
#define kStorage 2
#endif
layout(std430, set = 2, binding = kStorage) readonly buffer Polygons { Polygon polygons[]; };
layout(std430, set = 2, binding = kStorage + 1) readonly buffer Viewports { Viewport viewports[]; };
#ifdef QUADS
layout(std430, set = 2, binding = kStorage + 2) readonly buffer Quads { Quad quads[]; };
#endif
layout(std140, set = 3, binding = 0) uniform Frame {
    float scale;  // render pixels per screen pixel
    float extend; // widescreen: the screen's left edge, in screen pixels from the picture's
};

#ifdef OPAQUE
layout(location = 0) out vec4 colour;
layout(location = 1) out vec4 layer0; // cleared where an opaque pixel lands
layout(location = 2) out vec4 layer1;
layout(location = 3) out vec4 depth_out; // the depth, for the translucent passes
#else
layout(location = 0) out vec4 colour;
#endif

// ---- Texels ----

uint to8(float c) { return uint(clamp(c, 0.0, 1.0) * 255.0 + 0.5); }

// A texel to RGBA8 (red in bits 0-7), by format (Supermodel's
// ExtractColour; formats 8-11 are 1-4 with contour on).
uint decode(uint format, uint t) {
    vec4 c;
    if (format == 0u) { // T1RGB5: bit 15 transparent
        c = vec4(float((t >> 10) & 0x1Fu) / 31.0, float((t >> 5) & 0x1Fu) / 31.0, float(t & 0x1Fu) / 31.0,
                 (t & 0x8000u) != 0u ? 0.0 : 1.0);
    } else if (format == 1u) {
        c = vec4(vec3(float(t & 0xFu) / 15.0), float((t >> 4) & 0xFu) / 15.0);
    } else if (format == 2u) {
        c = vec4(vec3(float((t >> 4) & 0xFu) / 15.0), float(t & 0xFu) / 15.0);
    } else if (format == 3u) {
        c = vec4(vec3(float((t >> 8) & 0xFu) / 15.0), float((t >> 12) & 0xFu) / 15.0);
    } else if (format == 4u) {
        c = vec4(vec3(float((t >> 12) & 0xFu) / 15.0), float((t >> 8) & 0xFu) / 15.0);
    } else if (format == 7u) { // RGBA4
        c = vec4(float((t >> 12) & 0xFu) / 15.0, float((t >> 8) & 0xFu) / 15.0, float((t >> 4) & 0xFu) / 15.0,
                 float(t & 0xFu) / 15.0);
    } else { // luminance, opaque unless white
        float l;
        if (format == 5u) l = float(t & 0xFFu) / 255.0;
        else if (format == 6u) l = float((t >> 8) & 0xFFu) / 255.0;
        else if (format == 8u) l = float(t & 0xFu) / 15.0;
        else if (format == 9u) l = float((t >> 4) & 0xFu) / 15.0;
        else if (format == 10u) l = float((t >> 8) & 0xFu) / 15.0;
        else l = float((t >> 12) & 0xFu) / 15.0;
        c = vec4(vec3(l), l == 1.0 ? 0.0 : 1.0);
    }
    return to8(c.r) | to8(c.g) << 8 | to8(c.b) << 16 | to8(c.a) << 24;
}

// a + (b - a) * w / 256 on all four bytes at once (two at a time).
uint lerp8(uint a, uint b, uint w) {
    const uint iw = 256u - w;
    const uint rb = (((a & 0x00FF00FFu) * iw + (b & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
    const uint ag = (((a >> 8) & 0x00FF00FFu) * iw + ((b >> 8) & 0x00FF00FFu) * w) & 0xFF00FF00u;
    return rb | ag;
}

// log2 to about 0.01: enough to pick a texture level (the software
// renderer's, so both pick the same).
float fast_log2(float x) {
    const uint bits = floatBitsToUint(x);
    const float m = uintBitsToFloat((bits & 0x007FFFFFu) | 0x3F800000u);
    // The polynomial is log2(m) + 1 for m in [1, 2), hence the exponent less 128.
    return float(int((bits >> 23) & 0xFFu) - 128) + (-0.34484843 * m + 2.02466578) * m - 0.67487759;
}

// One texture axis: the two texels to blend and the second one's weight
// (0-256), for size s and wrap mode 0 repeat, 1 clamp, 2 mirror.
uvec3 axis_taps(float t, int s, uint mode) {
    if (!(abs(t) < 1e7)) t = 0.0;
    const float fs = float(s);
    if (mode == 2u) {
        const float period = floor(t / fs);
        t -= period * fs;
        if ((int(period) & 1) != 0) t = fs - t;
    } else if (mode == 1u) {
        t -= floor(t / fs) * fs;
    }
    t -= 0.5;
    const float fl = floor(t);
    int i0 = int(fl), i1 = i0 + 1;
    if (mode == 0u) {
        i0 &= s - 1;
        i1 &= s - 1;
    } else {
        i0 = clamp(i0, 0, s - 1);
        i1 = clamp(i1, 0, s - 1);
    }
    return uvec3(uint(i0), uint(i1), uint((t - fl) * 256.0));
}

uint texel(uint x, uint y) { return texelFetch(texture_ram, ivec2(int(x & 2047u), int(y & 2047u)), 0).r; }

// The cut-out spread: a texel takes the colour of a more opaque neighbour,
// so cut-out edges don't darken.
void spread(uint from, inout uint to) {
    if ((from >> 24) > (to >> 24)) to = (to & 0xFF000000u) | (from & 0x00FFFFFFu);
}

uint level_sample(Polygon p, float u, float v, int l, bool contour) {
    const int sw = max(1, int(p.tex.z) >> l), sh = max(1, int(p.tex.w) >> l);
    const float s = 1.0 / float(1 << l);
    const uvec3 a = axis_taps(u * s, sw, p.tex2.z & 0xFFu), b = axis_taps(v * s, sh, (p.tex2.z >> 8) & 0xFFu);
    const uint bx = (2048u - (2048u >> l)) + (p.tex.x >> l);
    const uint by = p.tex2.x + (1024u - (1024u >> l)) + ((p.tex.y & 1023u) >> l);
    const uint format = p.tex2.w;
    uint p00 = decode(format, texel(bx + a.x, by + b.x)), p10 = decode(format, texel(bx + a.y, by + b.x));
    uint p01 = decode(format, texel(bx + a.x, by + b.y)), p11 = decode(format, texel(bx + a.y, by + b.y));
    if (contour) {
        spread(p00, p10);
        spread(p00, p01);
        spread(p10, p00);
        spread(p10, p11);
        spread(p01, p00);
        spread(p01, p11);
        spread(p11, p01);
        spread(p11, p10);
    }
    return lerp8(lerp8(p00, p10, a.z), lerp8(p01, p11, a.z), b.z);
}

// The texture at (u, v), in texels of the full-size level, `distance` along
// the view: bilinear within a level, blended with the next by the fraction,
// and the microtexture close up.
vec4 sample_texture(Polygon p, float u, float v, float distance) {
    const bool contour = (p.flags.x & kFlagContour) != 0u;
    const int max_level = int(p.tex2.y);
    const float raw_lod = distance > 0.0 ? p.values.z + 2.0 * fast_log2(distance) : 0.0;
    float lod = raw_lod;
    if (!(lod > 0.0)) lod = 0.0;
    lod = min(lod, float(max_level));
    const int level = int(lod);
    const uint frac = uint((lod - float(level)) * 256.0);
    uint c = level_sample(p, u, v, level, contour);
    if (frac > 0u && level < max_level) {
        c = lerp8(c, level_sample(p, u, v, level + 1, contour), frac);
    } else if ((p.flags.x & kFlagMicro) != 0u && raw_lod < 0.0) {
        // A 128x128 detail texture in the other page (8 of them), repeating
        // at 2, 4, 16 or 256 times the texel rate, blended in up to half.
        const uint id = p.flags.y & 7u, min_lod = (p.flags.y >> 8) & 3u;
        const uint kMicroX[8] = uint[8](0u, 0u, 128u, 128u, 0u, 0u, 128u, 128u);
        const uint kMicroY[8] = uint[8](0u, 128u, 0u, 128u, 256u, 384u, 256u, 384u);
        const float kMicroScale[4] = float[4](2.0, 4.0, 16.0, 256.0);
        const float ms = kMicroScale[min_lod];
        const uvec3 a = axis_taps(u * ms, 128, 0u), b = axis_taps(v * ms, 128, 0u);
        const uint bx = kMicroX[id], by = (p.tex2.x ^ 1024u) + kMicroY[id];
        const uint format = p.tex2.w;
        const uint m = lerp8(lerp8(decode(format, texel(bx + a.x, by + b.x)), decode(format, texel(bx + a.y, by + b.x)), a.z),
                             lerp8(decode(format, texel(bx + a.x, by + b.y)), decode(format, texel(bx + a.y, by + b.y)), a.z), b.z);
        const float blend = min(-raw_lod * exp2(-float(min_lod)) * 0.5, 0.5);
        c = lerp8(c, m, uint(blend * 256.0));
    }
    if ((p.flags.x & kFlagInverted) != 0u) c ^= 0x00FFFFFFu;
    const float k = 1.0 / 255.0;
    return vec4(float(c & 0xFFu) * k, float((c >> 8) & 0xFFu) * k, float((c >> 16) & 0xFFu) * k, float(c >> 24) * k);
}

// ---- Lighting and fog ----

// The sun (or fixed shade), ambient, spotlight and specular, then fog, at
// pixel centre (cx, cy) of the 496x384 screen, `distance` along the view.
void light_and_fog(Polygon p, float cx, float cy, float distance, vec3 normal, float shade, inout vec3 rgb, inout float a) {
    const Viewport V = viewports[p.flags.z];
    float lobe = 0.0, lobe_fog = 0.0;
    if (V.spot2.z != 0.0) {
        const float ex = (cx - V.spot.x) / V.spot.z, ey = (cy - V.spot.y) / V.spot.w;
        const float ellipse = max(0.0, 1.0 - (ex * ex + ey * ey));
        const float enable = distance >= V.spot2.x ? 1.0 : 0.0;
        if (V.spot2.y != 0.0) {
            const float extent = abs(V.spot2.y);
            const float d = min(V.spot2.x + extent - distance, 0.0);
            const float k = d / (1.0 + extent) - 1.0;
            lobe = enable / (k * k) * ellipse;
        }
        lobe_fog = enable * ellipse;
    }
    const uint flags = p.flags.x;
    if ((flags & kFlagLit) != 0u) {
        const float n_dot_sun = dot(normal, V.sun.xyz);
        const float sun = clamp((flags & kFlagFixedShading) != 0u ? shade : n_dot_sun, 0.0, 1.0);
        const float diffuse = sun * V.sun.w + V.light.x;
        rgb *= vec3(diffuse) + V.spot_colour.rgb * lobe;
        if ((flags & kFlagSpecular) != 0u) {
            float s;
            if ((flags & kFlagSmooth) != 0u) { // a power of the sun's angle, by shininess
                const uint shininess = (p.flags.y >> 16) & 3u;
                const float kExponent[4] = float[4](8.0, 16.0, 32.0, 64.0);
                const float kScale[4] = float[4](1.6, 1.6, 2.4, 3.2);
                s = pow(sun, kExponent[shininess]) * kScale[shininess];
            } else { // flat: the sun reflected about the normal, toward the viewer (z)
                s = max(0.0, 2.0 * n_dot_sun * normal.z - V.sun.z);
            }
            s *= p.values.y * V.sun.w;
            if (p.colour.a < 1.0) a = max(a, s); // a highlight makes glass less clear
            rgb += vec3(s);
        }
    }
    rgb = min(rgb, vec3(1.0));
    const float fog = p.values.x;
    if (fog > 0.0) {
        const float f = fog * clamp(V.light.z + distance * V.light.y, 0.0, 1.0);
        const float spot = V.light.w * lobe_fog;
        rgb += (V.fog_colour.rgb + V.spot_fog_colour.rgb * spot * V.fog_colour_set.rgb - rgb) * f;
    }
}

#ifdef QUADS
// A quad's attributes at this pixel from all four corners: mean value
// coordinates of the pixel in the corners' screen positions, made
// perspective-correct with each corner's 1 / distance (the software
// renderer's quad_weights; Supermodel's R3DShaderQuads.h). False outside
// the quad.
bool quad_attributes(Polygon p, inout vec2 tc, inout float sh, inout vec3 n, inout float inv_distance) {
    const Quad q = quads[quad];
    const Viewport V = viewports[p.flags.z];
    const vec2 at = gl_FragCoord.xy / scale - vec2(extend, 0.0); // in screen pixels, as the corners
    float vx[4], vy[4], r[4], iw[4];
    for (int i = 0; i < 4; ++i) {
        const float w = -q.position[i].z;
        iw[i] = 1.0 / w;
        const float sx = V.rect.x + ((q.position[i].x / w) - V.frustum.x) / (V.frustum.y - V.frustum.x) * V.rect.z;
        const float sy = V.rect.y + (V.frustum.w - (q.position[i].y / w)) / (V.frustum.w - V.frustum.z) * V.rect.w;
        vx[i] = sx - at.x;
        vy[i] = sy - at.y;
        r[i] = sqrt(vx[i] * vx[i] + vy[i] * vy[i]);
        if (r[i] < 1e-6) { // on a corner
            tc = q.texture[i].xy;
            sh = q.texture[i].z;
            n = q.normal[i].xyz;
            inv_distance = iw[i];
            return true;
        }
    }
    float t[4];
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) & 3;
        const float area = vx[i] * vy[j] - vx[j] * vy[i];
        t[i] = area == 0.0 ? 0.0 : (r[i] * r[j] - (vx[i] * vx[j] + vy[i] * vy[j])) / area;
    }
    float u[4], weight[4], u_sum = 0.0, p_sum = 0.0;
    int signs = 0;
    for (int i = 0; i < 4; ++i) {
        const int prev = (i + 3) & 3;
        u[i] = (t[prev] + t[i]) / r[i];
        signs += t[prev] < -t[i] ? -1 : 1;
    }
    if (signs == 0) return false;
    for (int i = 0; i < 4; ++i) {
        weight[i] = u[i] * iw[i];
        p_sum += weight[i];
        u_sum += u[i];
    }
    if (p_sum == 0.0 || u_sum == 0.0 || isnan(p_sum)) return false;
    const float w = p_sum / u_sum;
    if (!(w > 0.0)) return false;
    tc = vec2(0.0);
    sh = 0.0;
    n = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        const float k = weight[i] / p_sum;
        tc += k * q.texture[i].xy;
        sh += k * q.texture[i].z;
        n += k * q.normal[i].xyz;
    }
    inv_distance = w;
    return true;
}
#endif

void main() {
    const Polygon p = polygons[polygon];
    const uint flags = p.flags.x;
    vec2 tc = uv;
    float sh = shade, inv_distance = gl_FragCoord.w;
    vec3 n = normal;
    float depth = gl_FragCoord.z;
#ifdef QUADS
    if (!quad_attributes(p, tc, sh, n, inv_distance)) discard; // outside the quad
    depth = kNear * ((flags & kFlagLayered) != 0u ? kDecalBias : 1.0) * inv_distance;
    gl_FragDepth = depth;
#endif
#ifndef OPAQUE
    // Only nearer than the opaque pixel (the depth buffer, cleared, then
    // keeps the nearest translucent one): as testing against a copy of the
    // opaque depth would.
    if (!(depth > texelFetch(opaque_depth, ivec2(gl_FragCoord.xy), 0).r)) discard;
#endif
    const float distance = 1.0 / inv_distance;
    const bool textured = (flags & kFlagTextured) != 0u, contour = (flags & kFlagContour) != 0u,
               texture_alpha = (flags & kFlagTextureAlpha) != 0u;
    vec3 rgb = p.colour.rgb;
    float texel_alpha = 1.0;
    if (textured) {
        const vec4 t = sample_texture(p, tc.x, tc.y, distance);
        rgb *= t.rgb;
        texel_alpha = t.a;
        if (contour && texel_alpha < 32.0 / 255.0) discard; // alpha test: cut out
        // Texture alpha counts only for the alpha test or with the polygon's texture alpha on.
        if (!contour && !texture_alpha) texel_alpha = 1.0;
    }
    float a;
#ifdef OPAQUE
    if (texture_alpha && texel_alpha < 1.0) discard; // left to a translucency layer
    a = 1.0;
#else
    a = texel_alpha * p.colour.a;
    if (texture_alpha && a >= 1.0) discard; // drawn in the opaque pass
#endif
    if (a < 1.0 / 32.0) discard;
    const vec2 c = gl_FragCoord.xy / scale - vec2(extend, 0.0);
    light_and_fog(p, c.x, c.y, distance, n, sh, rgb, a);
#ifdef OPAQUE
    colour = vec4(rgb, 1.0);
    layer0 = vec4(0.0);
    layer1 = vec4(0.0);
    depth_out = vec4(depth);
#else
    colour = vec4(rgb, a);
#endif
}

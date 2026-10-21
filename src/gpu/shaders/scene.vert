// A scene vertex: eye space to the polygon's viewport. Clip w is the
// distance along the view, and depth kNear / distance (decals a little
// nearer), so the depth buffer holds what the software renderer compares:
// larger is nearer. (Depth z / w greater than 1 is clipped: the near plane.)
#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_common.glsl"

layout(location = 0) in vec3 position; // eye space, z < 0 in front
layout(location = 1) in vec2 uv_in;    // texels
layout(location = 2) in float shade_in;
layout(location = 3) in vec3 normal_in;
layout(location = 4) in uint polygon_in;
layout(location = 5) in uint quad_in; // the quad drawn whole it belongs to, or kNoQuad

layout(std430, set = 0, binding = 0) readonly buffer Polygons { Polygon polygons[]; };
layout(std430, set = 0, binding = 1) readonly buffer Viewports { Viewport viewports[]; };
layout(std430, set = 0, binding = 2) readonly buffer Quads { Quad quads[]; };

layout(location = 0) out vec2 uv;
layout(location = 1) out float shade;
layout(location = 2) out vec3 normal;
layout(location = 3) flat out uint polygon;
layout(location = 4) flat out uint quad;

void main() {
    const Polygon p = polygons[polygon_in];
    const vec4 f = viewports[p.flags.z].frustum; // left, right, bottom, top
    const float w = -position.z;
    const float x = (2.0 * position.x - (f.x + f.y) * w) / (f.y - f.x);
    const float y = (2.0 * position.y - (f.z + f.w) * w) / (f.w - f.z);
    const float bias = (p.flags.x & kFlagLayered) != 0u ? kDecalBias : 1.0;
    gl_Position = vec4(x, y, kNear * bias, w);
    // A quad drawn whole wholly beyond the near plane: at its depth bound
    // (its nearest corner's, at most 1; Real3dGpu::build), constant over it,
    // which its own depth (scene.frag@QUADS@BOUNDED) never exceeds. It is
    // clipped as before: no corner is nearer than the near plane.
    if (quad_in != kNoQuad && quads[quad_in].position[0].w > 0.0) gl_Position.z = quads[quad_in].position[0].w * w;
    uv = uv_in;
    shade = shade_in;
    normal = normal_in;
    polygon = polygon_in;
    quad = quad_in;
}

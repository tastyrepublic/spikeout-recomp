// The finished frame (opaque), with the looks (look.glsl).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "look.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

layout(set = 2, binding = 0) uniform sampler2D frame;

void main() {
    colour = vec4(look(texture(frame, uv).rgb), 1.0);
}

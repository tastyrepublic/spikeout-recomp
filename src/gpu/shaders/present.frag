// The finished frame, as it is (opaque).
#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

layout(set = 2, binding = 0) uniform sampler2D frame;

void main() {
    colour = vec4(texture(frame, uv).rgb, 1.0);
}

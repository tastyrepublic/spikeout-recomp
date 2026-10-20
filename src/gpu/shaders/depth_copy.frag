// The opaque pass's depth into the depth buffer of a translucency layer,
// which is tested against it (and nearer translucent pixels replace
// farther ones).
#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour; // not written (the pipeline masks it)

layout(set = 2, binding = 0) uniform sampler2D opaque_depth;

void main() {
    gl_FragDepth = texelFetch(opaque_depth, ivec2(gl_FragCoord.xy), 0).r;
    colour = vec4(0.0);
}

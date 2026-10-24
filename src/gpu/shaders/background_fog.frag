// Fog over the 2D layers behind the 3D, drawn in its viewport before the
// scene (m3::Real3dScene::background_fog; the software renderer's
// draw_background_fog): a colour, plus the spotlight on fog in an ellipse.
#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

layout(std140, set = 3, binding = 0) uniform Fog {
    vec4 base;     // colour and alpha
    vec4 spot;     // the spotlight's colour on it (times attenuation), on (1/0)
    vec4 ellipse;  // centre x, y, radius x, y (screen pixels)
    float scale;   // render pixels per screen pixel
    float extend;  // widescreen: the screen's left edge, in screen pixels from the picture's
};

void main() {
    vec3 rgb = base.rgb;
    if (spot.w != 0.0) {
        const vec2 c = gl_FragCoord.xy / scale - vec2(extend, 0.0);
        const float ex = (c.x - ellipse.x) / ellipse.z, ey = (c.y - ellipse.y) / ellipse.w;
        rgb += spot.rgb * max(0.0, 1.0 - (ex * ex + ey * ey));
    }
    colour = vec4(rgb, base.a);
}

// The window's looks over the finished picture (gpu::Presenter::set_look):
//
// CRT colours, from Supermodel's (Src/Graphics/SuperAA.cpp, Copyright
// 2003-2026 The Supermodel Team, GPL-3.0): the picture back to linear light
// by the arcade monitor's gamma (or sRGB's curve), through the matrix from
// its phosphors and white to sRGB's (Dan Mons' colour_matrix_adaptations,
// https://github.com/danmons/colour_matrix_adaptations), and out by sRGB's
// curve.
//
// Screen filter, after vf3-recomp's (src/host/main.cpp, MIT, Copyright (c)
// 2026 mr-finster): the monitor's glass multiplied over the picture, at the
// window's own pixels. Scanlines: darker between the arcade's lines, fading
// in from 1.5 to 3 window pixels a line; CRT: shallower lines, an aperture
// grille (one colour a pixel column) and slightly darker corners. A gain
// keeps the picture about as bright.

layout(std140, set = 3, binding = 0) uniform Look {
    vec4 matrix[3]; // the colour matrix's rows (xyz); matrix[0].w the monitor's gamma (0: sRGB's curve)
    vec4 area;      // the picture in the window: x, y, width, height (pixels)
    vec4 screen;    // the filter's kind (0 none, 1 scanlines, 2 CRT), the picture's lines, the gain, CRT colours on (1)
};

float to_linear(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float from_linear(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

vec3 look(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    if (screen.w != 0.0) {
        c = matrix[0].w > 0.0 ? pow(c, vec3(matrix[0].w)) : vec3(to_linear(c.r), to_linear(c.g), to_linear(c.b));
        c = clamp(vec3(dot(matrix[0].xyz, c), dot(matrix[1].xyz, c), dot(matrix[2].xyz, c)), 0.0, 1.0);
        c = vec3(from_linear(c.r), from_linear(c.g), from_linear(c.b));
    }
    if (screen.x != 0.0) {
        const bool crt = screen.x == 2.0;
        const ivec2 at = ivec2(gl_FragCoord.xy - area.xy);
        const float per_line = area.w / screen.y;
        const float depth = (crt ? 0.45 : 0.6) * clamp((per_line - 1.5) / 1.5, 0.0, 1.0);
        // a whole number of window pixels a line, so every line looks the same
        const int period = max(1, int(round(per_line)));
        const float f = period == 2 ? float(at.y % 2) * 0.5 : (float(at.y % period) + 0.5) / float(period);
        vec3 glass = vec3(1.0 - depth * (1.0 - sin(f * 3.14159265)));
        if (crt) {
            const int column = at.x % 3;
            for (int i = 0; i < 3; ++i)
                if (i != column) glass[i] *= 0.78;
            const vec2 d2 = (vec2(at) + 0.5) / area.zw * 2.0 - 1.0;
            const float d = min(1.0, dot(d2, d2) / 2.0);
            glass *= 1.0 - 0.22 * d * d;
        }
        c = min(c * glass * screen.z, vec3(1.0));
    }
    return c;
}

// Draws the Real3D scene in m3::Real3d's memory into a 496x384 image: walks
// the viewports and their culling-node trees, decodes each model's
// polygons and rasterizes them in software (depth buffer, perspective-
// correct textures, sun and ambient light).
//
// The formats follow Supermodel's New3D (Src/Graphics/New3D: New3D.cpp,
// PolyHeader.h, R3DShaderCommon.h) and its notes; the code is ours. A first
// version: no spotlight, no specular, no LOD blending (the nearest
// level is drawn whole), no frustum culling (the rasterizer clips), and
// translucent polygons are blended in drawing order.
//
// Scene (word addresses into culling RAM, see Real3d):
//   Viewports: a list from 0x800000, linked by word 1 (0x01000000 ends
//   it). Word 0: bit 5 disabled, bits 3-4 priority; 2: the root node
//   pointer; 3: cot(angle); 4-7: sun direction (z, x, y) and intensity;
//   8-0xB: frustum (1/(l-r), 1/(t-b), t/b, l/r ratios); 0x14: size (14.2
//   fixed, height in the top half); 0x15: eye matrix index; 0x16: matrix
//   base; 0x17: LOD table; 0x1A: position (12.4, y from the top); 0x24
//   bits 8-15 ambient light.
//   Node pointers: bits 24-26 the kind (0 culling node, 1 model, 4 a list
//   of node pointers ending with bit 25).
//   Culling node (10 words, Step 1.5 and later): 0: flags (bits 0-1 type,
//   2 colour table, 3 four-level LOD table, 4 translation instead of a
//   matrix, 7 reset the rotation, 8-9 hidden); 1: model scale; 2: texture
//   offset (bit 15 on, 7-12 x / 32, 0-4 y / 32, 14 page); 3: matrix index
//   (bits 0-11), LOD table (12-18), colour table bits; 4-6: translation;
//   7: child (bit 24 set: the LOD entries are models); 8: sibling (bit 24
//   set: none); 9: blend and cull radius (16-bit floats).
//   Matrices: 12 floats from the matrix base, translation first, then the
//   rotation's three rows.
//   Models: polygons of a 7-word header and 4 words per vertex not taken
//   from the previous polygon (x, y, z as 13.11 fixed point with a normal
//   byte below each; texture u, v as 16-bit halves), see Real3dRenderer.cpp.
#pragma once

#include "runtime/real3d.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace m3 {

class Real3dRenderer {
public:
    static constexpr int kWidth = 496, kHeight = 384;

    // `threads`: how many to draw with (0: one per core, up to 8; 1: none).
    explicit Real3dRenderer(int threads = 0);

    // Draws the scene; pixels are 0xAARRGGBB, alpha 0 where nothing was drawn.
    void render(const Real3d& mem);
    std::span<const uint32_t> image() const { return color_; }

    struct Stats {
        uint32_t viewports = 0, nodes = 0, models = 0, polygons = 0, triangles_drawn = 0;
        uint32_t models_in_polygon_ram = 0, hidden_nodes = 0, lod_rejected = 0, frustum_culled = 0, faded_out = 0;
        uint32_t spotlights = 0; // viewports with the spotlight on
        uint32_t scroll_fog = 0, ambient_fog = 0;                // drawn this frame (0 or 1)
        uint32_t micro_polys = 0, translator_polys = 0, inverted_polys = 0;
        double setup_ms = 0, raster_ms = 0; // walking the scene and setting up triangles; drawing them
    };
    const Stats& stats() const { return stats_; }

    // The last frame's viewports, for reports.
    struct ViewportInfo {
        int priority;
        float x, y, width, height;
        uint32_t addr, eye, models;
        bool disabled;
        uint32_t spot_words[5]; // 0x1D-0x21: the spotlight
    };
    const std::vector<ViewportInfo>& viewport_info() const { return viewport_info_; }

    // Debugging: every triangle that covered one pixel in the last frame, in
    // drawing order, and what became of it there.
    struct ProbeHit {
        uint32_t polygon;               // address of its header
        std::array<uint32_t, 7> header;
        int priority, format;
        bool textured, contour;
        float alpha, texel_alpha, w;
        const char* pass;
        const char* outcome;
    };
    void set_probe(int x, int y) {
        probe_x_ = x;
        probe_y_ = y;
    }
    const std::vector<ProbeHit>& probe() const { return probe_hits_; }

    // Quads drawn whole (mean value coordinates, Supermodel's optional "proper
    // quad rendering"), or as two triangles (default: the same picture for
    // flat, linearly mapped quads, and cheaper).
    void set_quads(bool on) { quads_on_ = on; }

    struct Mat4 {
        std::array<float, 16> m{}; // column-major: m[col * 4 + row]
        static Mat4 identity();
        Mat4 operator*(const Mat4& b) const;
    };

private:
    struct Viewport {
        int priority = 0;
        float x = 0, y = 0, width = 0, height = 0; // pixels, y from the top
        float left = -1, right = 1, bottom = -1, top = 1;
        float cota = 1; // word 3, for texture levels
        std::array<float, 3> sun{};
        float sun_intensity = 0, ambient = 0;
        // Fog: colour (times fog ambient), density per unit of distance, start;
        // the colour as set and the attenuation, for the spotlight on fog.
        std::array<float, 3> fog_colour{}, fog_colour_set{};
        float fog_density = 0, fog_start = 0, fog_attenuation = 0;
        // Spotlight: an ellipse on the screen (centre, radii in pixels) lit
        // from a distance on, falling off over its extent; its colours.
        float spot_x = 0, spot_y = 0, spot_rx = 1, spot_ry = 1, spot_start = 0, spot_extent = 0;
        std::array<float, 3> spot_colour{}, spot_fog_colour{};
        bool spot = false; // has a start distance
        // Scroll and ambient fog: the fog drawn over the 2D layers behind the
        // 3D (word 0x20 low byte; 0x24 low byte the scroll attenuation; 0x25
        // bits 16-23 the fog ambient).
        float scroll_fog = 0, scroll_attenuation = 0, fog_ambient = 1;
        size_t first_model = 0, model_count = 0; // into models_
    };
    struct ModelRef {
        Mat4 matrix;
        uint32_t addr = 0;
        int tex_x = 0, tex_y = 0, page = 0; // texture offset and page flip of its node
        uint32_t colour_table = 0;
        float alpha = 1; // its nodes' fade (level of detail), times each polygon's
        float scale = 1; // its node's model scale: normals are divided by it
    };
    struct Attribs {
        int tex_x = 0, tex_y = 0, page = 0;
        bool no_culling = false;
        float model_scale = 1; // scales the nodes' cull and blend radii
        float alpha = 1;
    };

    void walk_viewports(uint32_t addr);
    void walk_pointer(uint32_t ptr, int depth);
    void walk_node(uint32_t addr, int depth);
    void add_model(uint32_t addr);
    void multiply_matrix(uint32_t index);
    void multiply_inverse_matrix(uint32_t index);
    void reset_rotation();
    float culling_float(uint32_t addr) const;

    // A triangle set up for drawing: screen bounds, edge functions and the
    // planes of 1/w and of each attribute over w (all linear on screen).
    struct Triangle {
        int x0, x1, y0, y1;
        float A[3], B[3], C[3];
        // Planes over 1/w: w itself, fixed shade, texture u and v, the normal.
        float wdx, wdy, wc, sdx, sdy, sc, udx, udy, uc, vdx, vdy, vc;
        float ndx[3], ndy[3], nc[3];
        float r, g, b, alpha;
        float fog; // intensity, 0 = none
        // Lighting: off for luminous polygons; fixed shading (the shade in
        // place of the sun's angle); specular, its strength and shininess
        // (0-3), smooth shading.
        bool lit, fixed_shading, specular, smooth;
        float specular_value;
        int shininess;
        uint16_t viewport; // index into viewports_
        bool textured, contour, texture_alpha; // contour: alpha test (cut-out)
        // Drawing order: translucent (polygon or node alpha), which of the two
        // translucency layers, high priority (over the rest of its priority),
        // layered (a decal, after the rest of its viewport).
        bool poly_alpha, high_priority, layered;
        uint8_t trans_layer;
        int format;
        // Texture: position in its page, size, wrap per axis (0 repeat, 1
        // clamp within the texture, 2 mirror), mipmap level at 1/w = 1 and the
        // last level.
        uint32_t tx, ty, tw, th, page_y;
        uint8_t wrap_u, wrap_v;
        float lod_base;
        int max_level;
        // Microtexture: a 128x128 detail texture in the other page, blended in
        // close up (id, finest scale); inverted: the texture's colours inverted.
        bool micro, inverted;
        int micro_id, micro_min_lod;
        int32_t quad; // the quad it is half of (index into quads_), or -1: a triangle
        uint32_t polygon; // for the probe
        int priority;
    };

    // A quad drawn whole, as the Real3D does: its two triangles say which
    // pixels it covers, and every pixel takes its attributes from all four
    // corners (mean value coordinates, perspective-correct; Supermodel's
    // R3DShaderQuads.h). Per corner: screen position, 1/w, and u, v, fixed
    // shade, normal x, y, z.
    struct Quad {
        float sx[4], sy[4], iw[4];
        float attr[4][6];
    };
    // The attributes at one pixel.
    struct PixelAttrs {
        float u, v, shade, nx, ny, nz;
    };
    // The corners' weights at a pixel (summing to 1, perspective-correct)
    // and the depth there; false when the pixel is outside the quad.
    static bool quad_weights(const Quad& q, float px, float py, float weight[4], float& w);

    void draw_model(const Viewport& vp, const ModelRef& model); // sets up its triangles
    void raster(int first, int step);                           // draws them on lines first, first + step, ...

    const Real3d* mem_ = nullptr;
    std::vector<Triangle> triangles_;
    std::vector<Quad> quads_;
    bool quads_on_ = false;
    std::vector<size_t> priority_starts_; // first triangle of each priority (depth is cleared there)
    int threads_ = 1;
    std::vector<Viewport> viewports_;
    std::vector<ModelRef> models_;
    Mat4 matrix_;
    Mat4 coord_sys_; // the eye's rotation, for nodes that reset theirs
    Attribs attribs_;
    uint32_t matrix_base_ = 0, lod_table_ = 0, colour_table_ = 0;
    // The viewport's frustum planes (words 0x0C-0x13): left u, v; top u, w;
    // right u, v; bottom u, w.
    std::array<float, 8> planes_{};
    uint32_t visits_ = 0; // guards against loops in a broken scene

    std::vector<uint32_t> color_;
    std::vector<float> depth_; // 1 / distance, 0 = nothing
    std::vector<float> trans_depth_;           // depth for the translucent passes
    std::vector<uint8_t> stencil_;             // decals written here (one per pixel)
    std::array<std::vector<uint32_t>, 2> trans_; // the translucency layers, ARGB
    Stats stats_;
    std::vector<ViewportInfo> viewport_info_;
    int probe_x_ = -1, probe_y_ = -1;
    std::vector<ProbeHit> probe_hits_;
    std::array<float, 4> sample(const Triangle& T, float u, float v, float inv_w) const;
    // Ambient and scroll fog over the 2D layers, before the scene.
    void draw_background_fog();
    // Lighting (sun, ambient, spotlight, specular) and fog at one pixel.
    void light_and_fog(const Triangle& T, float cx, float cy, float inv_w, const PixelAttrs& at, float& r, float& g,
                       float& b, float& a) const;
};

} // namespace m3

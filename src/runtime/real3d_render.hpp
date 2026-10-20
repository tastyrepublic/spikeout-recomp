// Draws the Real3D scene in m3::Real3d's memory into a 496x384 image, in
// software: the scene (m3::Real3dScene: viewports, polygons in eye space)
// is clipped, projected and rasterized with a depth buffer, perspective-
// correct mipmapped textures, the sun, ambient, specular and spotlight,
// fog, and the hardware's drawing order (priorities, decals, two
// translucency layers). It is the reference for the GPU renderer, and what
// headless runs use.
//
// The formats follow Supermodel's New3D (Src/Graphics/New3D: New3D.cpp,
// PolyHeader.h, R3DShaderCommon.h) and its notes; the code is ours.
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
//   byte below each; texture u, v as 16-bit halves), see real3d_scene.cpp.
#pragma once

#include "runtime/real3d.hpp"
#include "runtime/real3d_scene.hpp"

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
    // The scene the last frame was drawn from.
    const Real3dScene& scene() const { return scene_; }

    struct Stats {
        uint32_t viewports = 0, nodes = 0, models = 0, polygons = 0, triangles_drawn = 0;
        uint32_t models_in_polygon_ram = 0, hidden_nodes = 0, lod_rejected = 0, frustum_culled = 0, faded_out = 0;
        uint32_t spotlights = 0; // viewports with the spotlight on
        uint32_t scroll_fog = 0, ambient_fog = 0;                // drawn this frame (0 or 1)
        bool block_culled = false; // the Real3D's block culling was on: white, no scene
        uint32_t micro_polys = 0, translator_polys = 0, inverted_polys = 0;
        double setup_ms = 0, raster_ms = 0; // walking the scene and setting up triangles; drawing them
    };
    const Stats& stats() const { return stats_; }

    // The last frame's viewports, for reports.
    using ViewportInfo = Real3dScene::ViewportInfo;
    const std::vector<ViewportInfo>& viewport_info() const { return scene_.viewport_info; }

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

private:
    using Viewport = Real3dScene::Viewport;
    using EyeVertex = Real3dScene::Vertex;

    // A triangle set up for drawing: its polygon's attributes, screen
    // bounds, edge functions and the planes of 1/w and of each attribute
    // over w (all linear on screen).
    struct Triangle : Real3dScene::Polygon {
        int x0, x1, y0, y1;
        float A[3], B[3], C[3];
        // Planes over 1/w: w itself, fixed shade, texture u and v, the normal.
        float wdx, wdy, wc, sdx, sdy, sc, udx, udy, uc, vdx, vdy, vc;
        float ndx[3], ndy[3], nc[3];
        int32_t quad; // the quad it is half of (index into quads_), or -1: a triangle
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

    void set_up(const Real3dScene::Polygon& p); // its triangles (and quad)
    void raster(int first, int step);           // draws them on lines first, first + step, ...

    const Real3d* mem_ = nullptr;
    Real3dSceneBuilder builder_;
    Real3dScene scene_;
    std::vector<Triangle> triangles_;
    std::vector<Quad> quads_;
    bool quads_on_ = false;
    std::vector<size_t> priority_starts_; // first triangle of each priority (depth is cleared there)
    int threads_ = 1;

    std::vector<uint32_t> color_;
    std::vector<float> depth_; // 1 / distance, 0 = nothing
    std::vector<float> trans_depth_;           // depth for the translucent passes
    std::vector<uint8_t> stencil_;             // decals written here (one per pixel)
    std::array<std::vector<uint32_t>, 2> trans_; // the translucency layers, ARGB
    Stats stats_;
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

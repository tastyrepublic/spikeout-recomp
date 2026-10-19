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
        uint32_t models_in_polygon_ram = 0, hidden_nodes = 0, lod_rejected = 0;
    };
    const Stats& stats() const { return stats_; }

    // The last frame's viewports, for reports.
    struct ViewportInfo {
        int priority;
        float x, y, width, height;
        uint32_t addr, eye, models;
        bool disabled;
    };
    const std::vector<ViewportInfo>& viewport_info() const { return viewport_info_; }

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
        std::array<float, 3> sun{};
        float sun_intensity = 0, ambient = 0;
        // Fog: colour (times fog ambient), density per unit of distance, start.
        std::array<float, 3> fog_colour{};
        float fog_density = 0, fog_start = 0;
        size_t first_model = 0, model_count = 0; // into models_
    };
    struct ModelRef {
        Mat4 matrix;
        uint32_t addr = 0;
        int tex_x = 0, tex_y = 0, page = 0; // texture offset and page flip of its node
        uint32_t colour_table = 0;
    };
    struct Attribs {
        int tex_x = 0, tex_y = 0, page = 0;
        bool no_culling = false;
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
        float wdx, wdy, wc, ldx, ldy, lc, udx, udy, uc, vdx, vdy, vc;
        float r, g, b, alpha;
        float fog, fog_density, fog_start, fog_r, fog_g, fog_b; // fog: intensity 0 = none
        bool textured, contour, mirror_u, mirror_v;
        int format;
        uint32_t tx, ty, umask, vmask, tw, th;
    };

    void draw_model(const Viewport& vp, const ModelRef& model); // sets up its triangles
    void raster(int y_begin, int y_end);                        // draws them, lines [y_begin, y_end)

    const Real3d* mem_ = nullptr;
    std::vector<Triangle> triangles_;
    std::vector<size_t> priority_starts_; // first triangle of each priority (depth is cleared there)
    int threads_ = 1;
    std::vector<Viewport> viewports_;
    std::vector<ModelRef> models_;
    Mat4 matrix_;
    Mat4 coord_sys_; // the eye's rotation, for nodes that reset theirs
    Attribs attribs_;
    uint32_t matrix_base_ = 0, lod_table_ = 0, colour_table_ = 0;
    uint32_t visits_ = 0; // guards against loops in a broken scene

    std::vector<uint32_t> color_;
    std::vector<float> depth_; // 1 / distance, 0 = nothing
    Stats stats_;
    std::vector<ViewportInfo> viewport_info_;
};

} // namespace m3

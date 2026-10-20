// The Real3D's scene for one frame, decoded from its memory (m3::Real3d):
// the viewports, and every polygon to draw with its vertices in eye space
// and its attributes, in drawing order. Both renderers draw from it: the
// software one (m3::Real3dRenderer) and the GPU one.
//
// Real3dSceneBuilder walks the viewports and their culling-node trees
// (matrices, frustum culling, level of detail) and decodes each model's
// polygons (colour, texture, lighting and drawing-order flags), dropping
// those facing away. The formats follow Supermodel's New3D (Src/Graphics/
// New3D: New3D.cpp, PolyHeader.h, R3DShaderCommon.h) and its notes; the code
// is ours. See real3d_render.hpp for the memory layout.
#pragma once

#include "runtime/real3d.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace m3 {

struct Mat4 {
    std::array<float, 16> m{}; // column-major: m[col * 4 + row]
    static Mat4 identity();
    Mat4 operator*(const Mat4& b) const;
};

struct Real3dScene {
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
    };

    // A vertex in eye space (z < 0 in front), with its texture position (in
    // texels), fixed shade and lighting normal.
    struct Vertex {
        float x, y, z;
        float u, v;
        float shade;
        float nx, ny, nz;
    };

    // A polygon's attributes, the same at all its vertices.
    struct Polygon {
        float r, g, b, alpha;
        float fog; // intensity, 0 = none
        // Lighting: off for luminous polygons; fixed shading (the shade in
        // place of the sun's angle); specular, its strength and shininess
        // (0-3), smooth shading.
        bool lit, fixed_shading, specular, smooth;
        float specular_value;
        int shininess;
        uint16_t viewport; // index into viewports
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
        bool translator; // colour through the translator map (for reports)
        uint32_t address; // of its header, for the probe
        int priority;
        uint32_t first_vertex = 0; // into vertices
        uint8_t vertex_count = 3;  // 3, or 4: a quad
    };

    // Polygons in drawing order: priority by priority, viewport by viewport.
    std::vector<Viewport> viewports;
    std::vector<Polygon> polygons;
    std::vector<Vertex> vertices;
    bool block_culled = false; // the Real3D's block culling was on: white, no scene

    struct Stats {
        uint32_t viewports = 0, nodes = 0, models = 0, polygons = 0;
        uint32_t models_in_polygon_ram = 0, hidden_nodes = 0, lod_rejected = 0, frustum_culled = 0, faded_out = 0;
        uint32_t spotlights = 0; // viewports with the spotlight on
    } stats;

    // Fog over the 2D layers behind the 3D, drawn before the scene so the
    // scene covers it (Supermodel's DrawAmbientFog and DrawScrollFog, its
    // "best guess at the logic based upon what games are doing"; Spikeout
    // sets scroll fog on one viewport of a priority):
    //  - ambient fog: in the lowest priority with viewports, the lowest fog
    //    ambient among those with fog (a density or start) and no scroll fog
    //    darkens its viewport: black at 1 - ambient;
    //  - scroll fog: the highest scroll fog value of the lowest priority that
    //    has one fills its viewport with its fog colour (times the fog
    //    ambient, plus the spotlight on fog) at that alpha, if some viewport
    //    with the same fog colour has fog or a spotlight on fog.
    struct BackgroundFog {
        int ambient = -1; // viewport darkened (-1: none), and by how much
        float ambient_alpha = 0;
        int scroll = -1; // viewport filled with scroll fog (-1: none)
        int light = -1;  // the viewport whose fog ambient and spotlight colour it
        float attenuation = 0; // of the spotlight on fog
        bool spot = false;     // the spotlight shows on it
    };
    BackgroundFog background_fog() const;

    // Whether a quad (its four vertices) is flat with its attributes linear
    // across it: then mean value coordinates draw it exactly as its two
    // triangles, and only other quads need drawing whole (--quads).
    static bool quad_is_linear(const Vertex* e);

    // The viewports as read, for reports.
    struct ViewportInfo {
        int priority;
        float x, y, width, height;
        uint32_t addr, eye, models;
        bool disabled;
        uint32_t spot_words[5]; // 0x1D-0x21: the spotlight
    };
    std::vector<ViewportInfo> viewport_info;
};

class Real3dSceneBuilder {
public:
    // Decodes the frame in `mem` into `scene` (its old contents replaced).
    void build(const Real3d& mem, Real3dScene& scene);

private:
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
    void decode_model(size_t viewport, const ModelRef& model); // its polygons into the scene

    const Real3d* mem_ = nullptr;
    Real3dScene* scene_ = nullptr;
    std::vector<ModelRef> models_;
    std::vector<std::pair<size_t, size_t>> viewport_models_; // first model and count, by viewport
    Mat4 matrix_;
    Mat4 coord_sys_; // the eye's rotation, for nodes that reset theirs
    Attribs attribs_;
    uint32_t matrix_base_ = 0, lod_table_ = 0, colour_table_ = 0;
    // The viewport's frustum planes (words 0x0C-0x13): left u, v; top u, w;
    // right u, v; bottom u, w.
    std::array<float, 8> planes_{};
    uint32_t visits_ = 0; // guards against loops in a broken scene
};

} // namespace m3

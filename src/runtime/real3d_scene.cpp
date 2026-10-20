#include "runtime/real3d_scene.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace m3 {

namespace {

constexpr int kMaxDepth = 64;
constexpr uint32_t kMaxVisits = 200000;
constexpr uint32_t kMaxPolygonsPerModel = 65536;
constexpr int kScreenHeight = 384;

float as_float(uint32_t w) { return std::bit_cast<float>(w); }

// The Real3D's 16-bit float (radii): a 6-bit exponent (above 31 it is
// negative: value - 64) over a 10-bit mantissa, no sign (the format
// Supermodel's R3DFloat::GetFloat16 describes).
float float16(uint32_t v) {
    const uint32_t e = (v >> 10) & 0x3F;
    const uint32_t biased = e <= 31 ? e + 127 : e + 127 - 64;
    return as_float(biased << 23 | (v & 0x3FF) << 13);
}

struct ModelVertex {
    float x = 0, y = 0, z = 0;    // model space
    float nx = 0, ny = 0, nz = 0; // normal (smooth shading)
    uint8_t shade = 0;            // the x word's low byte: a fixed shade (fixed shading)
    uint16_t u = 0, v = 0;        // texture coordinates as stored
};

} // namespace

Mat4 Mat4::identity() {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}

Mat4 Mat4::operator*(const Mat4& b) const {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Real3dScene::BackgroundFog Real3dScene::background_fog() const {
    BackgroundFog fog;
    float lowest = 1.0f;
    for (int priority = 0; priority < 4; ++priority) {
        bool any = false;
        for (size_t i = 0; i < viewports.size(); ++i) {
            const Viewport& v = viewports[i];
            if (v.priority != priority) continue;
            any = true;
            if (v.scroll_fog > 0 || (v.fog_density <= 0 && v.fog_start <= 0)) continue;
            if (v.fog_ambient < lowest) {
                fog.ambient = static_cast<int>(i);
                lowest = v.fog_ambient;
            }
        }
        if (fog.ambient >= 0 || any) break;
    }
    fog.ambient_alpha = 1.0f - lowest;

    for (int priority = 0; priority < 4 && fog.scroll < 0; ++priority)
        for (size_t i = 0; i < viewports.size(); ++i) {
            const Viewport& v = viewports[i];
            if (v.priority == priority && v.scroll_fog > 0 &&
                (fog.scroll < 0 || viewports[static_cast<size_t>(fog.scroll)].scroll_fog < v.scroll_fog))
                fog.scroll = static_cast<int>(i);
        }
    if (fog.scroll < 0) return fog;
    const Viewport& scroll = viewports[static_cast<size_t>(fog.scroll)];
    for (size_t i = 0; i < viewports.size(); ++i) {
        const Viewport& v = viewports[i];
        if (v.fog_colour_set != scroll.fog_colour_set) continue;
        const bool spot_on_fog = v.spot_fog_colour[0] > 0 || v.spot_fog_colour[1] > 0 || v.spot_fog_colour[2] > 0;
        const bool has_fog = v.fog_density > 0 || v.fog_start > 0 || v.scroll_attenuation > 0;
        const bool spot_fog = v.fog_attenuation > 0 && spot_on_fog;
        if (!has_fog && !spot_fog) continue;
        // With regular fog on, the hardware seems to skip the spotlight on fog (Supermodel, ECA).
        fog.light = static_cast<int>(i);
        fog.attenuation = has_fog && spot_fog ? 0.0f : v.fog_attenuation;
        fog.spot = fog.attenuation > 0 && spot_on_fog;
        return fog;
    }
    fog.scroll = -1; // no viewport lights it
    return fog;
}

float Real3dSceneBuilder::culling_float(uint32_t addr) const { return as_float(mem_->culling(addr)); }

void Real3dSceneBuilder::build(const Real3d& mem, Real3dScene& scene) {
    mem_ = &mem;
    scene_ = &scene;
    scene.viewports.clear();
    scene.polygons.clear();
    scene.vertices.clear();
    scene.viewport_info.clear();
    scene.stats = {};
    scene.block_culled = mem.block_culling();
    models_.clear();
    viewport_models_.clear();
    visits_ = 0;
    if (scene.block_culled) return; // no 3D: Supermodel clears to white, over the layers below
    walk_viewports(0x800000);
    // The polygons, priority by priority.
    for (int priority = 0; priority < 4; ++priority)
        for (size_t v = 0; v < scene.viewports.size(); ++v)
            if (scene.viewports[v].priority == priority)
                for (size_t i = 0; i < viewport_models_[v].second; ++i) decode_model(v, models_[viewport_models_[v].first + i]);
}

// ---- Viewports and nodes ----

void Real3dSceneBuilder::walk_viewports(uint32_t addr) {
    for (int n = 0; n < 256 && (addr & 0xFFFFFF) != 0; ++n) {
        const uint32_t* node = mem_->culling_ptr(addr);
        if (!node) return;
        Real3dScene::Viewport vp;
        vp.priority = static_cast<int>((node[0] >> 3) & 3);
        // Position 12.4 and size 14.2, in whole pixels (as Supermodel's GL
        // viewport): its bottom edge is placed from the bottom of the screen.
        {
            const float x = static_cast<float>(node[0x1A] & 0xFFFF) / 16.0f, y = static_cast<float>(node[0x1A] >> 16) / 16.0f;
            const float w = static_cast<float>(node[0x14] & 0xFFFF) / 4.0f, h = static_cast<float>(node[0x14] >> 16) / 4.0f;
            const int bottom = static_cast<int>(static_cast<float>(kScreenHeight) - (y + h)), height = static_cast<int>(h);
            vp.x = static_cast<float>(static_cast<int>(x));
            vp.width = static_cast<float>(static_cast<int>(w));
            vp.height = static_cast<float>(height);
            vp.y = static_cast<float>(kScreenHeight - (bottom + height));
        }
        const float cv = as_float(node[0x8]), cw = as_float(node[0x9]);
        const float io = as_float(node[0xA]), jo = as_float(node[0xB]);
        vp.cota = as_float(node[0x3]);
        vp.left = -jo / cv;
        vp.right = (1.0f - jo) / cv;
        vp.bottom = -(1.0f - io) / cw;
        vp.top = io / cw;
        // The sun as GL axes see it (x, y, z = word 5, -6, -4).
        vp.sun = {as_float(node[0x05]), -as_float(node[0x06]), -as_float(node[0x04])};
        vp.sun_intensity = std::clamp(as_float(node[0x07]), 0.0f, 1.0f);
        if (!(vp.sun_intensity == vp.sun_intensity)) vp.sun_intensity = 0; // NaN
        vp.ambient = static_cast<float>((node[0x24] >> 8) & 0xFF) / 255.0f;
        // Fog: 0x22 colour (RGB888), 0x23 density (float), 0x25 start (low
        // half, signed, / 255) and fog ambient (bits 16-23, / 255).
        const float fog_ambient = static_cast<float>((node[0x25] >> 16) & 0xFF) / 255.0f;
        vp.fog_ambient = fog_ambient;
        vp.scroll_fog = static_cast<float>(node[0x20] & 0xFF) / 255.0f;
        vp.scroll_attenuation = static_cast<float>(node[0x24] & 0xFF) / 255.0f;
        vp.fog_colour = {static_cast<float>((node[0x22] >> 16) & 0xFF) / 255.0f * fog_ambient,
                         static_cast<float>((node[0x22] >> 8) & 0xFF) / 255.0f * fog_ambient,
                         static_cast<float>(node[0x22] & 0xFF) / 255.0f * fog_ambient};
        vp.fog_density = std::abs(as_float(node[0x23]));
        if (!(vp.fog_density < 99999.0f)) vp.fog_density = 0; // NaN or huge: none
        vp.fog_start = static_cast<float>(static_cast<int16_t>(node[0x25] & 0xFFFF)) / 255.0f;
        vp.fog_colour_set = {static_cast<float>((node[0x22] >> 16) & 0xFF) / 255.0f,
                             static_cast<float>((node[0x22] >> 8) & 0xFF) / 255.0f,
                             static_cast<float>(node[0x22] & 0xFF) / 255.0f};
        vp.fog_attenuation = static_cast<float>((node[0x24] >> 16) & 0xFF) / 255.0f;
        // Spotlight (Supermodel's reading): 0x1E / 0x1D the centre x / y
        // (13.3, screen pixels) in the low halves and sizes in the high ones
        // (radius 2047 / size); 0x21 1 / start distance, 0x1F extent; 0x20
        // colour indices, RGB one bit each: the light (bits 11-13) and on fog
        // (bits 8-10).
        static constexpr std::array<std::array<float, 3>, 8> kSpotColours{{
            {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0, 1, 1}, {1, 0, 0}, {1, 0, 1}, {1, 1, 0}, {1, 1, 1}}};
        vp.spot_x = static_cast<float>(static_cast<int16_t>(node[0x1E] & 0xFFFF)) / 8.0f;
        vp.spot_y = static_cast<float>(static_cast<int16_t>(node[0x1D] & 0xFFFF)) / 8.0f;
        vp.spot_rx = std::round(2047.0f / std::max(1.0f, static_cast<float>(node[0x1E] >> 16)));
        vp.spot_ry = std::round(2047.0f / std::max(1.0f, static_cast<float>(node[0x1D] >> 16)));
        vp.spot_start = 1.0f / as_float(node[0x21]);
        vp.spot_extent = as_float(node[0x1F]);
        vp.spot = vp.spot_start == vp.spot_start && std::abs(vp.spot_start) < 1e30f; // not NaN or infinite
        if (!(vp.spot_extent == vp.spot_extent)) vp.spot_extent = 0;
        vp.spot_colour = kSpotColours[(node[0x20] >> 11) & 7];
        vp.spot_fog_colour = kSpotColours[(node[0x20] >> 8) & 7];
        auto black = [](const std::array<float, 3>& c) { return c[0] == 0 && c[1] == 0 && c[2] == 0; };
        if (black(vp.spot_colour) && black(vp.spot_fog_colour)) vp.spot = false; // both colours off: nothing to light
        if (vp.spot && vp.spot_extent != 0) ++scene_->stats.spotlights;
        matrix_base_ = node[0x16] & 0xFFFFFF;
        lod_table_ = node[0x17] & 0xFFFFFF;
        for (uint32_t i = 0; i < 8; ++i) planes_[i] = as_float(node[0x0C + i]);
        const size_t first_model = models_.size();
        attribs_ = {};

        // The eye: games store X, Y, Z but the Real3D takes Z, X, Y with
        // Y and Z flipped; this turns that back into GL axes.
        matrix_ = Mat4{};
        matrix_.m = {0, 0, -1, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 1};
        const uint32_t eye = node[0x15] & 0xFFF;
        if (eye == 0) {
            multiply_matrix(0);
            coord_sys_ = matrix_;
        } else {
            coord_sys_ = matrix_;
            multiply_inverse_matrix(eye);
        }
        coord_sys_.m[12] = coord_sys_.m[13] = coord_sys_.m[14] = 0;
        coord_sys_.m[15] = 1;

        if (!(node[0] & 0x20) && vp.width > 0 && vp.height > 0 && cv != 0 && cw != 0) walk_pointer(node[0x02], 0);
        const size_t model_count = models_.size() - first_model;
        scene_->viewports.push_back(vp);
        viewport_models_.emplace_back(first_model, model_count);
        scene_->viewport_info.push_back({vp.priority, vp.x, vp.y, vp.width, vp.height, addr, eye,
                                         static_cast<uint32_t>(model_count), (node[0] & 0x20) != 0,
                                         {node[0x1D], node[0x1E], node[0x1F], node[0x20], node[0x21]}});
        ++scene_->stats.viewports;
        if (node[0x01] == 0x01000000) return;
        addr = node[0x01];
    }
}

void Real3dSceneBuilder::walk_pointer(uint32_t ptr, int depth) {
    if ((ptr & 0xFFFFFF) == 0) return;
    switch ((ptr >> 24) & 5) {
    case 0: walk_node(ptr & 0xFFFFFF, depth); break;
    case 1: add_model(ptr & 0xFFFFFF); break;
    case 4: { // a list of culling nodes
        const uint32_t* list = mem_->culling_ptr(ptr & 0xFFFFFF);
        for (int i = 0; list && i < 4096; ++i) {
            const uint32_t e = mem_->culling((ptr & 0xFFFFFF) + static_cast<uint32_t>(i));
            if (!(e & 0x01000000)) walk_node(e & 0xFFFFFF, depth + 1);
            if (e & 0x02000000) break;
        }
        break;
    }
    default: break;
    }
}

void Real3dSceneBuilder::walk_node(uint32_t addr, int depth) {
    if (depth > kMaxDepth || ++visits_ > kMaxVisits) return;
    const uint32_t* node = mem_->culling_ptr(addr);
    if (!node || addr + 10 > 0x800000 + 0x40000) return;
    const uint32_t flags = node[0];
    if ((flags & 3) == 0) return;              // a viewport, not drawn from here
    if ((flags & 0x300) == 0x300) { // hidden
        ++scene_->stats.hidden_nodes;
        return;
    }
    ++scene_->stats.nodes;
    const uint32_t child = node[7] & 0x7FFFFFF;
    const uint32_t sibling = node[8] & 0x1FFFFFF;

    // Siblings first, as Supermodel does (a colour-table node has none).
    const bool colour_table_node = (flags & 7) == 6, point_ref = flags & 0x10000000;
    if ((!colour_table_node || point_ref) && sibling && !(sibling & 0x1000000)) walk_node(sibling, depth + 1);

    if (flags & 4) // the colour table address, in pieces
        colour_table_ = ((node[3] >> 19) | ((node[7] >> 28) << 13) | ((node[8] >> 25) << 17)) & 0xFFFFF;

    const Attribs saved_attribs = attribs_;
    const Mat4 saved_matrix = matrix_;
    if (node[2] & 0x8000) {
        attribs_.tex_x = 32 * static_cast<int>((node[2] >> 7) & 0x3F);
        attribs_.tex_y = 32 * static_cast<int>(node[2] & 0x1F);
        attribs_.page = static_cast<int>((node[2] >> 14) & 1);
    }
    if (node[1] & 1) attribs_.model_scale = as_float(node[1] & ~3u);
    if (node[1] & 2) attribs_.no_culling = true;
    if (flags & 0x10) { // translation only
        Mat4 t = Mat4::identity();
        t.m[12] = as_float(node[4]);
        t.m[13] = as_float(node[5]);
        t.m[14] = as_float(node[6]);
        matrix_ = matrix_ * t;
    } else if (node[3] & 0xFFF) {
        multiply_matrix(node[3] & 0xFFF);
    }
    if (flags & 0x80) reset_rotation();

    // Frustum: the node's bounding sphere (cull radius) against the
    // viewport's four planes; its position is the matrix's translation.
    const float x = matrix_.m[12], y = matrix_.m[13], z = matrix_.m[14];
    const float cull_radius = float16(node[9] & 0xFFFF) * attribs_.model_scale;
    const bool outside = (z * planes_[0] - x * planes_[1] > cull_radius || z * planes_[2] + y * planes_[3] > cull_radius ||
                         z * planes_[4] - x * planes_[5] > cull_radius || z * planes_[6] + y * planes_[7] > cull_radius);

    // Level of detail: blend radius over distance against the table's delete
    // sizes. Past a level's delete size the node fades by its blend factor,
    // and between two levels both are drawn, cross-faded (Supermodel).
    const uint32_t lod_index = (node[3] >> 12) & 0x7F;
    const bool no_lod = lod_index == 0x7F; // no table (point-light billboards)
    const float blend_radius = float16(node[9] >> 16) * attribs_.model_scale;
    float lod_scale = (attribs_.no_culling || no_lod) ? std::numeric_limits<float>::max()
                                                      : blend_radius / std::hypot(x, y, z);
    lod_scale = std::clamp(lod_scale, 0.0f, std::numeric_limits<float>::max()); // and NaN to 0
    if (!(lod_scale == lod_scale)) lod_scale = 0;
    auto lod_entry = [&](int level, int word) {
        return culling_float(lod_table_ + lod_index * 8 + static_cast<uint32_t>(level) * 2 + static_cast<uint32_t>(word));
    };
    auto delete_size = [&](int level) { return lod_entry(level, 0); };
    auto fade = [&](int level) { // a node's alpha for this level, snapped as the hardware's 1/32 steps would
        float a = std::clamp(lod_entry(level, 1) * (lod_scale - delete_size(level)), 0.0f, 1.0f);
        if (!(a == a)) a = 0;
        return a > 31.0f / 32.0f ? 1.0f : a < 1.0f / 32.0f ? 0.0f : a;
    };
    if (outside && !attribs_.no_culling) ++scene_->stats.frustum_culled;
    else if (!(no_lod || attribs_.no_culling || lod_scale >= delete_size(3))) ++scene_->stats.lod_rejected;
    if (attribs_.no_culling || (!outside && (no_lod || lod_scale >= delete_size(3)))) {
        if (flags & 8) { // a four-level LOD table at the child pointer
            int level = 0;
            for (; level < 3; ++level)
                if (lod_scale >= delete_size(level) && (mem_->culling(child + static_cast<uint32_t>(level)) & 0x1000000)) break;
            const float parent_alpha = attribs_.alpha;
            const float node_alpha = fade(level);
            auto draw_level = [&](int l) {
                const uint32_t entry = mem_->culling(child + static_cast<uint32_t>(l)) & 0xFFFFFF;
                if (child & 0x01000000) add_model(entry);
                else walk_node(entry, depth + 1);
            };
            attribs_.alpha = parent_alpha * node_alpha;
            draw_level(level);
            if (node_alpha < 1.0f && level != 3) { // the next level fades in as this one fades out
                attribs_.alpha = parent_alpha * (1.0f - node_alpha);
                draw_level(level + 1);
            }
        } else {
            attribs_.alpha *= no_lod ? 1.0f : fade(3);
            walk_pointer(child, depth + 1);
        }
    }
    matrix_ = saved_matrix;
    attribs_ = saved_attribs;
}

void Real3dSceneBuilder::add_model(uint32_t addr) {
    if (attribs_.alpha <= 0) { // faded out
        ++scene_->stats.faded_out;
        return;
    }
    ModelRef m;
    m.matrix = matrix_;
    m.addr = addr;
    m.tex_x = attribs_.tex_x;
    m.tex_y = attribs_.tex_y;
    m.page = attribs_.page;
    m.colour_table = colour_table_;
    m.scale = attribs_.model_scale;
    m.alpha = attribs_.alpha;
    if ((addr & 0xFFFFFF) < 0x100000) ++scene_->stats.models_in_polygon_ram;
    models_.push_back(m);
    ++scene_->stats.models;
}

// A Real3D matrix: translation, then the rotation's rows.
void Real3dSceneBuilder::multiply_matrix(uint32_t index) {
    const uint32_t at = matrix_base_ + index * 12;
    auto f = [&](uint32_t i) { return culling_float(at + i); };
    Mat4 r;
    r.m = {f(3), f(6), f(9), 0, f(4), f(7), f(10), 0, f(5), f(8), f(11), 0, f(0), f(1), f(2), 1};
    matrix_ = matrix_ * r;
}

void Real3dSceneBuilder::multiply_inverse_matrix(uint32_t index) {
    const uint32_t at = matrix_base_ + index * 12;
    auto f = [&](uint32_t i) { return culling_float(at + i); };
    // Inverse of a rotation plus translation: transpose, and -R^T t.
    Mat4 r;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) r.m[col * 4 + row] = f(3 + static_cast<uint32_t>(col * 3 + row));
    for (int row = 0; row < 3; ++row)
        r.m[12 + row] = -(f(3 + static_cast<uint32_t>(row)) * f(0) + f(6 + static_cast<uint32_t>(row)) * f(1) +
                          f(9 + static_cast<uint32_t>(row)) * f(2));
    r.m[15] = 1;
    matrix_ = matrix_ * r;
}

// Undo the rotation so far (keeping position and scale), then apply the
// eye's: the node faces the viewer (billboards).
void Real3dSceneBuilder::reset_rotation() {
    Mat4 inv = matrix_;
    std::swap(inv.m[1], inv.m[4]);
    std::swap(inv.m[2], inv.m[8]);
    std::swap(inv.m[6], inv.m[9]);
    inv.m[12] = inv.m[13] = inv.m[14] = 0;
    inv.m[15] = 1;
    for (int c = 0; c < 3; ++c) {
        const float s = std::hypot(inv.m[c * 4], inv.m[c * 4 + 1], inv.m[c * 4 + 2]);
        if (s > 0)
            for (int r = 0; r < 3; ++r) inv.m[c * 4 + r] /= s;
    }
    matrix_ = matrix_ * inv * coord_sys_;
}

// ---- Polygons ----

void Real3dSceneBuilder::decode_model(size_t viewport, const ModelRef& model) {
    const Real3dScene::Viewport& vp = scene_->viewports[viewport];
    const Mat4& M = model.matrix;
    // Normals turn with the model (rotation part, normalized per use).
    auto transform = [&](const ModelVertex& v) {
        return std::array<float, 3>{M.m[0] * v.x + M.m[4] * v.y + M.m[8] * v.z + M.m[12],
                                    M.m[1] * v.x + M.m[5] * v.y + M.m[9] * v.z + M.m[13],
                                    M.m[2] * v.x + M.m[6] * v.y + M.m[10] * v.z + M.m[14]};
    };
    auto rotate = [&](float x, float y, float z) {
        std::array<float, 3> n{M.m[0] * x + M.m[4] * y + M.m[8] * z, M.m[1] * x + M.m[5] * y + M.m[9] * z,
                               M.m[2] * x + M.m[6] * y + M.m[10] * z};
        const float len = std::hypot(n[0], n[1], n[2]);
        if (len > 0)
            for (float& c : n) c /= len;
        return n;
    };

    std::array<ModelVertex, 4> prev{};
    uint32_t a = model.addr;
    for (uint32_t count = 0; count < kMaxPolygonsPerModel; ++count) {
        uint32_t h[7];
        for (uint32_t i = 0; i < 7; ++i) h[i] = mem_->model(a + i);
        if (h[6] == 0) break; // end of the model
        const uint32_t polygon_addr = a;
        ++scene_->stats.polygons;
        const int n = (h[0] & 0x40) ? 4 : 3;
        std::array<ModelVertex, 4> v{};
        int j = 0;
        for (int i = 0; i < 4; ++i)
            if (h[0] & (1u << i)) v[static_cast<size_t>(j++)] = prev[static_cast<size_t>(i)];
        const int shared = j;
        uint32_t data = a + 7;
        for (; j < n; ++j, data += 4) {
            const uint32_t ix = mem_->model(data), iy = mem_->model(data + 1), iz = mem_->model(data + 2),
                           it = mem_->model(data + 3);
            ModelVertex& w = v[static_cast<size_t>(j)];
            w.x = static_cast<float>(static_cast<int32_t>(ix) >> 8) / 2048.0f;
            w.y = static_cast<float>(static_cast<int32_t>(iy) >> 8) / 2048.0f;
            w.z = static_cast<float>(static_cast<int32_t>(iz) >> 8) / 2048.0f;
            // Signed bytes as the hardware reads them: (2b + 1) / 255 (Supermodel).
            auto byte_float = [](uint32_t b) { return (2.0f * static_cast<float>(static_cast<int8_t>(b & 0xFF)) + 1.0f) / 255.0f; };
            w.nx = byte_float(ix);
            w.ny = byte_float(iy);
            w.nz = byte_float(iz);
            w.shade = static_cast<uint8_t>(ix & 0xFF);
            w.u = static_cast<uint16_t>(it >> 16);
            w.v = static_cast<uint16_t>(it);
        }
        prev = v;
        const bool last = h[1] & 4;
        a += 7 + static_cast<uint32_t>(n - shared) * 4;

        if ((h[0] & 0x300) != 0x300) { // not discarded
            // Colour: RGB in the header, or an entry of the colour table.
            uint32_t rgb;
            if (h[1] & 2) rgb = h[4] >> 8;
            else rgb = mem_->model(model.colour_table + ((h[4] >> 8) & 0xFFF)) & 0xFFFFFF;
            // The translator map (header 4 bit 7) scales the colour by 16.
            const float colour_scale = (h[4] & 0x80) ? 16.0f / 255.0f : 1.0f / 255.0f;
            const float cr = static_cast<float>((rgb >> 16) & 0xFF) * colour_scale, cg = static_cast<float>((rgb >> 8) & 0xFF) * colour_scale,
                        cb = static_cast<float>(rgb & 0xFF) * colour_scale;
            float alpha = (h[6] & 0x800000) ? 1.0f : static_cast<float>((h[6] >> 18) & 0x3F) / 32.0f;
            if ((h[0] & 0x300) == 0x200) alpha /= 2; // one discard bit: half as opaque (Supermodel)
            alpha *= model.alpha;
            const bool textured = h[6] & 0x400;
            const bool contour = h[6] & 0x80000000u;
            int format = static_cast<int>((h[6] >> 7) & 7);
            if (contour && format >= 1 && format <= 4) format += 7;
            // The alpha test is 1-bit; RGBA4 (format 7) has 16 levels and skips it (Supermodel).
            const bool alpha_test = contour && format != 7;
            const bool lighting = !(h[6] & 0x10000);
            const bool smooth = h[1] & 8;
            const bool fixed_shading = (h[1] & 0x20) && !smooth; // smooth shading's normals win
            const bool specular = h[0] & 0x80;
            const float uv_scale = (h[1] & 0x40) ? 1.0f : 0.125f;
            auto size = [](uint32_t s) { return 32 << (s >= 6 ? 0 : s); };
            const int tw = size((h[3] >> 3) & 7), th = size(h[3] & 7);
            const int tx = 32 * static_cast<int>(((h[4] & 0x1F) << 1) | ((h[5] >> 7) & 1)) + model.tex_x;
            const int page = static_cast<int>((h[4] >> 6) & 1) ^ model.page;
            const int ty = 32 * static_cast<int>(h[5] & 0x1F) + model.tex_y;
            // Wrap: mirrored (header 2), else repeating smoothly across the edge
            // (header 3) or clamped inside the texture; alpha-tested textures
            // never wrap smoothly (Supermodel).
            auto wrap_mode = [&](bool mirror, bool smooth_wrap) -> uint8_t {
                return mirror ? 2 : (smooth_wrap && !contour) ? 0 : 1;
            };
            const uint8_t wrap_u = wrap_mode(h[2] & 2, h[3] & 0x80), wrap_v = wrap_mode(h[2] & 1, h[3] & 0x40);
            const float fx = static_cast<float>(static_cast<int32_t>(h[1]) >> 8) / 4194304.0f,
                        fy = static_cast<float>(static_cast<int32_t>(h[2]) >> 8) / 4194304.0f,
                        fz = static_cast<float>(static_cast<int32_t>(h[3]) >> 8) / 4194304.0f;
            std::array<float, 3> face = rotate(fx, fy, fz);
            // Normals for lighting: turned with the model and divided by its
            // node's model scale, not normalized (Supermodel: a model scaled up
            // without a model scale lights brighter, clamped later).
            const float inv_scale = model.scale != 0 ? 1.0f / model.scale : 1.0f;
            auto light_normal = [&](float x, float y, float z) {
                return std::array<float, 3>{(M.m[0] * x + M.m[4] * y + M.m[8] * z) * inv_scale,
                                            (M.m[1] * x + M.m[5] * y + M.m[9] * z) * inv_scale,
                                            (M.m[2] * x + M.m[6] * y + M.m[10] * z) * inv_scale};
            };
            const float face_scale = std::hypot(M.m[0] * fx + M.m[4] * fy + M.m[8] * fz, M.m[1] * fx + M.m[5] * fy + M.m[9] * fz,
                                                M.m[2] * fx + M.m[6] * fy + M.m[10] * fz);
            // Facing away from the eye: a one-sided polygon isn't drawn; a
            // two-sided one shows its back, lit from that side (Supermodel).
            const auto p0 = transform(v[0]);
            const bool back = p0[0] * face[0] + p0[1] * face[1] + p0[2] * face[2] > 0;
            const bool double_sided = h[1] & 0x10;
            if (back && !double_sided) {
                if (last) break;
                continue;
            }
            const float side = back ? -1.0f : 1.0f;
            for (float& c : face) c *= side;
            // Texture level: -log2((1/w)^2 * base), base = the plane's distance
            // from the eye along its (scaled) normal * cot of the view * the
            // polygon's texture NP (header 5, 10.14) (Supermodel, as the hardware).
            const float plane_distance = -(p0[0] * face[0] + p0[1] * face[1] + p0[2] * face[2]) * face_scale;
            const float lod_scale = plane_distance * vp.cota * static_cast<float>(h[5] >> 8) / 16384.0f;
            const float lod_base = lod_scale > 0 ? -std::log2(lod_scale) : 0.0f;
            int max_level = 0;
            for (int s = std::min(tw, th); s > 2; s >>= 1) ++max_level; // down to 2 texels

            Real3dScene::Polygon P{};
            P.first_vertex = static_cast<uint32_t>(scene_->vertices.size());
            P.vertex_count = static_cast<uint8_t>(n);
            for (int k = 0; k < n; ++k) {
                const ModelVertex& w = v[static_cast<size_t>(k)];
                const auto p = transform(w);
                const std::array<float, 3> normal = smooth ? light_normal(side * w.nx, side * w.ny, side * w.nz)
                                                           : light_normal(side * fx, side * fy, side * fz);
                // Fixed shading's value: signed, or unsigned with specular on (Supermodel).
                const float shade = specular ? static_cast<float>(w.shade) / 255.0f
                                             : (2.0f * static_cast<float>(static_cast<int8_t>(w.shade)) + 1.0f) / 255.0f;
                scene_->vertices.push_back({p[0], p[1], p[2], static_cast<float>(w.u) * uv_scale,
                                            static_cast<float>(w.v) * uv_scale, shade, normal[0], normal[1], normal[2]});
            }
            P.r = cr;
            P.g = cg;
            P.b = cb;
            P.alpha = alpha;
            // Fog intensity: header word 6 bits 11-15 / 16 (Supermodel's "light modifier").
            P.fog = static_cast<float>((h[6] >> 11) & 0x1F) / 16.0f;
            P.lit = lighting;
            P.fixed_shading = fixed_shading;
            P.specular = specular;
            P.smooth = smooth;
            P.specular_value = static_cast<float>(h[0] >> 26) / 63.0f;
            P.shininess = static_cast<int>((h[6] >> 5) & 3);
            P.viewport = static_cast<uint16_t>(viewport);
            P.textured = textured;
            P.contour = alpha_test;
            P.inverted = ((h[6] >> 24) & 0x7F) == 2;
            P.micro = textured && (h[2] & 0x10);
            P.micro_id = static_cast<int>((h[2] >> 5) & 7);
            P.micro_min_lod = static_cast<int>((h[2] >> 2) & 3);
            P.translator = h[4] & 0x80;
            P.texture_alpha = (h[6] & 7) != 0;
            P.poly_alpha = !(h[6] & 0x800000) || alpha < 1.0f;
            P.trans_layer = (h[6] & 0x20000) ? 1 : 0;
            P.high_priority = h[6] & 0x10;
            P.layered = h[6] & 0x8;
            P.format = format;
            P.wrap_u = wrap_u;
            P.wrap_v = wrap_v;
            P.tx = static_cast<uint32_t>(tx);
            P.ty = static_cast<uint32_t>(ty);
            P.page_y = static_cast<uint32_t>(page) * 1024;
            P.lod_base = lod_base;
            P.max_level = max_level;
            P.tw = static_cast<uint32_t>(tw);
            P.th = static_cast<uint32_t>(th);
            P.address = polygon_addr;
            P.priority = vp.priority;
            scene_->polygons.push_back(P);
        }
        if (last) break;
    }
}

} // namespace m3

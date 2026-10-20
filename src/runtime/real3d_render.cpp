#include "runtime/real3d_render.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

namespace m3 {

namespace {

constexpr float kNear = 1e-3f;
constexpr int kMaxDepth = 64;
constexpr uint32_t kMaxVisits = 200000;
constexpr uint32_t kMaxPolygonsPerModel = 65536;

float as_float(uint32_t w) { return std::bit_cast<float>(w); }

// The Real3D's 16-bit float (radii): a 6-bit exponent (above 31 it is
// negative: value - 64) over a 10-bit mantissa, no sign (the format
// Supermodel's R3DFloat::GetFloat16 describes).
float float16(uint32_t v) {
    const uint32_t e = (v >> 10) & 0x3F;
    const uint32_t biased = e <= 31 ? e + 127 : e + 127 - 64;
    return as_float(biased << 23 | (v & 0x3FF) << 13);
}

struct Vertex {
    float x = 0, y = 0, z = 0;    // model space
    float nx = 0, ny = 0, nz = 0; // normal (smooth shading)
    uint8_t shade = 0;            // the x word's low byte: a fixed shade (fixed shading)
    uint16_t u = 0, v = 0;        // texture coordinates as stored
};

// A vertex ready to draw: eye space, then screen.
struct EyeVertex {
    float x, y, z;    // eye space (z < 0 in front)
    float u, v;       // texels
    float shade;      // fixed shading's value
    float nx, ny, nz; // eye-space normal
};

EyeVertex lerp(const EyeVertex& p, const EyeVertex& q, float t) {
    auto l = [t](float a, float b) { return a + (b - a) * t; };
    return {l(p.x, q.x), l(p.y, q.y), l(p.z, q.z), l(p.u, q.u), l(p.v, q.v), l(p.shade, q.shade), l(p.nx, q.nx), l(p.ny, q.ny), l(p.nz, q.nz)};
}

// A texel to RGBA (0..1), by the polygon's texture format (Supermodel's
// R3DShaderCommon.h ExtractColour; formats 8-11 are 1-4 with contour on).
std::array<float, 4> texel_colour(int format, uint32_t t) {
    auto n = [](uint32_t v) { return static_cast<float>(v & 0xF) / 15.0f; };
    auto opaque_unless_white = [](float l) { return std::array<float, 4>{l, l, l, l == 1.0f ? 0.0f : 1.0f}; };
    switch (format) {
    case 0: // T1RGB5: bit 15 transparent
        return {static_cast<float>((t >> 10) & 0x1F) / 31.0f, static_cast<float>((t >> 5) & 0x1F) / 31.0f,
                static_cast<float>(t & 0x1F) / 31.0f, (t & 0x8000) ? 0.0f : 1.0f};
    case 1: return {n(t), n(t), n(t), n(t >> 4)};             // A4L4, low byte
    case 2: return {n(t >> 4), n(t >> 4), n(t >> 4), n(t)};   // L4A4, low byte
    case 3: return {n(t >> 8), n(t >> 8), n(t >> 8), n(t >> 12)};
    case 4: return {n(t >> 12), n(t >> 12), n(t >> 12), n(t >> 8)};
    case 5: return opaque_unless_white(static_cast<float>(t & 0xFF) / 255.0f);
    case 6: return opaque_unless_white(static_cast<float>((t >> 8) & 0xFF) / 255.0f);
    case 7: return {n(t >> 12), n(t >> 8), n(t >> 4), n(t)}; // RGBA4
    case 8: return opaque_unless_white(n(t));
    case 9: return opaque_unless_white(n(t >> 4));
    case 10: return opaque_unless_white(n(t >> 8));
    default: return opaque_unless_white(n(t >> 12));
    }
}

} // namespace

Real3dRenderer::Mat4 Real3dRenderer::Mat4::identity() {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}

Real3dRenderer::Mat4 Real3dRenderer::Mat4::operator*(const Mat4& b) const {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Real3dRenderer::Real3dRenderer(int threads)
    : color_(kWidth * kHeight), depth_(kWidth * kHeight), trans_depth_(kWidth * kHeight), stencil_(kWidth * kHeight),
      trans_{std::vector<uint32_t>(kWidth * kHeight), std::vector<uint32_t>(kWidth * kHeight)} {
    threads_ = threads > 0 ? threads : static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
}

float Real3dRenderer::culling_float(uint32_t addr) const { return as_float(mem_->culling(addr)); }

void Real3dRenderer::render(const Real3d& mem) {
    mem_ = &mem;
    stats_ = {};
    const auto start = std::chrono::steady_clock::now();
    viewports_.clear();
    viewport_info_.clear();
    models_.clear();
    probe_hits_.clear();
    visits_ = 0;
    if (mem.block_culling()) { // no 3D: Supermodel clears to white, over the layers below
        std::fill(color_.begin(), color_.end(), 0xFFFFFFFFu);
        stats_.block_culled = true;
        return;
    }
    walk_viewports(0x800000);
    // Set up every triangle, priority by priority, then draw them, each
    // thread taking every n-th line.
    triangles_.clear();
    quads_.clear();
    priority_starts_.clear();
    for (int priority = 0; priority < 4; ++priority) {
        priority_starts_.push_back(triangles_.size());
        for (const Viewport& vp : viewports_)
            if (vp.priority == priority)
                for (size_t i = 0; i < vp.model_count; ++i) draw_model(vp, models_[vp.first_model + i]);
    }
    const auto set_up = std::chrono::steady_clock::now();
    stats_.setup_ms = std::chrono::duration<double, std::milli>(set_up - start).count();
    std::fill(color_.begin(), color_.end(), 0u);
    draw_background_fog();
    if (threads_ <= 1) {
        raster(0, 1);
    } else {
        std::vector<std::thread> workers;
        for (int t = 0; t < threads_; ++t)
            workers.emplace_back([this, t] { raster(t, threads_); });
        for (std::thread& w : workers) w.join();
    }
    stats_.raster_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - set_up).count();
}

// ---- Scene ----

void Real3dRenderer::walk_viewports(uint32_t addr) {
    for (int n = 0; n < 256 && (addr & 0xFFFFFF) != 0; ++n) {
        const uint32_t* node = mem_->culling_ptr(addr);
        if (!node) return;
        Viewport vp;
        vp.priority = static_cast<int>((node[0] >> 3) & 3);
        // Position 12.4 and size 14.2, in whole pixels (as Supermodel's GL
        // viewport): its bottom edge is placed from the bottom of the screen.
        {
            const float x = static_cast<float>(node[0x1A] & 0xFFFF) / 16.0f, y = static_cast<float>(node[0x1A] >> 16) / 16.0f;
            const float w = static_cast<float>(node[0x14] & 0xFFFF) / 4.0f, h = static_cast<float>(node[0x14] >> 16) / 4.0f;
            const int bottom = static_cast<int>(static_cast<float>(kHeight) - (y + h)), height = static_cast<int>(h);
            vp.x = static_cast<float>(static_cast<int>(x));
            vp.width = static_cast<float>(static_cast<int>(w));
            vp.height = static_cast<float>(height);
            vp.y = static_cast<float>(kHeight - (bottom + height));
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
        if (vp.spot && vp.spot_extent != 0) ++stats_.spotlights;
        matrix_base_ = node[0x16] & 0xFFFFFF;
        lod_table_ = node[0x17] & 0xFFFFFF;
        for (uint32_t i = 0; i < 8; ++i) planes_[i] = as_float(node[0x0C + i]);
        vp.first_model = models_.size();
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
        vp.model_count = models_.size() - vp.first_model;
        viewports_.push_back(vp);
        viewport_info_.push_back({vp.priority, vp.x, vp.y, vp.width, vp.height, addr, eye,
                                  static_cast<uint32_t>(vp.model_count), (node[0] & 0x20) != 0,
                                  {node[0x1D], node[0x1E], node[0x1F], node[0x20], node[0x21]}});
        ++stats_.viewports;
        if (node[0x01] == 0x01000000) return;
        addr = node[0x01];
    }
}

void Real3dRenderer::walk_pointer(uint32_t ptr, int depth) {
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

void Real3dRenderer::walk_node(uint32_t addr, int depth) {
    if (depth > kMaxDepth || ++visits_ > kMaxVisits) return;
    const uint32_t* node = mem_->culling_ptr(addr);
    if (!node || addr + 10 > 0x800000 + 0x40000) return;
    const uint32_t flags = node[0];
    if ((flags & 3) == 0) return;              // a viewport, not drawn from here
    if ((flags & 0x300) == 0x300) { // hidden
        ++stats_.hidden_nodes;
        return;
    }
    ++stats_.nodes;
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
    if (outside && !attribs_.no_culling) ++stats_.frustum_culled;
    else if (!(no_lod || attribs_.no_culling || lod_scale >= delete_size(3))) ++stats_.lod_rejected;
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

void Real3dRenderer::add_model(uint32_t addr) {
    if (attribs_.alpha <= 0) { // faded out
        ++stats_.faded_out;
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
    if ((addr & 0xFFFFFF) < 0x100000) ++stats_.models_in_polygon_ram;
    models_.push_back(m);
    ++stats_.models;
}

// A Real3D matrix: translation, then the rotation's rows.
void Real3dRenderer::multiply_matrix(uint32_t index) {
    const uint32_t at = matrix_base_ + index * 12;
    auto f = [&](uint32_t i) { return culling_float(at + i); };
    Mat4 r;
    r.m = {f(3), f(6), f(9), 0, f(4), f(7), f(10), 0, f(5), f(8), f(11), 0, f(0), f(1), f(2), 1};
    matrix_ = matrix_ * r;
}

void Real3dRenderer::multiply_inverse_matrix(uint32_t index) {
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
void Real3dRenderer::reset_rotation() {
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

// ---- Drawing ----

void Real3dRenderer::draw_model(const Viewport& vp, const ModelRef& model) {
    const Mat4& M = model.matrix;
    // Normals turn with the model (rotation part, normalized per use).
    auto transform = [&](const Vertex& v) {
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
    const auto viewport_index = static_cast<uint16_t>(&vp - viewports_.data());

    std::array<Vertex, 4> prev{};
    uint32_t a = model.addr;
    for (uint32_t count = 0; count < kMaxPolygonsPerModel; ++count) {
        uint32_t h[7];
        for (uint32_t i = 0; i < 7; ++i) h[i] = mem_->model(a + i);
        if (h[6] == 0) break; // end of the model
        const uint32_t polygon_addr = a;
        ++stats_.polygons;
        const int n = (h[0] & 0x40) ? 4 : 3;
        std::array<Vertex, 4> v{};
        int j = 0;
        for (int i = 0; i < 4; ++i)
            if (h[0] & (1u << i)) v[static_cast<size_t>(j++)] = prev[static_cast<size_t>(i)];
        const int shared = j;
        uint32_t data = a + 7;
        for (; j < n; ++j, data += 4) {
            const uint32_t ix = mem_->model(data), iy = mem_->model(data + 1), iz = mem_->model(data + 2),
                           it = mem_->model(data + 3);
            Vertex& w = v[static_cast<size_t>(j)];
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

            std::array<EyeVertex, 4> e{};
            for (int k = 0; k < n; ++k) {
                const Vertex& w = v[static_cast<size_t>(k)];
                const auto p = transform(w);
                const std::array<float, 3> normal = smooth ? light_normal(side * w.nx, side * w.ny, side * w.nz)
                                                           : light_normal(side * fx, side * fy, side * fz);
                // Fixed shading's value: signed, or unsigned with specular on (Supermodel).
                const float shade = specular ? static_cast<float>(w.shade) / 255.0f
                                             : (2.0f * static_cast<float>(static_cast<int8_t>(w.shade)) + 1.0f) / 255.0f;
                e[static_cast<size_t>(k)] = {p[0], p[1], p[2], static_cast<float>(w.u) * uv_scale, static_cast<float>(w.v) * uv_scale,
                                             shade, normal[0], normal[1], normal[2]};
            }

            // Triangles (a quad is two), clipped at the near plane, set up for drawing.
            auto setup_triangle = [&](EyeVertex t0, EyeVertex t1, EyeVertex t2, int32_t quad) {
                // Clipped against the near plane and the view's four sides
                // (with a screen's margin around it, so edges stay exact), as a
                // GPU would: a large polygon passing close by the eye would
                // otherwise project millions of pixels wide.
                std::array<EyeVertex, 9> clipped{t0, t1, t2};
                int out = 3;
                const float margin_x = vp.right - vp.left, margin_y = vp.top - vp.bottom;
                const float L = vp.left - margin_x, R = vp.right + margin_x, B = vp.bottom - margin_y,
                            Tp = vp.top + margin_y;
                auto clip = [&](auto inside) { // keeps inside(p) >= 0
                    std::array<EyeVertex, 9> in = clipped;
                    const int n_in = out;
                    out = 0;
                    for (int k = 0; k < n_in; ++k) {
                        const EyeVertex& p = in[static_cast<size_t>(k)];
                        const EyeVertex& q = in[static_cast<size_t>((k + 1) % n_in)];
                        const float dp = inside(p), dq = inside(q);
                        if (dp >= 0 && out < 9) clipped[static_cast<size_t>(out++)] = p;
                        if ((dp >= 0) != (dq >= 0) && out < 9) clipped[static_cast<size_t>(out++)] = lerp(p, q, dp / (dp - dq));
                    }
                };
                clip([](const EyeVertex& p) { return -p.z - kNear; });
                clip([&](const EyeVertex& p) { return p.x + L * p.z; });   // x / w >= L (w = -z)
                clip([&](const EyeVertex& p) { return -R * p.z - p.x; });  // x / w <= R
                clip([&](const EyeVertex& p) { return p.y + B * p.z; });   // y / w >= B
                clip([&](const EyeVertex& p) { return -Tp * p.z - p.y; }); // y / w <= T
                for (int k = 1; k + 1 < out; ++k) {
                    const EyeVertex* tri[3] = {&clipped[0], &clipped[static_cast<size_t>(k)], &clipped[static_cast<size_t>(k + 1)]};
                    // Screen positions and 1/w for perspective-correct interpolation.
                    float sx[3], sy[3], iw[3];
                    for (int q = 0; q < 3; ++q) {
                        const float w = -tri[q]->z;
                        iw[q] = 1.0f / w;
                        sx[q] = vp.x + ((tri[q]->x / w) - vp.left) / (vp.right - vp.left) * vp.width;
                        sy[q] = vp.y + (vp.top - (tri[q]->y / w)) / (vp.top - vp.bottom) * vp.height;
                    }
                    const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
                    if (area == 0 || !(std::abs(area) < 1e12f)) continue; // degenerate or not a number
                    Triangle T{};
                    T.x0 = std::max({static_cast<int>(std::floor(std::min({sx[0], sx[1], sx[2]}))), static_cast<int>(vp.x), 0});
                    T.x1 = std::min({static_cast<int>(std::ceil(std::max({sx[0], sx[1], sx[2]}))), static_cast<int>(vp.x + vp.width), kWidth});
                    T.y0 = std::max({static_cast<int>(std::floor(std::min({sy[0], sy[1], sy[2]}))), static_cast<int>(vp.y), 0});
                    T.y1 = std::min({static_cast<int>(std::ceil(std::max({sy[0], sy[1], sy[2]}))), static_cast<int>(vp.y + vp.height), kHeight});
                    if (T.x0 >= T.x1 || T.y0 >= T.y1) continue;
                    // Barycentrics as edge functions, linear in x and y:
                    // b_i = A_i x + B_i y + C_i; attributes over w interpolate
                    // linearly too, so a pixel needs one divide.
                    const float inv_area = 1.0f / area;
                    for (int q = 0; q < 3; ++q) {
                        const int i1 = (q + 1) % 3, i2 = (q + 2) % 3;
                        T.A[q] = (sy[i1] - sy[i2]) * inv_area;
                        T.B[q] = (sx[i2] - sx[i1]) * inv_area;
                        T.C[q] = (sx[i1] * sy[i2] - sx[i2] * sy[i1]) * inv_area;
                    }
                    auto plane = [&](float a0, float a1, float a2, float& dx, float& dy, float& c0) {
                        const float v0 = a0 * iw[0], v1 = a1 * iw[1], v2 = a2 * iw[2];
                        dx = T.A[0] * v0 + T.A[1] * v1 + T.A[2] * v2;
                        dy = T.B[0] * v0 + T.B[1] * v1 + T.B[2] * v2;
                        c0 = T.C[0] * v0 + T.C[1] * v1 + T.C[2] * v2;
                    };
                    plane(1, 1, 1, T.wdx, T.wdy, T.wc);
                    if (lighting) {
                        if (fixed_shading) plane(tri[0]->shade, tri[1]->shade, tri[2]->shade, T.sdx, T.sdy, T.sc);
                        plane(tri[0]->nx, tri[1]->nx, tri[2]->nx, T.ndx[0], T.ndy[0], T.nc[0]);
                        plane(tri[0]->ny, tri[1]->ny, tri[2]->ny, T.ndx[1], T.ndy[1], T.nc[1]);
                        plane(tri[0]->nz, tri[1]->nz, tri[2]->nz, T.ndx[2], T.ndy[2], T.nc[2]);
                    }
                    if (textured) {
                        plane(tri[0]->u, tri[1]->u, tri[2]->u, T.udx, T.udy, T.uc);
                        plane(tri[0]->v, tri[1]->v, tri[2]->v, T.vdx, T.vdy, T.vc);
                    }
                    T.r = cr;
                    T.g = cg;
                    T.b = cb;
                    T.alpha = alpha;
                    // Fog intensity: header word 6 bits 11-15 / 16 (Supermodel's "light modifier").
                    T.fog = static_cast<float>((h[6] >> 11) & 0x1F) / 16.0f;
                    T.lit = lighting;
                    T.fixed_shading = fixed_shading;
                    T.specular = specular;
                    T.smooth = smooth;
                    T.specular_value = static_cast<float>(h[0] >> 26) / 63.0f;
                    T.shininess = static_cast<int>((h[6] >> 5) & 3);
                    T.viewport = viewport_index;
                    T.textured = textured;
                    T.contour = alpha_test;
                    T.inverted = ((h[6] >> 24) & 0x7F) == 2;
                    T.micro = textured && (h[2] & 0x10);
                    T.micro_id = static_cast<int>((h[2] >> 5) & 7);
                    T.micro_min_lod = static_cast<int>((h[2] >> 2) & 3);
                    stats_.micro_polys += T.micro ? 1 : 0;
                    stats_.translator_polys += (h[4] & 0x80) ? 1 : 0;
                    stats_.inverted_polys += T.inverted ? 1 : 0;
                    T.texture_alpha = (h[6] & 7) != 0;
                    T.poly_alpha = !(h[6] & 0x800000) || alpha < 1.0f;
                    T.trans_layer = (h[6] & 0x20000) ? 1 : 0;
                    T.high_priority = h[6] & 0x10;
                    T.layered = h[6] & 0x8;
                    T.format = format;
                    T.wrap_u = wrap_u;
                    T.wrap_v = wrap_v;
                    T.tx = static_cast<uint32_t>(tx);
                    T.ty = static_cast<uint32_t>(ty);
                    T.page_y = static_cast<uint32_t>(page) * 1024;
                    T.lod_base = lod_base;
                    T.max_level = max_level;
                    T.tw = static_cast<uint32_t>(tw);
                    T.th = static_cast<uint32_t>(th);
                    T.polygon = polygon_addr;
                    T.quad = quad;
                    T.priority = vp.priority;
                    triangles_.push_back(T);
                    ++stats_.triangles_drawn;
                }
            };
            // A quad with all four corners in front of the eye (and not absurdly
            // far off screen) is drawn whole; otherwise as two triangles.
            int32_t quad = -1;
            if (n == 4 && quads_on_) {
                Quad q{};
                bool ok = true;
                for (size_t k = 0; k < 4 && ok; ++k) {
                    const EyeVertex& p = e[k];
                    const float w = -p.z;
                    if (!(w >= kNear)) {
                        ok = false;
                        break;
                    }
                    q.iw[k] = 1.0f / w;
                    q.sx[k] = vp.x + ((p.x / w) - vp.left) / (vp.right - vp.left) * vp.width;
                    q.sy[k] = vp.y + (vp.top - (p.y / w)) / (vp.top - vp.bottom) * vp.height;
                    ok = std::abs(q.sx[k]) < 1e5f && std::abs(q.sy[k]) < 1e5f;
                    q.attr[k][0] = p.u;
                    q.attr[k][1] = p.v;
                    q.attr[k][2] = p.shade;
                    q.attr[k][3] = p.nx;
                    q.attr[k][4] = p.ny;
                    q.attr[k][5] = p.nz;
                }
                // Mean value coordinates reproduce anything linear across a flat
                // polygon, so a flat quad whose attributes at corner 3 follow
                // from the other three draws exactly as its two triangles: only
                // bent quads, or ones mapped non-linearly, need the quad path.
                if (ok) {
                    const EyeVertex &p0q = e[0], &p1q = e[1], &p2q = e[2], &p3q = e[3];
                    const float ax = p1q.x - p0q.x, ay = p1q.y - p0q.y, az = p1q.z - p0q.z;
                    const float bx = p2q.x - p0q.x, by = p2q.y - p0q.y, bz = p2q.z - p0q.z;
                    const float cx3 = p3q.x - p0q.x, cy3 = p3q.y - p0q.y, cz3 = p3q.z - p0q.z;
                    // p3 = p0 + s a + t b (least squares), then the leftover off that plane.
                    const float aa = ax * ax + ay * ay + az * az, ab = ax * bx + ay * by + az * bz,
                                bb = bx * bx + by * by + bz * bz, ac = ax * cx3 + ay * cy3 + az * cz3,
                                bc = bx * cx3 + by * cy3 + bz * cz3;
                    const float det = aa * bb - ab * ab;
                    bool linear = false;
                    if (det != 0) {
                        const float s = (ac * bb - bc * ab) / det, t = (bc * aa - ac * ab) / det;
                        const float rx = cx3 - s * ax - t * bx, ry = cy3 - s * ay - t * by, rz = cz3 - s * az - t * bz;
                        const float extent = std::max({aa, bb, cx3 * cx3 + cy3 * cy3 + cz3 * cz3});
                        linear = rx * rx + ry * ry + rz * rz <= extent * 1e-8f;
                        for (size_t c = 0; c < 6 && linear; ++c) {
                            const float predicted = q.attr[0][c] + s * (q.attr[1][c] - q.attr[0][c]) + t * (q.attr[2][c] - q.attr[0][c]);
                            const float scale = std::max({std::abs(q.attr[0][c]), std::abs(q.attr[1][c]), std::abs(q.attr[2][c]),
                                                          std::abs(q.attr[3][c]), 1.0f});
                            linear = std::abs(predicted - q.attr[3][c]) <= scale * 1e-4f;
                        }
                    }
                    if (!linear) {
                        quad = static_cast<int32_t>(quads_.size());
                        quads_.push_back(q);
                    }
                }
            }
            setup_triangle(e[0], e[1], e[2], quad);
            if (n == 4) setup_triangle(e[0], e[2], e[3], quad);
        }
        if (last) break;
    }
}

// The Real3D's drawing order (Supermodel's New3D RenderScene and
// R3DFrameBuffers, after the hardware): each priority is drawn twice, its
// ordinary polygons and then its high-priority ones, each time from a fresh
// depth buffer. Each time comes the opaque pass (a viewport's layered
// polygons, decals, after the rest of it, winning against the surface they
// sit on), then the translucent polygons into one of two translucency
// layers each. Within a layer a translucent polygon replaces what is there
// when it is nearer, with no blending (the chip makes translucency with two
// screen-door patterns), and an opaque pixel clears both layers. At the end
// the layers go over the opaque picture: one alone by its alpha, both
// together mixed into an opaque colour.
//
// Lines first, first + step, ...: each line is independent, so threads
// taking every n-th line give the same picture as one, with the work spread
// evenly.
void Real3dRenderer::raster(int first, int step) {
    enum class Pass { Opaque, Trans0, Trans1 };
    static constexpr const char* kPassName[3] = {"opaque pass", "translucency layer 0", "translucency layer 1"};
    auto to8 = [](float c) { return static_cast<uint32_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f); };
    auto rows = [&](auto&& f) {
        for (int y = first; y < kHeight; y += step) f(static_cast<size_t>(y) * kWidth);
    };
    rows([&](size_t row) {
        std::fill_n(trans_[0].begin() + static_cast<std::ptrdiff_t>(row), kWidth, 0u);
        std::fill_n(trans_[1].begin() + static_cast<std::ptrdiff_t>(row), kWidth, 0u);
    });

    auto draw_triangle = [&](const Triangle& T, Pass pass) {
        const bool opaque = pass == Pass::Opaque;
        const size_t layer = pass == Pass::Trans1 ? 1 : 0;
        const float bias = T.layered ? 1.0f + 1.0f / 512.0f : 1.0f; // decals win against their surface
        const int y0 = T.y0 + ((first - T.y0) % step + step) % step;  // this thread's first line
        for (int py = y0; py < T.y1; py += step) {
            const size_t row = static_cast<size_t>(py) * kWidth;
            const float cy = static_cast<float>(py) + 0.5f;
            float cx = static_cast<float>(T.x0) + 0.5f;
            float e0 = T.A[0] * cx + T.B[0] * cy + T.C[0], e1 = T.A[1] * cx + T.B[1] * cy + T.C[1];
            float* const z_row = (opaque ? depth_.data() : trans_depth_.data()) + row;
            uint8_t* const stencil_row = stencil_.data() + row;
            for (int px = T.x0; px < T.x1; ++px, cx += 1.0f, e0 += T.A[0], e1 += T.A[1]) {
                const float e2 = 1.0f - e0 - e1;
                if (e0 < 0 || e1 < 0 || e2 < 0) continue;
                const size_t at = row + static_cast<size_t>(px);
                const bool probing = py == probe_y_ && px == probe_x_;
                float weight[4];
                float w;
                if (T.quad >= 0) {
                    if (!quad_weights(quads_[static_cast<size_t>(T.quad)], cx, cy, weight, w)) continue; // outside the quad
                } else {
                    w = T.wdx * cx + T.wdy * cy + T.wc;
                }
                float texel_alpha = 1.0f;
                auto note = [&](const char* outcome) {
                    if (!probing) return;
                    ProbeHit hit{T.polygon, {}, T.priority, T.format, T.textured, T.contour, T.alpha, texel_alpha,
                                 w, kPassName[static_cast<int>(pass)], outcome};
                    for (uint32_t i = 0; i < 7; ++i) hit.header[i] = mem_->model(T.polygon + i);
                    probe_hits_.push_back(hit);
                };
                const float wz = w * bias;
                if (opaque ? wz < z_row[px] : wz <= z_row[px]) { // nearer (or as near, opaque) wins
                    note("behind (depth)");
                    continue;
                }
                if (T.layered && (stencil_row[px] & 0x7F)) {
                    note("a decal is already there");
                    continue;
                }
                const float inv_w = 1.0f / w;
                PixelAttrs pa;
                if (T.quad >= 0) {
                    const Quad& q = quads_[static_cast<size_t>(T.quad)];
                    float s[6] = {};
                    for (size_t k = 0; k < 4; ++k)
                        for (size_t c = 0; c < 6; ++c) s[c] += weight[k] * q.attr[k][c];
                    pa = {s[0], s[1], s[2], s[3], s[4], s[5]};
                } else {
                    pa = {};
                    if (T.textured) {
                        pa.u = (T.udx * cx + T.udy * cy + T.uc) * inv_w;
                        pa.v = (T.vdx * cx + T.vdy * cy + T.vc) * inv_w;
                    }
                    if (T.lit) {
                        if (T.fixed_shading) pa.shade = (T.sdx * cx + T.sdy * cy + T.sc) * inv_w;
                        pa.nx = (T.ndx[0] * cx + T.ndy[0] * cy + T.nc[0]) * inv_w;
                        pa.ny = (T.ndx[1] * cx + T.ndy[1] * cy + T.nc[1]) * inv_w;
                        pa.nz = (T.ndx[2] * cx + T.ndy[2] * cy + T.nc[2]) * inv_w;
                    }
                }
                float r = T.r, g = T.g, b = T.b;
                if (T.textured) {
                    const auto t = sample(T, pa.u, pa.v, inv_w);
                    r *= t[0];
                    g *= t[1];
                    b *= t[2];
                    texel_alpha = t[3];
                    if (T.contour && texel_alpha < 32.0f / 255.0f) { // alpha test: cut out
                        note("cut out (texel alpha)");
                        continue;
                    }
                    // Texture alpha counts only for the alpha test or with the
                    // polygon's texture alpha on.
                    if (!T.contour && !T.texture_alpha) texel_alpha = 1.0f;
                }
                float a;
                if (opaque) {
                    if (T.texture_alpha && texel_alpha < 1.0f) {
                        note("translucent texel: left to a translucency layer");
                        continue;
                    }
                    a = 1.0f;
                } else {
                    a = texel_alpha * T.alpha;
                    if (T.texture_alpha && a >= 1.0f) {
                        note("opaque texel: drawn in the opaque pass");
                        continue;
                    }
                }
                if (a < 1.0f / 32.0f) {
                    note("transparent (alpha)");
                    continue;
                }
                light_and_fog(T, cx, cy, inv_w, pa, r, g, b, a);
                const uint32_t rgb = to8(r) << 16 | to8(g) << 8 | to8(b);
                if (opaque) {
                    color_[at] = 0xFF000000u | rgb;
                    trans_[0][at] = trans_[1][at] = 0;
                } else {
                    trans_[layer][at] = to8(a) << 24 | rgb;
                }
                z_row[px] = wz;
                if (T.layered) stencil_row[px] = static_cast<uint8_t>((stencil_row[px] & 0x80) | ((stencil_row[px] + 1) & 0x7F));
                note(opaque ? "drawn" : "drawn into the layer");
            }
        }
    };

    for (size_t group = 0; group < priority_starts_.size(); ++group) {
        const size_t begin = priority_starts_[group];
        const size_t end = group + 1 < priority_starts_.size() ? priority_starts_[group + 1] : triangles_.size();
        const bool any_high = std::any_of(triangles_.begin() + static_cast<std::ptrdiff_t>(begin),
                                          triangles_.begin() + static_cast<std::ptrdiff_t>(end),
                                          [](const Triangle& T) { return T.high_priority; });
        // One pass over the priority's triangles: viewport by viewport, its
        // decals after the rest of it.
        auto draw_pass = [&](Pass pass, bool high) {
            for (size_t a = begin; a < end;) {
                size_t b = a;
                while (b < end && triangles_[b].viewport == triangles_[a].viewport) ++b;
                for (int decals = 0; decals < 2; ++decals)
                    for (size_t i = a; i < b; ++i) {
                        const Triangle& T = triangles_[i];
                        if (T.high_priority != high || T.layered != (decals == 1)) continue;
                        const bool wanted = pass == Pass::Opaque
                                                ? !T.poly_alpha
                                                : (T.texture_alpha || T.poly_alpha) &&
                                                      T.trans_layer == (pass == Pass::Trans1 ? 1 : 0);
                        if (wanted) draw_triangle(T, pass);
                    }
                a = b;
            }
        };
        for (int high = 0; high < 2; ++high) {
            if (high && !any_high) break;
            rows([&](size_t row) {
                std::fill_n(depth_.begin() + static_cast<std::ptrdiff_t>(row), kWidth, 0.0f);
                std::fill_n(stencil_.begin() + static_cast<std::ptrdiff_t>(row), kWidth, uint8_t{0});
            });
            draw_pass(Pass::Opaque, high == 1);
            // Each translucency layer is tested against the opaque depth.
            for (Pass pass : {Pass::Trans0, Pass::Trans1}) {
                rows([&](size_t row) {
                    std::copy_n(depth_.begin() + static_cast<std::ptrdiff_t>(row), kWidth,
                                trans_depth_.begin() + static_cast<std::ptrdiff_t>(row));
                });
                draw_pass(pass, high == 1);
            }
        }
    }

    // The translucency layers over the opaque picture.
    rows([&](size_t row) {
        auto ch = [](uint32_t c, int s) { return static_cast<float>((c >> s) & 0xFF) / 255.0f; };
        for (size_t i = row; i < row + kWidth; ++i) {
            const uint32_t t0 = trans_[0][i], t1 = trans_[1][i];
            if (!(t0 >> 24) && !(t1 >> 24)) continue;
            const uint32_t c = color_[i];
            uint32_t out;
            if ((t0 >> 24) && (t1 >> 24)) { // both: an opaque mix
                const float f = (ch(t1, 24) + (1.0f - ch(t0, 24))) / 2.0f;
                out = 0xFF000000u;
                for (int s : {16, 8, 0}) out |= to8(ch(t0, s) + (ch(t1, s) - ch(t0, s)) * f) << s;
            } else {
                // One layer over the 3D below it, straight (not premultiplied)
                // alpha, as the 2D compositor expects: where nothing is below,
                // the layer's own colour at its alpha (over the 2D layers).
                const uint32_t t = (t0 >> 24) ? t0 : t1;
                const float a = ch(t, 24), below = ch(c, 24) * (1.0f - a), oa = a + below;
                out = to8(oa) << 24;
                for (int s : {16, 8, 0}) out |= to8((ch(t, s) * a + ch(c, s) * below) / oa) << s;
            }
            color_[i] = out;
        }
    });
}

} // namespace m3

namespace m3 {
namespace {

// One texture axis: from coordinate t (texels of this level) to the two
// texels to blend and the second one's weight (0-256), for size s (a power of
// two) and wrap mode 0 repeat, 1 clamp within the texture, 2 mirror
// (Supermodel's LinearTexLocations, in texels).
struct Taps {
    uint32_t i0, i1, w;
};

Taps axis_taps(float t, int s, int mode) {
    if (!(std::abs(t) < 1e7f)) t = 0;
    const float fs = static_cast<float>(s);
    if (mode == 2) { // every other period backwards
        const float period = std::floor(t / fs);
        t -= period * fs;
        if (static_cast<int64_t>(period) & 1) t = fs - t;
    } else if (mode == 1) {
        t -= std::floor(t / fs) * fs;
    }
    t -= 0.5f; // texel centres
    const float fl = std::floor(t);
    int i0 = static_cast<int>(fl), i1 = i0 + 1;
    if (mode == 0) {
        i0 &= s - 1;
        i1 &= s - 1;
    } else {
        i0 = std::clamp(i0, 0, s - 1);
        i1 = std::clamp(i1, 0, s - 1);
    }
    return {static_cast<uint32_t>(i0), static_cast<uint32_t>(i1), static_cast<uint32_t>((t - fl) * 256.0f)};
}

// Every texel value of every format as RGBA8 (red in bits 0-7, alpha in
// 24-31), so sampling is a table look-up.
struct DecodeTables {
    std::vector<uint32_t> table = std::vector<uint32_t>(12 * 65536);
    DecodeTables() {
        auto to8 = [](float c) { return static_cast<uint32_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f); };
        for (int format = 0; format < 12; ++format)
            for (uint32_t t = 0; t < 65536; ++t) {
                const auto c = texel_colour(format, t);
                table[static_cast<size_t>(format) * 65536 + t] = to8(c[0]) | to8(c[1]) << 8 | to8(c[2]) << 16 | to8(c[3]) << 24;
            }
    }
};

const uint32_t* decode_table(int format) {
    static const DecodeTables tables;
    return tables.table.data() + static_cast<size_t>(std::clamp(format, 0, 11)) * 65536;
}

// a + (b - a) * w / 256 on all four bytes at once (two at a time).
uint32_t lerp8(uint32_t a, uint32_t b, uint32_t w) {
    const uint32_t iw = 256 - w;
    const uint32_t rb = (((a & 0x00FF00FFu) * iw + (b & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
    const uint32_t ag = (((a >> 8) & 0x00FF00FFu) * iw + ((b >> 8) & 0x00FF00FFu) * w) & 0xFF00FF00u;
    return rb | ag;
}

// log2 to about 0.01: enough to pick a texture level.
float fast_log2(float x) {
    const auto bits = std::bit_cast<uint32_t>(x);
    const float m = std::bit_cast<float>((bits & 0x007FFFFFu) | 0x3F800000u); // [1, 2)
    return static_cast<float>(static_cast<int>((bits >> 23) & 0xFF) - 127) + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}

} // namespace

// The texture at (u, v), in texels of the full-size level: bilinear within a
// mipmap level, blended with the next level by the fraction (Supermodel's
// texBiLinear and GetTextureValue). Level i of a texture at (x, y) is stored
// at (2048 - 2048 >> i, 1024 - 1024 >> i) + (x, y) >> i in its page.
std::array<float, 4> Real3dRenderer::sample(const Triangle& T, float u, float v, float inv_w) const {
    const float raw_lod = inv_w > 0 ? T.lod_base - 2.0f * fast_log2(inv_w) : 0.0f;
    float lod = raw_lod;
    if (!(lod > 0)) lod = 0; // and NaN
    lod = std::min(lod, static_cast<float>(T.max_level));
    const int level = static_cast<int>(lod);
    const auto frac = static_cast<uint32_t>((lod - static_cast<float>(level)) * 256.0f);
    const uint32_t* decode = decode_table(T.format);

    auto level_sample = [&](int l) {
        const int sw = std::max(1, static_cast<int>(T.tw) >> l), sh = std::max(1, static_cast<int>(T.th) >> l);
        const float scale = 1.0f / static_cast<float>(1 << l);
        const Taps a = axis_taps(u * scale, sw, T.wrap_u), b = axis_taps(v * scale, sh, T.wrap_v);
        const uint32_t bx = (2048u - (2048u >> l)) + (T.tx >> l);
        const uint32_t by = T.page_y + (1024u - (1024u >> l)) + ((T.ty & 1023u) >> l);
        auto texel = [&](uint32_t x, uint32_t y) { return decode[mem_->texel(bx + x, by + y)]; };
        uint32_t p00 = texel(a.i0, b.i0), p10 = texel(a.i1, b.i0), p01 = texel(a.i0, b.i1), p11 = texel(a.i1, b.i1);
        if (T.contour) { // cut-out texels take the colour of more opaque neighbours, so edges don't darken
            auto spread = [](uint32_t from, uint32_t& to) {
                if ((from >> 24) > (to >> 24)) to = (to & 0xFF000000u) | (from & 0x00FFFFFFu);
            };
            spread(p00, p10);
            spread(p00, p01);
            spread(p10, p00);
            spread(p10, p11);
            spread(p01, p00);
            spread(p01, p11);
            spread(p11, p01);
            spread(p11, p10);
        }
        return lerp8(lerp8(p00, p10, a.w), lerp8(p01, p11, a.w), b.w);
    };
    uint32_t c = level_sample(level);
    if (frac > 0 && level < T.max_level) {
        c = lerp8(c, level_sample(level + 1), frac);
    } else if (T.micro && raw_lod < 0) {
        // Close up, the microtexture: a 128x128 detail texture in the other
        // page (8 of them), repeating at 2, 4, 16 or 256 times the texture's
        // texel rate, blended in up to half as the texture is magnified
        // (Supermodel's GetTextureValue).
        static constexpr uint32_t kMicroX[8] = {0, 0, 128, 128, 0, 0, 128, 128};
        static constexpr uint32_t kMicroY[8] = {0, 128, 0, 128, 256, 384, 256, 384};
        static constexpr float kMicroScale[4] = {2, 4, 16, 256};
        const float s = kMicroScale[T.micro_min_lod];
        const Taps a = axis_taps(u * s, 128, 0), b = axis_taps(v * s, 128, 0);
        const uint32_t bx = kMicroX[T.micro_id], by = (T.page_y ^ 1024u) + kMicroY[T.micro_id];
        auto texel = [&](uint32_t x, uint32_t y) { return decode[mem_->texel(bx + x, by + y)]; };
        const uint32_t m = lerp8(lerp8(texel(a.i0, b.i0), texel(a.i1, b.i0), a.w), lerp8(texel(a.i0, b.i1), texel(a.i1, b.i1), a.w), b.w);
        const float blend = std::min(-raw_lod * std::exp2(-static_cast<float>(T.micro_min_lod)) * 0.5f, 0.5f);
        c = lerp8(c, m, static_cast<uint32_t>(blend * 256.0f));
    }
    if (T.inverted) c ^= 0x00FFFFFFu; // colours inverted, alpha kept
    constexpr float k = 1.0f / 255.0f;
    return {static_cast<float>(c & 0xFF) * k, static_cast<float>((c >> 8) & 0xFF) * k,
            static_cast<float>((c >> 16) & 0xFF) * k, static_cast<float>(c >> 24) * k};
}

} // namespace m3

namespace m3 {

// One pixel's lighting and fog (Supermodel's R3DShaderTriangles.h): the
// normal interpolated per pixel; light = sun angle (or the fixed shade) *
// sun intensity + ambient + the spotlight's colour * its lobe, with no upper
// limit on a Step 2 board, the colour clamped after; specular added on top;
// then fog toward its colour plus the spotlight on fog. `inv_w` is the
// distance along the view.
void Real3dRenderer::light_and_fog(const Triangle& T, float cx, float cy, float inv_w, const PixelAttrs& at, float& r,
                                   float& g, float& b, float& a) const {
    const Viewport& V = viewports_[T.viewport];
    float lobe = 0, lobe_fog = 0; // the spotlight here, and on fog
    if (V.spot) {
        const float ex = (cx - V.spot_x) / V.spot_rx, ey = (cy - V.spot_y) / V.spot_ry;
        const float ellipse = std::max(0.0f, 1.0f - (ex * ex + ey * ey));
        const float enable = inv_w >= V.spot_start ? 1.0f : 0.0f;
        if (V.spot_extent != 0) { // falls off with distance past start + extent
            const float extent = std::abs(V.spot_extent);
            const float d = std::min(V.spot_start + extent - inv_w, 0.0f);
            const float k = d / (1.0f + extent) - 1.0f;
            lobe = enable / (k * k) * ellipse;
        }
        lobe_fog = enable * ellipse;
    }
    if (T.lit) {
        const float nx = at.nx, ny = at.ny, nz = at.nz;
        const float n_dot_sun = nx * V.sun[0] + ny * V.sun[1] + nz * V.sun[2];
        const float sun = std::clamp(T.fixed_shading ? at.shade : n_dot_sun, 0.0f, 1.0f);
        const float diffuse = sun * V.sun_intensity + V.ambient;
        r *= diffuse + V.spot_colour[0] * lobe;
        g *= diffuse + V.spot_colour[1] * lobe;
        b *= diffuse + V.spot_colour[2] * lobe;
        if (T.specular) {
            float s;
            if (T.smooth) { // a power of the sun's angle, by shininess
                static constexpr float kExponent[4] = {8, 16, 32, 64}, kScale[4] = {1.6f, 1.6f, 2.4f, 3.2f};
                s = std::pow(sun, kExponent[T.shininess]) * kScale[T.shininess];
            } else { // flat: the sun reflected about the normal, toward the viewer (z)
                s = std::max(0.0f, 2.0f * n_dot_sun * nz - V.sun[2]);
            }
            s *= T.specular_value * V.sun_intensity;
            if (T.alpha < 1.0f) a = std::max(a, s); // a highlight makes glass less clear
            r += s;
            g += s;
            b += s;
        }
    }
    r = std::min(r, 1.0f);
    g = std::min(g, 1.0f);
    b = std::min(b, 1.0f);
    if (T.fog > 0) { // toward the fog colour (plus the spotlight on fog) with distance
        const float f = T.fog * std::clamp(V.fog_start + inv_w * V.fog_density, 0.0f, 1.0f);
        const float spot = V.fog_attenuation * lobe_fog;
        r += (V.fog_colour[0] + V.spot_fog_colour[0] * spot * V.fog_colour_set[0] - r) * f;
        g += (V.fog_colour[1] + V.spot_fog_colour[1] * spot * V.fog_colour_set[1] - g) * f;
        b += (V.fog_colour[2] + V.spot_fog_colour[2] * spot * V.fog_colour_set[2] - b) * f;
    }
}

} // namespace m3

namespace m3 {

// Fog over the 2D layers behind the 3D, drawn before the scene so the scene
// covers it (Supermodel's DrawAmbientFog and DrawScrollFog, its "best guess
// at the logic based upon what games are doing"; Spikeout sets scroll fog on
// one viewport of a priority):
//  - ambient fog: in the lowest priority with viewports, the lowest fog
//    ambient among those with fog (a density or start) and no scroll fog
//    darkens its viewport: black at 1 - ambient;
//  - scroll fog: the highest scroll fog value of the lowest priority that
//    has one fills its viewport with its fog colour (times the fog ambient,
//    plus the spotlight on fog) at that alpha, if some viewport with the
//    same fog colour has fog or a spotlight on fog.
void Real3dRenderer::draw_background_fog() {
    // Fills a viewport's rectangle; `constant`: the colour is the same everywhere.
    auto fill = [&](const Viewport& rect, bool constant, auto&& rgba) {
        const int x0 = std::max(0, static_cast<int>(rect.x)), x1 = std::min(kWidth, static_cast<int>(rect.x + rect.width));
        const int y0 = std::max(0, static_cast<int>(rect.y)), y1 = std::min(kHeight, static_cast<int>(rect.y + rect.height));
        if (x0 >= x1) return;
        auto pack = [](const std::array<float, 4>& c) {
            auto to8 = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            return to8(c[3]) << 24 | to8(c[0]) << 16 | to8(c[1]) << 8 | to8(c[2]);
        };
        const uint32_t same = constant ? pack(rgba(0.0f, 0.0f)) : 0;
        for (int y = y0; y < y1; ++y) {
            uint32_t* row = color_.data() + static_cast<size_t>(y) * kWidth;
            if (constant) {
                std::fill(row + x0, row + x1, same);
                continue;
            }
            for (int x = x0; x < x1; ++x) row[x] = pack(rgba(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f));
        }
    };

    const Viewport* ambient = nullptr;
    float lowest = 1.0f;
    for (int priority = 0; priority < 4; ++priority) {
        bool any = false;
        for (const Viewport& v : viewports_) {
            if (v.priority != priority) continue;
            any = true;
            if (v.scroll_fog > 0 || (v.fog_density <= 0 && v.fog_start <= 0)) continue;
            if (v.fog_ambient < lowest) {
                ambient = &v;
                lowest = v.fog_ambient;
            }
        }
        if (ambient || any) break;
    }
    if (ambient) ++stats_.ambient_fog;
    if (ambient) fill(*ambient, true, [&](float, float) { return std::array<float, 4>{0, 0, 0, 1.0f - lowest}; });

    const Viewport* scroll = nullptr;
    for (int priority = 0; priority < 4 && !scroll; ++priority)
        for (const Viewport& v : viewports_)
            if (v.priority == priority && v.scroll_fog > 0 && (!scroll || scroll->scroll_fog < v.scroll_fog)) scroll = &v;
    if (!scroll) return;
    for (const Viewport& v : viewports_) {
        if (v.fog_colour_set != scroll->fog_colour_set) continue;
        const bool spot_on_fog = v.spot_fog_colour[0] > 0 || v.spot_fog_colour[1] > 0 || v.spot_fog_colour[2] > 0;
        const bool fog = v.fog_density > 0 || v.fog_start > 0 || v.scroll_attenuation > 0;
        const bool spot_fog = v.fog_attenuation > 0 && spot_on_fog;
        if (!fog && !spot_fog) continue;
        // With regular fog on, the hardware seems to skip the spotlight on fog (Supermodel, ECA).
        const float attenuation = fog && spot_fog ? 0.0f : v.fog_attenuation;
        ++stats_.scroll_fog;
        const bool spot = attenuation > 0 && spot_on_fog;
        fill(*scroll, !spot, [&](float x, float y) {
            const float ex = (x - v.spot_x) / v.spot_rx, ey = (y - v.spot_y) / v.spot_ry;
            const float ellipse = spot ? std::max(0.0f, 1.0f - (ex * ex + ey * ey)) : 0.0f;
            std::array<float, 4> c{};
            for (size_t i = 0; i < 3; ++i)
                c[i] = scroll->fog_colour_set[i] * v.fog_ambient +
                       v.spot_fog_colour[i] * ellipse * scroll->fog_colour_set[i] * attenuation;
            c[3] = scroll->scroll_fog;
            return c;
        });
        return;
    }
}

} // namespace m3

namespace m3 {

// Mean value coordinates of a pixel in a quad's screen positions (Floater's
// generalized barycentrics, as Supermodel's quad shader: t_i =
// (r_i r_i+1 - v_i . v_i+1) / (v_i x v_i+1), weight_i = (t_i-1 + t_i) / r_i),
// then made perspective-correct with each corner's 1 / w. A pixel whose
// weights don't share a sign is outside the quad.
bool Real3dRenderer::quad_weights(const Quad& q, float px, float py, float weight[4], float& w) {
    float vx[4], vy[4], r[4];
    for (int i = 0; i < 4; ++i) {
        vx[i] = q.sx[i] - px;
        vy[i] = q.sy[i] - py;
        r[i] = std::sqrt(vx[i] * vx[i] + vy[i] * vy[i]);
        if (r[i] < 1e-6f) { // on a corner
            for (int k = 0; k < 4; ++k) weight[k] = k == i ? 1.0f : 0.0f;
            w = q.iw[i];
            return true;
        }
    }
    float t[4];
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) & 3;
        const float area = vx[i] * vy[j] - vx[j] * vy[i];
        t[i] = area == 0 ? 0.0f : (r[i] * r[j] - (vx[i] * vx[j] + vy[i] * vy[j])) / area;
    }
    float u[4], u_sum = 0, p_sum = 0;
    int signs = 0;
    for (int i = 0; i < 4; ++i) {
        const int prev = (i + 3) & 3;
        u[i] = (t[prev] + t[i]) / r[i];
        signs += t[prev] < -t[i] ? -1 : 1;
    }
    if (signs == 0) return false;
    for (int i = 0; i < 4; ++i) {
        weight[i] = u[i] * q.iw[i];
        p_sum += weight[i];
        u_sum += u[i];
    }
    if (p_sum == 0 || u_sum == 0 || !(p_sum == p_sum)) return false;
    w = p_sum / u_sum;
    for (int i = 0; i < 4; ++i) weight[i] /= p_sum;
    return w > 0;
}

} // namespace m3

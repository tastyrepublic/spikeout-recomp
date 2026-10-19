#include "runtime/real3d_render.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <thread>

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
    uint16_t u = 0, v = 0;        // texture coordinates as stored
};

// A vertex ready to draw: eye space, then screen.
struct EyeVertex {
    float x, y, z;    // eye space (z < 0 in front)
    float u, v;       // texels
    float light;      // 0..1+ multiplier
};

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

Real3dRenderer::Real3dRenderer(int threads) : color_(kWidth * kHeight), depth_(kWidth * kHeight) {
    threads_ = threads > 0 ? threads : static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
}

float Real3dRenderer::culling_float(uint32_t addr) const { return as_float(mem_->culling(addr)); }

void Real3dRenderer::render(const Real3d& mem) {
    mem_ = &mem;
    stats_ = {};
    viewports_.clear();
    viewport_info_.clear();
    models_.clear();
    visits_ = 0;
    walk_viewports(0x800000);
    // Set up every triangle, priority by priority, then draw them in bands
    // of lines, one per thread.
    triangles_.clear();
    priority_starts_.clear();
    for (int priority = 0; priority < 4; ++priority) {
        priority_starts_.push_back(triangles_.size());
        for (const Viewport& vp : viewports_)
            if (vp.priority == priority)
                for (size_t i = 0; i < vp.model_count; ++i) draw_model(vp, models_[vp.first_model + i]);
    }
    std::fill(color_.begin(), color_.end(), 0u);
    if (threads_ <= 1) {
        raster(0, kHeight);
        return;
    }
    std::vector<std::thread> workers;
    for (int t = 0; t < threads_; ++t)
        workers.emplace_back([this, t] { raster(kHeight * t / threads_, kHeight * (t + 1) / threads_); });
    for (std::thread& w : workers) w.join();
}

// ---- Scene ----

void Real3dRenderer::walk_viewports(uint32_t addr) {
    for (int n = 0; n < 256 && (addr & 0xFFFFFF) != 0; ++n) {
        const uint32_t* node = mem_->culling_ptr(addr);
        if (!node) return;
        Viewport vp;
        vp.priority = static_cast<int>((node[0] >> 3) & 3);
        vp.x = static_cast<float>(node[0x1A] & 0xFFFF) / 16.0f;
        vp.y = static_cast<float>(node[0x1A] >> 16) / 16.0f;
        vp.width = static_cast<float>(node[0x14] & 0xFFFF) / 4.0f;
        vp.height = static_cast<float>(node[0x14] >> 16) / 4.0f;
        const float cv = as_float(node[0x8]), cw = as_float(node[0x9]);
        const float io = as_float(node[0xA]), jo = as_float(node[0xB]);
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
        vp.fog_colour = {static_cast<float>((node[0x22] >> 16) & 0xFF) / 255.0f * fog_ambient,
                         static_cast<float>((node[0x22] >> 8) & 0xFF) / 255.0f * fog_ambient,
                         static_cast<float>(node[0x22] & 0xFF) / 255.0f * fog_ambient};
        vp.fog_density = std::abs(as_float(node[0x23]));
        if (!(vp.fog_density < 99999.0f)) vp.fog_density = 0; // NaN or huge: none
        vp.fog_start = static_cast<float>(static_cast<int16_t>(node[0x25] & 0xFFFF)) / 255.0f;
        matrix_base_ = node[0x16] & 0xFFFFFF;
        lod_table_ = node[0x17] & 0xFFFFFF;
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
                                  static_cast<uint32_t>(vp.model_count), (node[0] & 0x20) != 0});
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

    // Level of detail: blend radius over distance against the table's
    // delete sizes (the nearest level is drawn whole; no blending).
    const uint32_t lod_index = (node[3] >> 12) & 0x7F;
    const bool no_lod = lod_index == 0x7F;
    const float dist = std::hypot(matrix_.m[12], matrix_.m[13], matrix_.m[14]);
    const float blend_radius = float16(node[9] >> 16);
    float lod_scale = (attribs_.no_culling || no_lod || dist == 0) ? std::numeric_limits<float>::max() : blend_radius / dist;
    auto delete_size = [&](int level) { return culling_float(lod_table_ + lod_index * 8 + static_cast<uint32_t>(level) * 2); };
    if (!(no_lod || lod_scale >= delete_size(3) || attribs_.no_culling)) ++stats_.lod_rejected;
    if (no_lod || lod_scale >= delete_size(3) || attribs_.no_culling) {
        if (flags & 8) { // a four-level LOD table at the child pointer
            int level = 0;
            for (; level < 3; ++level)
                if (lod_scale >= delete_size(level) && (mem_->culling(child + static_cast<uint32_t>(level)) & 0x1000000)) break;
            const uint32_t entry = mem_->culling(child + static_cast<uint32_t>(level)) & 0xFFFFFF;
            if (child & 0x01000000) add_model(entry);
            else walk_node(entry, depth + 1);
        } else {
            walk_pointer(child, depth + 1);
        }
    }
    matrix_ = saved_matrix;
    attribs_ = saved_attribs;
}

void Real3dRenderer::add_model(uint32_t addr) {
    ModelRef m;
    m.matrix = matrix_;
    m.addr = addr;
    m.tex_x = attribs_.tex_x;
    m.tex_y = attribs_.tex_y;
    m.page = attribs_.page;
    m.colour_table = colour_table_;
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
    auto lit = [&](const std::array<float, 3>& n) {
        const float d = n[0] * vp.sun[0] + n[1] * vp.sun[1] + n[2] * vp.sun[2];
        return std::clamp(vp.ambient + vp.sun_intensity * std::max(0.0f, d), 0.0f, 1.0f);
    };

    std::array<Vertex, 4> prev{};
    uint32_t a = model.addr;
    for (uint32_t count = 0; count < kMaxPolygonsPerModel; ++count) {
        uint32_t h[7];
        for (uint32_t i = 0; i < 7; ++i) h[i] = mem_->model(a + i);
        if (h[6] == 0) break; // end of the model
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
            w.nx = static_cast<float>(static_cast<int8_t>(ix & 0xFF)) / 127.0f;
            w.ny = static_cast<float>(static_cast<int8_t>(iy & 0xFF)) / 127.0f;
            w.nz = static_cast<float>(static_cast<int8_t>(iz & 0xFF)) / 127.0f;
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
            const float cr = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, cg = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                        cb = static_cast<float>(rgb & 0xFF) / 255.0f;
            float alpha = (h[6] & 0x800000) ? 1.0f : static_cast<float>((h[6] >> 18) & 0x3F) / 32.0f;
            if ((h[0] & 0x300) == 0x200) alpha /= 2; // one discard bit: half as opaque (Supermodel)
            const bool textured = h[6] & 0x400;
            const bool contour = h[6] & 0x80000000u;
            int format = static_cast<int>((h[6] >> 7) & 7);
            if (contour && format >= 1 && format <= 4) format += 7;
            const bool lighting = !(h[6] & 0x10000);
            const bool smooth = h[1] & 8;
            const float uv_scale = (h[1] & 0x40) ? 1.0f : 0.125f;
            auto size = [](uint32_t s) { return 32 << (s >= 6 ? 0 : s); };
            const int tw = size((h[3] >> 3) & 7), th = size(h[3] & 7);
            const int tx = 32 * static_cast<int>(((h[4] & 0x1F) << 1) | ((h[5] >> 7) & 1)) + model.tex_x;
            const int page = static_cast<int>((h[4] >> 6) & 1) ^ model.page;
            const int ty = 32 * static_cast<int>(h[5] & 0x1F) + model.tex_y + page * 1024;
            const bool mirror_u = h[2] & 2, mirror_v = h[2] & 1;
            const std::array<float, 3> face = rotate(static_cast<float>(static_cast<int32_t>(h[1]) >> 8) / 4194304.0f,
                                                     static_cast<float>(static_cast<int32_t>(h[2]) >> 8) / 4194304.0f,
                                                     static_cast<float>(static_cast<int32_t>(h[3]) >> 8) / 4194304.0f);
            const float face_light = lighting ? lit(face) : 1.0f;

            std::array<EyeVertex, 4> e{};
            for (int k = 0; k < n; ++k) {
                const Vertex& w = v[static_cast<size_t>(k)];
                const auto p = transform(w);
                float light = face_light;
                if (lighting && smooth) light = lit(rotate(w.nx, w.ny, w.nz));
                e[static_cast<size_t>(k)] = {p[0], p[1], p[2], static_cast<float>(w.u) * uv_scale,
                                             static_cast<float>(w.v) * uv_scale, light};
            }

            // Triangles (a quad is two), clipped at the near plane, set up for drawing.
            auto setup_triangle = [&](EyeVertex t0, EyeVertex t1, EyeVertex t2) {
                std::array<EyeVertex, 3> poly{t0, t1, t2};
                std::array<EyeVertex, 4> clipped{};
                int out = 0;
                for (int k = 0; k < 3; ++k) { // keep z <= -near
                    const EyeVertex& p = poly[static_cast<size_t>(k)];
                    const EyeVertex& q = poly[static_cast<size_t>((k + 1) % 3)];
                    const bool pin = p.z <= -kNear, qin = q.z <= -kNear;
                    if (pin && out < 4) clipped[static_cast<size_t>(out++)] = p;
                    if (pin != qin && out < 4) {
                        const float t = (-kNear - p.z) / (q.z - p.z);
                        clipped[static_cast<size_t>(out++)] = {p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, -kNear,
                                                                p.u + (q.u - p.u) * t, p.v + (q.v - p.v) * t,
                                                                p.light + (q.light - p.light) * t};
                    }
                }
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
                    if (area == 0 || !(std::abs(area) < 1e9f)) continue;
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
                    plane(tri[0]->light, tri[1]->light, tri[2]->light, T.ldx, T.ldy, T.lc);
                    if (textured) {
                        plane(tri[0]->u, tri[1]->u, tri[2]->u, T.udx, T.udy, T.uc);
                        plane(tri[0]->v, tri[1]->v, tri[2]->v, T.vdx, T.vdy, T.vc);
                    }
                    T.r = cr;
                    T.g = cg;
                    T.b = cb;
                    T.alpha = alpha;
                    // Fog intensity: header word 6 bits 11-15 / 16 (Supermodel's "light modifier").
                    T.fog = vp.fog_density > 0 ? static_cast<float>((h[6] >> 11) & 0x1F) / 16.0f : 0.0f;
                    T.fog_density = vp.fog_density;
                    T.fog_start = vp.fog_start;
                    T.fog_r = vp.fog_colour[0];
                    T.fog_g = vp.fog_colour[1];
                    T.fog_b = vp.fog_colour[2];
                    T.textured = textured;
                    T.contour = contour;
                    T.format = format;
                    T.mirror_u = mirror_u;
                    T.mirror_v = mirror_v;
                    T.tx = static_cast<uint32_t>(tx);
                    T.ty = static_cast<uint32_t>(ty);
                    T.tw = static_cast<uint32_t>(tw);
                    T.th = static_cast<uint32_t>(th);
                    T.umask = T.tw - 1;
                    T.vmask = T.th - 1;
                    triangles_.push_back(T);
                    ++stats_.triangles_drawn;
                }
            };
            setup_triangle(e[0], e[1], e[2]);
            if (n == 4) setup_triangle(e[0], e[2], e[3]);
        }
        if (last) break;
    }
}

// Draws every triangle, in order, on lines [y_begin, y_end): each band of
// lines is independent, so bands run in parallel with the same result.
void Real3dRenderer::raster(int y_begin, int y_end) {
    auto to8 = [](float c) { return static_cast<uint32_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (size_t group = 0; group < priority_starts_.size(); ++group) {
        // Each priority draws over the ones below: a fresh depth buffer.
        std::fill(depth_.begin() + static_cast<std::ptrdiff_t>(y_begin) * kWidth,
                  depth_.begin() + static_cast<std::ptrdiff_t>(y_end) * kWidth, 0.0f);
        const size_t end = group + 1 < priority_starts_.size() ? priority_starts_[group + 1] : triangles_.size();
        for (size_t i = priority_starts_[group]; i < end; ++i) {
            const Triangle& T = triangles_[i];
            const int y0 = std::max(T.y0, y_begin), y1 = std::min(T.y1, y_end);
            for (int py = y0; py < y1; ++py) {
                const float cy = static_cast<float>(py) + 0.5f;
                float cx = static_cast<float>(T.x0) + 0.5f;
                float e0 = T.A[0] * cx + T.B[0] * cy + T.C[0], e1 = T.A[1] * cx + T.B[1] * cy + T.C[1];
                uint32_t* const color_row = color_.data() + static_cast<size_t>(py) * kWidth;
                float* const depth_row = depth_.data() + static_cast<size_t>(py) * kWidth;
                for (int px = T.x0; px < T.x1; ++px, cx += 1.0f, e0 += T.A[0], e1 += T.A[1]) {
                    const float e2 = 1.0f - e0 - e1;
                    if (e0 < 0 || e1 < 0 || e2 < 0) continue;
                    const float w = T.wdx * cx + T.wdy * cy + T.wc;
                    if (w <= depth_row[px]) continue;
                    const float inv_w = 1.0f / w;
                    const float light = (T.ldx * cx + T.ldy * cy + T.lc) * inv_w;
                    float r = T.r * light, g = T.g * light, b = T.b * light, al = T.alpha;
                    if (T.textured) {
                        const float u = (T.udx * cx + T.udy * cy + T.uc) * inv_w;
                        const float v = (T.vdx * cx + T.vdy * cy + T.vc) * inv_w;
                        // Power-of-two sizes: wrap with a mask; a mirrored
                        // axis flips every other period.
                        const auto iu = static_cast<uint32_t>(static_cast<int>(std::floor(u)));
                        const auto iv = static_cast<uint32_t>(static_cast<int>(std::floor(v)));
                        uint32_t ux = iu & T.umask, vy = iv & T.vmask;
                        if (T.mirror_u && (iu & T.tw)) ux = T.umask - ux;
                        if (T.mirror_v && (iv & T.th)) vy = T.vmask - vy;
                        const auto t = texel_colour(T.format, mem_->texel(T.tx + ux, T.ty + vy));
                        r *= t[0];
                        g *= t[1];
                        b *= t[2];
                        al *= t[3];
                        if (t[3] < 0.5f && (T.contour || T.format == 0)) continue; // cut-out texels
                    }
                    if (al < 1.0f / 32.0f) continue;
                    if (T.fog > 0) { // toward the fog colour with distance (1 / w)
                        const float f = T.fog * std::clamp(T.fog_start + inv_w * T.fog_density, 0.0f, 1.0f);
                        r += (T.fog_r - r) * f;
                        g += (T.fog_g - g) * f;
                        b += (T.fog_b - b) * f;
                    }
                    if (al < 1.0f) { // translucent: blend over what is there, depth unchanged
                        const uint32_t d = color_row[px];
                        const float dr = static_cast<float>((d >> 16) & 0xFF) / 255.0f,
                                    dg = static_cast<float>((d >> 8) & 0xFF) / 255.0f,
                                    db = static_cast<float>(d & 0xFF) / 255.0f;
                        const float da = d ? 1.0f : 0.0f;
                        r = r * al + dr * (1 - al);
                        g = g * al + dg * (1 - al);
                        b = b * al + db * (1 - al);
                        color_row[px] = to8(al + da * (1 - al)) << 24 | to8(r) << 16 | to8(g) << 8 | to8(b);
                    } else {
                        color_row[px] = 0xFF000000u | to8(r) << 16 | to8(g) << 8 | to8(b);
                        depth_row[px] = w;
                    }
                }
            }
        }
    }
}

} // namespace m3

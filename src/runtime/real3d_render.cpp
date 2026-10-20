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

using EyeVertex = Real3dScene::Vertex;

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

Real3dRenderer::Real3dRenderer(int threads)
    : color_(kWidth * kHeight), depth_(kWidth * kHeight), trans_depth_(kWidth * kHeight), stencil_(kWidth * kHeight),
      trans_{std::vector<uint32_t>(kWidth * kHeight), std::vector<uint32_t>(kWidth * kHeight)} {
    threads_ = threads > 0 ? threads : static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
}

void Real3dRenderer::render(const Real3d& mem) {
    mem_ = &mem;
    stats_ = {};
    const auto start = std::chrono::steady_clock::now();
    probe_hits_.clear();
    builder_.build(mem, scene_);
    const Real3dScene::Stats& s = scene_.stats;
    stats_.viewports = s.viewports;
    stats_.nodes = s.nodes;
    stats_.models = s.models;
    stats_.polygons = s.polygons;
    stats_.models_in_polygon_ram = s.models_in_polygon_ram;
    stats_.hidden_nodes = s.hidden_nodes;
    stats_.lod_rejected = s.lod_rejected;
    stats_.frustum_culled = s.frustum_culled;
    stats_.faded_out = s.faded_out;
    stats_.spotlights = s.spotlights;
    if (scene_.block_culled) { // no 3D: Supermodel clears to white, over the layers below
        std::fill(color_.begin(), color_.end(), 0xFFFFFFFFu);
        stats_.block_culled = true;
        return;
    }
    // Set up every triangle, priority by priority, then draw them, each
    // thread taking every n-th line.
    triangles_.clear();
    quads_.clear();
    priority_starts_.clear();
    size_t next = 0;
    for (int priority = 0; priority < 4; ++priority) {
        priority_starts_.push_back(triangles_.size());
        for (; next < scene_.polygons.size() && scene_.polygons[next].priority == priority; ++next) set_up(scene_.polygons[next]);
    }
    const auto set_up_done = std::chrono::steady_clock::now();
    stats_.setup_ms = std::chrono::duration<double, std::milli>(set_up_done - start).count();
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
    stats_.raster_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - set_up_done).count();
}

// ---- Setting up ----

void Real3dRenderer::set_up(const Real3dScene::Polygon& poly) {
    const Viewport& vp = scene_.viewports[poly.viewport];
    const EyeVertex* e = scene_.vertices.data() + poly.first_vertex;
    const int n = poly.vertex_count;

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
            static_cast<Real3dScene::Polygon&>(T) = poly;
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
            if (poly.lit) {
                if (poly.fixed_shading) plane(tri[0]->shade, tri[1]->shade, tri[2]->shade, T.sdx, T.sdy, T.sc);
                plane(tri[0]->nx, tri[1]->nx, tri[2]->nx, T.ndx[0], T.ndy[0], T.nc[0]);
                plane(tri[0]->ny, tri[1]->ny, tri[2]->ny, T.ndx[1], T.ndy[1], T.nc[1]);
                plane(tri[0]->nz, tri[1]->nz, tri[2]->nz, T.ndx[2], T.ndy[2], T.nc[2]);
            }
            if (poly.textured) {
                plane(tri[0]->u, tri[1]->u, tri[2]->u, T.udx, T.udy, T.uc);
                plane(tri[0]->v, tri[1]->v, tri[2]->v, T.vdx, T.vdy, T.vc);
            }
            stats_.micro_polys += T.micro ? 1 : 0;
            stats_.translator_polys += T.translator ? 1 : 0;
            stats_.inverted_polys += T.inverted ? 1 : 0;
            T.quad = quad;
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
                    ProbeHit hit{T.address, {}, T.priority, T.format, T.textured, T.contour, T.alpha, texel_alpha,
                                 w, kPassName[static_cast<int>(pass)], outcome};
                    for (uint32_t i = 0; i < 7; ++i) hit.header[i] = mem_->model(T.address + i);
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
    const Viewport& V = scene_.viewports[T.viewport];
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
        for (const Viewport& v : scene_.viewports) {
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
        for (const Viewport& v : scene_.viewports)
            if (v.priority == priority && v.scroll_fog > 0 && (!scroll || scroll->scroll_fog < v.scroll_fog)) scroll = &v;
    if (!scroll) return;
    for (const Viewport& v : scene_.viewports) {
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

#include "runtime/real3d_interp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace m3 {

namespace {

using Vec3 = std::array<float, 3>;
using Mat3 = std::array<Vec3, 3>; // columns

Vec3 column(const Mat4& m, int c) { return {m.m[c * 4], m.m[c * 4 + 1], m.m[c * 4 + 2]}; }
Vec3 translation(const Mat4& m) { return {m.m[12], m.m[13], m.m[14]}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 apply(const Mat3& l, const Vec3& v) {
    return {l[0][0] * v[0] + l[1][0] * v[1] + l[2][0] * v[2], l[0][1] * v[0] + l[1][1] * v[1] + l[2][1] * v[2],
            l[0][2] * v[0] + l[1][2] * v[1] + l[2][2] * v[2]};
}
Mat3 times(const Mat3& a, const Mat3& b) { return {apply(a, b[0]), apply(a, b[1]), apply(a, b[2])}; }

bool inverse(const Mat3& l, Mat3& out) {
    const Vec3 c0 = cross(l[1], l[2]), c1 = cross(l[2], l[0]), c2 = cross(l[0], l[1]);
    const float det = dot(l[0], c0);
    if (!(std::fabs(det) > 1e-12f)) return false;
    // The inverse's rows are the cross products over the determinant.
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[static_cast<size_t>(c)][static_cast<size_t>(r)] = (r == 0 ? c0 : r == 1 ? c1 : c2)[static_cast<size_t>(c)] / det;
    return true;
}

// QR decomposition: a proper rotation and a signed upper-triangular stretch.
// Keep shear and reflection in the stretch: a quaternion cannot represent an
// improper rotation, and dropping shear would deform even a stationary model.
struct Parts {
    Mat3 stretch;
    std::array<float, 4> q; // x, y, z, w
    bool mirrored;
};

std::array<float, 4> quaternion(const Mat3& r) { // r[c][row]
    const float m00 = r[0][0], m11 = r[1][1], m22 = r[2][2];
    const float tr = m00 + m11 + m22;
    std::array<float, 4> q;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        q = {(r[1][2] - r[2][1]) / s, (r[2][0] - r[0][2]) / s, (r[0][1] - r[1][0]) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        q = {0.25f * s, (r[1][0] + r[0][1]) / s, (r[2][0] + r[0][2]) / s, (r[1][2] - r[2][1]) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        q = {(r[1][0] + r[0][1]) / s, 0.25f * s, (r[2][1] + r[1][2]) / s, (r[2][0] - r[0][2]) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        q = {(r[2][0] + r[0][2]) / s, (r[2][1] + r[1][2]) / s, 0.25f * s, (r[0][1] - r[1][0]) / s};
    }
    return q;
}

Mat3 rotation(const std::array<float, 4>& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {Vec3{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
            Vec3{2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
            Vec3{2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}};
}

bool decompose(const Mat3& l, Parts& out) {
    Mat3 r = l;
    // Gram-Schmidt: the columns made orthonormal, the first kept.
    auto normalize = [](Vec3& v) {
        const float n = length(v);
        if (std::isfinite(n) && n > 1e-12f)
            for (float& c : v) c /= n;
        return std::isfinite(n) && n > 1e-12f;
    };
    if (!normalize(r[0])) return false;
    const float along = dot(r[0], r[1]);
    for (size_t k = 0; k < 3; ++k) r[1][k] -= along * r[0][k];
    if (!normalize(r[1])) return false;
    r[2] = cross(r[0], r[1]);
    const float signed_z = dot(r[2], l[2]);
    if (!std::isfinite(signed_z) || std::fabs(signed_z) <= 1e-12f) return false;
    out.mirrored = signed_z < 0;
    for (size_t c = 0; c < 3; ++c)
        for (size_t row = 0; row < 3; ++row)
            out.stretch[c][row] = row <= c ? dot(r[row], l[c]) : 0;
    out.q = quaternion(r);
    return true;
}

std::array<float, 4> slerp(std::array<float, 4> a, const std::array<float, 4>& b, float t) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) { // the shorter way round
        for (float& c : a) c = -c;
        d = -d;
    }
    std::array<float, 4> q;
    if (d > 0.9995f) {
        for (size_t i = 0; i < 4; ++i) q[i] = a[i] + (b[i] - a[i]) * t;
    } else {
        const float angle = std::acos(std::clamp(d, -1.0f, 1.0f)), s = std::sin(angle);
        const float wa = std::sin((1 - t) * angle) / s, wb = std::sin(t * angle) / s;
        for (size_t i = 0; i < 4; ++i) q[i] = a[i] * wa + b[i] * wb;
    }
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& c : q) c /= n;
    return q;
}

float turn_degrees(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    const float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2.0f * std::acos(std::min(d, 1.0f)) * 57.2957795f;
}

Mat3 linear(const Mat4& m) { return {column(m, 0), column(m, 1), column(m, 2)}; }

struct ModelKey {
    uint32_t viewport, address, path;
    bool operator==(const ModelKey&) const = default;
};
struct ModelKeyHash {
    size_t operator()(const ModelKey& k) const {
        return static_cast<size_t>(static_cast<uint64_t>(k.viewport) << 48 ^ static_cast<uint64_t>(k.address) << 24 ^ k.path);
    }
};
ModelKey key(const Real3dScene::Model& m) {
    return {m.viewport, m.address, m.path};
}
uint64_t place(const Real3dScene::Model& m) { return static_cast<uint64_t>(m.viewport) << 32 | m.address; }

} // namespace

bool interpolate(const Real3dScene& a, const Real3dScene& b, float t, Real3dScene& out, InterpStats* stats) {
    out = b;
    InterpStats s;
    s.models = b.models.size();
    auto done = [&](bool blended) {
        s.cut = !blended;
        if (stats) *stats = s;
        return blended;
    };
    if (a.block_culled || b.block_culled || a.models.empty() || b.models.empty()) return done(false);

    // The older scene's models by their key, and by viewport and address.
    // Hash collisions must not identify different models as the same instance.
    std::unordered_map<ModelKey, int, ModelKeyHash> by_key; // -1: several
    std::unordered_map<uint64_t, std::vector<int>> by_place;
    for (size_t i = 0; i < a.models.size(); ++i) {
        const auto [it, added] = by_key.emplace(key(a.models[i]), static_cast<int>(i));
        if (!added) it->second = -1;
        by_place[place(a.models[i])].push_back(static_cast<int>(i));
    }

    struct Move {
        size_t model;
        Mat3 l;
        Vec3 t;
        bool identity = false;
    };
    std::vector<Move> moves;
    moves.reserve(b.models.size());
    std::vector<size_t> strays; // not found: moved with their nearest relative found
    for (size_t i = 0; i < b.models.size(); ++i) {
        const Real3dScene::Model& mb = b.models[i];
        const Vec3 tb = translation(mb.matrix);
        int found = -1;
        if (const auto it = by_key.find(key(mb)); it != by_key.end() && it->second >= 0) found = it->second;
        if (found < 0) // the same model reached through other nodes: the nearest
            if (const auto it = by_place.find(place(mb)); it != by_place.end()) {
                float best = 0, second = 0;
                for (int j : it->second) {
                    const Vec3 ta = translation(a.models[static_cast<size_t>(j)].matrix);
                    const Vec3 d{ta[0] - tb[0], ta[1] - tb[1], ta[2] - tb[2]};
                    const float far = dot(d, d);
                    if (found < 0 || far < best) {
                        second = found < 0 ? 0 : best;
                        best = far;
                        found = j;
                    } else if (second == 0 || far < second) {
                        second = far;
                    }
                }
                s.compared += it->second.size();
                ++s.nearest;
                if (it->second.size() > 1 && second < 4 * best) ++s.ambiguous;
            }
        if (found < 0) {
            ++s.unmatched;
            strays.push_back(i);
            continue;
        }
        const Real3dScene::Model& ma = a.models[static_cast<size_t>(found)];
        const Vec3 ta = translation(ma.matrix);
        const Vec3 d{tb[0] - ta[0], tb[1] - ta[1], tb[2] - ta[2]};
        const Mat3 la = linear(ma.matrix), lb = linear(mb.matrix);
        Parts pa{}, pb{};
        Mat3 lb_inverse{};
        if (length(d) > kMaxMove * std::max(length(ta), length(tb)) || !decompose(la, pa) || !decompose(lb, pb) ||
            pa.mirrored != pb.mirrored || !inverse(lb, lb_inverse)) {
            ++s.moved;
            continue;
        }
        if (ma.matrix.m == mb.matrix.m) {
            // Preserve stationary geometry exactly. Even a tiny round-trip
            // error can change which of two coplanar surfaces wins the depth
            // test, alternating between the original and interpolated picture.
            moves.push_back({i, {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}, {}, true});
            continue;
        }
        // The matrix between: position and stretch straight, rotation the
        // shortest turn. Turned too far to tell which way (a wheel spinning):
        // the newer rotation, moved with the rest (else it'd lag its car).
        const bool spun = turn_degrees(pa.q, pb.q) > kMaxTurn;
        if (spun) ++s.spun;
        Mat3 stretch{};
        for (size_t c = 0; c < 3; ++c)
            for (size_t row = 0; row < 3; ++row)
                stretch[c][row] = pa.stretch[c][row] + (pb.stretch[c][row] - pa.stretch[c][row]) * t;
        const Mat3 lt = times(rotation(spun ? pb.q : slerp(pa.q, pb.q, t)), stretch);
        const Vec3 tt{ta[0] + d[0] * t, ta[1] + d[1] * t, ta[2] + d[2] * t};
        // Its polygons, there in the newer scene, moved by (between) x (newer)^-1.
        const Mat3 l = times(lt, lb_inverse);
        const Vec3 lb_t = apply(l, tb);
        moves.push_back({i, l, {tt[0] - lb_t[0], tt[1] - lb_t[1], tt[2] - lb_t[2]}});
    }
    s.blended = moves.size();
    if (s.blended * 2 < s.models) return done(false); // a cut: the newer scene as it is

    // Models not found (another model each frame: a flipbook's, a wheel's
    // blur, a bird's wings, a gate's pieces) move as the one found nearest
    // them in the tree does (below the viewport's own node): else, drawn
    // where they are while all round them is drawn back along the way,
    // they'd jump between the two from picture to picture (ghosts).
    if (!strays.empty()) {
        std::unordered_map<uint64_t, size_t> by_ancestor; // (viewport, an ancestor's hash) -> a move under it
        for (size_t k = 0; k < s.blended; ++k) {
            const Real3dScene::Model& model = b.models[moves[k].model];
            for (int level = 1; level < model.depth; ++level)
                by_ancestor.emplace(static_cast<uint64_t>(model.viewport) << 32 | model.ancestors[static_cast<size_t>(level)], k);
        }
        for (size_t i : strays) {
            const Real3dScene::Model& model = b.models[i];
            for (int level = model.depth - 1; level >= 1; --level) {
                const auto it =
                    by_ancestor.find(static_cast<uint64_t>(model.viewport) << 32 | model.ancestors[static_cast<size_t>(level)]);
                if (it == by_ancestor.end()) continue;
                moves.push_back({i, moves[it->second].l, moves[it->second].t, moves[it->second].identity});
                ++s.followed;
                break;
            }
        }
    }

    for (const Move& move : moves) {
        if (move.identity) continue;
        const Real3dScene::Model& model = b.models[move.model];
        for (uint32_t p = model.first_polygon; p < model.first_polygon + model.polygon_count; ++p) {
            const Real3dScene::Polygon& poly = out.polygons[p];
            for (uint32_t k = 0; k < poly.vertex_count; ++k) {
                Real3dScene::Vertex& v = out.vertices[poly.first_vertex + k];
                const Vec3 pos = apply(move.l, {v.x, v.y, v.z}), n = apply(move.l, {v.nx, v.ny, v.nz});
                v.x = pos[0] + move.t[0];
                v.y = pos[1] + move.t[1];
                v.z = pos[2] + move.t[2];
                v.nx = n[0];
                v.ny = n[1];
                v.nz = n[2];
            }
        }
    }
    return done(true);
}

} // namespace m3

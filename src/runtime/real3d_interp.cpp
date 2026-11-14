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
Vec3 apply_linear(const Mat3& l, const Vec3& v) {
    return {l[0][0] * v[0] + l[1][0] * v[1] + l[2][0] * v[2], l[0][1] * v[0] + l[1][1] * v[1] + l[2][1] * v[2],
            l[0][2] * v[0] + l[1][2] * v[1] + l[2][2] * v[2]};
}
Mat3 times(const Mat3& a, const Mat3& b) { return {apply_linear(a, b[0]), apply_linear(a, b[1]), apply_linear(a, b[2])}; }

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

bool finite(const Mat4& m) {
    return std::all_of(m.m.begin(), m.m.end(), [](float value) { return std::isfinite(value); });
}

// A model's own pose: position/stretch straight, rotation along the shortest
// turn. For an ambiguous fast turn, retain the newer rotation but still move
// its position, as for a spinning wheel travelling with its car.
bool model_move(const Mat4& a, const Mat4& b, float t, Mat3& out_l, Vec3& out_t, bool& spun) {
    if (!finite(a) || !finite(b)) return false;
    if (a.m == b.m) {
        out_l = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
        out_t = {};
        return true;
    }
    Parts pa{}, pb{};
    Mat3 inverse_b{};
    if (!decompose(linear(a), pa) || !decompose(linear(b), pb) ||
        pa.mirrored != pb.mirrored || !inverse(linear(b), inverse_b)) return false;
    spun = turn_degrees(pa.q, pb.q) > kMaxTurn;
    Mat3 stretch{};
    for (size_t c = 0; c < 3; ++c)
        for (size_t r = 0; r < 3; ++r)
            stretch[c][r] = pa.stretch[c][r] + (pb.stretch[c][r] - pa.stretch[c][r]) * t;
    out_l = times(times(rotation(spun ? pb.q : slerp(pa.q, pb.q, t)), stretch), inverse_b);
    const Vec3 ta = translation(a), tb = translation(b), moved_b = apply_linear(out_l, tb);
    for (size_t r = 0; r < 3; ++r) out_t[r] = ta[r] + (tb[r] - ta[r]) * t - moved_b[r];
    return true;
}

// Interpolate a root's relative eye-space motion, independent of its model
// origin. A rigid move follows its screw (rotation about a fixed axis plus
// translation along it), so both a camera turning in place and an object
// turning about a fixed point retain that point. Stretch/shear are blended
// continuously on this same path; there is no rigid/non-rigid threshold.
bool root_move(const Mat4& a, const Mat4& b, float t, Mat3& out_l, Vec3& out_t, bool& spun) {
    if (!finite(a) || !finite(b)) return false;
    if (a.m == b.m) return model_move(a, b, t, out_l, out_t, spun);
    using DVec = std::array<double, 3>;
    using DMat = std::array<DVec, 3>;
    auto dcross = [](const DVec& x, const DVec& y) -> DVec {
        return {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
    };
    DMat la{}, lb{}, inverse_b{}, delta{};
    for (size_t c = 0; c < 3; ++c)
        for (size_t r = 0; r < 3; ++r) {
            la[c][r] = a.m[c * 4 + r];
            lb[c][r] = b.m[c * 4 + r];
        }
    const DMat cofactors{dcross(lb[1], lb[2]), dcross(lb[2], lb[0]), dcross(lb[0], lb[1])};
    const double det = lb[0][0] * cofactors[0][0] + lb[0][1] * cofactors[0][1] + lb[0][2] * cofactors[0][2];
    if (!std::isfinite(det) || std::fabs(det) <= 1e-12) return false;
    for (size_t c = 0; c < 3; ++c)
        for (size_t r = 0; r < 3; ++r) inverse_b[c][r] = cofactors[r][c] / det;
    Mat3 relative{};
    for (size_t c = 0; c < 3; ++c)
        for (size_t r = 0; r < 3; ++r) {
            for (size_t k = 0; k < 3; ++k) delta[c][r] += la[k][r] * inverse_b[c][k];
            relative[c][r] = static_cast<float>(delta[c][r]);
        }
    Parts parts{};
    if (!decompose(relative, parts) || parts.mirrored) return false;
    std::array<double, 4> q{};
    double norm = 0;
    for (size_t k = 0; k < 4; ++k) norm += double(parts.q[k]) * parts.q[k];
    norm = std::sqrt(norm);
    if (!(norm > 0) || !std::isfinite(norm)) return false;
    for (size_t k = 0; k < 4; ++k) q[k] = parts.q[k] / norm * (parts.q[3] < 0 ? -1 : 1);
    const double sine = std::hypot(q[0], q[1], q[2]), half = std::atan2(sine, q[3]);
    if (2 * half * 57.29577951308232 > kMaxTurn) {
        // Every descendant still receives the same move, including when the
        // root is an ordinary fast-turning object rather than the camera.
        return model_move(a, b, t, out_l, out_t, spun);
    }
    const double u = 1.0 - t;
    const double ratio = sine > 0 ? std::sin(u * half) / sine : u;
    std::array<float, 4> between{};
    for (size_t k = 0; k < 3; ++k) between[k] = static_cast<float>(q[k] * ratio);
    between[3] = static_cast<float>(std::cos(u * half));
    Mat3 stretch{};
    for (size_t c = 0; c < 3; ++c)
        for (size_t r = 0; r < 3; ++r) {
            const double identity = c == r ? 1 : 0;
            stretch[c][r] = static_cast<float>(identity + u * (parts.stretch[c][r] - identity));
        }
    out_l = times(rotation(between), stretch);
    DVec axis{}, shift{};
    for (size_t r = 0; r < 3; ++r) {
        axis[r] = sine > 0 ? q[r] / sine : 0;
        // Keep this cancellation in double precision for distant root origins.
        shift[r] = a.m[12 + r] - (delta[0][r] * b.m[12] + delta[1][r] * b.m[13] + delta[2][r] * b.m[14]);
    }
    const double along = axis[0] * shift[0] + axis[1] * shift[1] + axis[2] * shift[2];
    const double co = ratio * std::cos((u - 1) * half), si = ratio * std::sin((u - 1) * half);
    const DVec across = dcross(axis, shift);
    for (size_t r = 0; r < 3; ++r) {
        out_t[r] = static_cast<float>(co * shift[r] + (u - co) * along * axis[r] + si * across[r]);
        if (!std::isfinite(out_t[r])) return false;
    }
    return true;
}

// Compose Root(t) * Local(t) without changing the shared path when a local
// joint starts/stops animating. The local correction is expressed in the newer
// root's eye coordinates, then followed by the common root move. Use double
// for the conjugation so a distant root origin does not amplify cancellation.
bool add_local_move(const Mat4& root, const Mat3& local_l, const Vec3& local_t, Mat3& root_l, Vec3& root_t) {
    using DVec = std::array<double, 3>;
    using DMat = std::array<DVec, 3>;
    DMat b{}, inv{}, eye{};
    for (size_t c = 0; c < 3; ++c)
        for (size_t r = 0; r < 3; ++r) b[c][r] = root.m[c * 4 + r];
    auto dcross = [](const DVec& x, const DVec& y) -> DVec {
        return {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
    };
    const DMat cof{dcross(b[1], b[2]), dcross(b[2], b[0]), dcross(b[0], b[1])};
    const double det = b[0][0] * cof[0][0] + b[0][1] * cof[0][1] + b[0][2] * cof[0][2];
    if (!std::isfinite(det) || std::fabs(det) <= 1e-12) return false;
    for (size_t c = 0; c < 3; ++c)
        for (size_t r = 0; r < 3; ++r) inv[c][r] = cof[r][c] / det;
    for (size_t c = 0; c < 3; ++c)
        for (size_t r = 0; r < 3; ++r)
            for (size_t j = 0; j < 3; ++j)
                for (size_t k = 0; k < 3; ++k) eye[c][r] += b[j][r] * local_l[k][j] * inv[c][k];
    DVec offset{};
    for (size_t r = 0; r < 3; ++r)
        for (size_t k = 0; k < 3; ++k)
            offset[r] += b[k][r] * local_t[k] + ((k == r ? 1.0 : 0.0) - eye[k][r]) * root.m[12 + k];
    Mat3 composed{};
    Vec3 shifted{};
    for (size_t r = 0; r < 3; ++r) {
        double shift = root_t[r];
        for (size_t k = 0; k < 3; ++k) shift += root_l[k][r] * offset[k];
        shifted[r] = static_cast<float>(shift);
        if (!std::isfinite(shifted[r])) return false;
        for (size_t c = 0; c < 3; ++c) {
            double value = 0;
            for (size_t k = 0; k < 3; ++k) value += root_l[k][r] * eye[c][k];
            composed[c][r] = static_cast<float>(value);
            if (!std::isfinite(composed[c][r])) return false;
        }
    }
    root_l = composed;
    root_t = shifted;
    return true;
}

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
        const Vec3 model_tb = translation(mb.matrix);
        int found = -1;
        if (const auto it = by_key.find(key(mb)); it != by_key.end() && it->second >= 0) found = it->second;
        if (found < 0) // the same model reached through other nodes: the nearest
            if (const auto it = by_place.find(place(mb)); it != by_place.end()) {
                float best = 0, second = 0;
                for (int j : it->second) {
                    const Vec3 ta = translation(a.models[static_cast<size_t>(j)].matrix);
                    const Vec3 d{ta[0] - model_tb[0], ta[1] - model_tb[1], ta[2] - model_tb[2]};
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
        // Cut detection is relative to the model's distance from the eye,
        // independent of where the shared root happens to place its origin.
        const Vec3 model_ta = translation(ma.matrix);
        const Vec3 model_d{model_tb[0] - model_ta[0], model_tb[1] - model_ta[1], model_tb[2] - model_ta[2]};
        if (length(model_d) > kMaxMove * std::max(length(model_ta), length(model_tb))) {
            ++s.moved;
            continue;
        }
        // Both static and animated descendants follow the same root trajectory.
        // Local equality only skips an identity correction; it never selects a
        // different camera path. Locals were accumulated directly by the builder.
        const bool shared_root = ma.motion.valid && mb.motion.valid && ma.motion.path == mb.motion.path;
        const bool same_local = !shared_root || ma.motion.local.m == mb.motion.local.m;
        const Mat4& matrix_a = shared_root ? ma.motion.matrix : ma.matrix;
        const Mat4& matrix_b = shared_root ? mb.motion.matrix : mb.matrix;
        if (same_local && matrix_a.m == matrix_b.m && finite(matrix_b) &&
            (!shared_root || finite(mb.motion.local))) {
            // No inverse is needed for an unchanged model, even if collapsed.
            // Preserve its vertices exactly and let strays inherit that identity.
            moves.push_back({i, {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}, {}, true});
            continue;
        }
        Mat3 l{};
        Vec3 offset{};
        bool spun = false;
        if (shared_root) {
            if (!root_move(matrix_a, matrix_b, t, l, offset, spun)) {
                ++s.moved;
                continue;
            }
            if (!same_local) {
                Mat3 local_l{};
                Vec3 local_t{};
                bool local_spun = false;
                if (!model_move(ma.motion.local, mb.motion.local, t, local_l, local_t, local_spun) ||
                    !add_local_move(matrix_b, local_l, local_t, l, offset)) {
                    ++s.moved;
                    continue;
                }
                spun = spun || local_spun;
            }
        } else if (!model_move(matrix_a, matrix_b, t, l, offset, spun)) {
            ++s.moved;
            continue;
        }
        if (spun) ++s.spun;
        moves.push_back({i, l, offset});
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
                const Vec3 pos = apply_linear(move.l, {v.x, v.y, v.z}), n = apply_linear(move.l, {v.nx, v.ny, v.nz});
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

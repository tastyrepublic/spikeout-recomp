#include "runtime/real3d_interp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
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

// Culling-node paths can be recycled for another instance of the same mesh.
// An unchanged pose below a common root is stronger correspondence when it is
// unique in both scenes. The root's eye-space matrix may move with the camera.
struct PoseKey {
    uint32_t viewport, address, root;
    std::array<float, 16> local;
    bool operator==(const PoseKey&) const = default;
};
struct PoseKeyHash {
    size_t operator()(const PoseKey& k) const {
        size_t h = ModelKeyHash{}({k.viewport, k.address, k.root});
        for (float value : k.local) h ^= std::hash<float>{}(value) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};
bool has_pose(const Real3dScene::Model& m) {
    return m.motion.valid && finite(m.motion.local) && finite(m.motion.matrix);
}

// Paths identify groups within one scene. Across frames, viewports and nodes
// can be recycled: use unchanged anchors and reject ambiguous local motion.
struct GroupMatches {
    std::vector<int> previous, root;
    std::vector<bool> handled, used_old;
};
GroupMatches group_matches(const Real3dScene& a, const Real3dScene& b) {
    struct Group {
        size_t model;
        int root;
        std::vector<size_t> members;
        float radius = 0;
        bool valid = true, measured = false;
    };
    struct Root {
        size_t model;
        bool valid = true, invertible = false;
        Mat3 inverse{};
    };
    struct Groups {
        std::vector<Group> groups;
        std::vector<Root> roots;
        std::unordered_map<uint64_t, int> by_root, by_path;
        std::unordered_map<PoseKey, int, PoseKeyHash> by_node_pose, by_pose, anchors;
        std::unordered_map<PoseKey, std::vector<int>, PoseKeyHash> pose_members;
        std::unordered_map<ModelKey, int, ModelKeyHash> by_identity;
    };
    auto root_key = [](const Real3dScene::Model& m) { return uint64_t(m.viewport) << 32 | m.motion.path; };
    auto pose = [](const Real3dScene::Model& m, int root) { return PoseKey{0, 0, static_cast<uint32_t>(root), m.motion.local.m}; };
    auto collect = [&](const Real3dScene& scene) {
        Groups out;
        for (size_t i = 0; i < scene.models.size(); ++i) {
            const auto& m = scene.models[i];
            if (!has_pose(m)) continue;
            const auto [root_it, fresh_root] = out.by_root.emplace(root_key(m), static_cast<int>(out.roots.size()));
            if (fresh_root) {
                out.roots.push_back({i});
                auto& added = out.roots.back();
                added.invertible = inverse(linear(m.motion.matrix), added.inverse);
            }
            const int root = root_it->second;
            if (scene.models[out.roots[size_t(root)].model].motion.matrix.m != m.motion.matrix.m) out.roots[size_t(root)].valid = false;
            auto within_frame = pose(m, root);
            within_frame.address = m.path;
            const auto [where, fresh] = out.by_node_pose.emplace(within_frame, static_cast<int>(out.groups.size()));
            if (fresh) {
                const int index = static_cast<int>(out.groups.size());
                out.groups.push_back({i, root, {}});
                const auto [p, unique] = out.by_pose.emplace(pose(m, root), index);
                if (!unique) p->second = -1;
                out.pose_members[pose(m, root)].push_back(index);
                const auto [path, unique_path] = out.by_path.emplace(uint64_t(root) << 32 | m.path, index);
                if (!unique_path) path->second = -1;
            }
            // A mesh/pose seen in several roots cannot vote for root identity.
            auto anchor = pose(m, 0);
            anchor.address = m.address;
            const auto [at, unique_anchor] = out.anchors.emplace(anchor, root);
            if (!unique_anchor && at->second != root) at->second = -1;
            Group& g = out.groups[size_t(where->second)];
            g.members.push_back(i);
            const auto [id, unique_id] = out.by_identity.emplace(ModelKey{uint32_t(root), m.address, m.path}, where->second);
            if (!unique_id && id->second != where->second) id->second = -1;
        }
        return out;
    };
    // Exact stationary matches need no geometry scan. Only groups still
    // seeking local correspondence need a size-normalized movement bound.
    auto measure = [&](Groups& groups, size_t index, const Real3dScene& scene) {
        auto& g = groups.groups[index];
        if (g.measured) return;
        g.measured = true;
        const auto& basis = groups.roots[size_t(g.root)];
        for (size_t member : g.members) {
            const auto& m = scene.models[member];
            if (!basis.invertible) {
                g.valid = false;
                break;
            }
            const Vec3 origin = translation(m.matrix);
            if (m.first_polygon > scene.polygons.size() || m.polygon_count > scene.polygons.size() - m.first_polygon) {
                g.valid = false;
                continue;
            }
            for (uint32_t pi = m.first_polygon; pi < m.first_polygon + m.polygon_count; ++pi) {
                const auto& p = scene.polygons[pi];
                if (p.first_vertex > scene.vertices.size() || p.vertex_count > scene.vertices.size() - p.first_vertex) {
                    g.valid = false;
                    continue;
                }
                for (uint32_t vi = p.first_vertex; vi < p.first_vertex + p.vertex_count; ++vi) {
                    const auto& v = scene.vertices[vi];
                    const auto offset = apply_linear(basis.inverse, {v.x - origin[0], v.y - origin[1], v.z - origin[2]});
                    const float radius = dot(offset, offset);
                    if (!std::isfinite(radius)) g.valid = false;
                    else g.radius = std::max(g.radius, radius);
                }
            }
        }
        g.radius = std::sqrt(g.radius);
    };
    Groups old = collect(a), now = collect(b);
    auto compatible_view = [&](size_t old_root, size_t new_root) {
        const auto& ma = a.models[old.roots[old_root].model];
        const auto& mb = b.models[now.roots[new_root].model];
        if (ma.viewport >= a.viewports.size() || mb.viewport >= b.viewports.size()) return false;
        const auto& x = a.viewports[ma.viewport];
        const auto& y = b.viewports[mb.viewport];
        return x.priority == y.priority && x.x == y.x && x.y == y.y && x.width == y.width && x.height == y.height && x.left == y.left &&
               x.right == y.right && x.top == y.top && x.bottom == y.bottom;
    };
    // Count distinct full poses rather than mesh parts at one pose. Three
    // independent unchanged poses must agree on a unique root on both sides.
    std::unordered_map<uint64_t, std::unordered_map<PoseKey, bool, PoseKeyHash>> votes;
    for (const auto& [anchor, nr] : now.anchors) {
        if (nr < 0 || !now.roots[size_t(nr)].valid) continue;
        const auto found = old.anchors.find(anchor);
        if (found == old.anchors.end() || found->second < 0) continue;
        const size_t ar = size_t(found->second), br = size_t(nr);
        if (!old.roots[ar].valid || !compatible_view(ar, br)) continue;
        auto distinct = anchor;
        distinct.address = 0;
        votes[uint64_t(br) << 32 | ar].emplace(distinct, true);
    }
    struct BestRoot {
        int index = -1;
        size_t first = 0, second = 0;
    };
    std::vector<BestRoot> old_best(old.roots.size()), new_best(now.roots.size());
    auto vote = [](BestRoot& best, int index, size_t score) {
        if (score > best.first) {
            best.second = best.first;
            best.first = score;
            best.index = index;
        } else best.second = std::max(best.second, score);
    };
    for (const auto& [pair, poses] : votes) {
        const size_t br = size_t(pair >> 32), ar = size_t(pair & 0xffffffffu);
        const size_t score = poses.size();
        vote(new_best[br], static_cast<int>(ar), score);
        vote(old_best[ar], static_cast<int>(br), score);
    }
    auto decisive = [](const BestRoot& best) { return best.index >= 0 && best.first >= 3 && best.first > 2 * best.second; };
    std::vector<int> root_pairs(now.roots.size(), -1);
    std::vector<bool> root_reserved(old.roots.size(), false);
    std::vector<bool> root_supported(now.roots.size(), false);
    for (size_t br = 0; br < now.roots.size(); ++br) {
        const auto& best = new_best[br];
        if (!decisive(best)) continue;
        const size_t ar = size_t(best.index);
        if (old_best[ar].index != static_cast<int>(br) || !decisive(old_best[ar])) continue;
        root_pairs[br] = best.index;
        root_reserved[ar] = true;
        root_supported[br] = true;
    }
    for (size_t br = 0; br < now.roots.size(); ++br) {
        if (root_pairs[br] >= 0 || !now.roots[br].valid) continue;
        const auto& m = b.models[now.roots[br].model];
        const auto found = old.by_root.find(root_key(m));
        if (found == old.by_root.end()) continue;
        const size_t ar = size_t(found->second);
        // A recycled key must not steal a root already identified by anchors.
        // Contradictory anchor evidence is uncertainty, not a tie to break.
        if (root_reserved[ar] || !old.roots[ar].valid || !compatible_view(ar, br) || new_best[br].first >= 3 || old_best[ar].first >= 3)
            continue;
        root_pairs[br] = found->second;
        root_reserved[ar] = true;
    }
    GroupMatches result{std::vector<int>(b.models.size(), -1), std::vector<int>(b.models.size(), -1),
                        std::vector<bool>(b.models.size(), false), std::vector<bool>(a.models.size(), false)};
    std::vector<int> pairs(now.groups.size(), -1);
    std::vector<bool> supports_root(now.groups.size(), false);
    std::vector<bool> reserved(old.groups.size(), false);
    auto shares_mesh = [&](const Group& ga, const Group& gb) {
        for (size_t prior : ga.members)
            for (size_t current : gb.members)
                if (a.models[prior].address == b.models[current].address) return true;
        return false;
    };
    for (size_t i = 0; i < now.groups.size(); ++i) {
        const auto& g = now.groups[i];
        const auto& m = b.models[g.model];
        const int ar = root_pairs[size_t(g.root)];
        for (size_t member : g.members) {
            result.handled[member] = true;
            if (ar >= 0) result.root[member] = static_cast<int>(old.roots[size_t(ar)].model);
        }
        if (ar < 0) continue;
        const auto exact = old.by_pose.find(pose(m, ar));
        if (exact != old.by_pose.end() && exact->second >= 0 && now.by_pose.at(pose(m, g.root)) >= 0 &&
            shares_mesh(old.groups[size_t(exact->second)], g)) {
            pairs[i] = exact->second;
            supports_root[i] = true;
            reserved[size_t(exact->second)] = true;
        }
    }
    struct Nearest {
        int index = -1;
        double first = INFINITY, second = INFINITY;
    };
    std::vector<Nearest> from_new(now.groups.size()), from_old(old.groups.size());
    auto consider = [](Nearest& n, int index, double distance) {
        if (distance < n.first) {
            n.second = n.first;
            n.first = distance;
            n.index = index;
        } else n.second = std::min(n.second, distance);
    };
    auto distance_squared = [](const Real3dScene::Model& x, const Real3dScene::Model& y) {
        double distance = 0;
        for (size_t k = 12; k < 15; ++k) {
            const double d = double(x.motion.local.m[k]) - y.motion.local.m[k];
            distance += d * d;
        }
        return distance;
    };
    for (size_t j = 0; j < old.groups.size(); ++j)
        if (!reserved[j]) measure(old, j, a);
    for (size_t i = 0; i < now.groups.size(); ++i)
        if (pairs[i] < 0 && root_pairs[size_t(now.groups[i].root)] >= 0) measure(now, i, b);

    // A unique unchanged node/mesh is positive evidence for close articulated
    // parts, provided no stationary owner has reserved it and its local step
    // fits both bounds. Coincident groups must move together as a complete,
    // bijective cohort; a merge or split is not resolved by arbitrary paths.
    std::vector<int> proposed(now.groups.size(), -1), proposed_uses(old.groups.size(), 0);
    std::vector<bool> contradicted(now.groups.size(), false);
    for (size_t i = 0; i < now.groups.size(); ++i) {
        const auto& gb = now.groups[i];
        const int ar = root_pairs[size_t(gb.root)];
        if (pairs[i] >= 0 || ar < 0 || !gb.valid) continue;
        int candidate = -1;
        bool conflict = false;
        for (size_t member : gb.members) {
            const auto& m = b.models[member];
            const auto current = now.by_identity.find({uint32_t(gb.root), m.address, m.path});
            const auto prior = old.by_identity.find({uint32_t(ar), m.address, m.path});
            if (current == now.by_identity.end() || current->second != static_cast<int>(i) || prior == old.by_identity.end() ||
                prior->second < 0)
                continue;
            if (reserved[size_t(prior->second)]) {
                conflict = true;
                measure(old, size_t(prior->second), a);
                const auto& owner = old.groups[size_t(prior->second)];
                const double radius = std::min(owner.radius, gb.radius);
                if (owner.valid && radius > 0 && distance_squared(a.models[owner.model], m) <= radius * radius) contradicted[i] = true;
            }
            if (candidate >= 0 && candidate != prior->second) conflict = contradicted[i] = true;
            candidate = prior->second;
        }
        if (candidate < 0 || conflict) continue;
        const auto& ga = old.groups[size_t(candidate)];
        const double radius = std::min(ga.radius, gb.radius);
        if (!ga.valid || !(radius > 0) || distance_squared(a.models[ga.model], b.models[gb.model]) > radius * radius) continue;
        proposed[i] = candidate;
        ++proposed_uses[size_t(candidate)];
    }
    std::vector<bool> coherent(now.groups.size(), false);
    for (size_t i = 0; i < now.groups.size(); ++i) {
        if (proposed[i] < 0) continue;
        const auto& ga = old.groups[size_t(proposed[i])];
        const auto& gb = now.groups[i];
        const auto old_pose = pose(a.models[ga.model], ga.root);
        const auto& old_members = old.pose_members.at(old_pose);
        const auto& new_members = now.pose_members.at(pose(b.models[gb.model], gb.root));
        bool agrees = old_members.size() == new_members.size();
        for (int member : new_members) {
            const int prior = proposed[size_t(member)];
            if (prior < 0 || proposed_uses[size_t(prior)] != 1) {
                agrees = false;
                break;
            }
            const auto& peer = old.groups[size_t(prior)];
            if (!(pose(a.models[peer.model], peer.root) == old_pose)) {
                agrees = false;
                break;
            }
        }
        coherent[i] = agrees;
    }
    for (size_t i = 0; i < now.groups.size(); ++i)
        if (coherent[i]) {
            pairs[i] = proposed[i];
            supports_root[i] = true;
            reserved[size_t(pairs[i])] = true;
        }
    // Mesh-swapping flipbooks can retain an exact pose, but must not steal
    // the old pose of a different mesh whose bounded identity still moves.
    for (size_t i = 0; i < now.groups.size(); ++i) {
        const auto& g = now.groups[i];
        const int ar = root_pairs[size_t(g.root)];
        if (pairs[i] >= 0 || ar < 0 || contradicted[i]) continue;
        const auto& m = b.models[g.model];
        const auto exact = old.by_pose.find(pose(m, ar));
        if (exact == old.by_pose.end() || exact->second < 0 || reserved[size_t(exact->second)] || now.by_pose.at(pose(m, g.root)) < 0)
            continue;
        pairs[i] = exact->second;
        reserved[size_t(exact->second)] = true;
    }
    std::vector<std::vector<size_t>> root_candidates(old.roots.size());
    for (size_t j = 0; j < old.groups.size(); ++j) {
        const auto& ga = old.groups[j];
        if (!reserved[j] && ga.valid) root_candidates[size_t(ga.root)].push_back(j);
    }
    for (size_t i = 0; i < now.groups.size(); ++i) {
        const auto& gb = now.groups[i];
        if (pairs[i] >= 0 || !gb.valid || contradicted[i]) continue;
        const int ar = root_pairs[size_t(gb.root)];
        if (ar < 0) continue;
        const auto& mb = b.models[gb.model];
        for (size_t j : root_candidates[size_t(ar)]) {
            const auto& ga = old.groups[j];
            const double distance = distance_squared(a.models[ga.model], mb);
            consider(from_new[i], static_cast<int>(j), distance);
            consider(from_old[j], static_cast<int>(i), distance);
        }
    }
    auto separated = [](const Nearest& n) { return n.index >= 0 && n.second > 0 && n.first * 16 < n.second; };
    auto path_conflicts = [&](Groups& candidates, const Real3dScene& scene, const Group& query, const Real3dScene::Model& m, size_t chosen,
                              int candidate_root) {
        const auto path = candidates.by_path.find(uint64_t(candidate_root) << 32 | m.path);
        if (path == candidates.by_path.end() || path->second < 0 || size_t(path->second) == chosen) return false;
        measure(candidates, size_t(path->second), scene);
        const auto& other = candidates.groups[size_t(path->second)];
        const double radius = std::min(query.radius, other.radius);
        return other.valid && distance_squared(scene.models[other.model], m) <= radius * radius;
    };
    for (size_t i = 0; i < now.groups.size(); ++i) {
        if (pairs[i] >= 0) continue;
        const auto& n = from_new[i];
        if (!separated(n)) continue;
        const size_t j = size_t(n.index);
        if (from_old[j].index != static_cast<int>(i) || !separated(from_old[j])) continue;
        const auto& ga = old.groups[j];
        const auto& gb = now.groups[i];
        const auto& ma = a.models[ga.model];
        const auto& mb = b.models[gb.model];
        if (old.by_pose.at(pose(ma, ga.root)) < 0 || now.by_pose.at(pose(mb, gb.root)) < 0) continue;
        if (path_conflicts(old, a, gb, mb, j, ga.root) || path_conflicts(now, b, ga, ma, i, gb.root)) continue;
        const double radius = std::min(ga.radius, gb.radius);
        if (!(radius > 0) || n.first > radius * radius) continue;
        pairs[i] = n.index;
        supports_root[i] = true;
    }
    for (size_t i = 0; i < now.groups.size(); ++i)
        if (supports_root[i]) root_supported[size_t(now.groups[i].root)] = true;
    for (size_t i = 0; i < now.groups.size(); ++i)
        if (pairs[i] >= 0 && root_supported[size_t(now.groups[i].root)]) {
            const auto& ga = old.groups[size_t(pairs[i])];
            for (size_t member : now.groups[i].members)
                result.previous[member] = static_cast<int>(ga.model);
            for (size_t member : ga.members)
                result.used_old[member] = true;
        }
    // A surviving slot alone does not establish continuity across a scene
    // replacement. Cross-mesh exact-pose matches alone are also insufficient:
    // unrelated root-attached models can both have identity local transforms.
    for (const auto& g : now.groups)
        if (!root_supported[size_t(g.root)])
            for (size_t member : g.members)
                result.root[member] = -1;
    return result;
}

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

    const auto grouped = group_matches(a, b);
    std::unordered_map<ModelKey, int, ModelKeyHash> by_key;
    std::unordered_map<uint64_t, std::vector<int>> by_place;
    std::vector<bool> reserved = grouped.used_old;
    for (size_t i = 0; i < a.models.size(); ++i) {
        if (reserved[i]) continue;
        const auto [where, unique] = by_key.emplace(key(a.models[i]), static_cast<int>(i));
        if (!unique) where->second = -1;
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
        if (grouped.handled[i] && grouped.previous[i] < 0) {
            if (grouped.root[i] < 0) {
                ++s.unmatched;
                continue;
            }
            const auto& root_a = a.models[static_cast<size_t>(grouped.root[i])].motion.matrix;
            Mat3 common_l{};
            Vec3 common_t{};
            bool common_spun = false;
            Mat3 end_l{};
            Vec3 end_t{};
            bool end_spun = false;
            bool safe = root_move(root_a, mb.motion.matrix, 0, end_l, end_t, end_spun);
            Vec3 start = apply_linear(end_l, model_tb), delta{};
            for (size_t k = 0; k < 3; ++k) {
                start[k] += end_t[k];
                delta[k] = model_tb[k] - start[k];
            }
            safe = safe && length(delta) <= kMaxMove * std::max(length(start), length(model_tb));
            if (safe && root_move(root_a, mb.motion.matrix, t, common_l, common_t, common_spun)) {
                moves.push_back({i, common_l, common_t, root_a.m == mb.motion.matrix.m});
                ++s.followed;
                ++s.unmatched;
            } else ++s.moved;
            continue;
        }
        int found = grouped.handled[i] ? grouped.previous[i] : -1;
        if (found < 0)
            if (const auto it = by_key.find(key(mb));
                it != by_key.end() && it->second >= 0 && !reserved[static_cast<size_t>(it->second)]) found = it->second;
        if (found < 0) // the same model reached through other nodes: the nearest
            if (const auto it = by_place.find(place(mb)); it != by_place.end()) {
                float best = 0, second = 0;
                size_t candidates = 0;
                for (int j : it->second) {
                    if (reserved[static_cast<size_t>(j)]) continue;
                    ++candidates;
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
                s.compared += candidates;
                if (candidates) ++s.nearest;
                if (candidates > 1 && second < 4 * best) ++s.ambiguous;
            }
        if (found < 0) {
            ++s.unmatched;
            strays.push_back(i);
            continue;
        }
        reserved[static_cast<size_t>(found)] = true;
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
        const bool shared_root = grouped.handled[i] || (ma.motion.valid && mb.motion.valid && ma.motion.path == mb.motion.path);
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
    // A changed mesh can still be attached to a moving parent below the
    // motion root. Prefer a directly matched parent; otherwise require every
    // directly matched descendant to agree. Root-level siblings are not
    // attachment evidence, and followers never supply evidence to each other.
    struct SubtreeMove {
        size_t move;
        bool coherent = true;
    };
    std::unordered_map<ModelKey, SubtreeMove, ModelKeyHash> subtree_moves, node_moves;
    auto register_move = [&](auto& map, const ModelKey& node, size_t k) {
        const auto [at, unique] = map.emplace(node, SubtreeMove{k});
        if (!unique) {
            const auto& other = moves[at->second.move];
            if (other.l != moves[k].l || other.t != moves[k].t) at->second.coherent = false;
        }
    };
    for (size_t k = 0; k < moves.size(); ++k) {
        const auto& move = moves[k];
        if (!grouped.handled[move.model] || grouped.previous[move.model] < 0) continue;
        const auto& model = b.models[move.model];
        register_move(node_moves, {model.viewport, model.motion.path, model.path}, k);
        for (size_t level = 2; level < model.depth; ++level) {
            const ModelKey node{model.viewport, model.motion.path, model.ancestors[level]};
            register_move(subtree_moves, node, k);
        }
    }
    for (auto& move : moves) {
        if (!grouped.handled[move.model] || grouped.previous[move.model] >= 0) continue;
        const auto& model = b.models[move.model];
        for (int level = int(model.depth) - 1; level >= 2; --level) {
            const ModelKey node{model.viewport, model.motion.path, model.ancestors[size_t(level)]};
            const auto direct = node_moves.find(node);
            const auto subtree = subtree_moves.find(node);
            // A directly matched parent wins over independently animating
            // descendants. Otherwise require agreement throughout the subtree.
            const auto* found = direct != node_moves.end() ? &direct->second : subtree != subtree_moves.end() ? &subtree->second : nullptr;
            if (!found) continue;
            // A contradictory nearest subtree cannot borrow a more distant one.
            if (found->coherent) {
                const auto& relative = moves[found->move];
                move.l = relative.l;
                move.t = relative.t;
                move.identity = relative.identity;
            }
            break;
        }
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

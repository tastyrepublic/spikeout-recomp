#include "test.hpp"
#include "runtime/real3d_interp.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace {

m3::Mat4 transform(float degrees, const std::array<float, 3>& scale, float shear = 0) {
    m3::Mat4 m{};
    const float angle = degrees * 3.14159265358979323846f / 180;
    const float c = std::cos(angle), s = std::sin(angle);
    m.m = {c * scale[0], 0, -s * scale[0], 0,
           c * shear, scale[1], -s * shear, 0,
           s * scale[2], 0, c * scale[2], 0,
           3, 2, -20, 1};
    return m;
}

m3::Real3dScene triangle(const m3::Mat4& matrix) {
    m3::Real3dScene scene;
    scene.viewports.resize(1);
    m3::Real3dScene::Polygon polygon{};
    polygon.vertex_count = 3;
    scene.polygons.push_back(polygon);
    const std::array<std::array<float, 3>, 3> points{{{1, 2, 3}, {-2, 1, -1}, {0, -3, 2}}};
    for (const auto& p : points) {
        const auto& m = matrix.m;
        scene.vertices.push_back({m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
                                  m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
                                  m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14],
                                  0, 0, 1, m[8], m[9], m[10]});
    }
    m3::Real3dScene::Model model;
    model.address = 0x100;
    model.matrix = matrix;
    model.polygon_count = 1;
    scene.models.push_back(model);
    return scene;
}

float position_error(const m3::Real3dScene& a, const m3::Real3dScene& b) {
    if (a.vertices.size() != b.vertices.size()) return 1e9f;
    float error = 0;
    for (size_t i = 0; i < a.vertices.size(); ++i) {
        const auto& x = a.vertices[i];
        const auto& y = b.vertices[i];
        for (float value : {x.x, x.y, x.z, x.nx, x.ny, x.nz, y.x, y.y, y.z, y.nx, y.ny, y.nz})
            if (!std::isfinite(value)) return 1e9f;
        error = std::max({error, std::fabs(x.x - y.x), std::fabs(x.y - y.y), std::fabs(x.z - y.z)});
        error = std::max({error, std::fabs(x.nx - y.nx), std::fabs(x.ny - y.ny), std::fabs(x.nz - y.nz)});
    }
    return error;
}

m3::Real3dScene rooted(const m3::Mat4& root, const m3::Mat4& local) {
    auto scene = triangle(root * local);
    scene.models[0].motion = {root, local, 0x123, true};
    return scene;
}

} // namespace

TEST_CASE("smooth motion: a stationary mirrored model does not change shape") {
    for (const auto scale : {std::array<float, 3>{-1, 1, 1}, {1, -1, 1}, {1, 1, -1}, {2, -3, 4}}) {
        for (float angle : {0.0f, 30.0f, 90.0f, 170.0f}) {
            const auto scene = triangle(transform(angle, scale));
            for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
                m3::Real3dScene out;
                CHECK(m3::interpolate(scene, scene, t, out));
                CHECK(position_error(scene, out) == 0);
            }
        }
    }
}

TEST_CASE("smooth motion: a stationary sheared model does not change shape") {
    for (float shear : {-0.6f, 0.4f}) {
        const auto scene = triangle(transform(30, {1, 2, 3}, shear));
        for (float t : {0.0f, 0.5f, 1.0f}) {
            m3::Real3dScene out;
            CHECK(m3::interpolate(scene, scene, t, out));
            CHECK(position_error(scene, out) == 0);
        }
    }
}

TEST_CASE("smooth motion: stationary collapsed models do not cause a scene cut") {
    for (const auto scale : {std::array<float, 3>{0, 1, 1}, {1e-5f, 1e-5f, 1e-5f}}) {
        const auto scene = triangle(transform(0, scale));
        m3::Real3dScene out;
        m3::InterpStats stats;
        CHECK(m3::interpolate(scene, scene, 0.5f, out, &stats));
        CHECK(stats.blended == 1 && stats.moved == 0 && !stats.cut);
        CHECK(position_error(scene, out) == 0);
    }
}

TEST_CASE("smooth motion: reflected rotating model preserves endpoints and halfway rotation") {
    const auto before = triangle(transform(10, {2, 3, -4}));
    const auto after = triangle(transform(50, {2, 3, -4}));
    for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        m3::Real3dScene out;
        CHECK(m3::interpolate(before, after, t, out));
        CHECK(position_error(triangle(transform(10 + 40 * t, {2, 3, -4})), out) < 1e-4f);
    }
}

TEST_CASE("smooth motion: rotating shear and reflection preserve affine geometry") {
    const auto before = triangle(transform(10, {2, 3, -4}, -0.6f));
    const auto after = triangle(transform(50, {3, 2, -2}, 0.4f));
    for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        m3::Real3dScene out;
        CHECK(m3::interpolate(before, after, t, out));
        const auto expected = triangle(transform(10 + 40 * t, {2 + t, 3 - t, -4 + 2 * t}, -0.6f + t));
        CHECK(position_error(expected, out) < 1e-4f);
    }
}

TEST_CASE("smooth motion: colliding hashes do not match different model identities") {
    auto before = triangle(transform(0, {1, 1, 1}));
    auto after = triangle(transform(0, {1, 1, 1}));
    before.models[0].address = 0x100;
    after.models[0].address = 0x101;
    after.models[0].path = 0x01000000;
    before.models[0].matrix.m[12] -= 2;
    m3::Real3dScene out;
    m3::InterpStats stats;
    CHECK(!m3::interpolate(before, after, 0.5f, out, &stats));
    CHECK(stats.unmatched == 1);
    CHECK(position_error(after, out) == 0);
}

TEST_CASE("smooth motion: static descendants share their moving root without opening seams") {
    auto stage = [](float angle) {
        const auto root = transform(angle, {1, 1, 1});
        auto scene = triangle(root);
        auto local = transform(0, {1, 1, 1});
        local.m[12] = local.m[13] = local.m[14] = 0;
        scene.models[0].motion = {root, local, 0x123, true};
        auto other = scene.models[0];
        other.address += 1;
        other.first_polygon = 1;
        other.motion.local.m[12] += 70;
        other.motion.local.m[14] -= 50;
        // A different model origin for exactly the same eye-space surface.
        for (size_t row = 0; row < 3; ++row)
            other.matrix.m[12 + row] += root.m[row] * 70 - root.m[8 + row] * 50;
        scene.models.push_back(other);
        auto polygon = scene.polygons[0];
        polygon.first_vertex = 3;
        scene.polygons.push_back(polygon);
        const auto vertices = scene.vertices;
        scene.vertices.insert(scene.vertices.end(), vertices.begin(), vertices.end());
        return scene;
    };
    const auto before = stage(0);
    for (float angle : {0.25f, 1.0f, 4.0f}) {
        const auto after = stage(angle);
        for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            m3::Real3dScene out;
            CHECK(m3::interpolate(before, after, t, out));
            for (size_t k = 0; k < 3; ++k) {
                const auto& a = out.vertices[k];
                const auto& b = out.vertices[k + 3];
                CHECK(a.x == b.x && a.y == b.y && a.z == b.z);
                CHECK(a.nx == b.nx && a.ny == b.ny && a.nz == b.nz);
            }
        }
    }
}

TEST_CASE("smooth motion: animated and billboard descendants retain their own motion") {
    auto before = triangle(transform(10, {1, 1, 1}));
    auto after = triangle(transform(50, {1, 1, 1}));
    const auto root = m3::Mat4::identity();
    before.models[0].motion = {root, before.models[0].matrix, 0x123, true};
    after.models[0].motion = {root, after.models[0].matrix, 0x123, true};
    for (int mode = 0; mode < 3; ++mode) {
        if (mode == 1) after.models[0].motion.valid = false;
        if (mode == 2) {
            after.models[0].motion.valid = true;
            after.models[0].motion.local = before.models[0].motion.local;
            after.models[0].motion.path += 1;
        }
        m3::Real3dScene out;
        CHECK(m3::interpolate(before, after, 0.5f, out));
        CHECK(position_error(triangle(transform(30, {1, 1, 1})), out) < 1e-4f);
    }
}

TEST_CASE("smooth motion: cut detection uses model distance regardless of the root origin") {
    for (bool cut : {false, true}) {
        auto root_a = transform(0, {1, 1, 1});
        root_a.m[12] = root_a.m[13] = 0;
        root_a.m[14] = cut ? -5000.0f : 0.0f;
        auto root_b = root_a;
        root_b.m[12] = cut ? 200.0f : 1.0f;
        auto local = transform(0, {1, 1, 1});
        local.m[12] = local.m[13] = 0;
        local.m[14] = cut ? 4990.0f : -500.0f;
        auto model_a = root_a, model_b = root_b;
        model_a.m[14] += local.m[14];
        model_b.m[14] += local.m[14];
        auto before = triangle(model_a), after = triangle(model_b);
        before.models[0].motion = {root_a, local, 0x123, true};
        after.models[0].motion = {root_b, local, 0x123, true};
        m3::Real3dScene out;
        m3::InterpStats stats;
        CHECK(m3::interpolate(before, after, 0.5f, out, &stats) == !cut);
        CHECK(stats.moved == (cut ? 1u : 0u));
        if (cut) CHECK(position_error(after, out) == 0);
        else {
            model_a.m[12] = 0.5f;
            CHECK(position_error(triangle(model_a), out) < 1e-4f);
        }
    }
}

TEST_CASE("smooth motion: a turning camera is independent of a distant root origin") {
    auto pose = [](float degrees, float z) {
        auto m = transform(degrees, {1, 1, 1});
        for (size_t row = 0; row < 3; ++row) m.m[12 + row] = m.m[8 + row] * z;
        return m;
    };
    for (float distance : {20.0f, 5000.0f}) {
        auto local = m3::Mat4::identity();
        local.m[14] = distance - 10;
        auto before = triangle(pose(0, -10));
        auto after = triangle(pose(2, -10));
        before.models[0].motion = {pose(0, -distance), local, 0x123, true};
        after.models[0].motion = {pose(2, -distance), local, 0x123, true};
        for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            m3::Real3dScene out;
            CHECK(m3::interpolate(before, after, t, out));
            CHECK(position_error(triangle(pose(2 * t, -10)), out) < 5e-4f);
        }
    }
}

TEST_CASE("smooth motion: a shared rigid root retains its fixed pivot at small and large turns") {
    for (float angle : {0.1f, 1.0f, 3.0f, 3.6f, 3.7f, 40.0f}) {
        auto before = triangle(transform(0, {1, 1, 1}));
        auto after = triangle(transform(angle, {1, 1, 1}));
        before.models[0].motion = {before.models[0].matrix, m3::Mat4::identity(), 0x123, true};
        after.models[0].motion = {after.models[0].matrix, m3::Mat4::identity(), 0x123, true};
        for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            m3::Real3dScene out;
            CHECK(m3::interpolate(before, after, t, out));
            CHECK(position_error(triangle(transform(angle * t, {1, 1, 1})), out) < 1e-4f);
        }
    }
}

TEST_CASE("smooth motion: near-rigid shared roots do not switch interpolation trajectories") {
    auto before = triangle(transform(0, {1, 1, 1}));
    before.models[0].motion = {before.models[0].matrix, m3::Mat4::identity(), 0x123, true};
    m3::Real3dScene previous;
    for (float epsilon : {0.0f, 8e-6f, 9e-6f, 10e-6f, 11e-6f, 12e-6f}) {
        auto matrix = transform(40, {1 + epsilon, 1 + epsilon, 1 + epsilon});
        matrix.m[12] = 4;
        matrix.m[13] = 4;
        matrix.m[14] = -15;
        auto after = triangle(matrix);
        after.models[0].motion = {matrix, m3::Mat4::identity(), 0x123, true};
        m3::Real3dScene out;
        CHECK(m3::interpolate(before, after, 0.5f, out));
        if (!previous.vertices.empty()) CHECK(position_error(previous, out) < 1e-4f);
        previous = out;
    }
}

TEST_CASE("smooth motion: fast-turning roots retain position blending") {
    auto root_a = transform(0, {1, 1, 1});
    auto root_b = transform(150, {1, 1, 1});
    root_b.m[12] += 2;
    const auto local = m3::Mat4::identity();
    auto expected = root_b;
    expected.m[12] -= 1;
    m3::Real3dScene out;
    m3::InterpStats stats;
    CHECK(m3::interpolate(rooted(root_a, local), rooted(root_b, local), 0.5f, out, &stats));
    CHECK(stats.spun == 1 && stats.moved == 0 && !stats.cut);
    CHECK(position_error(rooted(expected, local), out) < 1e-4f);
}

TEST_CASE("smooth motion: a tiny local animation does not switch the root trajectory") {
    auto local = m3::Mat4::identity();
    local.m[14] = -20;
    auto changed = local;
    changed.m[14] = std::nextafter(local.m[14], 0.0f);
    for (float angle : {1.0f, 10.0f, 20.0f}) {
        const auto before = rooted(transform(0, {1, 1, 1}), local);
        for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            m3::Real3dScene stationary, animated;
            CHECK(m3::interpolate(before, rooted(transform(angle, {1, 1, 1}), local), t, stationary));
            CHECK(m3::interpolate(before, rooted(transform(angle, {1, 1, 1}), changed), t, animated));
            auto between = local;
            between.m[14] += t * (changed.m[14] - local.m[14]);
            CHECK(position_error(stationary, animated) < 1e-4f);
            CHECK(position_error(rooted(transform(angle * t, {1, 1, 1}), between), animated) < 1e-4f);
        }
    }
}

TEST_CASE("smooth motion: local joints animate under a turning reflected root") {
    for (const auto scale : {std::array<float, 3>{1, 1, 1}, {-1, 1, 1}}) {
      for (float angle : {30.0f, 150.0f}) {
        auto local_a = transform(0, {1, 1, 1}), local_b = transform(angle, {1, 1, 1});
        local_b.m[12] += 2;
        const auto before = rooted(transform(0, scale), local_a);
        const auto after = rooted(transform(20, scale), local_b);
        for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            auto local = transform(angle > m3::kMaxTurn ? angle : angle * t, {1, 1, 1});
            local.m[12] += 2 * t;
            m3::Real3dScene out;
            CHECK(m3::interpolate(before, after, t, out));
            CHECK(position_error(rooted(transform(20 * t, scale), local), out) < 1e-4f);
        }
      }
    }
}

TEST_CASE("smooth motion: strays inherit shared root and collapsed identity moves") {
    for (bool stationary : {false, true}) {
        const std::array<float, 3> scale{stationary ? 0.0f : 1.0f, 1, 1};
        const auto local = m3::Mat4::identity();
        auto before = rooted(transform(0, scale), local);
        auto after = rooted(transform(stationary ? 0.0f : 20.0f, scale), local);
        after.models[0].depth = 2;
        after.models[0].ancestors[1] = 0xabc;
        auto stray = after.models[0];
        stray.address += 1;
        stray.first_polygon = 1;
        after.models.push_back(stray);
        auto polygon = after.polygons[0];
        polygon.first_vertex = 3;
        after.polygons.push_back(polygon);
        const auto vertices = after.vertices;
        after.vertices.insert(after.vertices.end(), vertices.begin(), vertices.end());
        m3::Real3dScene out;
        m3::InterpStats stats;
        CHECK(m3::interpolate(before, after, 0.5f, out, &stats));
        CHECK(stats.blended == 1 && stats.unmatched == 1 && stats.followed == 1 && !stats.cut);
        for (size_t k = 0; k < 3; ++k) {
            CHECK(out.vertices[k].x == out.vertices[k + 3].x);
            CHECK(out.vertices[k].y == out.vertices[k + 3].y);
            CHECK(out.vertices[k].z == out.vertices[k + 3].z);
        }
        if (stationary) CHECK(position_error(after, out) == 0);
    }
}

TEST_CASE("smooth motion: root screw follows an offset axis and axial translation") {
    auto pose = [](float angle, float rise) {
        auto matrix = transform(angle, {1, 1, 1});
        const std::array<float, 3> pivot{10, 5, -7}, displacement{-7, -3, -13};
        for (size_t r = 0; r < 3; ++r) {
            matrix.m[12 + r] = pivot[r] + (r == 1 ? rise : 0);
            for (size_t c = 0; c < 3; ++c) matrix.m[12 + r] += matrix.m[c * 4 + r] * displacement[c];
        }
        return matrix;
    };
    const auto local = m3::Mat4::identity();
    for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        m3::Real3dScene out;
        CHECK(m3::interpolate(rooted(pose(0, 0), local), rooted(pose(20, 4), local), t, out));
        CHECK(position_error(rooted(pose(20 * t, 4 * t), local), out) < 1e-4f);
    }
}

TEST_CASE("smooth motion: invalid moving roots conservatively retain the newer scene") {
    const auto local = m3::Mat4::identity();
    const auto before = rooted(transform(0, {1, 1, 1}), local);
    for (float scale : {0.0f, std::numeric_limits<float>::infinity()}) {
        auto root = transform(10, {scale, 1, 1});
        auto after = rooted(root, local);
        m3::Real3dScene out;
        m3::InterpStats stats;
        CHECK(!m3::interpolate(before, after, 0.5f, out, &stats));
        CHECK(stats.cut && stats.blended == 0 && stats.moved == 1);
    }
}

TEST_CASE("smooth motion: moving x and y reflections exercise the affine path") {
    for (const auto scale : {std::array<float, 3>{-1, 1, 1}, {1, -1, 1}}) {
        const auto before = triangle(transform(10, scale, 0.3f));
        const auto after = triangle(transform(50, scale, 0.3f));
        for (float t : {0.0f, 0.5f, 1.0f}) {
            m3::Real3dScene out;
            CHECK(m3::interpolate(before, after, t, out));
            CHECK(position_error(triangle(transform(10 + 40 * t, scale, 0.3f)), out) < 1e-4f);
        }
    }
}

TEST_CASE("smooth motion: builder tracks root and local transforms across siblings and billboards") {
    m3::Real3d memory;
    auto word = [&](uint32_t address, uint32_t value) {
        const uint32_t swapped = (value >> 24) | ((value >> 8) & 0xff00) | ((value << 8) & 0xff0000) | (value << 24);
        if (address >= 0x800000) memory.write_culling_high((address - 0x800000) * 4, swapped);
        else memory.write_culling_low(address * 4, swapped);
    };
    auto real = [&](uint32_t address, float value) { word(address, std::bit_cast<uint32_t>(value)); };
    auto matrix = [&](uint32_t index, const m3::Mat4& value) {
        const uint32_t address = 0x1000 + index * 12;
        for (uint32_t k = 0; k < 3; ++k) real(address + k, value.m[12 + k]);
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t col = 0; col < 3; ++col) real(address + 3 + row * 3 + col, value.m[col * 4 + row]);
    };
    auto viewport = [&](uint32_t address, uint32_t next, uint32_t root) {
        word(address + 1, next);
        word(address + 2, root);
        for (uint32_t k : {8u, 9u, 10u, 11u}) real(address + k, 0.5f);
        word(address + 0x14, (384u * 4) << 16 | 496u * 4);
        word(address + 0x16, 0x1000);
    };
    auto node = [&](uint32_t address, uint32_t flags, uint32_t index, uint32_t child, uint32_t sibling = 0x1000000) {
        word(address, flags);
        word(address + 1, 2); // no culling
        word(address + 3, 0x7f000 | index);
        word(address + 7, child);
        word(address + 8, sibling);
    };
    const auto identity = m3::Mat4::identity();
    matrix(0, identity);
    auto root = identity;
    root.m[12] = 10;
    root.m[13] = 20;
    root.m[14] = 30;
    matrix(1, root);
    auto rotation = identity;
    rotation.m[0] = rotation.m[5] = 0;
    rotation.m[1] = 1;
    rotation.m[4] = -1;
    matrix(2, rotation);
    root.m[12] = 50;
    matrix(3, root);
    viewport(0x800000, 0x800100, 0x100);
    viewport(0x800100, 0x1000000, 0x01000500);
    node(0x100, 1, 1, 0x04000300, 0x180);
    node(0x180, 1, 3, 0x01000400);
    word(0x300, 0x200);
    word(0x301, 0x240);
    word(0x302, 0x02000280);
    node(0x200, 0x11, 0, 0x01000100);
    real(0x204, 3);
    real(0x205, 4);
    real(0x206, 5);
    node(0x240, 0x81, 2, 0x01000200); // billboard
    node(0x280, 1, 2, 0x01000300);    // ordinary sibling after billboard
    m3::Real3dScene scene;
    m3::Real3dSceneBuilder().build(memory, scene);
    auto find = [&](uint32_t address) -> const m3::Real3dScene::Model* {
        for (const auto& model : scene.models) if (model.address == address) return &model;
        return nullptr;
    };
    const auto *translated = find(0x100), *billboard = find(0x200), *rotated = find(0x300);
    const auto *sibling = find(0x400), *direct = find(0x500);
    CHECK(scene.models.size() == 5 && translated && billboard && rotated && sibling && direct);
    if (!translated || !billboard || !rotated || !sibling || !direct) return;
    auto local = identity;
    local.m[12] = 3;
    local.m[13] = 4;
    local.m[14] = 5;
    CHECK(translated->motion.valid && translated->motion.local.m == local.m);
    CHECK(rotated->motion.valid && rotated->motion.local.m == rotation.m);
    CHECK(!billboard->motion.valid);
    CHECK(translated->motion.path == rotated->motion.path && translated->motion.matrix.m == rotated->motion.matrix.m);
    CHECK(sibling->motion.valid && sibling->motion.path != translated->motion.path && sibling->motion.local.m == identity.m);
    CHECK(!direct->motion.valid);
    CHECK((translated->motion.matrix * translated->motion.local).m == translated->matrix.m);
    CHECK((rotated->motion.matrix * rotated->motion.local).m == rotated->matrix.m);
}

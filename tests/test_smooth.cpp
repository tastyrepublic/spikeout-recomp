#include "test.hpp"
#include "runtime/real3d_interp.hpp"

#include <algorithm>
#include <array>
#include <cmath>

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

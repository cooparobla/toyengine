/**
 * @file convex_hull_test.cpp
 * @brief The deterministic convex-hull builder: face merging, volume/centroid, support(),
 *        degenerate inputs, and the max_vertices cap.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/geometry/convex_hull.h>
#include <physxcoopa/dynamics/inertia.h>

#include <vector>

COOPA_TEST_SUITE("convex_hull");

using namespace coopa::physx;

namespace {

std::vector<glm::vec3> cube_corners(float half = 1.0f) {
    std::vector<glm::vec3> pts;
    for (float sx : {-half, half})
        for (float sy : {-half, half})
            for (float sz : {-half, half}) pts.push_back(glm::vec3(sx, sy, sz));
    return pts;
}

} // namespace

COOPA_TEST(cube_corners_build_six_merged_faces) {
    auto pts = cube_corners();
    geometry::ConvexHull hull;
    ASSERT_TRUE(geometry::build_convex_hull(pts.data(), pts.size(), hull));
    ASSERT_EQ(hull.vertices.size(), 8u);
    ASSERT_EQ(hull.face_count(), 6u); // not 12 -- proves coplanar triangles merged
    for (std::size_t i = 0; i < hull.face_count(); ++i) {
        std::uint32_t n = hull.face_starts[i + 1] - hull.face_starts[i];
        ASSERT_EQ(n, 4u);
    }
}

COOPA_TEST(interior_points_do_not_change_the_hull) {
    auto pts = cube_corners();
    for (int i = 0; i < 200; ++i) {
        float t = float(i) / 200.0f;
        pts.push_back(glm::vec3(-0.9f + 1.8f * t, 0.1f * (i % 7 - 3), 0.1f * (i % 5 - 2)));
    }
    geometry::ConvexHull hull;
    ASSERT_TRUE(geometry::build_convex_hull(pts.data(), pts.size(), hull));
    ASSERT_EQ(hull.vertices.size(), 8u);
    ASSERT_EQ(hull.face_count(), 6u);
    ASSERT_NEAR(hull.volume, 8.0f, 1e-3f);
}

COOPA_TEST(cube_hull_volume_and_centroid) {
    auto pts = cube_corners();
    geometry::ConvexHull hull;
    ASSERT_TRUE(geometry::build_convex_hull(pts.data(), pts.size(), hull));
    ASSERT_NEAR(hull.volume, 8.0f, 1e-3f); // 2x2x2 cube
    ASSERT_VEC_NEAR(hull.centroid, glm::vec3(0.0f), 1e-3f);
}

COOPA_TEST(tetrahedron_matches_analytic_volume) {
    std::vector<glm::vec3> pts = {
        glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)};
    geometry::ConvexHull hull;
    ASSERT_TRUE(geometry::build_convex_hull(pts.data(), pts.size(), hull));
    ASSERT_NEAR(hull.volume, 1.0f / 6.0f, 1e-4f);
}

COOPA_TEST(hull_mass_properties_matches_box_inertia) {
    auto pts = cube_corners(0.5f); // half_extents (0.5,0.5,0.5) -- a unit cube
    geometry::ConvexHull hull;
    ASSERT_TRUE(geometry::build_convex_hull(pts.data(), pts.size(), hull));

    float density = 2.0f;
    dynamics::MassProperties hull_mp = dynamics::hull_mass_properties(hull, density);
    glm::vec3 box_inv_inertia = dynamics::box_inverse_inertia(glm::vec3(0.5f), hull_mp.mass);

    ASSERT_NEAR(hull_mp.mass, 1.0f * density, 1e-3f);
    ASSERT_VEC_NEAR(hull_mp.inv_inertia_local, box_inv_inertia, box_inv_inertia.x * 0.02f + 1e-4f);
}

COOPA_TEST(degenerate_clouds_are_rejected) {
    geometry::ConvexHull hull;
    std::vector<glm::vec3> too_few = {glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)};
    ASSERT_TRUE(!geometry::build_convex_hull(too_few.data(), too_few.size(), hull));

    std::vector<glm::vec3> coincident(5, glm::vec3(1, 2, 3));
    ASSERT_TRUE(!geometry::build_convex_hull(coincident.data(), coincident.size(), hull));

    std::vector<glm::vec3> collinear;
    for (int i = 0; i < 6; ++i) collinear.push_back(glm::vec3(float(i), 0, 0));
    ASSERT_TRUE(!geometry::build_convex_hull(collinear.data(), collinear.size(), hull));

    std::vector<glm::vec3> coplanar;
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 3; ++y) coplanar.push_back(glm::vec3(float(x), float(y), 0.0f));
    ASSERT_TRUE(!geometry::build_convex_hull(coplanar.data(), coplanar.size(), hull));
}

COOPA_TEST(support_matches_brute_force_over_vertices) {
    auto pts = cube_corners();
    geometry::ConvexHull hull;
    ASSERT_TRUE(geometry::build_convex_hull(pts.data(), pts.size(), hull));

    glm::vec3 dirs[] = {glm::vec3(1, 0.3f, -0.2f), glm::vec3(-1, 1, 1), glm::vec3(0.1f, -1, 0.4f)};
    for (const glm::vec3& d : dirs) {
        glm::vec3 got = hull.support(d);
        glm::vec3 best = hull.vertices[0];
        float best_dot = glm::dot(best, d);
        for (const glm::vec3& v : hull.vertices) {
            float dp = glm::dot(v, d);
            if (dp > best_dot) {
                best_dot = dp;
                best = v;
            }
        }
        ASSERT_NEAR(glm::dot(got, d), best_dot, 1e-4f);
    }
}

COOPA_TEST(max_vertices_caps_and_stays_deterministic) {
    std::vector<glm::vec3> sphere_like;
    for (int i = 0; i < 500; ++i) {
        float theta = float(i) * 0.123f;
        float phi = float(i) * 0.071f;
        sphere_like.push_back(glm::vec3(std::sin(phi) * std::cos(theta), std::sin(phi) * std::sin(theta), std::cos(phi)));
    }
    geometry::ConvexHull a, b;
    ASSERT_TRUE(geometry::build_convex_hull(sphere_like.data(), sphere_like.size(), a, 1e-5f, 32));
    ASSERT_TRUE(geometry::build_convex_hull(sphere_like.data(), sphere_like.size(), b, 1e-5f, 32));
    ASSERT_TRUE(a.vertices.size() <= 32u);
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    for (std::size_t i = 0; i < a.vertices.size(); ++i) ASSERT_VEC_NEAR(a.vertices[i], b.vertices[i], 1e-6f);
}

COOPA_TEST(building_the_same_cube_twice_is_bit_identical) {
    auto pts = cube_corners();
    geometry::ConvexHull a, b;
    ASSERT_TRUE(geometry::build_convex_hull(pts.data(), pts.size(), a));
    ASSERT_TRUE(geometry::build_convex_hull(pts.data(), pts.size(), b));
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    ASSERT_EQ(a.face_vertices.size(), b.face_vertices.size());
    for (std::size_t i = 0; i < a.vertices.size(); ++i) ASSERT_VEC_NEAR(a.vertices[i], b.vertices[i], 0.0f);
    for (std::size_t i = 0; i < a.face_vertices.size(); ++i) ASSERT_EQ(a.face_vertices[i], b.face_vertices[i]);
}

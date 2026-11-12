/**
 * @file geometry_test.cpp
 * @brief Geometry primitives and mass properties: ray/AABB, segment closest points, orthonormal bases,
 *        and the closed-form inertia tensors every body is built from. Shape-vs-shape contact
 *        generation lives in narrowphase_test.cpp.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/util/math.h>
#include <physxcoopa/geometry/aabb.h>
#include <physxcoopa/geometry/ray.h>
#include <physxcoopa/geometry/capsule.h>
#include <physxcoopa/dynamics/inertia.h>

COOPA_TEST_SUITE("geometry");

using namespace coopa::physx;

COOPA_TEST(ray_hits_aabb_at_hand_computed_distance) {
    geometry::AABB box;
    box.min = glm::vec3(-1.0f, -1.0f, -1.0f);
    box.max = glm::vec3(1.0f, 1.0f, 1.0f);

    geometry::Ray ray;
    ray.origin = glm::vec3(0.0f, 0.0f, 5.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    ray.max_distance = 100.0f;

    float t;
    ASSERT_TRUE(ray.intersect(box, t));
    ASSERT_NEAR(t, 4.0f, 1e-5f); // travels from z=5 to the box's near face at z=1

    geometry::Ray miss;
    miss.origin = glm::vec3(5.0f, 5.0f, 5.0f);
    miss.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    miss.max_distance = 100.0f;
    ASSERT_TRUE(!miss.intersect(box, t));
}

COOPA_TEST(closest_points_between_segments_are_exact) {
    // Two parallel segments, offset by 1 unit in x, both spanning z in [0,1].
    glm::vec3 c1, c2;
    geometry::closest_points_segment_segment(
        glm::vec3(0, 0, 0), glm::vec3(0, 0, 1),
        glm::vec3(1, 0, 0), glm::vec3(1, 0, 1),
        c1, c2);
    ASSERT_NEAR(glm::length(c2 - c1), 1.0f, 1e-4f);
    ASSERT_NEAR(c1.z, c2.z, 1e-4f); // parallel segments -> closest points at equal z

    // Perpendicular, crossing segments should meet exactly at the origin.
    geometry::closest_points_segment_segment(
        glm::vec3(-1, 0, 0), glm::vec3(1, 0, 0),
        glm::vec3(0, -1, 0), glm::vec3(0, 1, 0),
        c1, c2);
    ASSERT_VEC_NEAR(c1, glm::vec3(0, 0, 0), 1e-4f);
    ASSERT_VEC_NEAR(c2, glm::vec3(0, 0, 0), 1e-4f);
}

COOPA_TEST(orthonormal_basis_is_orthonormal) {
    glm::vec3 n = util::safe_normalize(glm::vec3(0.3f, -0.7f, 0.4f));
    glm::vec3 t1, t2;
    util::orthonormal_basis(n, t1, t2);
    ASSERT_NEAR(glm::dot(n, t1), 0.0f, 1e-4f);
    ASSERT_NEAR(glm::dot(n, t2), 0.0f, 1e-4f);
    ASSERT_NEAR(glm::dot(t1, t2), 0.0f, 1e-4f);
    ASSERT_NEAR(glm::length(t1), 1.0f, 1e-4f);
    ASSERT_NEAR(glm::length(t2), 1.0f, 1e-4f);
}

COOPA_TEST(shape_inertia_matches_closed_forms) {
    glm::vec3 box_inv = dynamics::box_inverse_inertia(glm::vec3(0.5f), 2.0f);
    ASSERT_TRUE(box_inv.x > 0.0f && box_inv.y > 0.0f && box_inv.z > 0.0f);

    glm::vec3 sphere_inv = dynamics::sphere_inverse_inertia(1.0f, 2.0f);
    float expected = 1.0f / (0.4f * 2.0f * 1.0f * 1.0f);
    ASSERT_NEAR(sphere_inv.x, expected, 1e-5f);
    ASSERT_NEAR(sphere_inv.y, expected, 1e-5f);
    ASSERT_NEAR(sphere_inv.z, expected, 1e-5f);

    // A capsule's axial inertia (about its own long axis) must be less than or equal to its
    // perpendicular inertia -- i.e. its inverse-axial must be >= inverse-perpendicular --
    // since mass is concentrated closer to the long axis than to a perpendicular one.
    glm::vec3 cap_inv = dynamics::capsule_inverse_inertia(0.5f, 1.0f, 2.0f);
    ASSERT_TRUE(cap_inv.z >= cap_inv.x);
}

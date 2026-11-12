/**
 * @file queries_test.cpp
 * @brief Scene queries on PhysicsWorld: raycasts (including a hull reproducing an equivalent
 *        box), overlaps, exact sphere/box/capsule sweeps (box corners, capsules, mesh ramps,
 *        started-inside and resting-contact rules), QueryFilter, compute_penetration, and
 *        Shape::local_rotation being honoured by every query path. The PhysicsSystem wrappers
 *        are in physics_system_test.cpp.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/world.h>
#include <physxcoopa/query/queries.h>
#include <physxcoopa/debug/debug_draw.h>
#include <algorithm>
#include "support/physics_fixtures.h"

COOPA_TEST_SUITE("queries");

using namespace coopa::physx;
using namespace physxtest;

COOPA_TEST(raycast_hits_box_with_exact_distance_point_and_normal) {
    PhysicsWorld world;
    dynamics::BodyId id = add_static_box(world, glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f));

    geometry::Ray ray;
    ray.origin = glm::vec3(0.0f, 0.0f, 0.0f);
    ray.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    ray.max_distance = 100.0f;

    query::RaycastHit hit;
    ASSERT_TRUE(world.raycast(ray, hit));
    ASSERT_NEAR(hit.distance, 4.5f, 1e-3f); // box's near face at x=4.5
    ASSERT_VEC_NEAR(hit.point, glm::vec3(4.5f, 0.0f, 0.0f), 1e-3f);
    ASSERT_VEC_NEAR(hit.normal, glm::vec3(-1.0f, 0.0f, 0.0f), 1e-3f);
    ASSERT_TRUE(hit.body.index == id.index);
}

/** @brief A cube hull raycast hits at the same distance/point/normal as an equivalent box --
 *         raycast_shape()'s ConvexHull case (exact per-face half-space clipping) must reproduce
 *         ray_vs_obb() exactly for an axis-aligned cube. */
COOPA_TEST(raycast_hits_hull_where_it_hits_an_equivalent_box) {
    PhysicsWorld world;

    glm::vec3 pts[8];
    int k = 0;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2) pts[k++] = glm::vec3(float(sx), float(sy), float(sz)) * 0.5f;
    geometry::ConvexHull hull;
    ASSERT_TRUE(geometry::build_convex_hull(pts, 8, hull));

    dynamics::Body body;
    body.type = dynamics::BodyType::Static;
    body.position = glm::vec3(5.0f, 0.0f, 0.0f);
    dynamics::BodyId id = world.add_body(body, collision::Shape::make_hull(&hull));

    geometry::Ray ray;
    ray.origin = glm::vec3(0.0f, 0.0f, 0.0f);
    ray.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    ray.max_distance = 100.0f;

    query::RaycastHit hit;
    ASSERT_TRUE(world.raycast(ray, hit));
    ASSERT_NEAR(hit.distance, 4.5f, 1e-3f); // hull's near face at x=4.5, same as the box above
    ASSERT_VEC_NEAR(hit.point, glm::vec3(4.5f, 0.0f, 0.0f), 1e-3f);
    ASSERT_VEC_NEAR(hit.normal, glm::vec3(-1.0f, 0.0f, 0.0f), 1e-3f);
    ASSERT_TRUE(hit.body.index == id.index);
}

/** @brief raycast_all() returns every hit, nearest first, at exact distances; raycast_any()
 *         agrees on hit/miss. */
COOPA_TEST(raycast_all_sorts_hits_and_raycast_any_agrees) {
    PhysicsWorld world;
    add_static_box(world, glm::vec3(9.0f, 0.0f, 0.0f), glm::vec3(0.5f));
    add_static_box(world, glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(0.5f));
    add_static_box(world, glm::vec3(6.0f, 0.0f, 0.0f), glm::vec3(0.5f));

    geometry::Ray ray;
    ray.origin = glm::vec3(0.0f, 0.0f, 0.0f);
    ray.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    ray.max_distance = 100.0f;

    std::vector<query::RaycastHit> hits = world.raycast_all(ray);
    ASSERT_TRUE(hits.size() == 3);
    ASSERT_TRUE(hits[0].distance < hits[1].distance);
    ASSERT_TRUE(hits[1].distance < hits[2].distance);
    ASSERT_NEAR(hits[0].distance, 2.5f, 1e-3f);
    ASSERT_NEAR(hits[1].distance, 5.5f, 1e-3f);
    ASSERT_NEAR(hits[2].distance, 8.5f, 1e-3f);

    // raycast_any(): correctness of the boolean, not traversal order (it may stop at any hit).
    EXPECT_TRUE(world.raycast_any(ray));
    geometry::Ray miss_ray = ray;
    miss_ray.direction = glm::vec3(-1.0f, 0.0f, 0.0f); // nothing behind the origin
    EXPECT_FALSE(world.raycast_any(miss_ray));
}

/** @brief overlap_sphere/box/capsule return exactly the body they touch, not a distant one. */
COOPA_TEST(overlaps_find_only_touched_bodies) {
    PhysicsWorld world;
    dynamics::BodyId near_id = add_static_box(world, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.5f));
    add_static_box(world, glm::vec3(20.0f, 0.0f, 0.0f), glm::vec3(0.5f));

    std::vector<dynamics::BodyId> sphere_hits = world.overlap_sphere(glm::vec3(0.2f, 0.0f, 0.0f), 0.5f);
    ASSERT_TRUE(sphere_hits.size() == 1);
    ASSERT_TRUE(sphere_hits[0].index == near_id.index);

    geometry::OBB query_box;
    query_box.center = glm::vec3(0.0f);
    query_box.half_extents = glm::vec3(1.0f);
    std::vector<dynamics::BodyId> box_hits = world.overlap_box(query_box);
    ASSERT_TRUE(box_hits.size() == 1);
    ASSERT_TRUE(box_hits[0].index == near_id.index);

    geometry::Capsule query_capsule;
    query_capsule.a = glm::vec3(0.0f, 0.0f, -0.4f);
    query_capsule.b = glm::vec3(0.0f, 0.0f, 0.4f);
    query_capsule.radius = 0.3f;
    std::vector<dynamics::BodyId> capsule_hits = world.overlap_capsule(query_capsule);
    ASSERT_TRUE(capsule_hits.size() == 1);
    ASSERT_TRUE(capsule_hits[0].index == near_id.index);
}

/**
 * Sweep exactness against a box CORNER: the sphere's path passes 0.3/0.3 off the box's
 * top-side edge line, so it first touches the corner vertex (4.5, 0.5, 0.5) -- where the old
 * "inflate the box, raycast" approximation reported the sharp inflated face instead (4.0).
 * Solving |(x - 4.5, 0.3, 0.3)| = 0.5 gives x = 4.5 - sqrt(0.07). A rotated box cast and a
 * capsule-vs-capsule sweep check the other two exact paths (swept SAT, segment-segment).
 */
COOPA_TEST(sweeps_are_exact_against_box_corner_and_capsule) {
    PhysicsWorld world;
    dynamics::BodyId box_id = add_static_box(world, glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f));

    query::RaycastHit hit;
    ASSERT_TRUE(world.sphere_cast(glm::vec3(0.0f, 0.8f, 0.8f), 0.5f, glm::vec3(1.0f, 0.0f, 0.0f), 100.0f, hit));
    const float expected = 4.5f - std::sqrt(0.07f);
    ASSERT_NEAR(hit.distance, expected, 1e-3f);
    ASSERT_VEC_NEAR(hit.point, glm::vec3(4.5f, 0.5f, 0.5f), 1e-3f);
    ASSERT_VEC_NEAR(hit.normal, glm::vec3(-std::sqrt(0.07f), 0.3f, 0.3f) / 0.5f, 1e-2f);
    ASSERT_TRUE(hit.body == box_id);
    ASSERT_TRUE(hit.shape_index == world.shape_at(box_id));
    ASSERT_TRUE(!hit.started_inside);

    // A box yawed 45 degrees reaches 0.5*sqrt(2) along +X: its leading vertical edge meets the
    // face x=4.5 at d = 4.5 - 0.7071.
    query::RaycastHit box_hit;
    ASSERT_TRUE(world.box_cast(glm::vec3(0.0f), glm::vec3(0.5f), glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 0, 1)),
                               glm::vec3(1.0f, 0.0f, 0.0f), 100.0f, box_hit));
    ASSERT_NEAR(box_hit.distance, 4.5f - 0.5f * std::sqrt(2.0f), 1e-3f);
    ASSERT_VEC_NEAR(box_hit.normal, glm::vec3(-1.0f, 0.0f, 0.0f), 1e-3f);
    ASSERT_VEC_NEAR(box_hit.point, glm::vec3(4.5f, 0.0f, 0.0f), 1e-2f);

    // Capsule (Z axis, r 0.3, half-height 0.5) swept +X past a Y-axis capsule (r 0.2) at
    // (3, 0, 0.8): the cast's top core end (x, 0, 0.5) meets it when
    // (3 - x)^2 + 0.3^2 = 0.5^2 -> x = 2.6, normal along (-0.4, 0, -0.3) / 0.5.
    PhysicsWorld capsules;
    dynamics::Body target;
    target.type = dynamics::BodyType::Static;
    target.position = glm::vec3(3.0f, 0.0f, 0.8f);
    capsules.add_body(target, collision::Shape::make_capsule(0.2f, 1.0f, /*axis=*/1));
    query::RaycastHit cap_hit;
    ASSERT_TRUE(capsules.capsule_cast(glm::vec3(0.0f), 0.3f, 0.5f, 2, glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                      glm::vec3(1.0f, 0.0f, 0.0f), 100.0f, cap_hit));
    ASSERT_NEAR(cap_hit.distance, 2.6f, 1e-3f);
    ASSERT_VEC_NEAR(cap_hit.normal, glm::vec3(-0.8f, 0.0f, -0.6f), 1e-2f);
    // A path that clears it (top core end 0.3 + radii 0.5 below z = 1.4) misses.
    ASSERT_TRUE(!capsules.capsule_cast(glm::vec3(0.0f, 0.0f, -0.65f), 0.3f, 0.5f, 2, glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                       glm::vec3(1.0f, 0.0f, 0.0f), 100.0f, cap_hit));
}

/**
 * Sphere/box/capsule casts against a mesh ramp (30 degrees, normal n = (-0.5, 0, 0.866)) are
 * real sweeps through the mesh BVH, not a raycast of the surface: a sphere dropped at x=2
 * stops when its centre is 0.5 off the plane (z = 1.1547 + 0.5 / cos30), a box when its
 * downhill-lower edge (x + 0.25, z - 0.25) touches it.
 */
COOPA_TEST(sweeps_are_exact_against_mesh_ramp) {
    geometry::TriangleMesh ramp = make_ramp_mesh();
    PhysicsWorld world;
    dynamics::BodyId ramp_id = add_static_mesh(world, &ramp);
    const float tan30 = std::tan(glm::radians(30.0f));
    const glm::vec3 n = glm::normalize(glm::vec3(-tan30, 0.0f, 1.0f));
    const glm::vec3 down(0.0f, 0.0f, -1.0f);

    query::RaycastHit hit;
    ASSERT_TRUE(world.sphere_cast(glm::vec3(2.0f, 0.0f, 5.0f), 0.5f, down, 10.0f, hit));
    ASSERT_NEAR(hit.distance, 5.0f - (2.0f * tan30 + 0.5f / n.z), 1e-3f);
    ASSERT_VEC_NEAR(hit.normal, n, 1e-3f);
    ASSERT_VEC_NEAR(hit.point, glm::vec3(2.0f, 0.0f, 5.0f - hit.distance) - n * 0.5f, 1e-3f);
    ASSERT_TRUE(hit.body == ramp_id);

    query::RaycastHit box_hit;
    ASSERT_TRUE(world.box_cast(glm::vec3(2.0f, 0.0f, 5.0f), glm::vec3(0.25f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), down,
                               10.0f, box_hit));
    ASSERT_NEAR(box_hit.distance, 5.0f - (0.25f + 2.25f * tan30), 1e-3f);
    ASSERT_VEC_NEAR(box_hit.normal, n, 1e-3f);
    ASSERT_VEC_NEAR(box_hit.point, glm::vec3(2.25f, 0.0f, 2.25f * tan30), 1e-2f);

    // A vertical capsule (r 0.3, half-height 0.6) lands on its bottom hemisphere: same rule as
    // the sphere, with the sphere centre 0.6 above the bottom core point.
    query::RaycastHit cap_hit;
    ASSERT_TRUE(world.capsule_cast(glm::vec3(2.0f, 0.0f, 5.0f), 0.3f, 0.6f, 2, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), down,
                                   10.0f, cap_hit));
    ASSERT_NEAR(cap_hit.distance, 5.0f - (2.0f * tan30 + 0.3f / n.z + 0.6f), 1e-3f);
    ASSERT_VEC_NEAR(cap_hit.normal, n, 1e-3f);

    // Sweeping uphill parallel to the slope, clear of it, hits nothing; aimed into it, hits.
    const glm::vec3 uphill = glm::normalize(glm::vec3(1.0f, 0.0f, tan30));
    const glm::vec3 above = glm::vec3(0.5f, 0.0f, 0.5f * tan30) + n * 0.6f;
    ASSERT_TRUE(!world.sphere_cast(above, 0.5f, uphill, 3.0f, hit));
    ASSERT_TRUE(world.sphere_cast(above, 0.5f, glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f)), 3.0f, hit));
    ASSERT_VEC_NEAR(hit.normal, n, 1e-3f);
}

/**
 * Initial-overlap rules: a cast starting inside a target reports it at distance 0 with
 * `started_inside` only when moving deeper; moving out ignores it. A shape merely resting on a
 * surface (touching, no depth) can sweep along it freely, reports distance 0 without
 * `started_inside` when pushed into it -- and on a tessellated mesh floor the shared internal
 * edge doesn't produce a ghost hit for a slightly sunk sphere sliding across it.
 */
COOPA_TEST(sweep_reports_started_inside_and_ignores_resting_contact) {
    PhysicsWorld world;
    dynamics::BodyId box_id = add_static_box(world, glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f));

    query::RaycastHit hit;
    // Sphere centre 0.3 inside the -X face: deeper (+X) is a started-inside hit; out (-X) is not.
    ASSERT_TRUE(world.sphere_cast(glm::vec3(4.8f, 0.0f, 0.0f), 0.25f, glm::vec3(1.0f, 0.0f, 0.0f), 2.0f, hit));
    ASSERT_TRUE(hit.started_inside);
    ASSERT_NEAR(hit.distance, 0.0f, 1e-6f);
    ASSERT_VEC_NEAR(hit.normal, glm::vec3(-1.0f, 0.0f, 0.0f), 1e-3f);
    ASSERT_TRUE(hit.body == box_id);
    ASSERT_TRUE(!world.sphere_cast(glm::vec3(4.8f, 0.0f, 0.0f), 0.25f, glm::vec3(-1.0f, 0.0f, 0.0f), 2.0f, hit));
    // Same for a box cast overlapping by 0.1 (SAT minimum-overlap axis).
    ASSERT_TRUE(world.box_cast(glm::vec3(4.1f, 0.0f, 0.0f), glm::vec3(0.5f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                               glm::vec3(1.0f, 0.0f, 0.0f), 2.0f, hit));
    ASSERT_TRUE(hit.started_inside);
    ASSERT_VEC_NEAR(hit.normal, glm::vec3(-1.0f, 0.0f, 0.0f), 1e-3f);
    ASSERT_TRUE(!world.box_cast(glm::vec3(4.1f, 0.0f, 0.0f), glm::vec3(0.5f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                glm::vec3(-1.0f, 0.0f, 0.0f), 2.0f, hit));

    // Resting exactly on a floor box: sliding along it is free, pressing into it is a
    // distance-0 contact that is NOT started_inside.
    PhysicsWorld floor_world;
    add_static_box(floor_world, glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(10.0f, 10.0f, 0.5f));
    ASSERT_TRUE(!floor_world.sphere_cast(glm::vec3(0.0f, 0.0f, 0.5f), 0.5f, glm::vec3(1.0f, 0.0f, 0.0f), 3.0f, hit));
    ASSERT_TRUE(!floor_world.capsule_cast(glm::vec3(0.0f, 0.0f, 0.9f), 0.4f, 0.5f, 2, glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                          glm::vec3(0.0f, 1.0f, 0.0f), 3.0f, hit));
    ASSERT_TRUE(!floor_world.box_cast(glm::vec3(0.0f, 0.0f, 0.25f), glm::vec3(0.25f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                      glm::vec3(1.0f, 0.0f, 0.0f), 3.0f, hit));
    ASSERT_TRUE(floor_world.sphere_cast(glm::vec3(0.0f, 0.0f, 0.5f), 0.5f, glm::vec3(0.0f, 0.0f, -1.0f), 3.0f, hit));
    ASSERT_NEAR(hit.distance, 0.0f, 1e-3f);
    ASSERT_TRUE(!hit.started_inside);

    // Two-triangle mesh floor, diagonal internal edge x == y: a sphere sunk 0.01 near the edge
    // slides across it without catching.
    geometry::TriangleMesh floor_mesh = make_two_triangle_floor();
    PhysicsWorld mesh_world;
    add_static_mesh(mesh_world, &floor_mesh);
    const glm::vec3 across = glm::normalize(glm::vec3(-1.0f, 1.0f, 0.0f));
    ASSERT_TRUE(!mesh_world.sphere_cast(glm::vec3(0.05f, 0.0f, 0.49f), 0.5f, across, 1.0f, hit));
    // ...but pressing it down is a started-inside hit with the face normal.
    ASSERT_TRUE(mesh_world.sphere_cast(glm::vec3(0.05f, 0.0f, 0.49f), 0.5f, glm::vec3(0.0f, 0.0f, -1.0f), 1.0f, hit));
    ASSERT_TRUE(hit.started_inside);
    ASSERT_VEC_NEAR(hit.normal, glm::vec3(0.0f, 0.0f, 1.0f), 1e-3f);
}

/** QueryFilter: `ignore` skips one body for every query kind; `predicate` can skip any; the
 *  (layer_mask, include_triggers) overloads still behave as before, include_triggers=false
 *  hiding trigger colliders. */
COOPA_TEST(query_filter_skips_ignored_vetoed_masked_and_trigger_bodies) {
    PhysicsWorld world;
    dynamics::BodyId near_id = add_static_box(world, glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(0.5f));
    dynamics::BodyId far_id = add_static_box(world, glm::vec3(6.0f, 0.0f, 0.0f), glm::vec3(0.5f));

    geometry::Ray ray;
    ray.origin = glm::vec3(0.0f);
    ray.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    ray.max_distance = 100.0f;

    query::QueryFilter ignore_near;
    ignore_near.ignore = near_id;

    query::RaycastHit hit;
    ASSERT_TRUE(world.raycast(ray, hit));
    ASSERT_TRUE(hit.body == near_id);
    ASSERT_TRUE(world.raycast(ray, hit, ignore_near));
    ASSERT_TRUE(hit.body == far_id);
    ASSERT_NEAR(hit.distance, 5.5f, 1e-4f);

    ASSERT_TRUE(world.raycast_all(ray, ignore_near).size() == 1);
    ASSERT_TRUE(world.sphere_cast(glm::vec3(0.0f), 0.25f, ray.direction, 100.0f, hit, ignore_near));
    ASSERT_TRUE(hit.body == far_id);
    ASSERT_TRUE(world.capsule_cast(glm::vec3(0.0f), 0.25f, 0.5f, 2, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), ray.direction,
                                   100.0f, hit, ignore_near));
    ASSERT_TRUE(hit.body == far_id);
    ASSERT_TRUE(world.box_cast(glm::vec3(0.0f), glm::vec3(0.25f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), ray.direction,
                               100.0f, hit, ignore_near));
    ASSERT_TRUE(hit.body == far_id);
    ASSERT_NEAR(hit.distance, 5.25f, 1e-3f);

    ASSERT_TRUE(world.overlap_sphere(glm::vec3(3.0f, 0.0f, 0.0f), 0.5f).size() == 1);
    ASSERT_TRUE(world.overlap_sphere(glm::vec3(3.0f, 0.0f, 0.0f), 0.5f, ignore_near).empty());
    ASSERT_TRUE(world.compute_penetration(collision::Shape::make_sphere(0.5f), glm::vec3(3.0f, 0.0f, 0.9f),
                                          glm::quat(1.0f, 0.0f, 0.0f, 0.0f), ignore_near).empty());

    // The predicate sees every candidate body and can veto any of them.
    query::QueryFilter none;
    none.predicate = [](dynamics::BodyId) { return false; };
    ASSERT_TRUE(!world.raycast(ray, hit, none));
    ASSERT_TRUE(!world.raycast_any(ray, none));
    query::QueryFilter only_far;
    only_far.predicate = [&](dynamics::BodyId id) { return id == far_id; };
    ASSERT_TRUE(world.sphere_cast(glm::vec3(0.0f), 0.25f, ray.direction, 100.0f, hit, only_far));
    ASSERT_TRUE(hit.body == far_id);

    // Layer mask through the filter matches the shorthand overload.
    world.get_shape(near_id)->layer = 3;
    query::QueryFilter skip_layer3;
    skip_layer3.layer_mask = ~(1u << 3);
    ASSERT_TRUE(world.raycast(ray, hit, skip_layer3) && hit.body == far_id);
    ASSERT_TRUE(world.raycast(ray, hit, ~(1u << 3)) && hit.body == far_id);

    // `include_triggers` (default true: every enabled shape is visible to queries regardless of
    // is_trigger) lets a caller exclude trigger colliders.
    world.get_shape(far_id)->is_trigger = true;
    EXPECT_EQ(world.overlap_sphere(glm::vec3(6.0f, 0.0f, 0.0f), 1.0f, ~0u, true).size(), 1u);
    EXPECT_TRUE(world.overlap_sphere(glm::vec3(6.0f, 0.0f, 0.0f), 1.0f, ~0u, false).empty());
}

/** compute_penetration(): exact MTVs against boxes (face contact, a capsule whose core is
 *  inside the box), and per-direction merged entries against meshes. */
COOPA_TEST(compute_penetration_is_exact_against_box_and_mesh) {
    PhysicsWorld world;
    dynamics::BodyId floor_id = add_static_box(world, glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(2.0f, 2.0f, 0.5f));
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);

    auto pens = world.compute_penetration(collision::Shape::make_sphere(0.5f), glm::vec3(0.0f, 0.0f, 0.4f), identity);
    ASSERT_TRUE(pens.size() == 1);
    ASSERT_NEAR(pens[0].depth, 0.1f, 1e-4f);
    ASSERT_VEC_NEAR(pens[0].normal, glm::vec3(0.0f, 0.0f, 1.0f), 1e-4f);
    ASSERT_TRUE(pens[0].body == floor_id);

    // Box sunk 0.15 into the floor's top face.
    pens = world.compute_penetration(collision::Shape::make_box(glm::vec3(0.25f)), glm::vec3(0.5f, 0.0f, 0.1f), identity);
    ASSERT_TRUE(pens.size() == 1);
    ASSERT_NEAR(pens[0].depth, 0.15f, 1e-4f);
    ASSERT_VEC_NEAR(pens[0].normal, glm::vec3(0.0f, 0.0f, 1.0f), 1e-4f);

    // Lying capsule (X axis, r 0.2) whose core is 0.3 BELOW the floor's top: push-out is up
    // through the top face, 0.3 + 0.2 deep.
    pens = world.compute_penetration(collision::Shape::make_capsule(0.2f, 0.5f, 0), glm::vec3(0.0f, 0.0f, -0.3f), identity);
    ASSERT_TRUE(pens.size() == 1);
    ASSERT_NEAR(pens[0].depth, 0.5f, 1e-4f);
    ASSERT_VEC_NEAR(pens[0].normal, glm::vec3(0.0f, 0.0f, 1.0f), 1e-4f);

    // Not touching: nothing.
    ASSERT_TRUE(world.compute_penetration(collision::Shape::make_sphere(0.5f), glm::vec3(0.0f, 0.0f, 0.6f), identity).empty());

    // Mesh corner: a floor (z=0) and a wall (x=0, facing +X), two triangles each. A sphere
    // pressed into the corner gets one entry per surface, not per triangle.
    geometry::TriangleMesh corner = make_mesh_with_adjacency(
        {glm::vec3(0, -2, 0), glm::vec3(4, -2, 0), glm::vec3(4, 2, 0), glm::vec3(0, 2, 0), glm::vec3(0, -2, 4), glm::vec3(0, 2, 4)},
        {0, 1, 2, 0, 2, 3, 0, 3, 5, 0, 5, 4});
    PhysicsWorld mesh_world;
    dynamics::BodyId mesh_id = add_static_mesh(mesh_world, &corner);
    pens = mesh_world.compute_penetration(collision::Shape::make_sphere(0.5f), glm::vec3(0.4f, 0.0f, 0.45f), identity);
    ASSERT_TRUE(pens.size() == 2);
    std::sort(pens.begin(), pens.end(), [](const query::Penetration& a, const query::Penetration& b) { return a.depth < b.depth; });
    ASSERT_VEC_NEAR(pens[0].normal, glm::vec3(0.0f, 0.0f, 1.0f), 1e-4f);
    ASSERT_NEAR(pens[0].depth, 0.05f, 1e-4f);
    ASSERT_VEC_NEAR(pens[1].normal, glm::vec3(1.0f, 0.0f, 0.0f), 1e-4f);
    ASSERT_NEAR(pens[1].depth, 0.1f, 1e-4f);
    ASSERT_TRUE(pens[0].body == mesh_id && pens[1].body == mesh_id);

    // A sphere straddling the floor's internal diagonal edge: one merged upward entry.
    pens = mesh_world.compute_penetration(collision::Shape::make_sphere(0.5f), glm::vec3(2.0f, 0.0f, 0.45f), identity);
    ASSERT_TRUE(pens.size() == 1);
    ASSERT_VEC_NEAR(pens[0].normal, glm::vec3(0.0f, 0.0f, 1.0f), 1e-4f);
    ASSERT_NEAR(pens[0].depth, 0.05f, 1e-4f);
}

/**
 * Shape::local_rotation -- a single (non-compound) collider's shape posed at an angle relative
 * to its own body. A capsule authored along local Z, rotated 90 degrees about X, actually points along
 * world -Y once instanced -- confirmed three independent ways (raycast, overlap, debug draw) so
 * a mistake in any one of the world_*()/raycast_shape() call sites that read it is caught.
 */
COOPA_TEST(shape_local_rotation_is_honoured_by_every_query_path) {
    PhysicsWorld world;
    dynamics::Body body;
    body.type = dynamics::BodyType::Static;
    body.position = glm::vec3(0.0f);
    collision::Shape shape = collision::Shape::make_capsule(0.3f, 1.0f); // default axis Z, local_center 0
    shape.local_rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    dynamics::BodyId id = world.add_body(body, shape);

    // Rotating local Z (0,0,1) by +90 degrees about X maps it to world (0,-1,0) -- so the
    // capsule's actual endpoints are (0,1,0) and (0,-1,0), radius 0.3, entirely off the Z axis
    // it would occupy without local_rotation.
    const glm::vec3 expected_a(0.0f, 1.0f, 0.0f);
    const glm::vec3 expected_b(0.0f, -1.0f, 0.0f);

    // 1. Raycast: a ray straight down world -Y should hit the capsule's near end cap.
    geometry::Ray ray;
    ray.origin = glm::vec3(0.0f, 3.0f, 0.0f);
    ray.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    ray.max_distance = 100.0f;
    query::RaycastHit hit;
    ASSERT_TRUE(world.raycast(ray, hit));
    ASSERT_NEAR(hit.distance, 3.0f - 1.3f, 1e-3f); // end cap surface at y = 1.0 + radius 0.3

    // A ray held at z=1.3 throughout (sweeping along Y, well clear of the origin) must miss --
    // the ACTUAL segment lies entirely in the z=0 plane (radius 0.3 << 1.3), so this only hits
    // if the shape were still sitting at its un-rotated Z-axis position (whose end cap sits
    // right around z=1.3). Deliberately not a ray straight down world -Z through the origin:
    // the rotated segment ALSO passes through the origin (it's centered there), so that ray
    // would hit either orientation and wouldn't distinguish them.
    geometry::Ray miss_ray;
    miss_ray.origin = glm::vec3(0.0f, 3.0f, 1.3f);
    miss_ray.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    miss_ray.max_distance = 100.0f;
    query::RaycastHit miss_hit;
    ASSERT_TRUE(!world.raycast(miss_ray, miss_hit));

    // 2. Overlap: a small sphere at the actual (rotated) capsule center overlaps; the same
    // sphere at the UN-rotated capsule's would-be center (0,0,0.9) does not.
    ASSERT_TRUE(!world.overlap_sphere(glm::vec3(0.0f), 0.1f).empty());
    ASSERT_TRUE(world.overlap_sphere(glm::vec3(0.0f, 0.0f, 0.9f), 0.1f).empty());

    // 3. Debug draw: every emitted line endpoint for this capsule must lie within `radius` (plus
    // a small tolerance for the ring/cap approximation) of the ACTUAL rotated segment, not the
    // un-rotated one.
    debug::DebugDraw draw;
    world.debug_draw(draw, debug::DebugDrawFlags::Colliders);
    ASSERT_TRUE(!draw.lines.empty());
    for (const auto& line : draw.lines) {
        float dist_a = glm::length(line.a - geometry::closest_point_on_segment(line.a, expected_a, expected_b));
        float dist_b = glm::length(line.b - geometry::closest_point_on_segment(line.b, expected_a, expected_b));
        ASSERT_TRUE(dist_a < 0.35f);
        ASSERT_TRUE(dist_b < 0.35f);
    }
    (void)id;
}

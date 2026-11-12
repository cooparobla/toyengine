/**
 * @file narrowphase_test.cpp
 * @brief Contact generation: box-box SAT (face and edge-edge features, exact depths), hull SAT
 *        (hull-sphere, hull-box and hull-hull reproducing the box-box results exactly, feature-id
 *        stability across repeated calls, edge-edge resolution), BVH-narrowed mesh contacts
 *        against a brute-force oracle, internal-edge correction on a tiled mesh floor, and
 *        box-on-mesh support: no phantom contacts from a wall below a ledge, triangle vertices
 *        holding a box up on a triangle smaller than its face, and a box settling flat on a finely
 *        tessellated floor.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/world.h>
#include <physxcoopa/collision/sat.h>
#include <physxcoopa/collision/shape.h>
#include <physxcoopa/geometry/convex_hull.h>
#include <physxcoopa/collision/sat_hull.h>
#include <random>
#include "support/physics_fixtures.h"

COOPA_TEST_SUITE("narrowphase");

using namespace coopa::physx;
using namespace physxtest;

COOPA_TEST(box_box_face_contact_has_four_points_at_known_depth) {
    geometry::OBB a;
    a.center = glm::vec3(0.0f);
    a.half_extents = glm::vec3(0.5f);
    geometry::OBB b;
    b.center = glm::vec3(0.8f, 0.0f, 0.0f);
    b.half_extents = glm::vec3(0.5f);

    collision::ContactManifold m;
    ASSERT_TRUE(collision::generate_box_box_contacts(a, b, m));
    ASSERT_NEAR(m.normal.x, 1.0f, 1e-3f);
    ASSERT_NEAR(std::abs(m.normal.y), 0.0f, 1e-3f);
    ASSERT_NEAR(std::abs(m.normal.z), 0.0f, 1e-3f);
    ASSERT_TRUE(m.count == 4); // full face-face overlap -> 4 clipped points
    for (uint8_t i = 0; i < m.count; ++i) {
        ASSERT_NEAR(m.points[i].penetration, 0.2f, 1e-3f);
    }
}

/**
 * Regression for a sign bug found in sat_hull.h's hull_vs_sphere() inside-case: a sphere fully
 * enclosed in a hull must resolve OUTWARD through the nearest face (sphere_vs_box()'s inside
 * branch does the same for a box), not back toward where the center already was -- which,
 * before the fix, pushed the sphere further through the OPPOSITE side instead.
 */
COOPA_TEST(sphere_fully_embedded_in_hull_resolves_outward_through_nearest_face) {
    glm::vec3 pts[8];
    int k = 0;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2) pts[k++] = glm::vec3(float(sx), float(sy), float(sz)) * 0.5f;
    geometry::ConvexHull hull;
    ASSERT_TRUE(geometry::build_convex_hull(pts, 8, hull));

    collision::Shape hull_shape = collision::Shape::make_hull(&hull);
    // Off-center toward +X: the nearest face is unambiguously +X, well clear of every other face.
    const glm::vec3 sphere_pos(0.3f, 0.0f, 0.0f);
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);

    collision::PosedHull posed = collision::world_posed_hull(hull_shape, glm::vec3(0.0f), identity);
    glm::vec3 n, p;
    float pen = 0.0f;
    ASSERT_TRUE(collision::hull_vs_sphere(posed, geometry::Sphere{sphere_pos, 0.1f}, n, pen, p));
    ASSERT_NEAR(n.x, 1.0f, 1e-3f);
    ASSERT_NEAR(n.y, 0.0f, 1e-3f);
    ASSERT_NEAR(n.z, 0.0f, 1e-3f);

    // The correction must land the sphere genuinely OUTSIDE the hull, not merely "separated by
    // the reported depth" in a direction that still leaves it enclosed.
    const glm::vec3 pushed = sphere_pos + n * pen;
    ASSERT_TRUE(pushed.x > 0.5f - 1e-3f);
}

COOPA_TEST(box_box_edge_contact_is_a_single_edge_feature) {
    geometry::OBB a;
    a.center = glm::vec3(0.0f);
    a.half_extents = glm::vec3(0.5f);

    // An arbitrary tilt + diagonal offset, found by search, that lands the minimum-penetration
    // SAT axis on an edge-cross axis rather than either box's face normal -- unlike a
    // deliberately "corner-on" or single-axis-rotated setup, which (perhaps counter-
    // intuitively) still tends to resolve to a face axis for two equal-sized cubes.
    geometry::OBB b;
    b.orientation = glm::quat(-0.996603f, -0.0278176f, -0.0606705f, -0.0482471f);
    b.center = glm::vec3(0.864856f, 0.686217f, 0.79309f);
    b.half_extents = glm::vec3(0.5f);

    collision::ContactManifold m;
    ASSERT_TRUE(collision::generate_box_box_contacts(a, b, m));
    ASSERT_TRUE(m.count == 1);
    ASSERT_TRUE(m.points[0].feature_id >= 0x1000u); // edge-edge feature-id range
}

namespace {
/** @brief An axis-aligned cube hull of the given half-extent, centred on the origin in its own
 *         local space -- the hull analog of an OBB with that half_extents, used to pose against
 *         sat_hull.h's hull-box/hull-hull routines and check they reproduce sat.h's box-box
 *         result exactly. */
geometry::ConvexHull make_cube_hull_(float half_extent) {
    glm::vec3 pts[8];
    int k = 0;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2) pts[k++] = glm::vec3(float(sx), float(sy), float(sz)) * half_extent;
    geometry::ConvexHull hull;
    bool ok = geometry::build_convex_hull(pts, 8, hull);
    (void)ok; // callers assert their own expectations on the hull/result, not the build itself
    return hull;
}
} // namespace

/**
 * The cheapest check that generalizing SAT from boxes to hulls did not invert a normal or scale
 * a depth wrong: a cube hull against a world-posed box reproduces box_box_face_contact_has_
 * four_points_at_known_depth's exact normal and depth.
 */
COOPA_TEST(hull_vs_box_reproduces_box_vs_box_face_contact) {
    geometry::ConvexHull hull_a = make_cube_hull_(0.5f);
    collision::PosedHull posed_a{&hull_a, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f};

    geometry::OBB b;
    b.center = glm::vec3(0.8f, 0.0f, 0.0f);
    b.half_extents = glm::vec3(0.5f);

    collision::ContactManifold m;
    ASSERT_TRUE(collision::generate_hull_box_contacts(posed_a, b, m));
    ASSERT_NEAR(m.normal.x, 1.0f, 1e-3f);
    ASSERT_NEAR(std::abs(m.normal.y), 0.0f, 1e-3f);
    ASSERT_NEAR(std::abs(m.normal.z), 0.0f, 1e-3f);
    ASSERT_TRUE(m.count == 4);
    for (uint8_t i = 0; i < m.count; ++i) ASSERT_NEAR(m.points[i].penetration, 0.2f, 1e-3f);
}

/** @brief Same reproduction, hull-vs-hull this time: both boxes of box_box_face_contact_has_
 *         four_points_at_known_depth built as cube hulls instead. */
COOPA_TEST(hull_vs_hull_reproduces_box_vs_box_face_contact) {
    geometry::ConvexHull hull_a = make_cube_hull_(0.5f);
    geometry::ConvexHull hull_b = make_cube_hull_(0.5f);
    collision::PosedHull posed_a{&hull_a, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f};
    collision::PosedHull posed_b{&hull_b, glm::vec3(0.8f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f};

    collision::ContactManifold m;
    ASSERT_TRUE(collision::generate_hull_hull_contacts(posed_a, posed_b, m));
    ASSERT_NEAR(m.normal.x, 1.0f, 1e-3f);
    ASSERT_NEAR(std::abs(m.normal.y), 0.0f, 1e-3f);
    ASSERT_NEAR(std::abs(m.normal.z), 0.0f, 1e-3f);
    ASSERT_TRUE(m.count == 4);
    for (uint8_t i = 0; i < m.count; ++i) ASSERT_NEAR(m.points[i].penetration, 0.2f, 1e-3f);
}

/**
 * The warm-start guarantee SAT exists to provide (see sat_hull.h's file doc): two successive
 * calls on the SAME resting pair -- unmoved -- must return the SAME feature ids, point for
 * point, so the solver's accumulated impulses stay attached to the same contact next step. This
 * is exactly the property EPA could not give (v1 plan's objection); it is the one assertion that
 * justifies choosing SAT over it.
 */
COOPA_TEST(hull_hull_feature_ids_are_stable_across_repeated_calls) {
    geometry::ConvexHull hull_a = make_cube_hull_(0.5f);
    geometry::ConvexHull hull_b = make_cube_hull_(0.5f);
    collision::PosedHull posed_a{&hull_a, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f};
    collision::PosedHull posed_b{&hull_b, glm::vec3(0.8f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f};

    collision::ContactManifold m1, m2;
    ASSERT_TRUE(collision::generate_hull_hull_contacts(posed_a, posed_b, m1));
    ASSERT_TRUE(collision::generate_hull_hull_contacts(posed_a, posed_b, m2));
    ASSERT_TRUE(m1.count == m2.count);
    for (uint8_t i = 0; i < m1.count; ++i) ASSERT_TRUE(m1.points[i].feature_id == m2.points[i].feature_id);
}

/**
 * The hull-hull generalization of box_box_edge_contact_is_a_single_edge_feature: the same tilt
 * and offset that lands sat.h's box-box routine on an edge-cross axis must do the same for two
 * cube hulls -- a single point tagged with sat_hull.h's edge-edge bit (0x50000000), not a face.
 */
COOPA_TEST(hull_hull_edge_configuration_resolves_to_the_edge_axis) {
    geometry::ConvexHull hull_a = make_cube_hull_(0.5f);
    geometry::ConvexHull hull_b = make_cube_hull_(0.5f);
    collision::PosedHull posed_a{&hull_a, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f};
    // Same tilt + diagonal offset as box_box_edge_contact_is_a_single_edge_feature.
    collision::PosedHull posed_b{&hull_b, glm::vec3(0.864856f, 0.686217f, 0.79309f),
                                 glm::quat(-0.996603f, -0.0278176f, -0.0606705f, -0.0482471f), 1.0f};

    collision::ContactManifold m;
    ASSERT_TRUE(collision::generate_hull_hull_contacts(posed_a, posed_b, m));
    ASSERT_TRUE(m.count == 1);
    ASSERT_TRUE((m.points[0].feature_id & 0x50000000u) == 0x50000000u);
}

/**
 * BVH-narrowed mesh contact generation matches the brute-force every-triangle scan exactly --
 * same points, depths, feature ids and normal -- for spheres, capsules and boxes at random
 * poses around a bumpy 16x16-cell heightfield.
 */
COOPA_TEST(mesh_contacts_via_bvh_match_brute_force) {
    const int cells = 16;
    const float cell = 0.5f;
    auto height = [](float x, float y) { return 0.4f * std::sin(x * 1.3f) * std::cos(y * 0.9f); };
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
    for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i) {
            float x = i * cell, y = j * cell;
            vertices.push_back(glm::vec3(x, y, height(x, y)));
        }
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
            uint32_t a = j * (cells + 1) + i, b = a + 1, c = a + cells + 1, d = c + 1;
            indices.insert(indices.end(), {a, b, d, a, d, c});
        }
    geometry::TriangleMesh terrain = make_mesh_with_adjacency(std::move(vertices), std::move(indices));
    collision::Shape mesh_shape = collision::Shape::make_mesh(&terrain);
    const glm::vec3 mesh_pos(-1.0f, 0.5f, 0.2f);
    const glm::quat mesh_rot = glm::angleAxis(0.3f, glm::normalize(glm::vec3(0.2f, 0.1f, 1.0f)));

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    int compared = 0, touching = 0;
    for (int trial = 0; trial < 600; ++trial) {
        collision::Shape probe;
        switch (trial % 3) {
            case 0: probe = collision::Shape::make_sphere(0.2f + 0.4f * unit(rng)); break;
            case 1: probe = collision::Shape::make_capsule(0.15f + 0.2f * unit(rng), 0.2f + 0.4f * unit(rng)); break;
            default: probe = collision::Shape::make_box(glm::vec3(0.1f + 0.4f * unit(rng), 0.1f + 0.4f * unit(rng), 0.1f + 0.3f * unit(rng))); break;
        }
        float lx = 0.5f + 7.0f * unit(rng), ly = 0.5f + 7.0f * unit(rng);
        glm::vec3 local(lx, ly, height(lx, ly) + 0.1f + 0.4f * (unit(rng) - 0.5f));
        glm::vec3 pos = mesh_pos + mesh_rot * local;
        glm::quat rot = glm::angleAxis(6.28f * unit(rng), glm::normalize(glm::vec3(unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) + 0.1f)));

        collision::ContactManifold fast, brute;
        bool hit_fast = collision::generate_mesh_contacts(probe, pos, rot, mesh_shape, mesh_pos, mesh_rot, fast, true);
        bool hit_brute = collision::generate_mesh_contacts(probe, pos, rot, mesh_shape, mesh_pos, mesh_rot, brute, false);
        ASSERT_TRUE(hit_fast == hit_brute);
        ++compared;
        if (!hit_fast) continue;
        ++touching;
        ASSERT_TRUE(fast.count == brute.count);
        ASSERT_VEC_NEAR(fast.normal, brute.normal, 1e-6f);
        for (uint8_t k = 0; k < fast.count; ++k) {
            ASSERT_VEC_NEAR(fast.points[k].position, brute.points[k].position, 1e-6f);
            ASSERT_NEAR(fast.points[k].penetration, brute.points[k].penetration, 1e-6f);
            ASSERT_TRUE(fast.points[k].feature_id == brute.points[k].feature_id);
        }
    }
    ASSERT_TRUE(compared == 600);
    ASSERT_TRUE(touching > 150); // the pose range really exercises contacts, not just misses
}

COOPA_TEST(box_slides_across_internal_mesh_edge_without_a_bump) {
    geometry::TriangleMesh floor_mesh = make_two_triangle_floor();

    dynamics::PhysicsMaterial low_friction;
    low_friction.dynamic_friction = 0.05f;
    low_friction.static_friction = 0.05f;

    PhysicsWorld world;
    dynamics::Body floor_body;
    floor_body.type = dynamics::BodyType::Static;
    collision::Shape floor_shape = collision::Shape::make_mesh(&floor_mesh);
    floor_shape.material = &low_friction;
    world.add_body(floor_body, floor_shape);

    const float half_extent = 0.25f;
    dynamics::Body box;
    box.position = glm::vec3(-1.5f, 0.0f, half_extent + 0.02f);
    box.mass = 1.0f;
    box.inv_mass = 1.0f;
    box.inv_inertia_local = dynamics::box_inverse_inertia(glm::vec3(half_extent), box.mass);
    collision::Shape box_shape = collision::Shape::make_box(glm::vec3(half_extent));
    box_shape.material = &low_friction;
    dynamics::BodyId id = world.add_body(box, box_shape);

    // Settle onto the floor first, well clear of the shared diagonal edge (crossed at x == y, so
    // starting/staying at y=0 crosses it once, at x=0).
    for (int i = 0; i < 60; ++i) world.step_fixed(util::k_default_fixed_dt);

    dynamics::Body* b = world.get_body(id);
    b->wake();
    b->linear_velocity = glm::vec3(3.0f, 0.0f, 0.0f);

    // 40 steps at 3 m/s covers 2 units (x: -1.5 -> 0.5), crossing the shared seam at x=0 while
    // staying well inside the mesh's [-2,2] bounds on both sides.
    float max_vz_jump = 0.0f;
    float prev_vz = b->linear_velocity.z;
    for (int i = 0; i < 40; ++i) {
        world.step_fixed(util::k_default_fixed_dt);
        const dynamics::Body* cur = world.get_body(id);
        max_vz_jump = std::max(max_vz_jump, std::abs(cur->linear_velocity.z - prev_vz));
        prev_vz = cur->linear_velocity.z;
    }

    const dynamics::Body* final_b = world.get_body(id);
    ASSERT_TRUE(final_b->position.x > 0.0f); // slid across the shared edge, past x=0
    ASSERT_NEAR(final_b->position.z, half_extent, 0.05f); // stayed flush on the floor throughout
    ASSERT_TRUE(max_vz_jump < 1.0f); // no discontinuity crossing the internal edge
}

namespace {

/** @brief A ledge: a flat top (x in [-2,0], z = 0, facing +z) and the wall under its edge (x = 0, facing +x). */
geometry::TriangleMesh make_ledge() {
    return make_mesh_with_adjacency({glm::vec3(-2.0f, -2.0f, 0.0f), glm::vec3(0.0f, -2.0f, 0.0f), glm::vec3(0.0f, 2.0f, 0.0f),
                                     glm::vec3(-2.0f, 2.0f, 0.0f), glm::vec3(0.0f, -2.0f, -2.0f), glm::vec3(0.0f, 2.0f, -2.0f)},
                                    {1, 4, 5, 1, 5, 2, 0, 1, 2, 0, 2, 3});   // wall first: it is met first
}

/** @brief A floor of `n` x `n` quads over [-2,2]^2 at z = 0 -- many triangles under one box. */
geometry::TriangleMesh make_tiled_floor(int n) {
    std::vector<glm::vec3> v;
    std::vector<uint32_t> idx;
    const float step = 4.0f / static_cast<float>(n);
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) v.push_back(glm::vec3(-2.0f + i * step, -2.0f + j * step, 0.0f));
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const uint32_t a = j * (n + 1) + i, b = a + 1, c = a + n + 2, d = a + n + 1;
            idx.insert(idx.end(), {a, b, c, a, c, d});
        }
    }
    return make_mesh_with_adjacency(std::move(v), std::move(idx));
}

} // namespace

/**
 * A box resting on a ledge, overhanging its edge: its corners are behind the plane of the wall
 * below, within the old corner footprint, by most of the box's width. Only the top supports it --
 * the wall (whose plane the box's centre is behind) must not push it sideways off the ledge.
 */
COOPA_TEST(box_on_a_ledge_gets_no_push_from_the_wall_below) {
    geometry::TriangleMesh ledge = make_ledge();
    const collision::Shape mesh_shape = collision::Shape::make_mesh(&ledge);
    const collision::Shape box = collision::Shape::make_box(glm::vec3(0.5f));
    collision::ContactManifold m;
    ASSERT_TRUE(collision::generate_mesh_contacts(box, glm::vec3(-0.4f, 0.0f, 0.49f), glm::quat(1, 0, 0, 0), mesh_shape,
                                                  glm::vec3(0.0f), glm::quat(1, 0, 0, 0), m));
    ASSERT_NEAR(m.normal.z, 1.0f, 1e-3f);
    for (uint8_t i = 0; i < m.count; ++i) ASSERT_NEAR(m.points[i].penetration, 0.01f, 1e-3f);
}

/** A box sunk 1 cm onto a triangle much smaller than its face: no corner is over it, its vertices hold the box up. */
COOPA_TEST(box_on_a_small_triangle_is_held_by_its_vertices) {
    geometry::TriangleMesh tri = make_mesh_with_adjacency(
        {glm::vec3(-0.1f, -0.1f, 0.0f), glm::vec3(0.1f, -0.1f, 0.0f), glm::vec3(0.0f, 0.1f, 0.0f)}, {0, 1, 2});
    const collision::Shape mesh_shape = collision::Shape::make_mesh(&tri);
    const collision::Shape box = collision::Shape::make_box(glm::vec3(0.5f));
    collision::ContactManifold m;
    ASSERT_TRUE(collision::generate_mesh_contacts(box, glm::vec3(0.0f, 0.0f, 0.49f), glm::quat(1, 0, 0, 0), mesh_shape,
                                                  glm::vec3(0.0f), glm::quat(1, 0, 0, 0), m));
    ASSERT_TRUE(m.count == 3);
    ASSERT_NEAR(m.normal.z, 1.0f, 1e-3f);
    for (uint8_t i = 0; i < m.count; ++i) ASSERT_NEAR(m.points[i].penetration, 0.01f, 1e-3f);
}

/**
 * Dropped tilted onto a floor of 0.25 m tiles -- each corner within reach of several triangles --
 * a box comes to rest flat and still, where it landed, instead of balancing on an edge,
 * tumbling or sinking.
 */
COOPA_TEST(box_settles_flat_on_a_finely_tessellated_floor) {
    geometry::TriangleMesh floor_mesh = make_tiled_floor(16);
    PhysicsWorld world;
    dynamics::Body floor_body;
    floor_body.type = dynamics::BodyType::Static;
    world.add_body(floor_body, collision::Shape::make_mesh(&floor_mesh));

    const float half = 0.6f;
    dynamics::Body box;
    box.position = glm::vec3(0.1f, -0.2f, 1.5f);
    box.orientation = glm::normalize(glm::quat(glm::vec3(0.17f, 0.35f, 0.52f)));
    box.mass = 40.0f;
    box.inv_mass = 1.0f / box.mass;
    box.inv_inertia_local = dynamics::box_inverse_inertia(glm::vec3(half), box.mass);
    dynamics::BodyId id = world.add_body(box, collision::Shape::make_box(glm::vec3(half)));

    step_world(world, 300);
    const dynamics::Body* b = world.get_body(id);
    ASSERT_TRUE(best_up_alignment(b->orientation) > 0.999f);
    ASSERT_NEAR(b->position.z, half, 0.02f);
    ASSERT_TRUE(glm::length(glm::vec2(b->position)) < 1.0f);
    ASSERT_TRUE(glm::length(b->linear_velocity) < 0.05f && glm::length(b->angular_velocity) < 0.05f);
}


/**
 * A box whose CENTRE has ended up under a mesh floor is embedded, and is given the one contact
 * that gets it out: along the floor's own normal, by the whole distance it reaches past it.
 * Without that there is nothing to report at all -- every corner test needs the centre in front
 * of the face -- so a body that sank once (a hard contact resolving, a joint dragging a bone
 * through, a spawn inside geometry) simply keeps sinking, which is what a ragdoll falling
 * through the ground actually is.
 *
 * As far as the box can still reach, which is its own bounds: a body already further under than
 * its own size is past recovering here, and is instead kept from ever getting there by the speed
 * limit a limp body is held under (toy::scene::Ragdoll::max_limp_speed).
 */
COOPA_TEST(box_embedded_under_a_mesh_floor_is_pushed_back_out) {
    geometry::TriangleMesh floor_mesh = make_tiled_floor(8);
    const collision::Shape mesh_shape = collision::Shape::make_mesh(&floor_mesh);
    const float half = 0.11f;                                  // a ragdoll bone's size
    const collision::Shape box = collision::Shape::make_box(glm::vec3(half));

    collision::ContactManifold m;
    ASSERT_TRUE(collision::generate_mesh_contacts(box, glm::vec3(0.3f, -0.4f, -0.1f), glm::quat(1, 0, 0, 0),
                                                  mesh_shape, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), m));
    ASSERT_TRUE(m.count == 1);
    ASSERT_NEAR(m.normal.z, 1.0f, 1e-3f);                      // out of the floor, not along it
    ASSERT_NEAR(m.points[0].penetration, 0.1f + half, 1e-3f);  // the whole way out

    // The BVH-narrowed path and the brute-force scan agree, as everywhere else in this file.
    collision::ContactManifold brute;
    ASSERT_TRUE(collision::generate_mesh_contacts(box, glm::vec3(0.3f, -0.4f, -0.1f), glm::quat(1, 0, 0, 0),
                                                  mesh_shape, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), brute, false));
    ASSERT_TRUE(brute.count == m.count);
    ASSERT_NEAR(brute.points[0].penetration, m.points[0].penetration, 1e-5f);

    // And it is recovery only: a box under the floor but not under any triangle of it (out past
    // the mesh's edge, falling past the cliff) is not dragged back in.
    collision::ContactManifold off;
    ASSERT_FALSE(collision::generate_mesh_contacts(box, glm::vec3(3.0f, 0.0f, -0.1f), glm::quat(1, 0, 0, 0),
                                                   mesh_shape, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), off));
}

/**
 * The same embedded box, in a world: it is lifted back onto the floor and settles there, rather
 * than resting inside the ground for ever.
 */
COOPA_TEST(an_embedded_box_climbs_back_onto_the_floor) {
    geometry::TriangleMesh floor_mesh = make_tiled_floor(8);
    PhysicsWorld world;
    dynamics::Body floor_body;
    floor_body.type = dynamics::BodyType::Static;
    world.add_body(floor_body, collision::Shape::make_mesh(&floor_mesh));

    const float half = 0.11f;
    dynamics::Body sunk;
    sunk.position = glm::vec3(0.3f, -0.4f, -0.1f);
    sunk.mass = 1.2f;
    sunk.inv_mass = 1.0f / sunk.mass;
    sunk.inv_inertia_local = dynamics::box_inverse_inertia(glm::vec3(half), sunk.mass);
    dynamics::BodyId id = world.add_body(sunk, collision::Shape::make_box(glm::vec3(half)));

    step_world(world, 240);
    const dynamics::Body* b = world.get_body(id);
    ASSERT_NEAR(b->position.z, half, 0.02f);
    ASSERT_TRUE(glm::length(b->linear_velocity) < 0.05f);
}

/**
 * The same recovery for a capsule, which is what a character stands on a mesh floor with. Its
 * core is the segment between the cap centres, so "through the surface" means the segment itself
 * has crossed: from there the unsigned closest-point test reports a penetration that SHRINKS the
 * further in it goes, under a direction that points back INTO the mesh -- a character pushed
 * down through the ground, and then nothing at all once it is a radius past.
 */
COOPA_TEST(capsule_embedded_under_a_mesh_floor_is_pushed_back_out) {
    geometry::TriangleMesh floor_mesh = make_tiled_floor(8);
    const collision::Shape mesh_shape = collision::Shape::make_mesh(&floor_mesh);
    const collision::Shape capsule = collision::Shape::make_capsule(0.3f, 0.5f);   // radius, half-height

    // Standing on the floor: held up, out of it.
    collision::ContactManifold rest;
    ASSERT_TRUE(collision::generate_mesh_contacts(capsule, glm::vec3(0.0f, 0.0f, 0.78f), glm::quat(1, 0, 0, 0),
                                                  mesh_shape, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), rest));
    ASSERT_NEAR(rest.normal.z, 1.0f, 1e-3f);
    ASSERT_NEAR(rest.points[0].penetration, 0.02f, 1e-3f);

    // Core 0.2 m under it: still out, and by the whole way (radius + how far past).
    collision::ContactManifold sunk;
    ASSERT_TRUE(collision::generate_mesh_contacts(capsule, glm::vec3(0.0f, 0.0f, -0.7f), glm::quat(1, 0, 0, 0),
                                                  mesh_shape, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), sunk));
    ASSERT_NEAR(sunk.normal.z, 1.0f, 1e-3f);
    ASSERT_NEAR(sunk.points[0].penetration, 0.5f, 1e-3f);

    // Past the mesh's edge there is nothing to come back to.
    collision::ContactManifold off;
    ASSERT_FALSE(collision::generate_mesh_contacts(capsule, glm::vec3(3.0f, 0.0f, -0.7f), glm::quat(1, 0, 0, 0),
                                                   mesh_shape, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), off));
}

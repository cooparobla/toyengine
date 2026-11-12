/**
 * @file cloth_collision_test.cpp
 * @brief Cloth against rigid colliders: draping over a sphere, stage order (rigid projection after
 *        self-collision), still and swept collider projection, and the mesh-collider rules
 *        (tunnelling recovery, front-face projection, concave pockets, resting at the standoff).
 */
#include <coopa/testing/test.h>

#include <physxcoopa/world.h>
#include <physxcoopa/cloth/cloth_builder.h>
#include <physxcoopa/cloth/cloth_solver.h>
#include "support/physics_fixtures.h"
#include "support/cloth_fixtures.h"

COOPA_TEST_SUITE("cloth_collision");

using namespace coopa::physx;
using namespace physxtest;

/**
 * @brief A sheet dropped onto a static sphere ends up entirely OUTSIDE it, and stays taut.
 *
 * The headline correctness test: no particle inside radius + thickness after settling is exactly
 * what "the cloth does not sink through the ball" means, and the stretch bound is what separates
 * a draped sheet from one that has exploded.
 */
COOPA_TEST(sheet_drapes_over_sphere_without_penetrating) {
    PhysicsWorld world;

    const float radius = 1.0f;
    const glm::vec3 sphere_center(0.0f, 0.0f, 3.0f);
    dynamics::Body ball;
    ball.type = dynamics::BodyType::Static;
    ball.position = sphere_center;
    ball.inv_mass = 0.0f;
    world.add_body(ball, collision::Shape::make_sphere(radius));

    cloth::ClothParams params;
    params.thickness = 0.03f;
    cloth::Cloth c = make_test_sheet(21, sphere_center + glm::vec3(0.0f, 0.0f, radius + 0.05f), params);
    cloth::pin_static(c, sphere_center + glm::vec3(0.0f, 0.0f, radius + 0.05f), 0.4f);
    cloth::build_tethers(c);
    cloth::ClothId id = world.add_cloth(std::move(c));

    for (int i = 0; i < 300; ++i) world.step(util::k_default_fixed_dt);

    const cloth::Cloth* sim = world.get_cloth(id);
    for (const cloth::ClothParticle& p : sim->particles) {
        // A small tolerance below `thickness`: a particle may sit slightly inside the standoff
        // between the collision projection and the next substep's constraint pass. Inside the
        // SPHERE itself is what must never happen.
        ASSERT_TRUE(glm::length(p.position - sphere_center) >= radius);
    }

    float max_stretch = 0.0f;
    for (const cloth::ClothConstraint& k : sim->stretch) {
        const float len = glm::length(sim->particles[k.a].position - sim->particles[k.b].position);
        max_stretch = std::max(max_stretch, len / k.rest_length);
    }
    ASSERT_TRUE(max_stretch < 1.25f);

    // And the sheet must actually have wrapped the ball, not just hovered above it.
    ASSERT_TRUE(sim->bounds.min.z < sphere_center.z);
}

/**
 * @brief Self-collision must never be the last word on a particle's position: whatever it pushes,
 *        the rigid-collision stage runs afterwards and puts it back outside the body.
 *
 * Built to fail loudly on the stage order, not to hope for it. Two non-neighbouring particles are
 * placed 2 cm apart just outside a sphere with self_distance forced to 20 cm, so self-collision
 * must shove them 9 cm apart -- driving the inner one ~5 cm INSIDE the sphere. Every other force is
 * removed (no constraints, no gravity, every other particle parked far away) so the assertion can
 * only be about ordering.
 */
COOPA_TEST(self_collision_cannot_push_a_particle_into_a_collider) {
    PhysicsWorld world;

    const float radius = 1.0f;
    dynamics::Body ball;
    ball.type = dynamics::BodyType::Static;
    ball.position = glm::vec3(0.0f);
    ball.inv_mass = 0.0f;
    world.add_body(ball, collision::Shape::make_sphere(radius));

    cloth::ClothParams params;
    params.self_collision = true;
    params.self_distance = 0.2f;   // >> the 2 cm gap below, so the push is large and predictable
    params.gravity_scale = 0.0f;
    params.air_drag = 0.0f;
    params.air_lift = 0.0f;
    params.thickness = 0.03f;
    // ONE substep, so exactly one pass of each stage runs and the ordering is directly observable.
    // At the default 4 substeps the defect hides itself: a particle self-collision shoved inside the
    // sphere gets projected back out by the NEXT substep's rigid stage, and after four rounds the
    // residual is under a centimetre -- small enough to pass a naive assertion while still being
    // exactly the intermittent surface-popping this ordering causes in a real drape.
    params.substeps = 1;
    // Parked far from the sphere; only the two probe particles are moved onto it.
    cloth::Cloth c = make_test_sheet(9, glm::vec3(0.0f, 0.0f, 50.0f), params);
    c.stretch.clear();
    c.bend.clear();
    c.stretch_batches.clear();
    c.bend_batches.clear();

    const uint32_t probe_a = 0;                 // (0,0)
    const uint32_t probe_b = 5 * c.columns + 5; // (5,5) -- well outside the exempt 1-ring
    c.particles[probe_a].position = glm::vec3(radius + 0.06f, 0.0f, 0.0f);
    c.particles[probe_b].position = glm::vec3(radius + 0.04f, 0.0f, 0.0f);
    for (uint32_t i : {probe_a, probe_b}) {
        c.particles[i].prev_position = c.particles[i].position;
        c.particles[i].velocity = glm::vec3(0.0f);
    }

    cloth::ClothId id = world.add_cloth(std::move(c));
    world.step(util::k_default_fixed_dt);

    const cloth::Cloth* sim = world.get_cloth(id);
    // They must have been separated -- otherwise the test proves nothing about ordering.
    const float gap = glm::length(sim->particles[probe_a].position - sim->particles[probe_b].position);
    ASSERT_TRUE(gap > 0.05f);
    // ...and neither may have been left inside the sphere by that separation. The rigid stage runs
    // last, so the surviving clearance is the FULL standoff, not merely "not interpenetrating".
    ASSERT_TRUE(glm::length(sim->particles[probe_a].position) >= radius + params.thickness - 1e-4f);
    ASSERT_TRUE(glm::length(sim->particles[probe_b].position) >= radius + params.thickness - 1e-4f);
}

/**
 * @brief A still collider's projection is unchanged by the swept code path, to the bit.
 *
 * The swept branch has to be inert when nothing is moving, or every settled drape in the engine
 * would silently shift. Asserted analytically rather than by hashing: a particle inside a stationary
 * unit sphere lands exactly on radius + thickness along its own radial direction.
 */
COOPA_TEST(still_collider_projection_is_exact) {
    collision::Shape sphere = collision::Shape::make_sphere(1.0f);
    sphere.enabled = true;

    cloth::ClothCollider collider;
    collider.shape = &sphere;
    collider.position = glm::vec3(0.0f);
    collider.bounds = collision::world_bounds(sphere, collider.position, collider.orientation);
    ASSERT_TRUE(!collider.is_swept());

    glm::vec3 p(0.5f, 0.0f, 0.0f);
    glm::vec3 normal(0.0f);
    float depth = 0.0f;
    ASSERT_TRUE(cloth::project_particle(collider, 0.03f, p, normal, depth));
    ASSERT_VEC_NEAR(p, glm::vec3(1.03f, 0.0f, 0.0f), 1e-5f);
    ASSERT_VEC_NEAR(normal, glm::vec3(1.0f, 0.0f, 0.0f), 1e-5f);
    ASSERT_NEAR(depth, 0.53f, 1e-5f);

    // A particle already clear of the surface is untouched.
    glm::vec3 q(2.0f, 0.0f, 0.0f);
    ASSERT_TRUE(!cloth::project_particle(collider, 0.03f, q, normal, depth));
    ASSERT_VEC_NEAR(q, glm::vec3(2.0f, 0.0f, 0.0f), 1e-6f);
}

/**
 * @brief A moving collider is projected out of the volume it SWEEPS, not just the pose it starts at.
 *
 * This is what keeps the sheet clear of a collider that the renderer will draw at several
 * intermediate poses before the cloth is solved again (above 60 Hz). Both shape paths are covered:
 * a sphere, whose swept volume is a capsule and therefore exact; and a box, which falls back to
 * probing the start and end poses and keeping the deeper correction.
 */
COOPA_TEST(swept_collider_projects_out_of_the_motion_path) {
    glm::vec3 normal(0.0f);
    float depth = 0.0f;

    // --- Sphere: exact, via the capsule the motion segment describes ---
    collision::Shape sphere = collision::Shape::make_sphere(1.0f);
    sphere.enabled = true;
    cloth::ClothCollider moving;
    moving.shape = &sphere;
    moving.position = glm::vec3(0.0f);
    moving.sweep = glm::vec3(2.0f, 0.0f, 0.0f);
    moving.bounds = geometry::AABB::merge(
        collision::world_bounds(sphere, moving.position, moving.orientation),
        collision::world_bounds(sphere, moving.position + moving.sweep, moving.orientation));
    ASSERT_TRUE(moving.is_swept());

    // 1.58 m from the start centre -- comfortably clear of the sphere where it is now, but only
    // 0.5 m off the line it travels along.
    const glm::vec3 start(1.5f, 0.5f, 0.0f);
    glm::vec3 p = start;
    cloth::ClothCollider still = moving;
    still.sweep = glm::vec3(0.0f);
    ASSERT_TRUE(!cloth::project_particle(still, 0.03f, p, normal, depth)); // unswept: no contact
    ASSERT_VEC_NEAR(p, start, 1e-6f);

    ASSERT_TRUE(cloth::project_particle(moving, 0.03f, p, normal, depth)); // swept: pushed clear
    ASSERT_VEC_NEAR(p, glm::vec3(1.5f, 1.03f, 0.0f), 1e-5f);
    ASSERT_VEC_NEAR(normal, glm::vec3(0.0f, 1.0f, 0.0f), 1e-5f);

    // --- Box: start/end probe, deeper correction wins ---
    collision::Shape box = collision::Shape::make_box(glm::vec3(0.5f));
    box.enabled = true;
    cloth::ClothCollider moving_box;
    moving_box.shape = &box;
    moving_box.position = glm::vec3(0.0f);
    moving_box.sweep = glm::vec3(2.0f, 0.0f, 0.0f);
    moving_box.end_orientation = moving_box.orientation;
    moving_box.bounds = geometry::AABB::merge(
        collision::world_bounds(box, moving_box.position, moving_box.orientation),
        collision::world_bounds(box, moving_box.position + moving_box.sweep, moving_box.end_orientation));

    // x = 1.52 is 1.02 m clear of the box at the origin, but 2 cm INSIDE it once it has travelled.
    glm::vec3 b(1.52f, 0.0f, 0.0f);
    cloth::ClothCollider still_box = moving_box;
    still_box.sweep = glm::vec3(0.0f);
    ASSERT_TRUE(!cloth::project_particle(still_box, 0.03f, b, normal, depth));

    ASSERT_TRUE(cloth::project_particle(moving_box, 0.03f, b, normal, depth));
    ASSERT_NEAR(b.x, 2.0f - (0.5f + 0.03f), 1e-5f); // exits the near -X face of the END pose
    ASSERT_VEC_NEAR(normal, glm::vec3(-1.0f, 0.0f, 0.0f), 1e-5f);
}

/**
 * @brief A particle that crosses a mesh surface in one substep must be put back on the side it
 *        came from, not left on the far side.
 *
 * This is the failure the mesh branch's proximity-only test cannot see: the particle lands beyond
 * `thickness` of every triangle, so the BVH query returns nothing and it stays through the floor
 * permanently. Sphere/box/capsule have no equivalent hole -- they all handle "already inside"
 * explicitly -- but a triangle soup has no cheap inside to test against, so the crossing itself has
 * to be caught.
 */
COOPA_TEST(mesh_collider_returns_a_tunnelling_particle_to_its_entry_side) {
    geometry::TriangleMesh floor = make_two_triangle_floor(); // a 4x4 quad in the z = 0 plane
    collision::Shape shape = collision::Shape::make_mesh(&floor);
    const cloth::ClothCollider collider = still_collider(shape);

    const float thickness = 0.03f;
    const glm::vec3 start(0.0f, 0.0f, 0.5f);
    glm::vec3 p(0.0f, 0.0f, -0.5f); // one substep of a ~120 m/s particle: straight through
    glm::vec3 normal(0.0f);
    float depth = 0.0f;

    ASSERT_TRUE(cloth::project_particle(collider, thickness, p, normal, depth, &start));
    ASSERT_NEAR(p.z, thickness, 1e-4f);                       // back on the entry side
    ASSERT_VEC_NEAR(normal, glm::vec3(0.0f, 0.0f, 1.0f), 1e-4f);

    // ...and symmetrically from below: a mesh is a surface, not a one-way gate.
    const glm::vec3 start_below(0.0f, 0.0f, -0.5f);
    glm::vec3 q(0.0f, 0.0f, 0.5f);
    ASSERT_TRUE(cloth::project_particle(collider, thickness, q, normal, depth, &start_below));
    ASSERT_NEAR(q.z, -thickness, 1e-4f);
    ASSERT_VEC_NEAR(normal, glm::vec3(0.0f, 0.0f, -1.0f), 1e-4f);
}

/** @brief Driven through the real solver, not just project_particle(): a cloth fired hard at a mesh
 *         floor ends up above it. Catches a stage-7 wiring mistake that the unit test above cannot
 *         (e.g. failing to pass the particle's start-of-substep position). */
COOPA_TEST(mesh_collider_stops_tunnelling_through_the_solver) {
    geometry::TriangleMesh floor = make_two_triangle_floor();

    PhysicsWorld world;
    dynamics::Body floor_body;
    floor_body.type = dynamics::BodyType::Static;
    floor_body.inv_mass = 0.0f;
    world.add_body(floor_body, collision::Shape::make_mesh(&floor));

    cloth::ClothParams params;
    params.thickness = 0.03f;
    params.air_drag = 0.0f;
    params.air_lift = 0.0f;
    // A small sheet dropped from just above the floor at high speed.
    cloth::GridClothDesc desc;
    desc.columns = 5;
    desc.rows = 5;
    desc.width = 1.0f;
    desc.height = 1.0f;
    desc.center = glm::vec3(0.0f, 0.0f, 1.0f);
    desc.total_mass = 1.0f;
    desc.params = params;
    cloth::Cloth c = cloth::make_grid_cloth(desc);
    for (cloth::ClothParticle& particle : c.particles) particle.velocity = glm::vec3(0.0f, 0.0f, -60.0f);
    cloth::ClothId id = world.add_cloth(std::move(c));

    for (int i = 0; i < 120; ++i) world.step(util::k_default_fixed_dt);

    const cloth::Cloth* sim = world.get_cloth(id);
    for (const cloth::ClothParticle& particle : sim->particles) {
        ASSERT_TRUE(particle.position.z > 0.0f);
    }
}

/** @brief Shallow particles on either side of a mesh surface leave through the FRONT face at
 *         exactly `thickness`.
 *
 *  Behind: a particle sitting 1 cm under the floor is recovered through the front face, not pushed
 *  further behind -- deriving the push from (particle - closest point) points the wrong way for a
 *  penetrating particle and cements the penetration at `thickness` on the wrong side.
 *  In front: a particle a hair above keeps taking exactly the path it always has, pushed straight up
 *  to `thickness` -- the regression guard for every settled drape in the engine. */
COOPA_TEST(mesh_collider_projects_shallow_particles_out_the_front_face) {
    geometry::TriangleMesh floor = make_two_triangle_floor();
    collision::Shape shape = collision::Shape::make_mesh(&floor);
    const cloth::ClothCollider collider = still_collider(shape);

    const float thickness = 0.03f;
    glm::vec3 normal(0.0f);
    float depth = 0.0f;

    glm::vec3 behind(0.0f, 0.0f, -0.01f); // 1 cm under the floor, motionless
    ASSERT_TRUE(cloth::project_particle(collider, thickness, behind, normal, depth));
    EXPECT_NEAR(behind.z, thickness, 1e-4f);
    EXPECT_VEC_NEAR(normal, glm::vec3(0.0f, 0.0f, 1.0f), 1e-4f);

    glm::vec3 above(0.0f, 0.0f, 0.01f);
    ASSERT_TRUE(cloth::project_particle(collider, thickness, above, normal, depth));
    EXPECT_NEAR(above.z, thickness, 1e-4f);
    EXPECT_VEC_NEAR(normal, glm::vec3(0.0f, 0.0f, 1.0f), 1e-4f);

    // Beyond the standoff: untouched. Also pins the gate -- a particle whose motion is shorter than
    // `thickness` cannot have crossed, so the swept raycast must not fire and must not perturb it.
    const glm::vec3 start(0.0f, 0.0f, 0.11f);
    glm::vec3 q(0.0f, 0.0f, 0.10f);
    EXPECT_FALSE(cloth::project_particle(collider, thickness, q, normal, depth, &start));
    EXPECT_VEC_NEAR(q, glm::vec3(0.0f, 0.0f, 0.10f), 1e-6f);
}

/**
 * @brief A particle in a concave pocket -- in front of one triangle, behind its angled neighbour --
 *        must not be flipped across the mesh.
 *
 * This is the guard on the "behind EVERY nearby triangle" rule. Deciding inside-ness from the single
 * nearest triangle's normal would teleport this particle through the wall, which is the internal-
 * edge failure mesh_contact.h needs TriangleAdjacency to avoid for rigid contacts.
 */
COOPA_TEST(mesh_collider_concave_pocket_is_not_flipped) {
    // A right-angled inside corner: floor in the z = 0 plane (normal +Z) and wall in the x = 0
    // plane (normal +X). A particle just inside the corner is in front of both.
    std::vector<glm::vec3> vertices = {
        glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f, -1.0f, 0.0f), glm::vec3(1.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, -1.0f, 1.0f), glm::vec3(0.0f, 1.0f, 1.0f),
    };
    std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3, 0, 3, 5, 0, 5, 4};
    std::vector<glm::vec3> normals = {
        glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
    };
    std::vector<geometry::TriangleAdjacency> adjacency(4);
    geometry::TriangleMesh corner(std::move(vertices), std::move(indices),
                                   std::move(normals), std::move(adjacency));

    collision::Shape shape = collision::Shape::make_mesh(&corner);
    shape.enabled = true;
    cloth::ClothCollider collider;
    collider.shape = &shape;
    collider.position = glm::vec3(0.0f);
    collider.bounds = collision::world_bounds(shape, collider.position, collider.orientation);

    const float thickness = 0.03f;
    // 2 mm behind the WALL but 2 cm above the FLOOR -- so the nearest triangle (the wall, 2 mm away)
    // reports "behind" while the floor reports "in front". Deciding inside-ness from the nearest
    // triangle alone would flip this particle 3.2 cm across the wall plane and into the room; the
    // "behind EVERY nearby triangle" rule leaves it on the side it was already on.
    glm::vec3 p(-0.002f, 0.0f, 0.02f);
    glm::vec3 normal(0.0f);
    float depth = 0.0f;
    ASSERT_TRUE(cloth::project_particle(collider, thickness, p, normal, depth));

    ASSERT_TRUE(p.x <= 0.0f);   // never teleported through the wall
    ASSERT_TRUE(p.z >= 0.0f);   // and still above the floor

    // The genuinely-penetrating case still recovers: put it behind BOTH faces and it must come back
    // out, which is what stops the rule above from simply disabling recovery near a corner.
    glm::vec3 q(-0.002f, 0.0f, -0.002f);
    ASSERT_TRUE(cloth::project_particle(collider, thickness, q, normal, depth));
    ASSERT_TRUE(q.x > 0.0f || q.z > 0.0f);
}

/** @brief A sheet settling on a mesh floor rests ON it, at the standoff, with nothing below --
 *         the end-to-end regression guard for the common mesh-collider case. */
COOPA_TEST(sheet_settles_on_mesh_floor_at_thickness) {
    geometry::TriangleMesh floor = make_two_triangle_floor();

    PhysicsWorld world;
    dynamics::Body floor_body;
    floor_body.type = dynamics::BodyType::Static;
    floor_body.inv_mass = 0.0f;
    world.add_body(floor_body, collision::Shape::make_mesh(&floor));

    cloth::ClothParams params;
    params.thickness = 0.03f;
    cloth::GridClothDesc desc;
    desc.columns = 9;
    desc.rows = 9;
    desc.width = 2.0f;
    desc.height = 2.0f;
    desc.center = glm::vec3(0.0f, 0.0f, 0.6f);
    desc.total_mass = 1.0f;
    desc.params = params;
    cloth::ClothId id = world.add_cloth(cloth::make_grid_cloth(desc));

    for (int i = 0; i < 300; ++i) world.step(util::k_default_fixed_dt);

    const cloth::Cloth* sim = world.get_cloth(id);
    float lowest = 1e9f;
    for (const cloth::ClothParticle& p : sim->particles) lowest = std::min(lowest, p.position.z);
    ASSERT_TRUE(lowest >= 0.0f);                       // nothing sank through
    ASSERT_NEAR(lowest, params.thickness, 5e-3f);      // and it is resting at the standoff
}

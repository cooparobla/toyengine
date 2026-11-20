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
#include <physxcoopa/geometry/height_grid.h>
#include <physxcoopa/geometry/triangle_mesh.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>
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
 * probing the start pose and then the end pose.
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

    // --- Box: start/end probe, the end pose projected last ---
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
 * @brief Whatever a swept capsule's two poses disagree about, the particle ends up outside the
 *        END pose -- the one the frame draws.
 *
 * A property over a lattice rather than one hand-placed particle, because the geometry that breaks
 * the alternative (keeping whichever pose's correction is deeper) is a capsule that both translates
 * and ROTATES, as a swinging shin does: the start pose's shortest exit is radial to a different
 * axis, so resolving it can leave the particle well inside the rotated pose. Picking the deeper
 * correction satisfies "the cloth left the collider" for a pose nobody renders, which on a walk
 * cycle shows up as a hem sinking into the leg on the few frames of peak limb speed.
 *
 * The lattice also guards the sweep's reason for existing: the start pose must still push, so the
 * test asserts that some samples are moved by it alone.
 */
COOPA_TEST(swept_capsule_leaves_the_particle_outside_the_end_pose) {
    const float thickness = 0.01f;
    collision::Shape shin = collision::Shape::make_capsule(0.06f, 0.2f);
    shin.enabled = true;

    cloth::ClothCollider swinging;
    swinging.shape = &shin;
    swinging.position = glm::vec3(0.0f);
    swinging.orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);          // axis +Y
    swinging.sweep = glm::vec3(0.05f, 0.0f, 0.02f);                    // a stride's worth per frame
    swinging.end_orientation = glm::angleAxis(glm::radians(35.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    ASSERT_TRUE(swinging.is_swept());

    const geometry::Capsule end_cap = collision::world_capsule(
        shin, swinging.position + swinging.sweep, swinging.end_orientation);
    const float clear = end_cap.radius + thickness;

    int moved = 0, start_only = 0;
    for (int ix = -6; ix <= 6; ++ix) {
        for (int iy = -8; iy <= 8; ++iy) {
            for (int iz = -6; iz <= 6; ++iz) {
                glm::vec3 p(ix * 0.02f, iy * 0.04f, iz * 0.02f);
                glm::vec3 n(0.0f);
                float d = 0.0f;
                if (!cloth::project_particle(swinging, thickness, p, n, d)) continue;
                ++moved;

                // The guarantee: clear of the pose being drawn, every time.
                const glm::vec3 on_axis = geometry::closest_point_on_segment(p, end_cap.a, end_cap.b);
                ASSERT_TRUE(glm::length(p - on_axis) >= clear - 1e-4f);

                // ...and the start pose is still doing work: this sample was untouched by the
                // end pose on its own.
                glm::vec3 only_end(ix * 0.02f, iy * 0.04f, iz * 0.02f), ne(0.0f);
                float de = 0.0f;
                if (!cloth::detail::project_at_pose_(shin, swinging.position + swinging.sweep,
                                                     swinging.end_orientation, glm::vec3(0.0f),
                                                     thickness, only_end, ne, de, nullptr)) {
                    ++start_only;
                }
            }
        }
    }
    ASSERT_TRUE(moved > 50);       // the lattice actually straddles the collider
    ASSERT_TRUE(start_only > 10);  // and the swept volume reaches beyond the end pose
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

#include <physxcoopa/geometry/convex_hull.h>

/**
 * @brief The ConvexHull branch of project_particle(), previously the one shape with no cloth
 *        coverage: a shallow outside particle is pushed to the thickness standoff along the face
 *        normal, an interior particle exits through its most-violated face, a clear particle is
 *        untouched -- and a sheet dropped onto a static hull body settles at the standoff, which
 *        also exercises the broadphase gather for hull shapes end to end.
 */
COOPA_TEST(convex_hull_collider_projects_and_supports_draping) {
    // A unit cube as a hull (8 corners, faces merged), centred at the origin.
    const std::vector<glm::vec3> pts = {
        {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
        {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f},  {0.5f, 0.5f, 0.5f}};
    geometry::ConvexHull hull;
    ASSERT_TRUE(geometry::build_convex_hull(pts.data(), pts.size(), hull));

    collision::Shape shape = collision::Shape::make_hull(&hull);
    cloth::ClothCollider collider = still_collider(shape);
    const float thickness = 0.03f;

    // Shallow, just off the +Z face: projected out to the standoff along +Z.
    glm::vec3 p(0.1f, -0.05f, 0.51f);
    glm::vec3 normal(0.0f);
    float depth = 0.0f;
    ASSERT_TRUE(cloth::project_particle(collider, thickness, p, normal, depth));
    ASSERT_NEAR(p.z, 0.5f + thickness, 1e-4f);
    ASSERT_TRUE(normal.z > 0.99f);
    ASSERT_TRUE(depth > 0.0f);

    // Interior: exits through the nearest (most violated) face, here +Z again.
    p = glm::vec3(0.1f, 0.05f, 0.4f);
    normal = glm::vec3(0.0f);
    depth = 0.0f;
    ASSERT_TRUE(cloth::project_particle(collider, thickness, p, normal, depth));
    ASSERT_NEAR(p.z, 0.5f + thickness, 1e-4f);
    ASSERT_TRUE(normal.z > 0.99f);

    // Clear of the hull by more than thickness: untouched.
    p = glm::vec3(0.0f, 0.0f, 0.6f);
    const glm::vec3 before = p;
    ASSERT_TRUE(!cloth::project_particle(collider, thickness, p, normal, depth));
    ASSERT_TRUE(p == before);

    // End to end: a sheet dropped onto a static hull body settles at the standoff above its top
    // face, through the same broadphase gather every other shape takes.
    PhysicsWorld world;
    dynamics::Body box_body;
    box_body.type = dynamics::BodyType::Static;
    box_body.inv_mass = 0.0f;
    world.add_body(box_body, collision::Shape::make_hull(&hull));

    cloth::ClothParams params;
    params.thickness = thickness;
    cloth::GridClothDesc desc;
    desc.columns = 9;
    desc.rows = 9;
    desc.width = 0.8f; // narrower than the cube top, so every particle rests ON the hull
    desc.height = 0.8f;
    desc.center = glm::vec3(0.0f, 0.0f, 0.8f);
    desc.total_mass = 1.0f;
    desc.params = params;
    cloth::ClothId id = world.add_cloth(cloth::make_grid_cloth(desc));

    for (int i = 0; i < 300; ++i) world.step(util::k_default_fixed_dt);

    const cloth::Cloth* sim = world.get_cloth(id);
    float lowest = 1e9f;
    for (const cloth::ClothParticle& p2 : sim->particles) lowest = std::min(lowest, p2.position.z);
    ASSERT_TRUE(lowest >= 0.5f);                            // nothing sank into the hull
    ASSERT_NEAR(lowest, 0.5f + thickness, 5e-3f);           // resting at the standoff
}

/**
 * @brief Surface collision (stage 7b) holds the sheet's TRIANGLES off a sphere, not just its
 *        particles -- the artefact that reads as the collider bleeding through the cloth.
 *
 * Deliberately coarse: a 7x7 sheet over a 1 m ball has ~0.67 m chords, which sag ~6 cm below
 * the sphere's arc -- far past any sane `thickness`. With `collide_surface` off every particle
 * is correctly outside and the surface is still deep inside; turning it on fixes the surface
 * without touching the resolution. The A/B is the point: it proves the samples are doing the
 * work rather than the sheet happening to settle clear.
 */
COOPA_TEST(surface_samples_keep_the_drawn_sheet_off_a_sphere) {
    const float radius = 1.0f;
    collision::Shape sphere = collision::Shape::make_sphere(radius);

    auto worst_surface_depth = [&](bool collide_surface) {
        PhysicsWorld world;
        dynamics::Body ball;
        ball.type = dynamics::BodyType::Static;
        ball.inv_mass = 0.0f;
        ball.position = glm::vec3(0.0f);
        world.add_body(ball, sphere);

        cloth::ClothParams params;
        params.thickness = 0.02f;
        params.collide_surface = collide_surface;
        cloth::GridClothDesc desc;
        desc.columns = 7;          // ~0.67 m spacing over 4 m: deliberately coarse
        desc.rows = 7;
        desc.width = 4.0f;
        desc.height = 4.0f;
        desc.center = glm::vec3(0.0f, 0.0f, 1.6f);
        desc.total_mass = 1.0f;
        desc.params = params;
        cloth::ClothId id = world.add_cloth(cloth::make_grid_cloth(desc));

        for (int i = 0; i < 240; ++i) world.step(util::k_default_fixed_dt);

        const cloth::Cloth* sim = world.get_cloth(id);
        float worst_particle = 0.0f, worst_surface = 0.0f;
        for (const cloth::ClothParticle& p : sim->particles) {
            worst_particle = std::max(worst_particle, radius - glm::length(p.position));
        }
        // Sample each triangle's interior, where a chord hides.
        constexpr int k_steps = 5;
        for (std::size_t t = 0; t + 2 < sim->triangles.size(); t += 3) {
            const glm::vec3& a = sim->particles[sim->triangles[t]].position;
            const glm::vec3& b = sim->particles[sim->triangles[t + 1]].position;
            const glm::vec3& c = sim->particles[sim->triangles[t + 2]].position;
            for (int i = 0; i <= k_steps; ++i) {
                for (int j = 0; i + j <= k_steps; ++j) {
                    const float u = static_cast<float>(i) / k_steps;
                    const float v = static_cast<float>(j) / k_steps;
                    worst_surface = std::max(worst_surface,
                                             radius - glm::length(a * (1.0f - u - v) + b * u + c * v));
                }
            }
        }
        return std::make_pair(worst_particle, worst_surface);
    };

    const auto off = worst_surface_depth(false);
    const auto on = worst_surface_depth(true);

    ASSERT_TRUE(off.first <= 0.0f);   // control: particles were never the problem
    ASSERT_TRUE(on.first <= 0.0f);
    // Off, the drawn surface is deep inside the ball...
    ASSERT_TRUE(off.second > 0.03f);
    // ...and on, it is out of it entirely.
    ASSERT_TRUE(on.second <= 0.0f);
}

/**
 * @brief A sheet draped over OVERLAPPING colliders ends up outside every one of them.
 *
 * The case a single pass over the collider list cannot handle, and the one a body is made of: push
 * a particle out of one collider and it frequently lands inside a neighbour the pass has already
 * visited and will not revisit, so the particle ends the substep inside the union no matter how
 * many substeps run -- every substep repeats the same mistake. Stage 7 therefore re-sweeps each
 * particle until it is clear (`k_collider_passes`).
 *
 * Three heavily overlapping spheres rather than one, because one sphere is projected exactly and
 * proves nothing here. Measured on a character garment over a 17-capsule skeleton, the single pass
 * left up to 4.2 cm inside a shin.
 */
COOPA_TEST(sheet_clears_every_one_of_a_chain_of_overlapping_colliders) {
    PhysicsWorld world;

    const float radius = 0.5f;
    // Centres 0.6 m apart with radius 0.5: each sphere's lens of overlap with its neighbour is
    // wide, so a radial push out of one reaches well into the next.
    const glm::vec3 centers[3] = {glm::vec3(-0.6f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f),
                                  glm::vec3(0.6f, 0.0f, 1.0f)};
    for (const glm::vec3& centre : centers) {
        dynamics::Body body;
        body.type = dynamics::BodyType::Static;
        body.inv_mass = 0.0f;
        body.position = centre;
        world.add_body(body, collision::Shape::make_sphere(radius));
    }

    cloth::ClothParams params;
    params.thickness = 0.02f;
    cloth::GridClothDesc desc;
    desc.columns = 15;
    desc.rows = 15;
    desc.width = 2.6f;
    desc.height = 2.6f;
    desc.center = glm::vec3(0.0f, 0.0f, 1.9f);
    desc.total_mass = 1.0f;
    desc.params = params;
    cloth::ClothId id = world.add_cloth(cloth::make_grid_cloth(desc));

    for (int i = 0; i < 300; ++i) world.step(util::k_default_fixed_dt);

    const cloth::Cloth* sim = world.get_cloth(id);
    float worst = 0.0f, closest = 1e9f;
    for (const cloth::ClothParticle& p : sim->particles) {
        for (const glm::vec3& centre : centers) {
            const float d = glm::length(p.position - centre);
            worst = std::max(worst, radius - d);
            closest = std::min(closest, d - radius);
        }
    }
    ASSERT_TRUE(worst <= 0.0f);     // outside ALL three, not just the last one projected
    ASSERT_TRUE(closest < 0.05f);   // and it is genuinely draped on them, not floating clear
}

/**
 * @brief A sheet settles ON a heightfield collider's slope rather than falling through it.
 *
 * Heightmap terrain's collision is a `HeightfieldCollider`, and the shape it makes is a
 * `TriangleMesh` in heightfield mode (`TriangleMesh::from_height_grid()`) -- so cloth reaches it
 * through the same branch a mesh asset uses, with no heightfield case of its own anywhere in
 * `project_at_pose_()`. That is the right design, and it is also completely untested: nothing in
 * the tree had ever dropped cloth on terrain, and "the TriangleMesh branch covers it" was an
 * inference from the type rather than a measurement. Heightfield mode stores no triangle array at
 * all (`TriangleMesh::height_grid()` is non-null and the triangles are decoded on demand), so the
 * BVH query, the raycast and `triangle_normal()` the cloth branch depends on are a different code
 * path from a stored mesh's.
 *
 * A SLOPE rather than a flat grid, because a flat one would pass on the proximity query alone and
 * say nothing about the normals: on a slope the sheet has to be pushed along the surface normal and
 * has somewhere to slide if the normal comes out wrong.
 */
COOPA_TEST(a_sheet_settles_on_a_heightfield_collider) {
    // A 4 m square ramp climbing in +x at 1:4, sampled every 0.5 m.
    constexpr int k_n = 9;
    constexpr float k_cell = 0.5f;
    std::vector<float> heights(static_cast<std::size_t>(k_n) * k_n);
    for (int j = 0; j < k_n; ++j) {
        for (int i = 0; i < k_n; ++i) {
            heights[static_cast<std::size_t>(j) * k_n + i] = static_cast<float>(i) * k_cell * 0.25f;
        }
    }
    auto grid = std::make_shared<const geometry::HeightGrid>(k_n, k_n, glm::vec2(k_cell, k_cell),
                                                             std::move(heights));
    const geometry::TriangleMesh surface = geometry::TriangleMesh::from_height_grid(grid);
    ASSERT_TRUE(surface.height_grid() != nullptr);   // the shape really is in heightfield mode
    collision::Shape shape = collision::Shape::make_mesh(&surface);
    const cloth::ClothCollider collider = still_collider(shape);
    const std::vector<cloth::ClothCollider> colliders{collider};

    cloth::ClothParams params;
    params.thickness = 0.03f;
    params.air_drag = 0.0f;   // a free sheet with aero would sail off the ramp; this is a drape test
    params.air_lift = 0.0f;
    cloth::GridClothDesc desc;
    desc.columns = desc.rows = 9;
    desc.width = desc.height = 2.0f;
    desc.center = glm::vec3(2.0f, 2.0f, 1.6f);   // centred over the ramp, well clear of it
    desc.params = params;
    cloth::Cloth c = cloth::make_grid_cloth(desc);

    cloth::ClothSolverScratch scratch;
    for (int frame = 0; frame < 300; ++frame) {
        cloth::solve_cloth(c, colliders, glm::vec3(0.0f, 0.0f, -9.81f), 4, 1, 1.0f / 60.0f, scratch);
    }

    // Every particle at or above the ramp under it -- the claim that would fail outright if the
    // heightfield branch did nothing.
    float worst_below = 0.0f;
    int on_surface = 0;
    for (const cloth::ClothParticle& p : c.particles) {
        const float gx = p.position.x / k_cell, gy = p.position.y / k_cell;
        if (gx < 0.0f || gy < 0.0f || gx > k_n - 1 || gy > k_n - 1) continue;   // off the grid
        const float ground = p.position.x * 0.25f;   // the ramp is analytic, so this is exact
        const float above = p.position.z - ground;
        worst_below = std::min(worst_below, above);
        if (above < params.thickness + 0.02f) ++on_surface;
    }
    std::cout << "    deepest below the ramp " << worst_below << " m; " << on_surface
              << " particles resting on it\n";
    // A sheet that fell through would be metres under, and one left hanging in the air would rest
    // on nothing -- so both failure modes are covered, per the lesson that a penetration bound
    // alone passes on a sheet that is not there.
    ASSERT_TRUE(worst_below > -0.01f);
    ASSERT_TRUE(on_surface > 20);
}

/**
 * @brief Projecting an already-projected particle corrects nothing, so the re-sweep breaks out.
 *
 * `k_collider_passes` re-sweeps a particle against the whole collider set until a pass corrects
 * nothing, which is what makes overlapping colliders work (projecting out of a thigh can put a
 * particle inside the shin). The loop's cost therefore rests entirely on `project_particle()` being
 * IDEMPOTENT: a particle sitting exactly at `radius + thickness` must be reported as outside.
 *
 * That is a float-equality question, not an obvious one. Every branch places the particle at
 * `point + normal * thickness` and then rejects on `len >= target` -- so if the placement lands a
 * single ulp short of `target`, the next pass re-projects, reports a correction, and every
 * contacting particle burns all four passes every substep instead of two. On a resting sheet that
 * is most of the collision stage, silently, with no behavioural tell at all.
 *
 * Checked on each analytic shape the passes can actually revisit, from the inside case too, since
 * the exit placement is a different code path from the proximity one.
 */
COOPA_TEST(projection_is_idempotent_so_the_collider_re_sweep_breaks_out) {
    const float thickness = 0.03f;

    collision::Shape sphere = collision::Shape::make_sphere(1.0f);
    collision::Shape box = collision::Shape::make_box(glm::vec3(0.5f));
    collision::Shape capsule = collision::Shape::make_capsule(0.4f, 1.0f);

    // The mesh and hull branches share the same clamp-and-subtract shape the box branch does, so
    // they are checked here too rather than assumed exact.
    geometry::TriangleMesh floor = make_two_triangle_floor();   // a 4x4 quad in the z = 0 plane
    collision::Shape mesh = collision::Shape::make_mesh(&floor);

    const std::vector<glm::vec3> cube_pts = {
        {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
        {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f},  {0.5f, 0.5f, 0.5f}};
    geometry::ConvexHull hull;
    ASSERT_TRUE(geometry::build_convex_hull(cube_pts.data(), cube_pts.size(), hull));
    collision::Shape hull_shape = collision::Shape::make_hull(&hull);

    struct Case { const char* name; collision::Shape* shape; glm::vec3 from; };
    const Case cases[] = {
        {"sphere, outside",  &sphere,  glm::vec3(0.0f, 0.0f, 1.01f)},
        {"sphere, inside",   &sphere,  glm::vec3(0.0f, 0.0f, 0.20f)},
        {"box, outside",     &box,     glm::vec3(0.0f, 0.0f, 0.51f)},
        {"box, inside",      &box,     glm::vec3(0.0f, 0.0f, 0.10f)},
        {"box, off a face",  &box,     glm::vec3(0.52f, 0.13f, 0.27f)},
        {"box, off an edge", &box,     glm::vec3(0.51f, 0.51f, 0.10f)},
        {"capsule, outside", &capsule, glm::vec3(0.41f, 0.0f, 0.0f)},
        {"capsule, inside",  &capsule, glm::vec3(0.05f, 0.0f, 0.0f)},
        {"mesh, above",      &mesh,    glm::vec3(0.3f, 0.2f, 0.01f)},
        {"hull, above",      &hull_shape, glm::vec3(0.0f, 0.0f, 0.51f)},
    };

    for (const Case& c : cases) {
        const cloth::ClothCollider collider = still_collider(*c.shape);
        glm::vec3 p = c.from;
        glm::vec3 normal(0.0f);
        float depth = 0.0f;

        // Pass 1 moves it to the standoff.
        ASSERT_TRUE(cloth::project_particle(collider, thickness, p, normal, depth, nullptr));
        const glm::vec3 settled = p;

        // Pass 2 onward must report NOTHING, or the re-sweep loop never breaks. This is the check.
        int extra = 0;
        for (uint32_t pass = 0; pass < cloth::k_collider_passes; ++pass) {
            glm::vec3 n2(0.0f);
            float d2 = 0.0f;
            if (cloth::project_particle(collider, thickness, p, n2, d2, nullptr)) ++extra;
        }
        const float drift = glm::length(p - settled);
        std::cout << "    " << c.name << ": " << extra << " redundant passes, drift " << drift
                  << " m\n";
        ASSERT_TRUE(extra == 0);
        ASSERT_NEAR(drift, 0.0f, 1e-7f);
    }
}

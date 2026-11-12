/**
 * @file sleep_test.cpp
 * @brief Sleeping and waking: bodies come to rest and sleep, kinematic motion (linear or pure spin)
 *        wakes what it touches, a sleeping body is immune to an awake neighbour's impulse, and
 *        joints unite islands for sleep. API-level wake (add_force etc.) is in physics_world_test.cpp.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/world.h>
#include "support/physics_fixtures.h"

COOPA_TEST_SUITE("sleep");

using namespace coopa::physx;
using namespace physxtest;

COOPA_TEST(resting_sphere_falls_asleep_at_rest_height) {
    PhysicsWorld world;
    add_static_ground(world);
    dynamics::BodyId sphere_id = add_dynamic_sphere(world, glm::vec3(0.0f, 0.0f, 2.0f), 0.5f);

    bool slept = false;
    for (int i = 0; i < 400 && !slept; ++i) {
        world.step_fixed(util::k_default_fixed_dt);
        if (!world.get_body(sphere_id)->awake) slept = true;
    }
    ASSERT_TRUE(slept);

    const dynamics::Body* b = world.get_body(sphere_id);
    ASSERT_NEAR(b->position.z, 0.5f, 0.05f);
    ASSERT_TRUE(glm::length(b->linear_velocity) < world.config().sleep_linear + 1e-4f);
}

/**
 * The kinematic wake rule (dynamics/solver.h's wake_if_platform_moving) needs a body that is
 * genuinely ASLEEP before the platform starts moving -- kinematic_platform_pushes_resting_body
 * (contact_solver_test.cpp) starts the platform moving from step 0, so the sphere is still awake the whole time and
 * never exercises this rule at all. Without it, a body asleep on a platform that starts sliding
 * out from under it would hover in place while the platform moves away, since a sleeping body
 * skips integration entirely.
 */
COOPA_TEST(moving_kinematic_platform_wakes_sleeping_body) {
    PhysicsWorld world;

    dynamics::Body platform;
    platform.type = dynamics::BodyType::Kinematic;
    platform.position = glm::vec3(0.0f, 0.0f, -0.5f);
    dynamics::BodyId platform_id = world.add_body(platform, collision::Shape::make_box(glm::vec3(2.0f, 2.0f, 0.5f)));

    dynamics::BodyId sphere_id = add_dynamic_sphere(world, glm::vec3(0.0f, 0.0f, 1.0f), 0.5f);

    // Let the sphere fall, rest on the stationary platform, and fall asleep.
    for (int i = 0; i < 120; ++i) world.step_fixed(util::k_default_fixed_dt);
    ASSERT_TRUE(!world.get_body(sphere_id)->awake);

    // Now slide the platform sideways -- the sphere should wake immediately, not hover in place.
    world.get_body(platform_id)->linear_velocity = glm::vec3(3.0f, 0.0f, 0.0f);
    world.step_fixed(util::k_default_fixed_dt);
    const dynamics::Body* s = world.get_body(sphere_id);
    ASSERT_TRUE(s->awake);
}

/**
 * The kinematic wake rule's linear check (exercised above) misses a kinematic body that only
 * ROTATES in place -- e.g. physics_test's spinner, hinged at its own Transform origin, whose
 * position never changes. Before dynamics/solver.h's wake_if_kinematic_moving also checked
 * angular_velocity, a sleeping body such a paddle sweeps into would never wake, and
 * solve_velocity_pass()'s "skip unless at least one side is an awake dynamic body" gate would
 * then skip that contact forever -- the paddle visually passing straight through it.
 */
COOPA_TEST(spinning_kinematic_body_wakes_sleeping_body_it_sweeps) {
    PhysicsWorld world;
    add_static_ground(world);
    dynamics::BodyId sphere_id = add_dynamic_sphere(world, glm::vec3(3.0f, 0.0f, 1.0f), 0.5f);

    // Let it settle onto the ground and fall asleep before the paddle appears.
    for (int i = 0; i < 120; ++i) world.step_fixed(util::k_default_fixed_dt);
    ASSERT_TRUE(!world.get_body(sphere_id)->awake);
    glm::vec3 rest_pos = world.get_body(sphere_id)->position;

    // A kinematic "paddle" already overlapping the sleeping sphere, spinning in place -- zero
    // linear_velocity, nonzero angular_velocity only.
    dynamics::Body paddle;
    paddle.type = dynamics::BodyType::Kinematic;
    paddle.position = rest_pos;
    paddle.angular_velocity = glm::vec3(0.0f, 0.0f, 2.0f);
    world.add_body(paddle, collision::Shape::make_box(glm::vec3(1.0f, 1.0f, 1.0f)));

    world.step_fixed(util::k_default_fixed_dt);
    ASSERT_TRUE(world.get_body(sphere_id)->awake);
}

/**
 * dynamics/solver.h's apply_impulse_pair() applies an impulse only to an AWAKE dynamic body,
 * consistent with warm_start()/solve_velocity_pass(), which zero a sleeping body's
 * inv_mass/inv_inertia (treating it as immovable) before deriving that impulse's magnitude.
 * Without the `awake` check, a sleeping body touched by an awake neighbor would get a nonzero
 * velocity baked in while still marked asleep -- integrate_velocities() skips a sleeping body,
 * so nothing visibly moves yet, but the next time the body woke (by any means) it would pop with
 * that stale, unaccounted-for velocity. Three flush-stacked boxes, where the bottom one sleeps
 * before its neighbors finish settling, is exactly this shape.
 */
COOPA_TEST(sleeping_body_ignores_awake_neighbours_impulse) {
    PhysicsWorld world;
    world.set_gravity(glm::vec3(0.0f)); // isolate the impulse effect from gravity/settling

    dynamics::BodyId sleeper_id = add_dynamic_sphere(world, glm::vec3(0.0f, 0.0f, 0.0f), 0.5f);
    dynamics::Body* sleeper = world.get_body(sleeper_id);
    sleeper->awake = false;
    sleeper->linear_velocity = glm::vec3(0.0f);
    sleeper->angular_velocity = glm::vec3(0.0f);

    // Overlapping at spawn (radii sum to 1.0, centers 0.9 apart) so a manifold exists from the
    // very first step, approaching further still.
    dynamics::BodyId mover_id = add_dynamic_sphere(world, glm::vec3(0.9f, 0.0f, 0.0f), 0.5f);
    world.get_body(mover_id)->linear_velocity = glm::vec3(-2.0f, 0.0f, 0.0f);

    world.step_fixed(util::k_default_fixed_dt);

    const dynamics::Body* s = world.get_body(sleeper_id);
    ASSERT_TRUE(!s->awake);
    ASSERT_VEC_NEAR(s->linear_velocity, glm::vec3(0.0f), 1e-6f);
    ASSERT_VEC_NEAR(s->angular_velocity, glm::vec3(0.0f), 1e-6f);
}

/**
 * Two DYNAMIC bodies joined only by a hinge (no contact between them) must island-unite for
 * sleep purposes, same as two bodies touching via a contact manifold already do -- see
 * update_islands_and_sleep()'s doc. Body A starts completely at rest (would satisfy its OWN
 * sleep_timer within sleep_time on its own if NOT unioned with B); body B starts with a fast
 * spin that easily outlasts sleep_time. If the union-find correctly includes joint pairs, A
 * must NOT have gone to sleep by the time B is still clearly moving.
 */
COOPA_TEST(joint_connected_bodies_share_a_sleep_island) {
    PhysicsWorld world;
    world.set_gravity(glm::vec3(0.0f));

    dynamics::Body a_body;
    a_body.position = glm::vec3(0.0f);
    dynamics::BodyId a_id = world.add_body(a_body); // completely at rest from frame 0

    dynamics::Body b_body;
    b_body.position = glm::vec3(1.0f, 0.0f, 0.0f);
    b_body.angular_velocity = glm::vec3(0.0f, 0.0f, 2.0f); // spins about the hinge axis, only
                                                             // damped by angular_drag -- stays
                                                             // well above sleep_angular for a
                                                             // long time (slow exponential decay)
    dynamics::BodyId b_id = world.add_body(b_body);

    dynamics::JointId joint = world.add_hinge_joint(a_id, b_id, glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
                                                      glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f),
                                                      /*use_limits=*/false);
    ASSERT_TRUE(joint.is_valid());

    // sleep_time defaults to 0.5s (30 steps) -- run well past that. B is still clearly moving
    // (angular_drag's exponential decay from 2.0 rad/s takes far longer than this to reach
    // sleep_angular=0.02), so if A were sleeping independently it would have slept by now.
    for (int i = 0; i < 90; ++i) world.step_fixed(util::k_default_fixed_dt); // 1.5s
    ASSERT_TRUE(glm::length(world.get_body(b_id)->angular_velocity) > 0.5f); // confirm B really is still moving
    ASSERT_TRUE(world.get_body(a_id)->awake); // A must still be awake -- gated by B via the shared island
}

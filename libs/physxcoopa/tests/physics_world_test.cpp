/**
 * @file physics_world_test.cpp
 * @brief PhysicsWorld as a container and integrator: free fall against the semi-implicit Euler answer,
 *        generational body handles, the immediate-impulse/force API and its wake rule, the
 *        on_substep signal, determinism (repeat and serial-vs-job-parallel), debug drawing, and
 *        convex-hull integration (a hull resting on and stacked on a floor, catching a bad
 *        manifold/normal/inertia at once). Contact resolution is contact_solver_test.cpp;
 *        sleeping is sleep_test.cpp.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/world.h>
#include <physxcoopa/debug/debug_draw.h>
#include <coopa/job/engine.h>

#include <random>
#include "support/physics_fixtures.h"

COOPA_TEST_SUITE("physics_world");

using namespace coopa::physx;
using namespace physxtest;

/** @brief One second of free fall lands exactly on semi-implicit Euler's closed form, while a
 *         static and a kinematic body (no velocity) in the same world never move. */
COOPA_TEST(free_fall_matches_analytic_while_static_and_kinematic_hold) {
    PhysicsWorld world;
    dynamics::Body b;
    b.type = dynamics::BodyType::Dynamic;
    b.position = glm::vec3(0.0f);
    b.inv_mass = 1.0f;
    dynamics::BodyId id = world.add_body(b);

    dynamics::Body kin;
    kin.type = dynamics::BodyType::Kinematic;
    kin.position = glm::vec3(1.0f, 2.0f, 3.0f);
    dynamics::BodyId kin_id = world.add_body(kin);

    dynamics::Body stat;
    stat.type = dynamics::BodyType::Static;
    stat.position = glm::vec3(4.0f, 5.0f, 6.0f);
    dynamics::BodyId stat_id = world.add_body(stat);

    const float h = util::k_default_fixed_dt;
    for (int i = 0; i < 60; ++i) world.step_fixed(h); // 1 second

    const dynamics::Body* body = world.get_body(id);
    ASSERT_TRUE(body != nullptr);
    // Semi-implicit Euler's known result for constant acceleration g over N steps of h:
    // z = -g * h * (1+2+...+N) * h = -g*h^2*N*(N+1)/2 (velocity applied BEFORE position).
    float n = 60.0f;
    float expected_z = util::k_gravity_z * h * h * n * (n + 1.0f) * 0.5f;
    ASSERT_NEAR(body->position.z, expected_z, 0.01f);
    ASSERT_NEAR(body->position.z, -0.5f * 9.81f, 0.2f); // close to the continuous-time answer too

    EXPECT_VEC_NEAR(world.get_body(kin_id)->position, glm::vec3(1.0f, 2.0f, 3.0f), 1e-5f);
    EXPECT_VEC_NEAR(world.get_body(stat_id)->position, glm::vec3(4.0f, 5.0f, 6.0f), 1e-5f);
}

COOPA_TEST(stale_body_handle_is_rejected_after_slot_reuse) {
    PhysicsWorld world;
    dynamics::BodyId id_a = world.add_body(dynamics::Body{});
    world.remove_body(id_a);
    dynamics::BodyId id_b = world.add_body(dynamics::Body{}); // recycles id_a's slot
    ASSERT_TRUE(id_a.index == id_b.index);
    ASSERT_TRUE(id_a.generation != id_b.generation);
    ASSERT_TRUE(!world.is_valid(id_a));
    ASSERT_TRUE(world.is_valid(id_b));
}

/**
 * Impulses change velocity IMMEDIATELY (unlike add_force()/add_torque(), which only take effect on
 * the next integrate_forces() call) -- confirmed with no step_fixed() call at all, so the change
 * can only have come from the impulse methods themselves. apply_impulse() adds impulse*inv_mass;
 * apply_angular_impulse() adds inv_inertia_world*J; apply_impulse_at_position() does both, with
 * angular part cross(r, impulse); velocity_at_point() is the rigid-body point velocity v + w x r.
 * An impulse through the center of mass must induce no spin.
 */
COOPA_TEST(impulses_change_velocity_immediately_and_exactly) {
    PhysicsWorld world;
    world.set_gravity(glm::vec3(0.0f));

    dynamics::Body* plain = world.get_body(add_dynamic_sphere(world, glm::vec3(0.0f), 0.5f)); // mass 1, inv_mass 1
    plain->apply_impulse(glm::vec3(4.0f, 0.0f, 0.0f));
    ASSERT_VEC_NEAR(plain->linear_velocity, glm::vec3(4.0f, 0.0f, 0.0f), 1e-6f); // impulse * inv_mass
    glm::vec3 plain_inv_i = plain->inv_inertia_local; // sphere: isotropic, so world == local here (identity orientation)
    plain->apply_angular_impulse(glm::vec3(0.0f, 0.0f, 2.0f));
    ASSERT_VEC_NEAR(plain->angular_velocity, glm::vec3(0.0f, 0.0f, 2.0f * plain_inv_i.z), 1e-6f);

    dynamics::BodyId id = add_dynamic_sphere(world, glm::vec3(1.0f, 2.0f, 3.0f), 0.5f); // mass 1
    dynamics::Body* b = world.get_body(id);
    b->awake = false;

    b->apply_impulse_at_position(glm::vec3(0.0f, 0.0f, 2.0f), b->position);
    ASSERT_TRUE(b->awake);
    ASSERT_VEC_NEAR(b->linear_velocity, glm::vec3(0.0f, 0.0f, 2.0f), 1e-6f);
    ASSERT_VEC_NEAR(b->angular_velocity, glm::vec3(0.0f), 1e-6f);

    // Off-center: r = (+1,0,0), J = (0,0,1) -> angular impulse cross(r, J) = (0,-1,0).
    glm::vec3 inv_i = b->inv_inertia_local; // isotropic sphere, identity orientation
    b->apply_impulse_at_position(glm::vec3(0.0f, 0.0f, 1.0f), b->position + glm::vec3(1.0f, 0.0f, 0.0f));
    ASSERT_VEC_NEAR(b->linear_velocity, glm::vec3(0.0f, 0.0f, 3.0f), 1e-6f);
    ASSERT_VEC_NEAR(b->angular_velocity, glm::vec3(0.0f, -inv_i.y, 0.0f), 1e-6f);

    // v + w x r at r = (1,0,0): w = (0,-k,0) -> w x r = (0,0,k).
    glm::vec3 vp = b->velocity_at_point(b->position + glm::vec3(1.0f, 0.0f, 0.0f));
    ASSERT_VEC_NEAR(vp, glm::vec3(0.0f, 0.0f, 3.0f + inv_i.y), 1e-6f);
    ASSERT_VEC_NEAR(b->velocity_at_point(b->position), b->linear_velocity, 1e-6f);

    // wake_body = false: a continuous field must not keep resetting the sleep timer.
    b->sleep_timer = 0.3f;
    b->apply_impulse_at_position(glm::vec3(0.0f, 0.0f, 0.1f), b->position, false);
    ASSERT_NEAR(b->sleep_timer, 0.3f, 1e-6f);
}

/**
 * add_force()/add_torque()/add_force_at_position()/apply_impulse()/apply_angular_impulse() all
 * wake a sleeping Dynamic body (matching Unity's AddForce) -- otherwise the change would sit in
 * force_accum/torque_accum or the velocity with no effect until something ELSE woke the body
 * (integrate_forces()/integrate_velocities() both early-out on `!awake`). Confirmed two ways: the flag itself, and that a subsequent step
 * actually integrates the change (not just that `awake` reads true).
 */
COOPA_TEST(force_and_impulse_apis_wake_a_sleeping_body) {
    PhysicsWorld world;
    world.set_gravity(glm::vec3(0.0f));

    auto sleeping_sphere = [&]() {
        dynamics::BodyId id = add_dynamic_sphere(world, glm::vec3(0.0f), 0.5f);
        dynamics::Body* b = world.get_body(id);
        b->awake = false;
        b->sleep_timer = 0.0f;
        return id;
    };

    dynamics::BodyId force_id = sleeping_sphere();
    world.get_body(force_id)->add_force(glm::vec3(1.0f, 0.0f, 0.0f));
    ASSERT_TRUE(world.get_body(force_id)->awake);

    dynamics::BodyId impulse_id = sleeping_sphere();
    world.get_body(impulse_id)->apply_impulse(glm::vec3(1.0f, 0.0f, 0.0f));
    ASSERT_TRUE(world.get_body(impulse_id)->awake);
    ASSERT_VEC_NEAR(world.get_body(impulse_id)->linear_velocity, glm::vec3(1.0f, 0.0f, 0.0f), 1e-6f);

    // Functional check, not just the flag: a woken body actually integrates on the next step.
    world.step_fixed(util::k_default_fixed_dt);
    ASSERT_TRUE(world.get_body(impulse_id)->position.x > 0.0f);
}

COOPA_TEST(on_substep_fires_each_substep_and_destroy_inside_it_is_safe) {
    PhysicsWorld world;
    dynamics::BodyId id = add_dynamic_sphere(world, glm::vec3(0.0f, 0.0f, 5.0f), 0.5f);

    int substep_calls = 0;
    bool destroyed_inside = false;
    world.on_substep.connect([&](PhysicsWorld& w, float) {
        ++substep_calls;
        if (substep_calls == 3) {
            w.destroy_body(id);
            destroyed_inside = true;
        }
    });

    for (int i = 0; i < 5; ++i) world.step_fixed(util::k_default_fixed_dt);

    ASSERT_TRUE(substep_calls == 5);
    ASSERT_TRUE(destroyed_inside);
    ASSERT_TRUE(!world.is_valid(id));
}

/**
 * @brief The determinism argument PhysicsWorld::discover_pairs_()/narrowphase_()'s docs make:
 *        rebuilding and re-running the same scene reproduces world_state_hash() exactly, and a
 *        job-parallel run is bit-identical to a fully serial one. Forces the parallel path on
 *        every stage (parallel_threshold(1)) so even this modest body count exercises it, rather
 *        than silently falling back serial.
 */
COOPA_TEST(stepping_is_deterministic_across_runs_and_serial_vs_parallel) {
    auto build_and_run = [](coopa::job::JobEngine* jobs) {
        PhysicsWorld world;
        world.set_parallel_threshold(1);
        world.set_job_engine(jobs);

        add_static_ground(world);

        std::mt19937 rng(1234);
        std::uniform_real_distribution<float> xy(-3.0f, 3.0f);
        std::uniform_real_distribution<float> zh(2.0f, 8.0f);
        for (int i = 0; i < 80; ++i) {
            dynamics::Body b;
            b.type = dynamics::BodyType::Dynamic;
            b.position = glm::vec3(xy(rng), xy(rng), zh(rng));
            b.mass = 1.0f;
            b.inv_mass = 1.0f;
            b.inv_inertia_local = dynamics::sphere_inverse_inertia(0.4f, 1.0f);
            world.add_body(b, collision::Shape::make_sphere(0.4f));
        }

        for (int i = 0; i < 180; ++i) world.step_fixed(util::k_default_fixed_dt); // 3 s: all landed and piling
        return world.world_state_hash();
    };

    const uint64_t serial_hash = build_and_run(nullptr);
    EXPECT_EQ(build_and_run(nullptr), serial_hash);

    coopa::job::JobEngine jobs(4);
    EXPECT_EQ(build_and_run(&jobs), serial_hash);
}

COOPA_TEST(debug_draw_emits_lines_and_respects_flags) {
    PhysicsWorld world;
    add_static_box(world, glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(5.0f, 5.0f, 0.5f));
    add_dynamic_sphere(world, glm::vec3(0.0f, 0.0f, 0.5f), 0.5f);

    for (int i = 0; i < 30; ++i) world.step_fixed(util::k_default_fixed_dt); // settle into contact

    debug::DebugDraw draw;
    world.debug_draw(draw);
    ASSERT_TRUE(!draw.lines.empty()); // colliders + BVH nodes always produce something

    debug::DebugDraw colliders_only;
    world.debug_draw(colliders_only, debug::DebugDrawFlags::Colliders);
    ASSERT_TRUE(!colliders_only.lines.empty());
    ASSERT_TRUE(colliders_only.lines.size() < draw.lines.size()); // strictly fewer without BVH/contacts
}

namespace {
/** @brief An axis-aligned cube hull of the given half-extent -- see narrowphase_test.cpp's
 *         make_cube_hull_() for the same construction; duplicated locally rather than shared
 *         across test binaries' translation units. */
geometry::ConvexHull make_cube_hull_for_world_test_(float half_extent) {
    glm::vec3 pts[8];
    int k = 0;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2) pts[k++] = glm::vec3(float(sx), float(sy), float(sz)) * half_extent;
    geometry::ConvexHull hull;
    bool ok = geometry::build_convex_hull(pts, 8, hull);
    (void)ok;
    return hull;
}
} // namespace

/**
 * The single cheapest integration check that catches a bad hull manifold, a bad normal, or a
 * bad inertia all at once (Stage 1's own risk list): a cube hull dropped on a static floor
 * must come to rest flat, its center at exactly half its own height above the floor -- not
 * sunk into it (a flipped normal) and not left floating (a wrong inertia stalling the solver).
 */
COOPA_TEST(dynamic_cube_hull_rests_on_the_floor_without_sinking) {
    PhysicsWorld world;
    add_static_ground(world);

    geometry::ConvexHull hull = make_cube_hull_for_world_test_(0.5f);
    dynamics::BodyId id = add_dynamic_hull(world, &hull, glm::vec3(0.0f, 0.0f, 2.0f));

    for (int i = 0; i < 300; ++i) world.step_fixed(util::k_default_fixed_dt);

    const dynamics::Body* b = world.get_body(id);
    EXPECT_TRUE(is_settled(*b));
    EXPECT_NEAR(b->position.z, 0.5f, 0.02f);
    EXPECT_GT(best_up_alignment(b->orientation), 0.999f);
}

/**
 * Three flush-stacked cube hulls must settle without drifting sideways or buzzing -- the
 * feature-id-stability test's (narrowphase_test.cpp) integration-level counterpart: an unstable
 * manifold between steps is exactly what would show up here as jitter that never sleeps.
 */
COOPA_TEST(three_cube_hulls_stacked_settle_without_jittering) {
    PhysicsWorld world;
    add_static_box(world, glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(5.0f, 5.0f, 0.5f));

    geometry::ConvexHull hull = make_cube_hull_for_world_test_(0.5f);
    const float half = 0.5f;
    std::vector<dynamics::BodyId> ids;
    for (int i = 0; i < 3; ++i) {
        glm::vec3 pos(0.0f, 0.0f, half * (2 * i + 1));
        ids.push_back(add_dynamic_hull(world, &hull, pos));
    }

    for (int i = 0; i < 1000; ++i) world.step_fixed(util::k_default_fixed_dt);

    for (auto id : ids) {
        const dynamics::Body* b = world.get_body(id);
        EXPECT_LT(std::abs(b->position.x), 0.05f);
        EXPECT_LT(std::abs(b->position.y), 0.05f);
        EXPECT_GT(best_up_alignment(b->orientation), 0.999f);
        EXPECT_TRUE(is_settled(*b));
    }
}

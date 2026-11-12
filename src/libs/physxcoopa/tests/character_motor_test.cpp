/**
 * @file character_motor_test.cpp
 * @brief character::move(): stepping up risers no taller than step_height, and the slope limit
 *        (walkable ramps are climbed, steep slopes block and slide). Controller-level behaviour
 *        (jumps, platforms) belongs to toyengine's CharacterController tests.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/world.h>
#include <physxcoopa/character/character_motor.h>
#include "support/physics_fixtures.h"

COOPA_TEST_SUITE("character_motor");

using namespace coopa::physx;
using namespace physxtest;

namespace {

/** @brief Runs `frames` motor moves of `step` per frame plus simple gravity, carrying grounded
 *         state like a controller would; returns the last result. */
character::MoveResult run_motor(const PhysicsWorld& world, glm::vec3& center, const glm::vec3& step, int frames,
                                       const character::MotorSettings& s) {
    character::MoveResult r;
    bool grounded = false;
    float vz = 0.0f;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < frames; ++i) {
        vz = grounded ? 0.0f : vz - 9.81f * dt;
        if (grounded) vz = -9.81f * dt;
        character::MoveOptions opt;
        opt.was_grounded = grounded;
        r = character::move(world, center, step + glm::vec3(0.0f, 0.0f, vz * dt), {}, s, opt);
        center = r.position;
        grounded = r.grounded;
        if (r.grounded) vz = 0.0f;
    }
    return r;
}

} // namespace

/**
 * @brief The motor walks up a riser at or under step_height and is stopped by a taller one,
 *        keeping its skin gap and staying grounded on each tread.
 */
COOPA_TEST(steps_up_low_risers_only) {
    PhysicsWorld world;
    add_static_box(world, glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(30.0f, 30.0f, 0.5f));
    add_static_box(world, glm::vec3(3.0f, 0.0f, 0.15f), glm::vec3(1.0f, 3.0f, 0.15f));   // 0.30 riser at x = 2
    add_static_box(world, glm::vec3(3.0f, 10.0f, 0.25f), glm::vec3(1.0f, 3.0f, 0.25f));  // 0.50 riser at x = 2
    character::MotorSettings s; // r 0.3, h 1.8, step 0.35, skin 0.02

    glm::vec3 low(0.0f, 0.0f, 0.92f);
    character::MoveResult r = run_motor(world, low, glm::vec3(0.05f, 0.0f, 0.0f), 50, s);
    ASSERT_TRUE(r.grounded);
    ASSERT_TRUE(low.x > 2.2f);
    ASSERT_NEAR(low.z, 0.3f + 0.9f + s.skin, 0.01f);

    glm::vec3 high(0.0f, 10.0f, 0.92f);
    r = run_motor(world, high, glm::vec3(0.05f, 0.0f, 0.0f), 60, s);
    ASSERT_TRUE(r.grounded);
    ASSERT_NEAR(high.x, 2.0f - s.radius - s.skin, 0.01f);
    ASSERT_NEAR(high.z, 0.9f + s.skin, 0.01f);
}

/**
 * @brief Slopes: a walkable ramp is walked up (grounded throughout); a slope steeper than the
 *        limit can't be walked up and a capsule left on it slides down it.
 */
COOPA_TEST(slope_limit_blocks_steep_slopes_and_slides_down_them) {
    character::MotorSettings s;
    const float dt = 1.0f / 60.0f;
    // 20 degree ramp rising toward +X, its top face through the origin.
    {
        PhysicsWorld world;
        const glm::quat rot = glm::angleAxis(glm::radians(-20.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        add_static_box(world, rot * glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(10.0f, 3.0f, 0.5f), rot);
        glm::vec3 c(-3.0f, 0.0f, -3.0f * std::tan(glm::radians(20.0f)) + 2.0f);
        character::MoveResult r = run_motor(world, c, glm::vec3(0.0f), 30, s); // settle
        ASSERT_TRUE(r.grounded);
        const float z0 = c.z;
        r = run_motor(world, c, glm::vec3(4.0f * dt, 0.0f, 0.0f), 60, s);
        ASSERT_TRUE(r.grounded);
        ASSERT_NEAR(c.x, 1.0f, 0.05f);
        ASSERT_NEAR(c.z - z0, 4.0f * std::tan(glm::radians(20.0f)), 0.05f);
    }
    // 60 degree slope: walking into it gains no height; standing on it slides down.
    {
        PhysicsWorld world;
        add_static_box(world, glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(30.0f, 30.0f, 0.5f));
        const glm::quat rot = glm::angleAxis(glm::radians(-60.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        add_static_box(world, glm::vec3(3.0f, 0.0f, 0.0f) + rot * glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(4.0f, 3.0f, 0.5f), rot);
        glm::vec3 c(0.0f, 0.0f, 0.92f);
        character::MoveResult r = run_motor(world, c, glm::vec3(4.0f * dt, 0.0f, 0.0f), 90, s);
        ASSERT_TRUE(c.z < 0.92f + 0.35f + 0.05f); // at most one step's worth onto the toe
        // Dropped onto the slope higher up: it slides down toward -X.
        glm::vec3 d(4.5f, 0.0f, 1.5f * std::tan(glm::radians(60.0f)) + 1.5f);
        r = character::move(world, d, glm::vec3(0.0f, 0.0f, -1.0f), {}, s, {});
        ASSERT_TRUE(!r.grounded);
        const glm::vec3 start = d;
        run_motor(world, d, glm::vec3(0.0f), 60, s);
        ASSERT_TRUE(d.x < start.x - 0.5f);
        ASSERT_TRUE(d.z < start.z - 0.5f);
    }
}

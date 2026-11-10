/**
 * @file gravity_test.cpp
 * @brief Gravity sources (`PhysicsWorld::GravitySource`): bodies fall toward a planet's centre from
 *        any side and come to rest on it; with no source the world steps exactly as before; two
 *        sources hand a body over to the stronger pull; the character motor walks round a sphere
 *        with up taken from the field.
 */
#include <coopa/testing/test.h>

#include <cmath>
#include <cstring>

#include <physxcoopa/character/character_motor.h>
#include <physxcoopa/world.h>

#include "support/physics_fixtures.h"

COOPA_TEST_SUITE("gravity");

using namespace coopa::physx;
using namespace physxtest;

namespace {

dynamics::BodyId add_ball(PhysicsWorld& world, const glm::vec3& at, float radius = 0.4f) {
    dynamics::Body b;
    b.type = dynamics::BodyType::Dynamic;
    b.position = at;
    b.inv_mass = 1.0f;
    return world.add_body(b, collision::Shape::make_sphere(radius));
}

dynamics::BodyId add_planet(PhysicsWorld& world, float radius) {
    dynamics::Body p;
    p.type = dynamics::BodyType::Static;
    p.position = glm::vec3(0.0f);
    world.add_body(p, collision::Shape::make_sphere(radius));
    PhysicsWorld::GravitySource g;
    g.centre = glm::vec3(0.0f);
    g.radius = radius;
    g.strength = 9.81f;
    g.range = radius * 4.0f;
    world.add_gravity_source(g);
    return {};
}

} // namespace

/** @brief No source: `gravity_at()` is `gravity()` everywhere and a world steps bit-identically. */
COOPA_TEST(no_source_is_the_uniform_world_exactly) {
    PhysicsWorld a, b;
    const glm::vec3 start(1.0f, 2.0f, 5.0f);
    const dynamics::BodyId ia = add_ball(a, start);
    const dynamics::BodyId ib = add_ball(b, start);
    // b gets a source and loses it again: back to the uniform path, untouched.
    b.remove_gravity_source(b.add_gravity_source(PhysicsWorld::GravitySource{}));
    ASSERT_TRUE(a.gravity_at(glm::vec3(3.0f, -7.0f, 2.0f)) == a.gravity());
    for (int i = 0; i < 90; ++i) {
        a.step_fixed(util::k_default_fixed_dt);
        b.step_fixed(util::k_default_fixed_dt);
    }
    const glm::vec3 pa = a.get_body(ia)->position, pb = b.get_body(ib)->position;
    ASSERT_TRUE(std::memcmp(&pa, &pb, sizeof(glm::vec3)) == 0);
}

/** @brief Balls dropped above six sides of a planet fall to its centre and rest on its surface. */
COOPA_TEST(bodies_fall_to_a_planet_from_every_side) {
    PhysicsWorld world;
    const float radius = 10.0f;
    add_planet(world, radius);
    const glm::vec3 dirs[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<dynamics::BodyId> balls;
    for (const glm::vec3& d : dirs) balls.push_back(add_ball(world, d * (radius + 3.0f)));
    for (int i = 0; i < 60 * 6; ++i) world.step_fixed(util::k_default_fixed_dt);
    for (std::size_t i = 0; i < balls.size(); ++i) {
        const glm::vec3 p = world.get_body(balls[i])->position;
        ASSERT_NEAR(glm::length(p), radius + 0.4f, 0.08f);                 // resting on the surface
        ASSERT_TRUE(glm::dot(glm::normalize(p), dirs[i]) > 0.99f);         // where it was dropped
    }
    // Up is away from the centre, wherever you stand.
    EXPECT_VEC_NEAR(world.up_at(glm::vec3(0.0f, -12.0f, 0.0f)), glm::vec3(0.0f, -1.0f, 0.0f), 1e-5f);
}

/** @brief Two planets: a body between them goes to the stronger pull; beyond range, the uniform. */
COOPA_TEST(the_strongest_source_pulls) {
    PhysicsWorld world;
    world.set_gravity(glm::vec3(0.0f));
    PhysicsWorld::GravitySource near_one{glm::vec3(0.0f), 9.81f, 5.0f, 40.0f};
    PhysicsWorld::GravitySource far_one{glm::vec3(30.0f, 0.0f, 0.0f), 9.81f, 5.0f, 40.0f};
    world.add_gravity_source(near_one);
    world.add_gravity_source(far_one);
    ASSERT_TRUE(world.gravity_at(glm::vec3(10.0f, 0.0f, 0.0f)).x < 0.0f);   // nearer the first
    ASSERT_TRUE(world.gravity_at(glm::vec3(22.0f, 0.0f, 0.0f)).x > 0.0f);   // nearer the second
    ASSERT_NEAR(glm::length(world.gravity_at(glm::vec3(0.0f, 10.0f, 0.0f))), 9.81f * 0.25f, 1e-4f);  // (R/r)^2
    ASSERT_TRUE(world.gravity_at(glm::vec3(0.0f, 0.0f, 100.0f)) == glm::vec3(0.0f));   // out of reach
}

/** @brief The character motor, given the planet's up, walks round a sphere and stays on it. */
COOPA_TEST(the_motor_walks_round_a_planet) {
    PhysicsWorld world;
    const float radius = 8.0f;
    add_planet(world, radius);
    character::MotorSettings s;
    s.radius = 0.3f;
    s.height = 1.6f;
    glm::vec3 pos(0.0f, 0.0f, radius + 0.8f + s.skin);
    character::MoveOptions options;
    options.was_grounded = true;
    query::QueryFilter filter;
    float travelled = 0.0f;
    for (int i = 0; i < 400; ++i) {
        options.up = world.up_at(pos);
        // Walk "east" along the ground under a little gravity, as a controller would.
        glm::vec3 east = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), options.up);
        if (glm::length(east) < 1e-4f) east = glm::vec3(1.0f, 0.0f, 0.0f);
        east = glm::normalize(east);
        const glm::vec3 step = east * 0.05f - options.up * 0.02f;
        const character::MoveResult r = character::move(world, pos, step, filter, s, options);
        travelled += glm::length(r.position - pos);
        pos = r.position;
        options.was_grounded = r.grounded;
        ASSERT_NEAR(glm::length(pos), radius + 0.8f + s.skin, 0.06f);   // on the ground, never through it
    }
    ASSERT_TRUE(options.was_grounded);
    ASSERT_TRUE(travelled > 15.0f);   // a good way round
}

/**
 * @file nav_flow_test.cpp
 * @brief Flow fields: Eikonal directions/distances up a staircase, multiple goals and max_distance,
 *        Auto staying exact in small worlds, and the hierarchical mode integrating only where its
 *        followers are while agreeing with the exact field there.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/nav/nav_baker.h>
#include <physxcoopa/nav/flow_field.h>
#include <coopa/job/engine.h>
#include <random>
#include <span>
#include "support/nav_fixtures.h"

COOPA_TEST_SUITE("nav_flow");

using namespace coopa::physx;
using namespace physxtest;

COOPA_TEST(flows_up_the_stairs_toward_goal) {
    PhysicsWorld world;
    navtest::stairs_world(world);
    coopa::job::JobEngine jobs(4);
    auto baker = navtest::bake(world, &jobs);
    auto mesh = baker->mesh();
    glm::vec3 goal(5.0f, 0.0f, 2.0f);
    auto flow = nav::FlowField::build(mesh, std::span<const glm::vec3>(&goal, 1), {}, &jobs);
    ASSERT_TRUE(flow->valid());

    // In front of the stairs it points up them (+X); on the far side of the platform it points
    // back around toward the staircase; on the platform it points at the goal.
    nav::FlowSample s = flow->sample({-6.0f, 0.0f, 0.0f});
    ASSERT_TRUE(s.valid && s.direction.x > 0.95f);
    s = flow->sample({0.0f, 0.0f, 1.2f});
    ASSERT_TRUE(s.valid && s.direction.x > 0.9f);
    s = flow->sample({5.0f, 7.0f, 0.0f});                 // ground, north of the platform
    ASSERT_TRUE(s.valid && s.direction.x < 0.0f);       // heads west, back toward the stairs
    s = flow->sample({7.0f, 3.0f, 2.0f});
    ASSERT_TRUE(s.valid);
    glm::vec2 want = glm::normalize(glm::vec2(goal) - glm::vec2(7.0f, 3.0f));
    ASSERT_TRUE(glm::dot(glm::vec2(s.direction), want) > 0.95f); // straight at it (Eikonal, not 8-way)
    // Distances grow away from the goal and are roughly metric.
    ASSERT_TRUE(flow->sample({-6.0f, 0.0f, 0.0f}).distance > flow->sample({0.0f, 0.0f, 1.2f}).distance);
    // Without the wall penalty (on by default, it charges extra near edges) distances are metric:
    // FMM's isotropic error on open ground is a few percent.
    nav::FlowFieldSettings metric;
    metric.filter.wall_penalty = 0.0f;
    auto plain = nav::FlowField::build(mesh, std::span<const glm::vec3>(&goal, 1), metric, &jobs);
    ASSERT_NEAR(plain->sample({7.0f, 3.0f, 2.0f}).distance, glm::distance(glm::vec2(7.0f, 3.0f), glm::vec2(goal)), 0.25f);
    ASSERT_TRUE(flow->sample({7.0f, 3.0f, 2.0f}).distance > plain->sample({7.0f, 3.0f, 2.0f}).distance); // penalty applied
}

COOPA_TEST(multiple_goals_and_max_distance) {
    PhysicsWorld world;
    navtest::static_box(world, {0.0f, 0.0f, -0.5f}, {20.0f, 5.0f, 0.5f});
    auto baker = navtest::bake(world);
    auto mesh = baker->mesh();
    glm::vec3 goals[2] = {{-15.0f, 0.0f, 0.0f}, {15.0f, 0.0f, 0.0f}};
    auto flow = nav::FlowField::build(mesh, goals);
    ASSERT_TRUE(flow->sample({-5.0f, 0.0f, 0.0f}).direction.x < -0.9f); // nearer the west goal
    ASSERT_TRUE(flow->sample({5.0f, 0.0f, 0.0f}).direction.x > 0.9f);
    nav::FlowFieldSettings fs;
    fs.max_distance = 6.0f;
    auto bounded = nav::FlowField::build(mesh, std::span<const glm::vec3>(goals, 1), fs);
    ASSERT_TRUE(bounded->sample({-12.0f, 0.0f, 0.0f}).valid);
    ASSERT_TRUE(!bounded->sample({0.0f, 0.0f, 0.0f}).valid);
    ASSERT_TRUE(bounded->reached_count() < flow->reached_count() / 3);
}

COOPA_TEST(auto_mode_stays_exact_in_small_worlds) {
    PhysicsWorld world;
    navtest::stairs_world(world);
    auto baker = navtest::bake(world);
    auto mesh = baker->mesh();
    glm::vec3 goal(5.0f, 0.0f, 2.0f);
    nav::FlowFieldSettings autos, exact;
    exact.mode = nav::FlowFieldMode::Exact;
    auto a = nav::FlowField::build(mesh, std::span<const glm::vec3>(&goal, 1), autos);
    auto e = nav::FlowField::build(mesh, std::span<const glm::vec3>(&goal, 1), exact);
    ASSERT_TRUE(!a->hierarchical());
    ASSERT_TRUE(a->reached_count() == e->reached_count());
    for (glm::vec3 q : {glm::vec3(-6, 6, 0), glm::vec3(0, 0, 1.2f), glm::vec3(7, 3, 2)}) {
        ASSERT_NEAR(a->sample(q).distance, e->sample(q).distance, 1e-4f);
    }
}

COOPA_TEST(hierarchical_integrates_only_where_followers_are) {
    PhysicsWorld world;
    auto baker = navtest::open_world(world, 11);
    auto mesh = baker->mesh();
    glm::vec3 goal(0.0f, 0.0f, 0.0f);
    nav::FlowFieldSettings hs, es;
    es.mode = nav::FlowFieldMode::Exact;
    auto e = nav::FlowField::build(mesh, std::span<const glm::vec3>(&goal, 1), es);
    std::vector<glm::vec3> pack;
    for (int k = 0; k < 30; ++k) {
        glm::vec3 q(85.0f + static_cast<float>(k % 6) * 0.9f, 60.0f + static_cast<float>(k / 6) * 0.9f, 0.0f);
        if (e->sample(q).valid) pack.push_back(q); // skip pockets the rocks wall off entirely
    }
    ASSERT_TRUE(pack.size() >= 15);
    hs.required_points = pack;
    auto h = nav::FlowField::build(mesh, std::span<const glm::vec3>(&goal, 1), hs);
    ASSERT_TRUE(h->hierarchical());
    ASSERT_TRUE(h->active_tile_count() * 10 < mesh->params.tile_count()); // a corridor's worth, not the world
    ASSERT_TRUE(h->reached_count() * 8 < e->reached_count());
    // Where the pack is, directions are exact-quality.
    float agree = 0.0f;
    for (const auto& q : pack) {
        nav::FlowSample a = h->sample(q), b = e->sample(q);
        ASSERT_TRUE(a.valid && !a.coarse && b.valid);
        agree += glm::dot(a.direction, b.direction);
    }
    ASSERT_TRUE(agree / static_cast<float>(pack.size()) > 0.85f);
    // Elsewhere the coarse layer still answers, broadly the right way.
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(-110.0f, 110.0f);
    int coarse = 0;
    float coarse_agree = 0.0f;
    for (int i = 0; i < 400; ++i) {
        glm::vec3 q(u(rng), u(rng), 0.0f);
        nav::FlowSample a = h->sample(q), b = e->sample(q);
        if (!b.valid || !a.valid || !a.coarse || b.distance < 20.0f) continue;
        ++coarse;
        coarse_agree += glm::dot(a.direction, b.direction);
    }
    ASSERT_TRUE(coarse > 50);
    ASSERT_TRUE(coarse_agree / static_cast<float>(coarse) > 0.6f);
}

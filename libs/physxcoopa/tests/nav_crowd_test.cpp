/**
 * @file nav_crowd_test.cpp
 * @brief Crowd simulation: a job-parallel crowd streaming up a staircase on one flow field, a path
 *        agent traversing an off-mesh link, and a pack routed through a wall gap by a hierarchical
 *        field rebuilt the way NavSystem rebuilds it.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/nav/nav_baker.h>
#include <physxcoopa/nav/path_query.h>
#include <physxcoopa/nav/flow_field.h>
#include <physxcoopa/nav/crowd.h>
#include <coopa/job/engine.h>
#include <span>
#include "support/nav_fixtures.h"

COOPA_TEST_SUITE("nav_crowd");

using namespace coopa::physx;
using namespace physxtest;

/**
 * 40 agents (all the 0.4 m-radius bodies the ~37 m^2 eroded platform can hold, with room to
 * spare) stream single file up the 1.2 m-wide (after erosion) staircase on one shared flow
 * field. Exercises the crowd as a whole: no agent may stall against a wall, fall off the mesh,
 * or overlap its neighbours much, and arrival contagion must settle the group around the goal
 * instead of letting it swirl -- while not spreading back down the queue and stopping agents
 * that still have room.
 */
COOPA_TEST(parallel_flow_crowd_reaches_goal_and_keeps_apart) {
    PhysicsWorld world;
    navtest::stairs_world(world);
    coopa::job::JobEngine jobs(4);
    auto baker = navtest::bake(world, &jobs);
    auto mesh = baker->mesh();
    glm::vec3 goal(5.0f, 0.0f, 2.0f);
    auto flow = nav::FlowField::build(mesh, std::span<const glm::vec3>(&goal, 1), {}, &jobs);

    nav::Crowd crowd;
    nav::CrowdAgentParams params;
    params.filter = mesh->default_filter();
    const int n = 40;
    for (int i = 0; i < n; ++i) {
        auto id = crowd.add_agent({-8.0f + static_cast<float>(i % 8) * 0.9f, -8.0f + static_cast<float>(i / 8) * 0.9f, 0.0f}, params);
        crowd.set_flow(id, flow);
    }
    float min_gap = 1e9f;
    for (int step = 0; step < 60 * 40; ++step) {
        crowd.update(1.0f / 60.0f, *mesh, &jobs, 16);
        if (step % 30 == 0) {
            for (int i = 0; i < n; ++i) {
                for (int j = i + 1; j < n; ++j) {
                    glm::vec3 d = crowd.agent(static_cast<uint32_t>(i)).position - crowd.agent(static_cast<uint32_t>(j)).position;
                    if (std::fabs(d.z) < 0.1f) min_gap = std::min(min_gap, glm::length(glm::vec2(d)));
                }
            }
        }
    }
    int on_platform = 0, arrived = 0;
    float speed = 0.0f;
    for (int i = 0; i < n; ++i) {
        const auto& a = crowd.agent(static_cast<uint32_t>(i));
        ASSERT_TRUE(a.ref != nav::k_invalid_span);       // nobody left the mesh
        on_platform += a.position.z > 1.9f;
        arrived += a.arrived;
        speed += glm::length(glm::vec2(a.velocity));
    }
    ASSERT_TRUE(on_platform == n);
    ASSERT_TRUE(arrived >= n / 2);                        // settled around the goal...
    ASSERT_TRUE(speed / static_cast<float>(n) < 0.5f);    // ...not swirling at full speed
    ASSERT_TRUE(min_gap > 0.5f);                          // radii 0.4 + 0.4: overlap stays small
}

COOPA_TEST(path_agent_follows_link_and_arrives) {
    PhysicsWorld world;
    navtest::static_box(world, {-5.0f, 0.0f, -0.5f}, {3.0f, 3.0f, 0.5f});
    navtest::static_box(world, {5.0f, 0.0f, -1.5f}, {3.0f, 3.0f, 0.5f}); // 1 m lower
    nav::NavBaker baker;
    std::vector<nav::SourceShape> sources;
    nav::gather_sources(world, {}, sources);
    baker.set_sources(sources);
    nav::OffMeshLink drop;
    drop.start = {-2.5f, 0.0f, 0.0f};
    drop.end = {2.5f, 0.0f, -1.0f};
    drop.bidirectional = false;
    baker.set_links({drop});
    baker.build_all(nullptr);
    auto mesh = baker.mesh();

    nav::NavPath path;
    ASSERT_TRUE(nav::find_path(*mesh, {-6.0f, 0.0f, 0.0f}, {6.0f, 1.0f, -1.0f}, mesh->default_filter(), path) == nav::PathStatus::Success);
    nav::Crowd crowd;
    nav::CrowdAgentParams params;
    params.filter = mesh->default_filter();
    auto id = crowd.add_agent({-6.0f, 0.0f, 0.0f}, params);
    crowd.set_path(id, path);
    bool saw_link = false;
    for (int i = 0; i < 60 * 10 && !crowd.agent(id).arrived; ++i) {
        crowd.update(1.0f / 60.0f, *mesh, nullptr);
        saw_link |= crowd.agent(id).on_link;
    }
    ASSERT_TRUE(saw_link);
    ASSERT_TRUE(crowd.agent(id).arrived);
    ASSERT_VEC_NEAR(crowd.agent(id).position, glm::vec3(6.0f, 1.0f, -1.0f), 0.2f);
}

/**
 * The goal is south of a 240 m wall whose only gap is at its east end; a pack starts far to the
 * north-west. Driven the way NavSystem drives it -- rebuilt every half second around the pack's
 * current positions -- the hierarchical field must route the pack to the gap and through, with
 * each rebuild integrating only a small fraction of the world.
 */
COOPA_TEST(hierarchical_flow_routes_crowd_through_the_gap) {
    PhysicsWorld world;
    auto baker = navtest::open_world(world, 23, /*long_wall=*/true);
    auto mesh = baker->mesh();
    glm::vec3 goal(0.0f, -80.0f, 0.0f);
    coopa::job::JobEngine jobs(4);
    nav::Crowd crowd;
    nav::CrowdAgentParams params;
    params.filter = mesh->default_filter();
    params.max_speed = 6.0f;
    for (int k = 0; k < 20; ++k) {
        glm::vec3 q(-60.0f + static_cast<float>(k % 5) * 0.9f, 10.0f + static_cast<float>(k / 5) * 0.9f, 0.0f);
        if (mesh->locate(q) != nav::k_invalid_span) crowd.add_agent(q, params);
    }
    std::size_t max_active = 0;
    int hierarchical_builds = 0;
    auto arrived_count = [&]() {
        int arrived = 0;
        for (std::size_t i = 0; i < crowd.capacity(); ++i) {
            arrived += glm::distance(glm::vec2(crowd.agent(static_cast<uint32_t>(i)).position), glm::vec2(goal)) < 8.0f;
        }
        return arrived;
    };
    // Up to 75 s of simulation, stopping as soon as the pack is home (all but one agent).
    int arrived = 0;
    for (int step = 0; step < 60 * 75; ++step) {
        if (step % 30 == 0) {
            arrived = arrived_count();
            if (arrived >= static_cast<int>(crowd.capacity()) - 1) break;
            nav::FlowFieldSettings fs;
            for (std::size_t i = 0; i < crowd.capacity(); ++i) fs.required_points.push_back(crowd.agent(static_cast<uint32_t>(i)).position);
            auto f = nav::FlowField::build(mesh, std::span<const glm::vec3>(&goal, 1), fs, &jobs);
            // Far out it is hierarchical; once the pack is near the goal the routed area is small
            // and Auto rightly switches to an exact field.
            if (f->hierarchical()) {
                ++hierarchical_builds;
                max_active = std::max(max_active, f->active_tile_count());
            }
            for (std::size_t i = 0; i < crowd.capacity(); ++i) crowd.set_flow(static_cast<uint32_t>(i), f);
        }
        crowd.update(1.0f / 60.0f, *mesh, &jobs, 8);
    }
    arrived = arrived_count();
    ASSERT_TRUE(arrived >= static_cast<int>(crowd.capacity()) - 1);
    ASSERT_TRUE(hierarchical_builds > 20);
    ASSERT_TRUE(max_active * 6 < mesh->params.tile_count());
}

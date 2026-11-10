/**
 * @file nav_path_test.cpp
 * @brief Navmesh queries: A* across floors, partial/no-path results, corridor-confined vs unconfined
 *        search, nav raycasts and move_along_surface, off-mesh links, and area costs/masks.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/nav/nav_baker.h>
#include <physxcoopa/nav/path_query.h>
#include "support/nav_fixtures.h"

COOPA_TEST_SUITE("nav_path");

using namespace coopa::physx;
using namespace physxtest;

COOPA_TEST(astar_climbs_stairs_to_upper_floor) {
    PhysicsWorld world;
    navtest::stairs_world(world);
    auto baker = navtest::bake(world);
    auto mesh = baker->mesh();
    nav::NavPath path;
    nav::QueryFilter f = mesh->default_filter();
    f.heuristic_weight = 1.0f;
    ASSERT_TRUE(nav::find_path(*mesh, {-6.0f, 6.0f, 0.0f}, {5.0f, 0.0f, 2.0f}, f, path) == nav::PathStatus::Success);
    ASSERT_NEAR(path.points.front().z, 0.0f, 1e-3f);
    ASSERT_NEAR(path.points.back().z, 2.0f, 1e-3f);
    ASSERT_VEC_NEAR(path.points.back(), glm::vec3(5.0f, 0.0f, 2.0f), 1e-3f);
    // The only way up is the 2 m wide staircase: the path must cross it.
    bool on_stairs = false;
    for (const glm::vec3& p : path.points) on_stairs |= p.x > -3.2f && p.x < 2.2f && std::fabs(p.y) < 1.0f && p.z > 0.05f && p.z < 1.95f;
    for (std::size_t i = 1; i < path.points.size(); ++i) {
        // No segment may jump floors outside the staircase footprint.
        const glm::vec3& a = path.points[i - 1];
        const glm::vec3& b = path.points[i];
        if (std::fabs(a.z - b.z) > 0.5f) on_stairs |= std::fabs(a.y) < 1.0f && std::fabs(b.y) < 1.0f;
    }
    ASSERT_TRUE(on_stairs);
    ASSERT_TRUE(path.points.size() <= 6); // smoothed to a handful of corners
    // Smoothed length is close to the true shortest route (~14.3 m around and up).
    ASSERT_TRUE(path.length > 13.0f && path.length < 15.5f);
}

COOPA_TEST(unreachable_goal_is_partial_or_none) {
    PhysicsWorld world;
    navtest::static_box(world, {0.0f, 0.0f, -0.5f}, {10.0f, 10.0f, 0.5f});
    // A closed 4 x 4 m pen.
    navtest::static_box(world, {5.0f, 3.0f, 1.0f}, {2.25f, 0.25f, 1.0f});
    navtest::static_box(world, {5.0f, 7.0f, 1.0f}, {2.25f, 0.25f, 1.0f});
    navtest::static_box(world, {3.0f, 5.0f, 1.0f}, {0.25f, 2.25f, 1.0f});
    navtest::static_box(world, {7.0f, 5.0f, 1.0f}, {0.25f, 2.25f, 1.0f});
    auto baker = navtest::bake(world);
    auto mesh = baker->mesh();
    nav::QueryFilter f = mesh->default_filter();
    nav::NavPath path;
    ASSERT_TRUE(nav::find_path(*mesh, {-6.0f, -6.0f, 0.0f}, {5.0f, 5.0f, 0.0f}, f, path) == nav::PathStatus::Partial);
    // Ends as close to the pen centre as the outside allows: just outside a wall.
    glm::vec3 end = path.points.back();
    ASSERT_TRUE(glm::distance(glm::vec2(end), glm::vec2(5.0f, 5.0f)) < 3.0f);
    ASSERT_TRUE(path.nodes_expanded < 20000); // planned to the nearest point, not a flood
    f.allow_partial = false;
    ASSERT_TRUE(nav::find_path(*mesh, {-6.0f, -6.0f, 0.0f}, {5.0f, 5.0f, 0.0f}, f, path) == nav::PathStatus::NoPath);
}

COOPA_TEST(corridor_search_matches_unconfined_on_open_ground) {
    PhysicsWorld world;
    navtest::static_box(world, {0.0f, 0.0f, -0.5f}, {40.0f, 40.0f, 0.5f});
    for (int i = 0; i < 6; ++i) navtest::static_box(world, {-25.0f + 10.0f * static_cast<float>(i), (i % 2 ? 8.0f : -8.0f), 1.0f}, {1.0f, 25.0f, 1.0f});
    auto baker = navtest::bake(world);
    auto mesh = baker->mesh();
    nav::QueryFilter f = mesh->default_filter();
    f.heuristic_weight = 1.0f;
    nav::PathQueryOptions with, without;
    without.use_corridor = false;
    nav::NavPath a, b;
    ASSERT_TRUE(nav::find_path(*mesh, {-35.0f, 0.0f, 0.0f}, {35.0f, 0.0f, 0.0f}, f, a, with) == nav::PathStatus::Success);
    ASSERT_TRUE(nav::find_path(*mesh, {-35.0f, 0.0f, 0.0f}, {35.0f, 0.0f, 0.0f}, f, b, without) == nav::PathStatus::Success);
    ASSERT_TRUE(a.length < b.length * 1.06f);      // corridor costs at most a few percent
    ASSERT_TRUE(a.nodes_expanded <= b.nodes_expanded);
}

COOPA_TEST(raycast_and_move_along_surface_slide_on_walls) {
    PhysicsWorld world;
    navtest::static_box(world, {0.0f, 0.0f, -0.5f}, {5.0f, 5.0f, 0.5f});
    navtest::static_box(world, {0.0f, 0.0f, 1.0f}, {0.25f, 3.0f, 1.0f});
    auto baker = navtest::bake(world);
    auto mesh = baker->mesh();
    nav::QueryFilter f = mesh->default_filter();
    nav::SpanRef s = mesh->locate({-2.0f, 0.0f, 0.0f});
    nav::NavRaycastHit hit;
    ASSERT_TRUE(!mesh->raycast(s, {-2.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, f, hit));
    ASSERT_TRUE(hit.normal.x < -0.9f);                      // hit the wall's -X face
    ASSERT_TRUE(hit.position.x < -0.25f - mesh->params.agent_radius + mesh->params.cs);
    ASSERT_TRUE(mesh->raycast(s, {-2.0f, 0.0f, 0.0f}, {-2.0f, 2.0f, 0.0f}, f, hit)); // along the wall: clear

    // Pushing diagonally into the wall slides along it in +Y.
    nav::NavMoveResult mv = mesh->move_along_surface(s, {-2.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, f);
    ASSERT_TRUE(mv.blocked);
    ASSERT_TRUE(mv.position.x < -0.5f);
    ASSERT_TRUE(mv.position.y > 0.8f);
}

COOPA_TEST(off_mesh_link_bridges_a_gap_one_way) {
    PhysicsWorld world;
    navtest::static_box(world, {-5.0f, 0.0f, -0.5f}, {3.0f, 3.0f, 0.5f});  // x in [-8, -2]
    navtest::static_box(world, {5.0f, 0.0f, -0.5f}, {3.0f, 3.0f, 0.5f});   // x in [2, 8]
    nav::NavBaker baker;
    std::vector<nav::SourceShape> sources;
    nav::gather_sources(world, {}, sources);
    baker.set_sources(sources);
    nav::OffMeshLink link;
    link.start = {-2.5f, 0.0f, 0.0f};
    link.end = {2.5f, 0.0f, 0.0f};
    link.bidirectional = false;
    baker.set_links({link});
    baker.build_all(nullptr);
    auto mesh = baker.mesh();
    ASSERT_TRUE(mesh->links.size() == 1 && mesh->links[0].start_ref != nav::k_invalid_span && mesh->links[0].end_ref != nav::k_invalid_span);

    nav::QueryFilter f = mesh->default_filter();
    nav::NavPath path;
    ASSERT_TRUE(nav::find_path(*mesh, {-6.0f, 1.0f, 0.0f}, {6.0f, -1.0f, 0.0f}, f, path) == nav::PathStatus::Success);
    bool used = false;
    for (std::size_t i = 0; i + 1 < path.points.size(); ++i) {
        if (path.flags[i] & nav::k_path_point_link_start) {
            used = true;
            ASSERT_TRUE(path.flags[i + 1] & nav::k_path_point_link_end);
            ASSERT_TRUE(path.points[i].x < -1.5f && path.points[i + 1].x > 1.5f);
        }
    }
    ASSERT_TRUE(used);
    // One-way: the way back has no route.
    f.allow_partial = false;
    ASSERT_TRUE(nav::find_path(*mesh, {6.0f, 0.0f, 0.0f}, {-6.0f, 0.0f, 0.0f}, f, path) == nav::PathStatus::NoPath);
}

COOPA_TEST(area_costs_steer_paths_and_masks_exclude_areas) {
    PhysicsWorld world;
    navtest::static_box(world, {0.0f, 0.0f, -0.5f}, {10.0f, 10.0f, 0.5f});
    nav::NavBuildSettings settings;
    settings.areas.names[2] = "Swamp";
    settings.areas.costs[2] = 10.0f;
    nav::NavBaker baker(settings);
    std::vector<nav::SourceShape> sources;
    nav::gather_sources(world, {}, sources);
    baker.set_sources(sources);
    nav::SourceVolume swamp;
    swamp.box.center = {0.0f, 0.0f, 0.0f};
    swamp.box.half_extents = {2.0f, 6.0f, 1.0f};
    swamp.area = 2;
    swamp.fingerprint = nav::fingerprint(swamp);
    baker.set_volumes({swamp});
    baker.build_all(nullptr);
    auto mesh = baker.mesh();
    ASSERT_TRUE(mesh->span(mesh->locate({0.0f, 0.0f, 0.0f}))->area == 2);

    auto max_abs_y = [](const nav::NavPath& p) {
        float m = 0.0f;
        for (const auto& q : p.points) m = std::max(m, std::fabs(q.y));
        return m;
    };
    nav::NavPath path;
    nav::QueryFilter f = mesh->default_filter();
    ASSERT_TRUE(nav::find_path(*mesh, {-6.0f, 0.0f, 0.0f}, {6.0f, 0.0f, 0.0f}, f, path) == nav::PathStatus::Success);
    ASSERT_TRUE(max_abs_y(path) > 5.9f);            // walks around the swamp
    f.area_cost[2] = 1.0f;
    ASSERT_TRUE(nav::find_path(*mesh, {-6.0f, 0.0f, 0.0f}, {6.0f, 0.0f, 0.0f}, f, path) == nav::PathStatus::Success);
    ASSERT_TRUE(max_abs_y(path) < 0.5f);             // straight through when it's cheap
    ASSERT_TRUE(path.points.size() == 2);
    f.area_mask &= ~(1ull << 2);
    f.area_cost[2] = 1.0f;
    ASSERT_TRUE(nav::find_path(*mesh, {-6.0f, 0.0f, 0.0f}, {6.0f, 0.0f, 0.0f}, f, path) == nav::PathStatus::Success);
    ASSERT_TRUE(max_abs_y(path) > 5.9f);            // excluded outright
}

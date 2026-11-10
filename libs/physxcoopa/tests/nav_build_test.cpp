/**
 * @file nav_build_test.cpp
 * @brief Navmesh baking: exact floor heights, agent-radius erosion, incremental tile rebuilds that
 *        share untouched tiles, serial == job-parallel builds, portal symmetry across tile borders,
 *        and nav settings YAML parsing.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/nav/nav_baker.h>
#include <physxcoopa/nav/path_query.h>
#include <physxcoopa/nav/nav_settings.h>
#include <coopa/job/engine.h>
#include <algorithm>
#include <cstring>
#include <sstream>
#include "support/nav_fixtures.h"

COOPA_TEST_SUITE("nav_build");

using namespace coopa::physx;
using namespace physxtest;

COOPA_TEST(heightfield_floor_heights_are_exact) {
    PhysicsWorld world;
    navtest::static_box(world, {0.0f, 0.0f, -0.5f}, {5.0f, 5.0f, 0.5f});
    navtest::static_box(world, {0.0f, 0.0f, 0.65f}, {1.5f, 1.5f, 0.65f}); // top at 1.3
    auto baker = navtest::bake(world);
    auto mesh = baker->mesh();
    nav::SpanRef ground = mesh->find_nearest({-3.0f, -3.0f, 0.0f}, glm::vec3(0.5f));
    nav::SpanRef top = mesh->find_nearest({0.0f, 0.0f, 1.3f}, glm::vec3(0.5f));
    ASSERT_TRUE(ground != nav::k_invalid_span && top != nav::k_invalid_span);
    ASSERT_NEAR(mesh->position_of(ground).z, 0.0f, 1e-4f);
    ASSERT_NEAR(mesh->position_of(top).z, 1.3f, 1e-4f);
    // 1.3 m is far above max_climb: the block's top is its own island.
    ASSERT_TRUE(mesh->component_of(ground) != mesh->component_of(top));
}

COOPA_TEST(erosion_keeps_agent_radius_from_walls) {
    PhysicsWorld world;
    navtest::static_box(world, {0.0f, 0.0f, -0.5f}, {5.0f, 5.0f, 0.5f});
    navtest::static_box(world, {0.0f, 0.0f, 1.0f}, {0.25f, 3.0f, 1.0f}); // thin wall along Y
    auto baker = navtest::bake(world);
    auto mesh = baker->mesh();
    const float r = mesh->params.agent_radius;
    // Nothing walkable within the agent radius of the wall face (x = +-0.25) or the ground's edge.
    for (float x : {0.3f, 0.5f, -0.5f}) {
        nav::SpanRef s = mesh->locate({x, 0.0f, 0.0f});
        if (s == nav::k_invalid_span) continue;
        ASSERT_TRUE(std::fabs(mesh->position_of(s).x) - 0.25f >= r - mesh->params.cs);
    }
    ASSERT_TRUE(mesh->locate({4.95f, 4.95f, 0.0f}) == nav::k_invalid_span ||
                mesh->position_of(mesh->locate({4.95f, 4.95f, 0.0f})).x < 5.0f - r + mesh->params.cs);
    // A 0.5 m wall is narrower than an agent: nobody stands on top of it.
    ASSERT_TRUE(mesh->find_nearest({0.0f, 0.0f, 2.0f}, glm::vec3(0.1f, 0.1f, 0.3f)) == nav::k_invalid_span);
    // The wall ends at |y| = 3 and the ground at 5: the two sides connect around it.
    ASSERT_TRUE(mesh->component_of(mesh->locate({-2.0f, 0.0f, 0.0f})) == mesh->component_of(mesh->locate({2.0f, 0.0f, 0.0f})));
}

COOPA_TEST(incremental_rebuild_only_touches_dirty_tiles) {
    PhysicsWorld world;
    navtest::static_box(world, {0.0f, 0.0f, -0.5f}, {20.0f, 20.0f, 0.5f});
    coopa::job::JobEngine jobs(4);
    nav::NavBuildSettings settings;
    settings.rebuild_delay = 0.0f;
    auto baker = navtest::bake(world, &jobs, settings);
    auto before = baker->mesh();
    nav::QueryFilter f = before->default_filter();
    nav::NavPath path;
    ASSERT_TRUE(nav::find_path(*before, {-15.0f, 0.0f, 0.0f}, {15.0f, 0.0f, 0.0f}, f, path) == nav::PathStatus::Success);
    ASSERT_TRUE(path.points.size() == 2);

    // A wall appears across the middle, with a gap at the north end.
    navtest::static_box(world, {0.0f, -3.0f, 1.0f}, {0.5f, 17.0f, 1.0f});
    std::vector<nav::SourceShape> sources;
    nav::gather_sources(world, {}, sources);
    baker->set_sources(sources);
    ASSERT_TRUE(baker->busy());
    baker->flush(&jobs);
    auto after = baker->mesh();
    ASSERT_TRUE(after->version > before->version);

    // Tiles far from the wall are shared with the old mesh, not rebuilt.
    std::size_t shared = 0, total = 0;
    for (std::size_t i = 0; i < after->tiles.size(); ++i) {
        if (!after->tiles[i]) continue;
        ++total;
        shared += after->tiles[i] == before->tiles[i];
    }
    ASSERT_TRUE(shared > 0 && shared < total);

    ASSERT_TRUE(nav::find_path(*after, {-15.0f, 0.0f, 0.0f}, {15.0f, 0.0f, 0.0f}, f, path) == nav::PathStatus::Success);
    float max_y = 0.0f;
    for (const auto& p : path.points) max_y = std::max(max_y, p.y);
    ASSERT_TRUE(max_y > 13.5f); // around the north end
    // The old snapshot still answers queries exactly as before.
    ASSERT_TRUE(nav::find_path(*before, {-15.0f, 0.0f, 0.0f}, {15.0f, 0.0f, 0.0f}, f, path) == nav::PathStatus::Success);
    ASSERT_TRUE(path.points.size() == 2);
}

COOPA_TEST(parallel_and_serial_builds_are_identical) {
    PhysicsWorld world;
    navtest::stairs_world(world);
    navtest::static_box(world, {-6.0f, -6.0f, 0.3f}, {1.0f, 1.0f, 0.3f});
    coopa::job::JobEngine jobs(4);
    auto a = navtest::bake(world, nullptr);
    auto b = navtest::bake(world, &jobs);
    auto ma = a->mesh(), mb = b->mesh();
    ASSERT_TRUE(ma->tiles.size() == mb->tiles.size());
    for (std::size_t i = 0; i < ma->tiles.size(); ++i) {
        ASSERT_TRUE(!ma->tiles[i] == !mb->tiles[i]);
        if (!ma->tiles[i]) continue;
        const auto& sa = ma->tiles[i]->spans;
        const auto& sb = mb->tiles[i]->spans;
        ASSERT_TRUE(sa.size() == sb.size());
        for (std::size_t s = 0; s < sa.size(); ++s) {
            ASSERT_TRUE(sa[s].floor == sb[s].floor && sa[s].ceiling == sb[s].ceiling && sa[s].region == sb[s].region);
            ASSERT_TRUE(std::memcmp(sa[s].link, sb[s].link, 4) == 0);
        }
    }
}

COOPA_TEST(portals_mirror_across_tile_borders) {
    PhysicsWorld world;
    navtest::static_box(world, {0.0f, 0.0f, -0.5f}, {12.0f, 12.0f, 0.5f});
    // A wall along x = 0 with one 3 m gap, so the border it crosses splits into two portals.
    navtest::static_box(world, {0.0f, -7.0f, 1.0f}, {0.25f, 5.0f, 1.0f});
    navtest::static_box(world, {0.0f, 7.0f, 1.0f}, {0.25f, 3.5f, 1.0f});
    auto baker = navtest::bake(world);
    auto mesh = baker->mesh();
    std::size_t portals = 0;
    for (uint32_t t = 0; t < mesh->tiles.size(); ++t) {
        const nav::NavTile* tile = mesh->tiles[t].get();
        if (!tile) continue;
        for (uint16_t p = 0; p < tile->portals.size(); ++p) {
            ++portals;
            uint16_t q = mesh->mirror_portal(t, p);
            ASSERT_TRUE(q != 0xFFFF);
            uint32_t nt = nav::ref_tile(tile->portals[p].to);
            ASSERT_TRUE(mesh->mirror_portal(nt, q) == p);                     // symmetric
            ASSERT_TRUE(glm::distance(tile->portals[p].mid, mesh->tiles[nt]->portals[q].mid) < mesh->params.cs * 2.5f);
            const nav::NavRegion& reg = tile->regions[tile->portals[p].region];
            ASSERT_TRUE(std::find(reg.portals.begin(), reg.portals.end(), p) != reg.portals.end());
        }
    }
    ASSERT_TRUE(portals > 8);
}

COOPA_TEST(settings_parse_from_yaml) {
    std::istringstream iss(
        "cell_size: 0.3\n"
        "cell_height: 0.05\n"
        "tile_size: 48\n"
        "max_slope: 30\n"
        "layers: [Ground, 3]\n"
        "agents:\n"
        "  - { name: Small, radius: 0.3, height: 1.2, max_climb: 0.3 }\n"
        "  - { name: Big, radius: 1.0, height: 3.0, max_climb: 0.6 }\n"
        "areas:\n"
        "  - { name: Grass, cost: 1.5 }\n"
        "  - { name: Water, cost: 5, id: 9 }\n"
        "max_path_requests_per_frame: 8\n"
        "flow: { rebuild_distance: 1.0, max_distance: 40, wall_penalty: 0.5 }\n"
        "debug_draw: [Mesh, Flow]\n");
    fkyaml::node node = fkyaml::node::deserialize(iss);
    nav::NavSettings s = nav::parse_nav_settings(node, {"Default", "Ground"});
    ASSERT_NEAR(s.build.cell_size, 0.3f, 1e-6f);
    ASSERT_NEAR(s.build.cell_height, 0.05f, 1e-6f);
    ASSERT_TRUE(s.build.tile_size == 48);
    ASSERT_TRUE(s.build.layer_mask == ((1u << 1) | (1u << 3)));
    ASSERT_TRUE(s.build.agents.size() == 2 && s.build.agents[1].name == "Big");
    ASSERT_TRUE(s.build.agent_index("Big") == 1);
    ASSERT_TRUE(s.build.areas.find("Grass") == 2 && s.build.areas.find("Water") == 9);
    ASSERT_NEAR(s.build.areas.costs[9], 5.0f, 1e-6f);
    ASSERT_TRUE(s.max_path_requests_per_frame == 8);
    ASSERT_NEAR(s.flow_max_distance, 40.0f, 1e-6f);
    ASSERT_TRUE(nav::has_flag(s.debug_draw, nav::NavDebugFlags::Flow) && !nav::has_flag(s.debug_draw, nav::NavDebugFlags::Paths));
}

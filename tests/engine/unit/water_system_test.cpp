/**
 * @file water_system_test.cpp
 * @brief WaterSystem's bookkeeping: ripples (splash, wake, silence at rest, tier cap and range),
 *        the underwater query, body destruction (a past editor crash), the deferred physics bake,
 *        and the render tiles' coverage and LOD chains.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <gfxcoopa/engine/data/mesh.h>
#include <toyengine/water/water_tiles.h>

#include "engine/support/checks.h"
#include "engine/support/water_fixtures.h"

COOPA_TEST_SUITE("water_system");

using namespace toy::test;

/** @brief Moving bodies ring the water: a splash on entry, a wake trail behind a moving body,
 *         and silence once everything is at rest. */
COOPA_TEST(ripples_from_splash_and_wake_fall_silent_at_rest) {
    BuoyScene bs = make_water_scene();
    SceneObject* drop = add_box(bs, "drop", glm::vec3(-6.0f, 0.0f, 3.0f), glm::vec3(1.0f), 300.0f);
    // A kinematic paddle driven straight through the water along +X at 2 m/s.
    auto paddle_obj = std::make_unique<SceneObject>("paddle");
    paddle_obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 8.0f, 0.0f));
    paddle_obj->add_component<coopa::physx::components::BoxCollider>()->set_size(glm::vec3(0.8f, 0.4f, 0.6f));
    auto* prb = paddle_obj->add_component<coopa::physx::components::RigidbodyComponent>();
    prb->is_kinematic = true;
    prb->use_gravity = false;
    SceneObject* paddle = paddle_obj.get();
    bs.scene->add_root_object(std::move(paddle_obj));
    start(bs);

    // Splash: the dropped crate hits the water within ~0.8 s.
    float strongest = 0.0f;
    for (int i = 0; i < 90; ++i) {
        run(bs, 1.0f / 60.0f);
        for (const auto& r : bs.water->ripples()) {
            if (glm::distance(r.position, glm::vec2(-6.0f, 0.0f)) < 1.0f) strongest = std::max(strongest, r.strength);
        }
    }
    expect(strongest > 0.3f, "ripples: a falling crate splashes (strength " + std::to_string(strongest) + ")");

    // Wake: 2 s of travel leaves a trail of rings along the path, in order.
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; ++i) {
        coopa::util::Transform& t = paddle->get_transform()->transform();
        t.set_position(t.position() + glm::vec3(2.0f * dt, 0.0f, 0.0f));
        run(bs, dt);
    }
    std::vector<float> trail;
    for (const auto& r : bs.water->ripples()) {
        if (std::fabs(r.position.y - 8.0f) < 0.5f) trail.push_back(r.position.x);
    }
    expect(trail.size() >= 6u, "ripples: a moving body leaves a wake trail (" + std::to_string(trail.size()) + " rings)");
    expect(std::is_sorted(trail.begin(), trail.end()), "ripples: ...laid down in the order it travelled");
    expect(!trail.empty() && trail.back() > 3.0f, "ripples: ...all along its path");

    // Rest: the paddle stops, the crate settles; after the ring lifetime the lake is quiet.
    run(bs, 12.0f);
    expect(bs.water->ripples().empty(), "ripples: everything at rest -> no rings left (" +
                                            std::to_string(bs.water->ripples().size()) + ")");
    (void)drop;
}

/** @brief The ripple cap follows the tier, and bodies beyond the ripple range ring nothing. */
COOPA_TEST(ripples_are_capped_by_tier_and_ranged) {
    BuoyScene bs = make_water_scene();
    add_box(bs, "drop", glm::vec3(-6.0f, 0.0f, 3.0f), glm::vec3(1.0f), 300.0f);
    start(bs);
    bs.water->set_settings(toy::water::WaterSettings::from_quality(toy::water::WaterQuality::Low));
    bs.water->set_focus(glm::vec3(-6.0f, 0.0f, 0.0f) + glm::vec3(bs.water->settings().ripple_range + 20.0f, 0.0f, 0.0f));
    run(bs, 1.5f);
    expect(bs.water->ripples().empty(), "ripples: a splash beyond the ripple range rings nothing");
    for (int i = 0; i < 40; ++i) bs.water->emit_ripple(glm::vec2(static_cast<float>(i) * 0.1f, 0.0f), 0.5f, 0.5f);
    expect(bs.water->ripples().size() == bs.water->settings().max_ripples,
           "ripples: live rings are capped at the tier's maximum (" + std::to_string(bs.water->ripples().size()) + ")");
    expect(std::fabs(bs.water->ripples().back().position.x - 3.9f) < 1e-4f, "ripples: ...keeping the newest");
}

/** @brief underwater_at(): below the surface, above it, and away from any water. */
COOPA_TEST(underwater_query_reports_depth_and_body) {
    BuoyScene bs = make_water_scene();
    start(bs);
    run(bs, 0.1f);
    auto below = bs.water->underwater_at(glm::vec3(0.0f, 0.0f, -2.0f));
    auto above = bs.water->underwater_at(glm::vec3(0.0f, 0.0f, 2.0f));
    auto away  = bs.water->underwater_at(glm::vec3(100.0f, 0.0f, -2.0f));
    expect(below.underwater && below.body == bs.body, "underwater: a point 2 m down is underwater");
    expect_near(below.depth, 2.0f, 0.05f, "underwater: ...at depth 2 m");
    expect(!above.underwater && above.body == bs.body, "underwater: a point above is not, but is over the body");
    expect(!away.underwater && away.body == nullptr, "underwater: a point away from any water is neither");
}

/** @brief A water object destroyed after the system's update (as the editor's deferred edits
 *         do) must not be visible to queries once the list is refreshed -- the regression behind
 *         an intermittent editor crash (Engine queried a freed WaterBody). */
COOPA_TEST(destroyed_body_disappears_from_queries) {
    BuoyScene bs = make_water_scene();
    start(bs);
    run(bs, 0.1f);
    expect(bs.water->underwater_at(glm::vec3(0.0f, 0.0f, -1.0f)).underwater, "query: underwater before");
    SceneObject* water_obj = bs.body->owner;
    bs.scene->remove_root_object(water_obj);   // destroyed now, like an editor rebuild
    bs.water->refresh_bodies(*bs.scene);
    const auto info = bs.water->underwater_at(glm::vec3(0.0f, 0.0f, -1.0f));
    expect(!info.underwater && info.body == nullptr, "query: a destroyed body is gone after refresh_bodies()");
}

/** @brief Render tiles cover the surface exactly; every LOD is a subset of LOD 0, wound +Z,
 *         with LOD switch sizes decreasing; a mesh tiling keeps every triangle once. */
COOPA_TEST(render_tiles_cover_the_surface_with_nested_lods) {
    using coopa::gfx::engine::data::Vertex;
    const int res = 40;
    const float size = 40.0f;
    std::vector<Vertex> verts;
    for (int y = 0; y <= res; ++y)
        for (int x = 0; x <= res; ++x) {
            Vertex v{};
            v.position = glm::vec3(-size * 0.5f + size * x / res, -size * 0.5f + size * y / res, 0.0f);
            v.normal = glm::vec3(0.0f, 0.0f, 1.0f);
            v.uv = glm::vec2(static_cast<float>(x), static_cast<float>(y)); // identifies the grid vertex
            verts.push_back(v);
        }
    toy::water::WaterTileLodParams lod;
    lod.wave_lambdas[0] = 6.0f;
    lod.wave_amps[0] = 0.5f;
    lod.wave_lambdas[3] = 1.4f;
    lod.wave_amps[3] = 0.02f;
    const auto tiles = toy::water::build_grid_tiles(verts, res, res, 16, size / res, lod, glm::vec3(0.1f));
    expect(tiles.size() == 9u, "water tiles: a 40-quad grid in 16-quad tiles is 3 x 3 (" + std::to_string(tiles.size()) + ")");
    std::size_t tris = 0;
    bool subset = true, up = true, decreasing = true, chains = true;
    for (const auto& t : tiles) {
        const auto& d = t.data;
        chains = chains && !d.lods.empty();
        if (d.lods.empty()) continue;
        tris += d.lods[0].index_count / 3;
        for (std::size_t k = 0; k < d.lods.size(); ++k) {
            const auto& l = d.lods[k];
            if (k >= 2) decreasing = decreasing && l.screen_size < d.lods[k - 1].screen_size;
            for (uint32_t i = l.first_index; i + 2 < l.first_index + l.index_count; i += 3) {
                const glm::vec3 a = d.vertices[l.vertex_offset + d.indices[i]].position;
                const glm::vec3 b = d.vertices[l.vertex_offset + d.indices[i + 1]].position;
                const glm::vec3 c = d.vertices[l.vertex_offset + d.indices[i + 2]].position;
                up = up && glm::cross(b - a, c - a).z > 0.0f;
            }
            for (int32_t v = l.vertex_offset; v < static_cast<int32_t>(d.vertices.size()); ++v) {
                const glm::vec2 g = d.vertices[v].uv;
                const Vertex& src = verts[static_cast<std::size_t>(g.y) * (res + 1) + static_cast<std::size_t>(g.x)];
                subset = subset && glm::distance(src.position, d.vertices[v].position) < 1e-6f;
            }
        }
        chains = chains && d.lods.size() >= 2;
    }
    expect(chains, "water tiles: every grid tile carries a LOD chain");
    expect(tris == static_cast<std::size_t>(res * res * 2), "water tiles: LOD 0 tiles cover the grid exactly once");
    expect(subset, "water tiles: every LOD vertex is a LOD 0 vertex (baked attributes carry over)");
    expect(up, "water tiles: every triangle faces +Z");
    expect(decreasing, "water tiles: coarser LODs switch in at smaller screen sizes");
    const float d4 = toy::water::water_lod_distance(4.0f, lod);
    toy::water::WaterTileLodParams calm;
    expect(toy::water::water_lod_crack(4.0f, d4, lod) <= 0.5f * toy::water::k_pixel_angle * d4 + 1e-6f,
           "water tiles: a coarse LOD waits until its T-junction gaps are under half a pixel");
    expect(d4 > toy::water::water_lod_distance(4.0f, calm),
           "water tiles: ...which waves push further out than calm water");

    // An arbitrary mesh: bucketed by centroid, every triangle exactly once, simplified LODs.
    std::vector<uint32_t> idx;
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            uint32_t i0 = static_cast<uint32_t>(y * (res + 1) + x), i1 = i0 + 1, i2 = i0 + res + 1, i3 = i2 + 1;
            idx.insert(idx.end(), {i0, i1, i3, i0, i3, i2});
        }
    const auto mtiles = toy::water::build_mesh_tiles(verts, idx, 16.0f, glm::vec3(0.1f), &lod);
    std::size_t mtris = 0;
    bool fewer = true;
    for (const auto& t : mtiles) {
        const auto& d = t.data;
        mtris += (d.lods.empty() ? d.indices.size() : d.lods[0].index_count) / 3;
        for (std::size_t k = 1; k < d.lods.size(); ++k) fewer = fewer && d.lods[k].index_count < d.lods[k - 1].index_count;
    }
    expect(mtiles.size() == 9u, "water tiles: a mesh is bucketed into tile squares");
    expect(mtris == idx.size() / 3, "water tiles: ...keeping every triangle exactly once");
    expect(fewer, "water tiles: ...with simplified LODs that shrink");
}

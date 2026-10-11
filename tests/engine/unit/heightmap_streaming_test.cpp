/**
 * @file heightmap_streaming_test.cpp
 * @brief How a heightmap terrain decides what to draw, device-free: `auto` settings resolve to
 *        the camera's far clip and a coarse tessellated patch, a fixed view distance caps them,
 *        and chunk distances are measured to each chunk's ground rectangle.
 */

#include <coopa/testing/test.h>

#include <toyengine/world/heightmap_terrain.h>
#include <toyengine/world/heightmap_terrain_system.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("heightmap_streaming");

using toy::world::HeightmapGeometry;
using toy::world::HeightmapTerrain;
using toy::world::HeightmapTerrainSystem;

COOPA_TEST(auto_settings_resolve) {
    HeightmapTerrain t;
    auto s = HeightmapTerrainSystem::resolve_settings(t, 1500.0f, 64.0f);
    expect(s.patch == 4 && s.max_factor == 64.0f, "streaming: tessellated terrain uses a coarse patch, full factor");
    expect(s.view_distance == 1500.0f, "streaming: auto view distance is the camera's far clip");
    expect(s.detail_distance == 1500.0f, "streaming: detail reaches as far as the view (capped at 2 km)");
    expect(s.shadow_distance == 0.0f, "streaming: auto casts shadows everywhere drawn");
    s = HeightmapTerrainSystem::resolve_settings(t, 1500.0f, 0.0f);
    expect(s.patch == 32 && s.max_factor == 1.0f, "streaming: no tessellation -> a dense patch");
    t.view_distance = 200.0f;
    t.tessellation.max_factor = 128.0f;
    s = HeightmapTerrainSystem::resolve_settings(t, 1500.0f, 64.0f);
    expect(s.view_distance == 200.0f && s.shadow_distance == 200.0f, "streaming: a fixed view distance caps shadows too");
    expect(s.max_factor == 64.0f, "streaming: the factor is clamped to the device");
}

COOPA_TEST(chunk_distance_is_to_the_rectangle) {
    HeightmapGeometry g;
    g.size = glm::vec2(256.0f);
    g.chunk_size = 64.0f;
    const glm::vec3 origin(-128.0f, -128.0f, 0.0f);
    // Chunk (1, 1) spans x, y in [-64, 0].
    expect_near(HeightmapTerrainSystem::chunk_distance(g, origin, 1, 1, glm::vec3(-30.0f, -30.0f, 50.0f)), 0.0f, 1e-5f,
                "streaming: inside a chunk is distance 0 (height ignored)");
    expect_near(HeightmapTerrainSystem::chunk_distance(g, origin, 3, 1, glm::vec3(-30.0f, -30.0f, 50.0f)), 94.0f, 1e-3f,
                "streaming: two chunks east, measured to its west edge");
    int within = 0;
    for (int cy = 0; cy < 4; ++cy) {
        for (int cx = 0; cx < 4; ++cx) within += HeightmapTerrainSystem::chunk_distance(g, origin, cx, cy, glm::vec3(0.0f)) <= 70.0f;
    }
    expect(within == 12, "streaming: from the centre, all but the 4 corner chunks (90.5 m away) are within 70 m");
}

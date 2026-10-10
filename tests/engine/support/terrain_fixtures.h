#pragma once

/**
 * @file terrain_fixtures.h
 * @brief Shared terrain inputs: a canonical unit quad and the flat side library baked from it,
 *        small test params and pads (terrain_chunk, terrain_sampler, styled_tiles), and the live
 *        chunk queries + world shrink the GPU terrain suites use to keep generation to a second.
 */

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

#include <coopa/scene/scene.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/tile_mesh_library.h>

namespace toy::test {

/**
 * @brief A welded unit quad in the canonical side orientation: the +Z face of `[0,1]^3`.
 *
 * Built directly rather than loaded from assets/meshes/tile_side_flat.yaml
 * so the counting assertions below rest on known numbers -- 4 vertices and 6 indices per
 * appended side -- instead of on however the YAML path happens to weld its corners.
 */
inline coopa::gfx::engine::data::SkinnedMeshSource make_canonical_quad() {
    using coopa::gfx::engine::data::Vertex;
    coopa::gfx::engine::data::SkinnedMeshSource source;

    const glm::vec3 positions[4] = {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f},
                                    {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
    const glm::vec2 uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    for (int i = 0; i < 4; ++i) {
        Vertex v{};
        v.position = positions[i];
        v.normal   = glm::vec3(0.0f, 0.0f, 1.0f);
        v.uv       = uvs[i];
        v.tangent  = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        source.vertices.push_back(v);
    }
    source.indices = {0, 1, 2, 0, 2, 3};
    return source;
}

/** @brief Indices per appended side, for the quad above. Two triangles. */
constexpr size_t kIndicesPerSide = 6;

/** @brief A library with the same flat quad baked onto all six faces. */
inline toy::world::TileMeshLibrary make_flat_library() {
    toy::world::TileMeshLibrary library;
    library.bake_canonical(make_canonical_quad());
    return library;
}

/** @brief Test params: a small chunk, unit cells, no bottom faces, generous wall clamp. */
inline toy::world::TerrainParams make_test_params(std::int32_t chunk_size = 4) {
    toy::world::TerrainParams params;
    params.chunk_size       = chunk_size;
    params.tile_size        = 1.0f;
    params.height_step      = 1.0f;
    params.max_wall_steps   = 64;
    params.soil_depth_steps = 3;
    params.emit_bottom      = false;
    return params;
}

/** @brief A pad whose every column -- skirt included -- stands at the same height. */
inline toy::world::ColumnPad make_flat_pad(std::int32_t chunk_size, std::int32_t steps) {
    toy::world::ColumnPad pad;
    pad.resize(chunk_size);
    for (std::int32_t y = -1; y <= chunk_size; ++y) {
        for (std::int32_t x = -1; x <= chunk_size; ++x) {
            pad.at(x, y).steps = steps;
        }
    }
    return pad;
}

/** @brief Number of sides in a merged chunk, from its index count. */
inline size_t side_count(const toy::world::ChunkMeshData& mesh) {
    return mesh.indices.size() / kIndicesPerSide;
}

/** @brief Live (uploaded and drawing) chunks in a terrain. */
inline int count_live_chunks(const toy::world::TerrainComponent& terrain) {
    int live = 0;
    for (const auto& entry : terrain.chunks()) {
        if (entry.second.state == toy::world::ChunkState::Live) ++live;
    }
    return live;
}

/** @brief True when this coordinate has a Live chunk. */
inline bool chunk_is_live(const toy::world::TerrainComponent& terrain, toy::world::ChunkCoord coord) {
    auto it = terrain.chunks().find(coord);
    return it != terrain.chunks().end() && it->second.state == toy::world::ChunkState::Live;
}

/**
 * @brief Shrinks a terrain to a test-sized world and parks its marker in the MIDDLE of it.
 *
 * Recentring is not optional. The scene authors the marker at tile (192, 192) -- the centre of
 * the shipped 96-cell world -- so shrinking `grid_size` without moving it leaves the camera off
 * the far corner, where most of the view radius falls outside the map and only a handful of
 * chunks are ever built. Every measurement taken from there is of mostly-empty sky.
 *
 * @param terrain The component to shrink; its params are overwritten.
 * @param scene   The scene, for resolving the marker object.
 * @return World-space centre the marker was parked at.
 */
inline glm::vec3 shrink_terrain_and_centre(toy::world::TerrainComponent& terrain,
                                    coopa::scene::Scene& scene) {
    terrain.grid_size                  = 48;
    terrain.params.tiles_per_grid_unit = 4;
    terrain.params.chunk_size          = 16;
    terrain.params.view_radius         = 2;

    const float tiles = float(terrain.grid_size * terrain.params.tiles_per_grid_unit);
    const glm::vec3 centre(tiles * 0.5f * terrain.params.tile_size,
                           tiles * 0.5f * terrain.params.tile_size, 26.0f);
    if (auto* marker = scene.find_object("focus_marker")) {
        if (auto* tc = marker->get_transform()) tc->transform().set_position(centre);
    }
    return centre;
}

} // namespace toy::test

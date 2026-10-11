#pragma once

/**
 * @file terrain_fixtures.h
 * @brief Shared terrain inputs: tile libraries baked from the shipped tile sets, small test
 *        params and pads (terrain_chunk, terrain_sampler, styled_tiles), and the live chunk
 *        queries + world shrink + voxel variant the GPU terrain suites use.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>

#include <glm/glm.hpp>

#include <root_directory.h>   // ROOT_DIR

#include <coopa/asset/asset_index.h>
#include <coopa/scene/scene.h>
#include <coopa/yaml/document.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/tile_mesh_library.h>
#include <toyengine/world/tile_topology.h>

#include "engine/support/scene_variant.h"

namespace toy::test {

/** @brief Loads one generated tile-set piece from the repo's assets, whatever the working directory. */
inline coopa::gfx::engine::data::SkinnedMeshSource load_tile_piece(const std::string& name) {
    // Found by name: the pieces live in tag folders (meshes/terrain/<set>/).
    const auto path = coopa::asset::AssetIndex::find(std::filesystem::path(ROOT_DIR) / "assets", "meshes/" + name + ".yaml");
    return coopa::gfx::engine::data::SkinnedMeshSource::from_node(coopa::yaml::load_document(
        path.value_or(std::filesystem::path(ROOT_DIR) / "assets" / "meshes" / (name + ".yaml"))));
}

/**
 * @brief A library baked from shipped tile sets, by name (`round`, `soft`, `rock`, `voxel`), in
 *        that order as styles 0, 1, ...; every kind maps to style 0. The voxel set keeps kind
 *        boundaries square, as its tile-set object says (round_kind_corners: false).
 */
inline toy::world::TileMeshLibrary make_tile_library(std::initializer_list<const char*> sets) {
    using namespace toy::world;
    TileMeshLibrary lib;
    for (const char* set : sets) {
        const std::uint8_t index = lib.add_style();
        lib.set_round_kind_corners(index, std::string(set) != "voxel");
        for (std::size_t p = 0; p < k_tile_piece_count; ++p) {
            const auto piece = static_cast<TilePiece>(p);
            lib.bake_style_piece(index, piece, load_tile_piece("tile_" + std::string(set) + "_" + tile_piece_name(piece)));
        }
    }
    lib.finish_styles();
    return lib;
}

/** @brief The shipped voxel (square block) tile set alone. Built once: it is immutable. */
inline const toy::world::TileMeshLibrary& voxel_library() {
    static const toy::world::TileMeshLibrary library = make_tile_library({"voxel"});
    return library;
}

/** @brief Test params: a small chunk, unit cells, generous wall clamp. */
inline toy::world::TerrainParams make_test_params(std::int32_t chunk_size = 4) {
    toy::world::TerrainParams params;
    params.chunk_size       = chunk_size;
    params.tile_size        = 1.0f;
    params.height_step      = 1.0f;
    params.max_wall_steps   = 64;
    params.soil_depth_steps = 3;
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

/**
 * @brief terrain_demo built from the voxel tile set: `tile_set` pointed at tileset_voxel and the
 *        per-kind overrides dropped -- the one change that turns the rounded world into blocks.
 *        The demo itself is rounded; this is the variant its header describes.
 */
inline std::string voxel_terrain_demo() {
    // A fresh bake per test: the tile store is process-wide, and tests shrink the world -- so
    // neither a stored grid in the store nor the demo's own .tiles file (copied with its folder).
    toy::world::TileMapStore::instance().clear();
    std::string path = scene_variant("assets/scenes/terrain/terrain_demo/scene.yaml", "voxel_terrain", [](fkyaml::node& doc) {
        fkyaml::node* t = scene_component(doc, "terrain", "TileTerrain");
        if (t == nullptr) return;
        t->as_map().erase(fkyaml::node(std::string("kind_tile_sets")));
        (*t)["tile_set"] = fkyaml::node(std::string("objects/terrain/tileset_voxel"));
        // The demo's falling bodies (`drop_*`) would keep the scene in motion -- out of these
        // rendering tests' view, but not out of the temporal passes' motion gate.
        auto& roots = doc["scene"]["root_objects"].as_seq();
        roots.erase(std::remove_if(roots.begin(), roots.end(), [](const fkyaml::node& o) {
                        return o.contains("name") && o.at("name").get_value<std::string>().rfind("drop_", 0) == 0;
                    }),
                    roots.end());
    });
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(path).parent_path() / "terrain" / "terrain.tiles", ec);
    return path;
}

} // namespace toy::test

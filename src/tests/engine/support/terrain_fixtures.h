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
#include <vector>

#include <glm/glm.hpp>

#include <root_directory.h>   // ROOT_DIR

#include <coopa/asset/asset_index.h>
#include <coopa/scene/scene.h>
#include <coopa/yaml/document.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/world/terra_store.h>
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/tile_mesh_library.h>
#include <toyengine/world/tile_topology.h>

#include "engine/support/scene_variant.h"

namespace toy::test {

/**
 * @brief Loads one generated tile-set piece from the shipped assets, whatever the working
 *        directory -- and whichever tree the runner was built in.
 *
 * `ROOT_DIR` is the top-level project's root, which for a game project's own suites is the GAME,
 * not the engine; the pieces ship with the engine. So both roots are searched, project first so
 * a project overriding a piece wins. `TOY_ENGINE_ASSETS_DIR` is defined on a project's test
 * targets by `toyengine_add_project_tests()` and is absent in the engine's own tree, where
 * `ROOT_DIR` already is the engine.
 */
inline coopa::gfx::engine::data::SkinnedMeshSource load_tile_piece(const std::string& name) {
    const std::string rel = "meshes/" + name + ".yaml";
    std::vector<std::filesystem::path> roots{std::filesystem::path(ROOT_DIR) / "assets"};
#ifdef TOY_ENGINE_ASSETS_DIR
    roots.emplace_back(TOY_ENGINE_ASSETS_DIR);
#endif
    for (const std::filesystem::path& root : roots) {
        // Found by name: the pieces live in tag folders (meshes/terrain/<set>/).
        if (const auto path = coopa::asset::AssetIndex::find(root, rel)) {
            return coopa::gfx::engine::data::SkinnedMeshSource::from_node(
                coopa::yaml::load_document(*path));
        }
    }
    // Nothing indexed it: let the flat path report which file is missing.
    return coopa::gfx::engine::data::SkinnedMeshSource::from_node(coopa::yaml::load_document(
        roots.back() / "meshes" / (name + ".yaml")));
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

/** @brief Test params: a small chunk with unit cells. */
inline toy::world::TerrainParams make_test_params(std::int32_t chunk_size = 4) {
    toy::world::TerrainParams params;
    params.chunk_size       = chunk_size;
    params.tile_size        = 1.0f;
    params.height_step      = 1.0f;
    params.soil_depth_steps = 3;
    return params;
}

/** @brief A pad whose every column -- skirt included -- stands at the same height. */
/** @brief The styled_tiles busy terrain: 3x3-tile blocks at 1-4 steps, 5x5 patches of kinds. */
inline toy::world::ColumnPad busy_pad(std::int32_t chunk_size, std::uint32_t seed) {
    toy::world::ColumnPad pad;
    pad.resize(chunk_size);
    auto hash = [seed](std::int32_t x, std::int32_t y) {
        std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^
                          static_cast<std::uint32_t>(y) * 19349663u ^ seed * 83492791u;
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        return h;
    };
    auto block = [](std::int32_t v, std::int32_t size) { return v >= 0 ? v / size : (v - size + 1) / size; };
    for (std::int32_t y = -1; y <= chunk_size; ++y) {
        for (std::int32_t x = -1; x <= chunk_size; ++x) {
            toy::world::TileColumn& c = pad.at(x, y);
            c.steps     = 1 + static_cast<std::int32_t>(hash(block(x, 3), block(y, 3)) % 4u);
            using toy::world::TileKind;
            static const TileKind kinds[] = {TileKind::Grass, TileKind::Grass, TileKind::Stone,
                                             TileKind::Sand,  TileKind::Snow};
            c.top_kind  = kinds[hash(block(x, 5) + 101, block(y, 5)) % 5u];
            c.side_kind = toy::world::TileKind::Dirt;
        }
    }
    return pad;
}

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
    terrain.generator.grid_size        = 48;
    terrain.generator.shape.shape      = coopa::maps::MapShape::Circle;   // a centred island
    terrain.generator.shape.diameter_m = 0.0;                             // ...fitted to the map
    terrain.params.tiles_per_grid_unit = 4;
    terrain.params.chunk_size          = 16;
    terrain.params.view_radius         = 2;

    const float tiles = float(terrain.generator.grid_size * terrain.params.tiles_per_grid_unit);
    const glm::vec3 centre(tiles * 0.5f * terrain.params.tile_size,
                           tiles * 0.5f * terrain.params.tile_size, 26.0f);
    if (auto* marker = scene.find_object("focus_marker")) {
        if (auto* tc = marker->get_transform()) tc->transform().set_position(centre);
    }
    // The camera must arrive WITH the marker, not fly there over frames from the demo's
    // authored pose across the map: mid-flight, the streamer legitimately builds chunks around
    // wherever the camera currently is, so a "N chunks live" wait can be satisfied by ground
    // nowhere near `centre` -- and a collision ring that settles later re-meshes chunks a test
    // snapshotted. Smoothing 0 snaps the orbit rig to its tracker on the next tick.
    if (auto* controller = scene.find_first_component<toy::scene::CameraController>()) {
        controller->follow_smoothing   = 0.0f;
        controller->movement_smoothing = 0.0f;
    }
    return centre;
}

/**
 * @brief terrain_demo built from the voxel tile set: `tile_set` pointed at tileset_voxel and the
 *        per-kind overrides dropped -- the one change that turns the rounded world into blocks.
 *        The demo itself is rounded; this is the variant its header describes.
 */
inline std::string voxel_terrain_demo() {
    // A fresh bake per test: the store is process-wide, and tests shrink the world -- so
    // neither a stored grid in the store nor the demo's own .terra file (copied with its folder).
    toy::world::TerraStore::instance().clear();
    std::string path = scene_variant("assets/scenes/terrain/terrain_demo/scene.yaml", "voxel_terrain", [](fkyaml::node& doc) {
        fkyaml::node* t = scene_component(doc, "terrain", "Terrain");
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
    std::filesystem::remove(std::filesystem::path(path).parent_path() / "terrain" / "terrain.terra", ec);
    return path;
}

/**
 * @brief terrain_demo as authored: `tile_set: tileset_soft` with `tileset_rock` on the rock/stone
 *        kinds, unmodified -- the ~8x-triangle-per-set geometry (ARC_SEG 3, LIP_SEG 4 vs voxel's
 *        1 and 1) that a mountainous world mostly draws. Only `voxel_terrain_demo()`'s cheap
 *        variant has ever run through a frame-budget test; this is the expensive one.
 */
inline std::string soft_terrain_demo() {
    toy::world::TerraStore::instance().clear();
    std::string path = scene_variant("assets/scenes/terrain/terrain_demo/scene.yaml", "soft_terrain", [](fkyaml::node& doc) {
        // Same motion-out-of-frame trim as voxel_terrain_demo(); no tile_set override, so this
        // keeps the demo's authored (expensive) tile sets.
        auto& roots = doc["scene"]["root_objects"].as_seq();
        roots.erase(std::remove_if(roots.begin(), roots.end(), [](const fkyaml::node& o) {
                        return o.contains("name") && o.at("name").get_value<std::string>().rfind("drop_", 0) == 0;
                    }),
                    roots.end());
    });
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(path).parent_path() / "terrain" / "terrain.terra", ec);
    return path;
}

/**
 * @brief heightmap_terrain_demo patched for a streaming frame-budget test: a finite
 *        `view_distance` so chunks actually churn in and out as the camera moves (the authored
 *        demo's 0 keeps the whole 16 x 16 grid live forever, which never exercises
 *        `stream_chunks_()`'s budget), and no CameraController -- the test drives the camera's
 *        own Transform directly instead of fighting an orbit rig that recomputes it every tick.
 *
 * Also drops the terrain's foliage scatter children (grass, pine trunks/canopies, bushes,
 * boulders): density-gated scatter (`terrain: { min_weight: ... }`) rebuilds inline the moment a
 * chunk streams in, billing its own cost to whatever this test measures. Dropping it isolates
 * what this test is actually for -- `stream_chunks_()`/`refresh_water_()`'s OWN pacing -- from
 * foliage's, regardless of how much of the terrain any splat layer happens to cover.
 */
inline std::string heightmap_frame_test_demo() {
    toy::world::TerraStore::instance().clear();
    return scene_variant("assets/scenes/terrain/heightmap_terrain_demo/scene.yaml", "heightmap_frame_budget",
                         [](fkyaml::node& doc) {
        if (fkyaml::node* t = scene_component(doc, "terrain", "Terrain")) {
            (*t)["view_distance"] = fkyaml::node(150.0);
        }
        for (auto& obj : doc["scene"]["root_objects"].as_seq()) {
            if (obj.contains("name") && obj["name"].get_value<std::string>() == "terrain" && obj.contains("children")) {
                obj["children"].as_seq().clear();
            }
            if (!obj.contains("name") || obj["name"].get_value<std::string>() != "camera") continue;
            auto& comps = obj["components"].as_seq();
            comps.erase(std::remove_if(comps.begin(), comps.end(), [](const fkyaml::node& c) {
                            return c.contains("type") && c["type"].get_value<std::string>() == "CameraController";
                        }),
                        comps.end());
        }
    });
}

} // namespace toy::test

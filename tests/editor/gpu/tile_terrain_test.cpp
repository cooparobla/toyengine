/**
 * @file tile_terrain_test.cpp
 * @brief TileTerrain in the editor: built from a tile-set object. A new terrain starts from
 *        tileset_round and its chunks collide; the panel lists every tile set; a component still on
 *        the old `styles:` / `side_mesh:` keys converts to `tile_set:` + `kind_tile_sets:`.
 */

#include <coopa/testing/test.h>
#include <physxcoopa/components/mesh_collider.h>

#include <algorithm>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("tile_terrain");

namespace toy::editor::testing {

/** @brief A new TileTerrain works straight away: the default tile set, drawn and collidable chunks. */
COOPA_TEST(a_new_tile_terrain_builds_from_its_tile_set) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;

    const auto& sets = app.tile_set_assets();
    for (const char* name : {"round", "soft", "rock", "voxel"}) {
        const std::string ref = std::string("objects/terrain/tileset_") + name;
        expect(std::find(sets.begin(), sets.end(), ref) != sets.end(), "the panel offers " + ref);
    }

    const ObjectId id = app.create_with_component("TileTerrain", "Tile Terrain");
    auto& doc = app.document();
    const int ci = doc.find_component(id, "TileTerrain");
    coopa::test::require(ci >= 0, "a TileTerrain object is created");
    const Node comp = doc.find(id)->at("components").as_seq()[static_cast<size_t>(ci)];
    expect(get_string(comp, "tile_set") == "objects/terrain/tileset_round", "a new terrain starts from tileset_round");

    // Drawn by the panel every frame while the terrain streams in.
    app.document().select(id);
    app.set_prop_tab(PropTab::Components);
    toy::world::TerrainComponent* live = nullptr;
    auto live_chunks = [&] {
        return live ? std::count_if(live->chunks().begin(), live->chunks().end(), [](const auto& c) {
            return c.second.state == toy::world::ChunkState::Live && c.second.object != nullptr;
        }) : 0;
    };
    for (int i = 0; i < 900 && live_chunks() < 4; ++i) {
        tick(engine, 1);
        coopa::scene::SceneObject* obj = app.sync().live(id);
        live = obj ? obj->get_component<toy::world::TerrainComponent>() : nullptr;
    }
    coopa::test::require(live != nullptr && live->is_ready(), "the live terrain built its tile set");
    expect(live->styles.size() == 1 && live->styles[0].prefix == "objects/terrain/tileset_round",
           "it is built from the one tile set it names");
    int collidable = 0;
    for (const auto& [coord, chunk] : live->chunks()) {
        if (chunk.object && chunk.object->get_component<coopa::physx::components::MeshCollider>()) ++collidable;
    }
    expect(live_chunks() >= 4 && collidable == static_cast<int>(live_chunks()),
           "its chunks draw and collide by default (" + std::to_string(collidable) + " colliders)");
    dump(engine, "tile_terrain_panel");   // EDITOR_DUMP_DIR=... for a look at the panel
}

/** @brief The old keys convert to `tile_set:` + `kind_tile_sets:`, keeping the same look. */
COOPA_TEST(old_tile_keys_convert_to_a_tile_set) {
    Node legacy = Node::mapping();
    legacy["type"] = Node(std::string("TileTerrain"));
    legacy["styles"] = Node::mapping();
    legacy["styles"]["round"] = Node(std::string("objects/tileset_round"));
    legacy["styles"]["rock"] = Node(std::string("objects/tileset_rock"));
    legacy["kind_styles"] = Node::mapping();
    legacy["kind_styles"]["default"] = Node(std::string("round"));
    legacy["kind_styles"]["grass"] = Node(std::string("round"));
    legacy["kind_styles"]["stone"] = Node(std::string("rock"));
    legacy["greedy_merge"] = Node(true);
    expect(EditorApp::convert_legacy_tile_terrain(legacy), "the old styles keys convert");
    expect(get_string(legacy, "tile_set") == "objects/tileset_round", "...the default style becomes the tile set");
    expect(legacy.contains("kind_tile_sets") && legacy.at("kind_tile_sets").size() == 1 &&
               get_string(legacy.at("kind_tile_sets"), "stone") == "objects/tileset_rock",
           "...the other styles become per-surface overrides");
    expect(!legacy.contains("styles") && !legacy.contains("kind_styles") && !legacy.contains("greedy_merge"),
           "...and the old keys are gone");

    Node voxel = Node::mapping();
    voxel["type"] = Node(std::string("TileTerrain"));
    voxel["side_mesh"] = Node(std::string("tile_side_flat"));
    expect(EditorApp::convert_legacy_tile_terrain(voxel) && get_string(voxel, "tile_set") == "objects/terrain/tileset_voxel" &&
               !voxel.contains("side_mesh"),
           "the retired voxel mesher's keys become the voxel tile set");

    Node current = Node::mapping();
    current["type"] = Node(std::string("TileTerrain"));
    current["tile_set"] = Node(std::string("objects/terrain/tileset_round"));
    expect(!EditorApp::convert_legacy_tile_terrain(current), "a terrain on tile_set is left alone");
}

} // namespace toy::editor::testing

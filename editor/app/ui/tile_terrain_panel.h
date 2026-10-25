/**
 * @file tile_terrain_panel.h
 * @brief The TileTerrain inspector (Properties > Components). A tile terrain has ~25 settings
 *        spread over four different concerns -- what it is built from, the generated world, its
 *        size, and how it streams -- so instead of the flat schema list this panel: - leads with
 *        what the component is and a live status line (generating / chunks / colliders); - puts
 *        the TILE SET first: a picker of tile-set objects (the template every edge, corner and
 *        wall is built from -- swap it to change the terrain's form), Open / Duplicate as New
 *        Tile Set, and per-surface overrides (stone from tileset_rock, say) in a collapsed table;
 *        - groups the rest into World / Size & Terraces / Streaming / Collision sections that
 *        each say in a sentence what they control, with read-outs in metres (world size,
 *        terraces, loaded area); - shows only what applies (continent settings only for continent
 *        / archipelago shapes); - offers a one-click conversion for scenes still on the old
 *        `styles:` / `side_mesh:` keys.
 *
 * Every field is still the schema's FieldDesc (same widgets, undo, prefab-override marks); the
 * panel only decides which appear, where, and with what words.
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_TILE_TERRAIN_PANEL_H
#define TOYEDITOR_APP_UI_TILE_TERRAIN_PANEL_H

#include "editor/app/editor_panel.h"

namespace toy::editor {

/** @brief See the file doc. */
class TileTerrainPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access level
    /// they had inside EditorApp's body.
    friend class EditorApp;

    /** @brief Draws the TileTerrain (or legacy Terrain) component `comp` of object `id`. Edits land in `comp`. */
    EditResult draw_tile_terrain_panel_(imm::Context& ctx, ObjectId id, Node& comp, const InspectorEnv& env);

    /** @brief The live TerrainComponent of `id` (null when not built). */
    toy::world::TerrainComponent* live_tile_terrain_(ObjectId id) const;

    /**
     * @brief Every tile-set object asset (`tile_set: true`), as references without `.yaml`
     *        (`objects/terrain/tileset_round`), sorted. Cached: rescanned when the object asset
     *        list changes, not every frame.
     */
    const std::vector<std::string>& tile_set_assets_();

public:
    /** @brief tile_set_assets_(), for tests. */
    const std::vector<std::string>& tile_set_assets() { return tile_set_assets_(); }

    /**
     * @brief Rewrites a component still on the old keys -- `styles:` / `kind_styles:` (named sets
     *        per kind) or the retired voxel mesher's `side_mesh:` / `sides:` / `greedy_merge:` /
     *        `emit_bottom:` -- into `tile_set:` + `kind_tile_sets:`. Returns true if it changed.
     */
    static bool convert_legacy_tile_terrain(Node& comp);

private:

    std::vector<std::string> tile_set_cache_keys_;   ///< The object asset list tile_set_cache_ was built from.
    std::vector<std::string> tile_set_cache_;        ///< tile_set_assets_()'s result.
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_TILE_TERRAIN_PANEL_H

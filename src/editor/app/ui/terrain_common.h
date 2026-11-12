/**
 * @file terrain_common.h
 * @brief One terrain component, three scene names (`Terrain`, `TileTerrain`,
 *        `HeightmapTerrain` -- see toyengine/world/terrain_component.h). Every editor entry
 *        point that needs to know "is this a terrain" or "which mode" goes through here, instead
 *        of each checking its own name and getting it wrong for the other two.
 */

#ifndef TOYEDITOR_APP_UI_TERRAIN_COMMON_H
#define TOYEDITOR_APP_UI_TERRAIN_COMMON_H

#include "editor/core/scene_document.h"

#include <toyengine/world/terrain_types.h>

#include <optional>

namespace toy::editor {

/** @brief True when `id` carries a terrain component, under any of its three scene names. */
bool is_any_terrain_object(const SceneDocument& doc, ObjectId id);

/**
 * @brief The mode of the terrain component on `id`, found under any of its three scene names,
 *        or nullopt when `id` carries none. An absent `mode:` key is inferred from the
 *        component's other keys (toyengine/world/terrain_yaml.h's `infer_terrain_mode`).
 */
std::optional<toy::world::TerrainMode> terrain_mode_of(const SceneDocument& doc, ObjectId id);

/**
 * @brief The component index (as find_component() returns it) of `id`'s terrain component, under
 *        any of its three scene names, when it is in Tiles mode -- or -1 (no terrain, or a
 *        heightmap one). World Generator's bake is tile-specific; this is the one check it needs.
 */
int find_tile_terrain_component(const SceneDocument& doc, ObjectId id);

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_TERRAIN_COMMON_H

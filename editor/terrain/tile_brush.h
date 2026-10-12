/**
 * @file tile_brush.h
 * @brief The TileTerrain tools, as pure functions on a TileMapData: square brushes (raise /
 *        lower, flatten, set height, paint, smooth, water), tile selections (rectangle,
 *        connected "similar" tiles, field edits over a selection), and the undo patch a stroke
 *        records. No GPU, no app: the editor's Tile Mode (editor/app/ui/tile_mode.cpp) drives
 *        these, and tests call them.
 *
 * Every function marks what it changed dirty on the TileMapData (so the terrain re-meshes) and
 * returns that rect.
 */

#ifndef TOYEDITOR_TERRAIN_TILE_BRUSH_H
#define TOYEDITOR_TERRAIN_TILE_BRUSH_H

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/world/tile_map_data.h>

namespace toy::editor {

enum class TileTool { Select = 0, Raise, Flatten, SetHeight, Paint, Smooth, Water };
inline constexpr int k_tile_tool_count = 7;

/** @brief Display name ("Raise / Lower", ...). */
const char* tile_tool_name(TileTool t);

struct TileBrushSettings {
    TileTool tool = TileTool::Raise;
    int size = 1;                  ///< Square brush edge, in tiles.
    int strength = 1;              ///< Steps per tile entered (Raise / Lower).
    int target_height = 4;         ///< Set Height; Flatten takes the clicked tile's instead.
    toy::world::TileKind paint_kind = toy::world::TileKind::Grass;
    bool paint_side = true;        ///< Paint also sets the side kind (to the top kind's usual side).
    int water_level = 5;           ///< Water: flood the brush's basin up to this terrace (steps).
    bool similar_by_kind = false;  ///< Select's double-click: same surface (true) or same height.
};

/** @brief The square of `size` tiles centred on `tile` (odd sizes centre exactly; even lean to +x/+y). */
toy::world::IRect tile_brush_rect(const glm::ivec2& tile, int size);

/**
 * @brief Raises (delta > 0) or lowers every tile's ground in `rect` by `delta` steps; never below
 *        0. Under water the bed moves. The water then re-settles (tile_water.h's settle_water()):
 *        a bed raised through its surface leaves land, ground lowered beside water floods -- so
 *        the returned rect can reach past `rect`.
 */
toy::world::IRect tile_raise(toy::world::TileMapData& d, const toy::world::IRect& rect, int delta);
/** @brief Sets every tile's ground in `rect` to `steps` (under water, the bed), then re-settles the water. */
toy::world::IRect tile_set_height(toy::world::TileMapData& d, const toy::world::IRect& rect, int steps);
/** @brief Paints the top kind (and, with `side`, the matching side kind) -- beds under water included. */
toy::world::IRect tile_paint(toy::world::TileMapData& d, const toy::world::IRect& rect, toy::world::TileKind kind, bool side);
/** @brief Moves each tile one step toward the median of its 3 x 3 neighbourhood (terraces stay whole steps), then re-settles the water. */
toy::world::IRect tile_smooth(toy::world::TileMapData& d, const toy::world::IRect& rect);
/**
 * @brief Water: `fill` floods the basin the brush is in to terrace `level` -- every tile
 *        connected to it whose ground is lower, as far as the ground holds the water; otherwise
 *        drains the whole body the brush touches. The returned rect is the flood's, not the brush's.
 */
toy::world::IRect tile_water(toy::world::TileMapData& d, const toy::world::IRect& rect, int level, bool fill);

/**
 * @class TileSelection
 * @brief A set of tiles: a mask over the grid plus its count and bounds.
 */
class TileSelection {
public:
    void reset(int w, int h);
    void clear();
    bool empty() const { return count_ == 0; }
    int count() const { return count_; }
    int width() const { return w_; }
    int height() const { return h_; }
    bool contains(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_ && mask_[static_cast<std::size_t>(y * w_ + x)]; }
    void set(int x, int y, bool on);
    /** @brief Adds (`on`) or removes every tile of `rect`. */
    void set_rect(const toy::world::IRect& rect, bool on);
    /** @brief The bounding rect of the selected tiles. */
    toy::world::IRect bounds() const;
    /** @brief Calls fn(x, y) for every selected tile, row-major. */
    void for_each(const std::function<void(int, int)>& fn) const;

private:
    int w_ = 0, h_ = 0, count_ = 0;
    std::vector<std::uint8_t> mask_;
};

/**
 * @brief Selects the tiles connected to `start` (4-neighbours) that match it: the same height,
 *        or with `by_kind` the same top kind. Adds to `sel`.
 */
void tile_select_similar(const toy::world::TileMapData& d, const glm::ivec2& start, bool by_kind, TileSelection& sel);

/** @brief Applies `edit` to every selected tile's record; marks and returns the selection's bounds. */
toy::world::IRect tile_edit_selection(toy::world::TileMapData& d, const TileSelection& sel,
                                      const std::function<void(toy::world::TileRecord&)>& edit);

/**
 * @struct TilePatch
 * @brief One undo step: a rect's records before and after.
 */
struct TilePatch {
    std::weak_ptr<toy::world::TileMapData> data;
    std::uint64_t uid = 0;           ///< The data it was taken from (skipped if the terrain was rebuilt).
    toy::world::IRect rect;
    std::vector<toy::world::TileRecord> records;
    /// A whole-grid patch of this size (a Regenerate that changed the world's size): applying it
    /// resizes the grid first. 0 for an ordinary rect patch.
    int width = 0, height = 0;
    std::size_t bytes() const { return records.size() * sizeof(toy::world::TileRecord) + sizeof(TilePatch); }
};

TilePatch capture_tile_patch(const std::shared_ptr<toy::world::TileMapData>& d, const toy::world::IRect& rect);
/** @brief Writes the patch back (and marks it dirty); false when its terrain is gone or reshaped. */
bool apply_tile_patch(const TilePatch& patch);

} // namespace toy::editor

#endif // TOYEDITOR_TERRAIN_TILE_BRUSH_H

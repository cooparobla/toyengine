/**
 * @file terrain_brush.h
 * @brief The heightmap terrain tools, as pure functions on a HeightmapData: raise / lower,
 *        smooth, flatten, set height, noise, erode and paint -- one dab at a time, each returning
 *        the texels it changed -- plus the undo patch a stroke records. No GPU, no app: the
 *        editor's Terrain mode (editor/app/ui/terrain.cpp) drives these, and tests call them.
 *
 * Brushes work in metres: a dab is a circle on the terrain's ground plane (local xy), its
 * weight `strength * falloff(distance / radius)`. Heights stay within [0, height].
 */

#ifndef TOYEDITOR_TERRAIN_TERRAIN_BRUSH_H
#define TOYEDITOR_TERRAIN_TERRAIN_BRUSH_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/world/heightmap_data.h>

namespace toy::editor {

enum class TerrainTool { Raise = 0, Smooth, Flatten, SetHeight, Noise, Erode, Paint };

/** @brief Display name ("Raise / Lower", ...). */
const char* terrain_tool_name(TerrainTool t);

struct TerrainBrushSettings {
    TerrainTool tool = TerrainTool::Raise;
    float radius = 12.0f;         ///< Metres.
    float strength = 0.5f;        ///< 0..1.
    float falloff = 0.6f;         ///< 0 = hard edge, 1 = smooth all the way in.
    float spacing = 0.2f;         ///< Dab spacing as a fraction of the radius.
    float target_height = 10.0f;  ///< Set Height (m above the terrain's base); Flatten picks its own per stroke.
    float noise_scale = 16.0f;    ///< Noise: metres per feature.
    int   layer = 0;              ///< Paint: the layer painted.
};

/** @brief The dab weight profile: 1 at the centre, 0 at the rim; `falloff` softens the edge. */
float terrain_falloff(float d01, float falloff);

/**
 * @brief One dab of `s.tool` at local xy `centre`. `invert` lowers (Raise), digs (Noise) or
 *        erases (Paint); `flatten_to` is Flatten's target height (m).
 * @return The height texels (or splat texels, for Paint) the dab changed.
 */
toy::world::IRect terrain_dab(toy::world::HeightmapData& data, const TerrainBrushSettings& s, glm::vec2 centre, bool invert,
                              float flatten_to = 0.0f, std::uint32_t seed = 1);

/**
 * @struct TerrainPatch
 * @brief A rectangle of a terrain's heights and/or splats, before or after an edit -- what one
 *        undo step restores. Holds the data weakly: a terrain removed since is skipped.
 */
struct TerrainPatch {
    std::weak_ptr<toy::world::HeightmapData> data;
    toy::world::IRect height_rect;   ///< Empty: no height texels in this patch.
    std::vector<float> heights;
    toy::world::IRect splat_rect;    ///< Empty: no splat texels.
    std::vector<std::uint8_t> splat0, splat1;
    /// Whole-terrain patches (import, generate): the grid dimensions they were taken at. A patch
    /// whose dimensions no longer match the data (the terrain was resized since) is not applied.
    int nx = 0, ny = 0, splat_size = 0;

    std::size_t bytes() const { return heights.size() * sizeof(float) + splat0.size() + splat1.size(); }
};

/** @brief Copies `height_rect` / `splat_rect` of `data` into a patch. */
TerrainPatch capture_patch(const std::shared_ptr<toy::world::HeightmapData>& data, const toy::world::IRect& height_rect,
                           const toy::world::IRect& splat_rect);

/** @brief Copies the same rectangles out of saved full copies (a stroke's starting state). */
TerrainPatch capture_patch_from(const std::shared_ptr<toy::world::HeightmapData>& data, const std::vector<float>& heights,
                                const std::vector<std::uint8_t>& splat0, const std::vector<std::uint8_t>& splat1,
                                const toy::world::IRect& height_rect, const toy::world::IRect& splat_rect);

/** @brief Writes a patch back (marking its texels dirty). False when the data is gone or resized. */
bool apply_patch(const TerrainPatch& patch);

} // namespace toy::editor

#endif // TOYEDITOR_TERRAIN_TERRAIN_BRUSH_H

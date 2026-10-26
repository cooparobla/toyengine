/**
 * @file terrain_types.h
 * @brief The vocabulary both terrain modes share: the dirty-rectangle type, the mode switch, the
 *        water kinds, and one authored surface layer.
 *
 * A terrain is a grid of tiles in either mode (see terrain_field.h). What differs is the
 * sub-resolution: one height sample per tile gives the terraced, tile-set-built world, several
 * give a smooth heightmap. Everything in this header is common to both, so it sits below both
 * `heightmap_data.h` and `tile_types.h` and pulls in nothing from either.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_TYPES_H
#define TOYENGINE_WORLD_TERRAIN_TYPES_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

namespace toy {
namespace world {

/**
 * @brief Surface layers a terrain may address.
 *
 * Sixteen because a tile stores its surface in a 4-bit nibble (TileRecord::kinds), which is also
 * the size of the palette atlas. The built-in tile layers occupy the first `k_tile_kind_count`.
 */
inline constexpr int k_terrain_layer_count = 16;

/**
 * @brief Layers splat painting reaches: two RGBA8 maps, one channel each.
 *
 * Layers past this are addressable -- a tile may name one, a material may describe one -- but
 * cannot be painted by weight until a third splat map exists.
 */
inline constexpr int k_heightmap_layer_count = 8;

/** @brief How a terrain's tiles carry their shape, and so how its chunks are drawn. */
enum class TerrainMode : std::uint8_t {
    /** @brief Several height samples per tile: a smooth grid, drawn as GPU-displaced patches. */
    Heightmap,
    /** @brief One height step per tile: a terraced world, drawn from a tile set's pieces. */
    Tiles
};

/** @brief `mode:` as scene YAML spells it; unknown text reads as Tiles. */
TerrainMode terrain_mode_from_name(const std::string& name);
/** @brief The YAML spelling of `mode`. */
const char* terrain_mode_name(TerrainMode mode);

/**
 * @brief What the water over a tile is.
 *
 * The kind decides how the water is drawn and how it spreads: Ocean and Lake are still and share
 * one surface mesh per chunk, River flows toward its tile's `flow` octant and gets its own. Water
 * is always a surface over the ground, never geometry in the ground.
 */
enum class WaterKind : std::uint8_t {
    None = 0,  /**< @brief Dry. */
    Ocean,     /**< @brief The sea: one body at one level over the whole world. */
    Lake,      /**< @brief Still water at a basin's spill level. */
    River      /**< @brief Flowing water in a cut channel, falling toward its mouth. */
};

/** @brief Number of distinct `WaterKind` values. */
inline constexpr std::size_t k_water_kind_count = 4;

/** @brief `WaterKind` as scene YAML and the editor spell it; unknown text reads as None. */
WaterKind water_kind_from_name(const std::string& name);
/** @brief The YAML spelling of `kind`. */
const char* water_kind_name(WaterKind kind);

/** @brief Still water (Ocean, Lake) shares one surface mesh; a River gets the flowing one. */
inline bool water_is_still(WaterKind kind) { return kind == WaterKind::Ocean || kind == WaterKind::Lake; }

/**
 * @struct IRect
 * @brief A half-open texel rectangle [x0, x1) x [y0, y1); empty when either extent is <= 0.
 */
struct IRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    bool empty() const { return x1 <= x0 || y1 <= y0; }
    int width() const { return std::max(0, x1 - x0); }
    int height() const { return std::max(0, y1 - y0); }

    /** @brief Grows this rectangle to cover `o` too (an empty side is ignored). */
    void include(const IRect& o) {
        if (o.empty()) return;
        if (empty()) { *this = o; return; }
        x0 = std::min(x0, o.x0); y0 = std::min(y0, o.y0);
        x1 = std::max(x1, o.x1); y1 = std::max(y1, o.y1);
    }

    /** @brief This rectangle clipped to [0, w) x [0, h). */
    IRect clipped(int w, int h) const {
        IRect r{std::max(0, x0), std::max(0, y0), std::min(w, x1), std::min(h, y1)};
        if (r.empty()) return IRect{};
        return r;
    }

    bool operator==(const IRect& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
};

/**
 * @struct TerrainLayer
 * @brief One authored surface a terrain can wear: its textures, how they tile, and how the
 *        surface responds to light.
 *
 * The same list serves both modes. A heightmap tile blends layers by the splat weights painted
 * over it; a tiles-mode tile names one layer per face through its 4-bit surface nibble. Which
 * layer a scatter system, a brush or a query means is therefore one index in both modes.
 */
struct TerrainLayer {
    std::string name;
    std::string albedo;            ///< Texture path (sRGB); empty paints the flat `tint`.
    std::string normal;            ///< Tangent-space normal map path; empty = flat.
    glm::vec3   tint{1.0f};        ///< Multiplies the albedo texture (or is the colour without one).
    float       tiling = 8.0f;     ///< Metres one texture repeat covers.
    float       roughness = 0.85f;
    float       metallic = 0.0f;
    float       normal_strength = 1.0f;
    bool        triplanar = false; ///< Project on three axes on steep slopes (cliffs) instead of top-down.
    float       triplanar_sharpness = 4.0f;

    /**
     * @brief The palette atlas cell this layer takes its colour and roughness from, or -1 for
     *        none.
     *
     * Set on the built-in tile layers (see tile_types.h), where the atlas is a palette and the
     * surface detail is drawn procedurally rather than sampled from `albedo`.
     */
    int atlas_cell = -1;

    /** @brief Draw this layer's detail procedurally from `tint` / the atlas cell, not from `albedo`. */
    bool procedural = false;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_TYPES_H

/**
 * @file terrain_field.h
 * @brief One terrain's data, in either mode: a grid of tiles where each tile owns a sub-grid of
 *        heights and a sub-grid of layer weights, plus the water that stands over it.
 *
 * ### The model
 *
 * A terrain is `tiles_x` x `tiles_y` **tiles** of `tile_size` metres. Per tile it stores a record
 * (surface layers, climate, map ids) and a water cell. The detail inside a tile is the
 * **sub-resolution**:
 *
 * | | `TerrainMode::Tiles` | `TerrainMode::Heightmap` |
 * |---|---|---|
 * | height | one step per tile, quantised to `height_step` | `height_sub` + 1 samples per tile edge, continuous metres |
 * | layers | one layer index per tile face (a 4-bit nibble) | `layer_sub`^2 splat texels per tile, up to eight weights each |
 * | drawn as | a welded mesh of tile-set pieces, per chunk | one shared patch per chunk, GPU-displaced |
 *
 * Storage stays **contiguous per plane**, because the GPU displacement map, the heightfield
 * collider and the PNG sidecars each need one array; a tile's sub-grid is a *view* into the plane
 * (tile_height_samples(), tile_layer_weights()), not a block of its own.
 *
 * ### Why a facade
 *
 * The two planes already exist and are already good at their jobs: HeightmapData owns the height
 * grid, the splats, bilinear sampling, normals, a surface raycast, resampling and an edit
 * journal; TileMapData owns the records, their journal, the column raycast and the `.tiles`
 * codec. This type owns one of each and presents one API over them, so a caller -- the foliage
 * scatter, GroundProbe, a brush, a gameplay query -- asks the same question in either mode.
 *
 * In `Tiles` mode there is no height plane at all: `TileRecord::steps` is the height, which is
 * what keeps `.tiles` files, the mesher and the tile edit operations byte-for-byte as they are.
 *
 * ### Coordinates
 *
 * "Local" is metres from the grid's (0, 0) corner, z up; the owning component adds its world
 * origin (see TerrainFieldView). Tile (tx, ty) covers local x in
 * `[tx * tile_size, (tx + 1) * tile_size)`. A height sample sits on a tile *corner*, so the plane
 * has one more sample per side than it has tiles; a layer texel is *cell-centred* inside its tile.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_FIELD_H
#define TOYENGINE_WORLD_TERRAIN_FIELD_H

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/world/heightmap_data.h>
#include <toyengine/world/terrain_types.h>
#include <toyengine/world/tile_map_data.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

/**
 * @struct WaterCell
 * @brief The water standing over one tile: what it is, where its surface is, and which way it
 *        runs.
 *
 * Depth is never stored -- it is `level - ground`, so the same cell describes water over a
 * terrace and water over smooth ground. `level` is metres in both modes; `Tiles` mode quantises
 * it to `height_step` on the way in and out.
 */
struct WaterCell {
    WaterKind    kind = WaterKind::None; /**< @brief Dry when None; `level` and `flow` are then unused. */
    float        level = 0.0f;           /**< @brief The water SURFACE height, metres. */
    std::uint8_t flow = 0;               /**< @brief A river's downstream octant, 1..8 (k_flow_directions); 0 none. */

    bool wet() const { return kind != WaterKind::None; }
};

/**
 * @struct TerrainFieldCursor
 * @brief How far a consumer has read each of a field's planes.
 *
 * One integer per plane, not one for the field: each backing store keeps its own revision
 * counter, so a single number could not be compared against either of them. A consumer -- a
 * scene's GPU copy, a collider, the foliage scatter, the chunk streamer -- holds its own cursor
 * and hands it back to dirty_since(), which is what lets any number of them follow the same data
 * without draining each other's changes.
 */
struct TerrainFieldCursor {
    std::uint64_t tiles = 0;   /**< @brief TileMapData::revision(). */
    std::uint64_t heights = 0; /**< @brief HeightmapData::revision(); 0 on a Tiles field. */

    bool operator==(const TerrainFieldCursor& o) const { return tiles == o.tiles && heights == o.heights; }
    bool operator!=(const TerrainFieldCursor& o) const { return !(*this == o); }
};

/**
 * @struct TerrainFieldGeometry
 * @brief The authored shape of a field: its tile grid, its sub-resolutions and its vertical scale.
 *
 * Changing any of it reshapes the data (heights resampled, splats resampled, records kept where
 * they still exist), so the editor stages these behind an Apply.
 */
struct TerrainFieldGeometry {
    /** @brief Which model the tiles carry their shape in. */
    TerrainMode mode = TerrainMode::Tiles;

    int tiles_x = 0; /**< @brief Tiles along +x. */
    int tiles_y = 0; /**< @brief Tiles along +y. */

    /**
     * @brief Metres covered, x and y.
     *
     * `Tiles` mode keeps tiles square, so this is `tiles * tile_size`. `Heightmap` mode may
     * cover a rectangle with a square sample grid, exactly as HeightmapGeometry does, which
     * makes cell() rectangular.
     */
    glm::vec2 size{0.0f};

    /**
     * @brief Height samples per tile edge; the grid is `tiles * height_sub + 1` per side.
     *
     * Forced to 1 in `Tiles` mode (a tile is one step). Raising it in `Heightmap` mode is what
     * "sub-resolution determines detail" means: `tile_size: 8` with `height_sub: 8` is a metre
     * per sample over eight-metre tiles.
     */
    int height_sub = 1;

    /** @brief Layer (splat) texels per tile edge; the plane is `tiles * layer_sub` per side. */
    int layer_sub = 1;

    /** @brief Metres at a normalised height of 1 -- the vertical scale of a `Heightmap` field. */
    float height = 64.0f;

    /** @brief Metres per step: the vertical quantum of a `Tiles` field. */
    float height_step = 1.0f;

    /** @brief Tiles per chunk edge -- the streaming, drawing and collision unit in both modes. */
    int chunk_tiles = 16;

    /** @brief Metres per tile edge along x. Tiles mode keeps this equal along y. */
    float tile_size() const {
        return tiles_x > 0 ? size.x / static_cast<float>(tiles_x) : 1.0f;
    }

    /** @brief Metres per tile, x and y (they differ only on a rectangular heightmap). */
    glm::vec2 tile_extent() const {
        return glm::vec2(tiles_x > 0 ? size.x / static_cast<float>(tiles_x) : 1.0f,
                         tiles_y > 0 ? size.y / static_cast<float>(tiles_y) : 1.0f);
    }

    /** @brief Height samples per side along x (`Heightmap` mode). */
    int height_nx() const { return tiles_x * height_sub + 1; }
    /** @brief Height samples per side along y (`Heightmap` mode). */
    int height_ny() const { return tiles_y * height_sub + 1; }
    /** @brief Layer texels along x. */
    int layer_nx() const { return tiles_x * layer_sub; }
    /** @brief Layer texels along y. */
    int layer_ny() const { return tiles_y * layer_sub; }

    /** @brief Metres between adjacent height samples, x and y. */
    glm::vec2 cell() const {
        const int nx = height_nx(), ny = height_ny();
        return glm::vec2(nx > 1 ? size.x / static_cast<float>(nx - 1) : size.x,
                         ny > 1 ? size.y / static_cast<float>(ny - 1) : size.y);
    }

    /** @brief Chunks along x and y (ceil, at least one each). */
    glm::ivec2 chunk_count() const;

    /** @brief Clamps every field into its supported range, and forces mode-only invariants. */
    TerrainFieldGeometry sanitized() const;

    /** @brief The HeightmapGeometry a `Heightmap` field's plane is kept in. */
    HeightmapGeometry heightmap_geometry() const;

    bool operator==(const TerrainFieldGeometry& o) const;
    bool operator!=(const TerrainFieldGeometry& o) const { return !(*this == o); }
};

/**
 * @struct TileHeights
 * @brief The height samples one tile owns, as a writable strided view into the field's plane.
 *
 * Mark what you write with TerrainField::mark_tiles_dirty().
 */
struct TileHeights {
    float* base = nullptr; /**< @brief Sample (0, 0); null when the tile is off the grid. */
    int stride = 0;        /**< @brief Samples per plane row. */
    int n = 0;             /**< @brief Samples per tile edge. */

    explicit operator bool() const { return base != nullptr; }
    float& at(int i, int j) { return base[static_cast<std::size_t>(j) * stride + i]; }
    float at(int i, int j) const { return base[static_cast<std::size_t>(j) * stride + i]; }
};

/**
 * @struct ConstTileHeights
 * @brief TileHeights, read-only.
 *
 * `n` is `height_sub + 1`: the tile's own corners, shared with its neighbours. A `Tiles` field
 * has no height plane -- its height is the record's step -- so the view is falsy there and
 * TerrainField::ground_at() is the way to ask.
 */
struct ConstTileHeights {
    const float* base = nullptr;
    int stride = 0;
    int n = 0;

    ConstTileHeights() = default;
    ConstTileHeights(const float* b, int row_stride, int edge) : base(b), stride(row_stride), n(edge) {}
    /** @brief A writable view, narrowed -- so a non-const field can still be asked read-only. */
    ConstTileHeights(const TileHeights& w) : base(w.base), stride(w.stride), n(w.n) {}

    explicit operator bool() const { return base != nullptr; }
    /** @brief Height in metres at sample (i, j) of this tile. */
    float at(int i, int j) const { return base[static_cast<std::size_t>(j) * stride + i]; }
};

class TerrainField;

/**
 * @struct TileLayerWeights
 * @brief The layer weights one tile owns: `n` x `n` texels of up to eight weights each.
 *
 * A handle rather than a pointer view, because the weights live in two interleaved RGBA8 planes
 * on a `Heightmap` field and in the record's surface nibble on a `Tiles` field (where `n` is 1
 * and the tile's layer has the whole weight). Reading one is the same call either way, which is
 * the point -- a brush, a scatter mask and a query all speak one layer index.
 */
struct TileLayerWeights {
    const TerrainField* field = nullptr;
    int tx = 0, ty = 0;
    int n = 0; /**< @brief Texels per tile edge (`layer_sub`, or 1 on a Tiles field). */

    explicit operator bool() const { return field != nullptr; }

    /** @brief Weight of `layer` at texel (i, j), 0..255. */
    std::uint8_t weight(int i, int j, int layer) const;
    /** @brief The layer with the greatest weight at texel (i, j). */
    int dominant(int i, int j) const;
};

/**
 * @class TerrainField
 * @brief See the file comment. Owns the planes; carries no per-frame logic and no GPU state.
 */
class TerrainField {
public:
    TerrainField() = default;

    /** @brief Process-unique id; consumers key caches on it, never on the address. */
    std::uint64_t uid() const;

    /**
     * @brief Reshapes to `geometry`, allocating the planes each mode needs.
     *
     * A `Tiles` field gets records only; a `Heightmap` field gets records and a height plane.
     * Everything is marked changed.
     */
    void reset(const TerrainFieldGeometry& geometry);

    /**
     * @brief Takes the shared stores a component acquired, and reads the geometry back off them.
     *
     * The stores outlive a component re-parse (TileMapStore, HeightmapStore), which is what
     * keeps unsaved editor strokes across the editor's parse-on-every-change.
     *
     * @param tiles   The record / water plane. Required.
     * @param heights The height plane; null on a `Tiles` field.
     */
    void adopt(std::shared_ptr<TileMapData> tiles, std::shared_ptr<HeightmapData> heights,
               const TerrainFieldGeometry& geometry);

    const TerrainFieldGeometry& geometry() const { return geometry_; }
    TerrainMode mode() const { return geometry_.mode; }
    bool empty() const { return !tiles_ || tiles_->empty(); }

    /** @brief The record / water plane; null before adopt(). */
    TileMapData* tiles() { return tiles_.get(); }
    const TileMapData* tiles() const { return tiles_.get(); }
    /** @brief The height plane; null on a `Tiles` field. */
    HeightmapData* heights() { return heights_.get(); }
    const HeightmapData* heights() const { return heights_.get(); }

    std::shared_ptr<TileMapData> tiles_shared() const { return tiles_; }
    std::shared_ptr<HeightmapData> heights_shared() const { return heights_; }

    // ------------------------------------------------------------------ tiles

    bool contains(glm::ivec2 tile) const { return tiles_ && tiles_->contains(tile.x, tile.y); }
    /** @brief True when local xy lies on the terrain. */
    bool contains(glm::vec2 local) const;
    /** @brief The tile covering local xy (unclamped: may be off the grid). */
    glm::ivec2 tile_at(glm::vec2 local) const;
    /** @brief Local xy of tile (tx, ty)'s centre. */
    glm::vec2 tile_centre(glm::ivec2 tile) const;

    /** @brief The record of a tile, clamped to the nearest edge tile; null on an empty field. */
    const TileRecord* record_at(glm::ivec2 tile) const;

    // ------------------------------------------------------------------ ground

    /**
     * @brief Ground height in metres under local xy -- under water, the bed.
     *
     * Bilinear over the height plane on a `Heightmap` field; the column's terrace on a `Tiles`
     * field. Clamped to the terrain's edge.
     */
    float ground_at(glm::vec2 local) const;

    /**
     * @brief Ground height in metres at a tile, clamped to the nearest edge tile.
     *
     * The same answer as `ground_at(tile_centre(tile))` without the round trip through local
     * metres -- which matters because the water operations walk thousands of tiles per flood.
     */
    float ground_at_tile(glm::ivec2 tile) const;

    /** @brief Unit surface normal at local xy. `Tiles` mode answers +Z on a top face. */
    glm::vec3 normal_at(glm::vec2 local) const;

    /** @brief Min and max ground height over chunk (cx, cy), its shared border included. */
    glm::vec2 chunk_height_range(int cx, int cy) const;

    /** @brief The height samples tile (tx, ty) owns; falsy off the grid or without a plane. */
    ConstTileHeights tile_height_samples(int tx, int ty) const;
    /** @brief The same, writable. */
    TileHeights tile_height_samples(int tx, int ty);

    // ------------------------------------------------------------------ layers

    /** @brief The layer weights tile (tx, ty) owns. */
    TileLayerWeights tile_layer_weights(int tx, int ty) const;

    /** @brief Normalised weight (0..1) of `layer` at local xy. */
    float layer_weight_at(int layer, glm::vec2 local) const;
    /** @brief All splat-paintable weights at local xy, normalised. */
    std::array<float, k_heightmap_layer_count> layer_weights_at(glm::vec2 local) const;
    /** @brief The layer with the greatest weight at local xy, or 0 on an empty field. */
    int dominant_layer_at(glm::vec2 local) const;

    // ------------------------------------------------------------------ water

    /** @brief The water over a tile, clamped to the nearest edge tile. */
    WaterCell water_at(glm::ivec2 tile) const;
    /** @brief The water over local xy. */
    WaterCell water_at(glm::vec2 local) const;

    /**
     * @brief Sets the water over a tile. Does NOT mark dirty -- a caller editing a region marks
     *        it once (see toyengine/world/terrain_water.h, which is this as operations).
     *
     * `Tiles` mode quantises `level` to the terrace at or below it; `Heightmap` mode keeps it
     * exact. Neither leaves water standing at or under the ground: a level that does not clear
     * the ground drains the tile.
     */
    void set_water(glm::ivec2 tile, const WaterCell& water);
    /** @brief Drains a tile. */
    void clear_water(glm::ivec2 tile) { set_water(tile, WaterCell{}); }

    // ------------------------------------------------------------------ change tracking

    /**
     * @brief Journals an edit covering a rectangle of TILES, in every plane it touches.
     *
     * One call per edited region, whatever was written in it -- heights, layers, water or the
     * record -- so a consumer following `dirty_since()` re-reads a region once.
     */
    void mark_tiles_dirty(const IRect& tiles);
    /** @brief Everything changed (a bake, a load, a resize). */
    void mark_all_dirty();

    /** @brief Where every plane stands now; a consumer keeps the one it last built from. */
    TerrainFieldCursor cursor() const;

    /**
     * @brief A revision for one chunk, so a consumer rebuilds only what actually moved.
     *
     * What the foliage scatter keys its per-chunk cache on: sculpting one hillside must not
     * re-scatter the whole terrain. A `Heightmap` field answers per chunk; a `Tiles` field has
     * no per-chunk revision of its own and answers with the grid's, so any tile edit re-scatters
     * every chunk -- correct, and coarser than it could be.
     */
    std::uint64_t chunk_revision(int cx, int cy) const;

    /**
     * @brief The union, in TILES, of everything edited in any plane since `since`.
     *
     * A conservative cover, never an exact set: the backing journals coalesce nearby entries to
     * stay bounded, so a small edit beside a large one is reported with it. Consumers re-read the
     * rectangle they are given, which is why covering is the contract and tightness is not.
     */
    IRect dirty_since(const TerrainFieldCursor& since) const;

    /**
     * @brief The tiles a height-texel rectangle covers.
     *
     * A height sample sits on a tile *corner*, so a texel belongs to the tiles on both sides of
     * it; the result is grown one tile each way and clipped to the grid.
     */
    IRect tiles_of_height_texels(const IRect& texels) const;

    /** @brief The tiles a layer-texel rectangle covers (texels are cell-centred inside a tile). */
    IRect tiles_of_layer_texels(const IRect& texels) const;

private:

    TerrainFieldGeometry geometry_;
    std::shared_ptr<TileMapData> tiles_;
    std::shared_ptr<HeightmapData> heights_;

    /**
     * @brief Water surface height per tile, metres -- the authority on a `Heightmap` field.
     *
     * Empty on a `Tiles` field, where the record's terrace (`steps + water_depth`) is the
     * authority and needs no second copy. The water's KIND and FLOW live in the record in both
     * modes, which is why only the level needs a plane of its own.
     */
    std::vector<float> water_level_;
};

/**
 * @brief A field that BORROWS an existing tile grid instead of owning it.
 *
 * For code that holds a bare `TileMapData` and wants the field operations over it -- the map
 * bake, and the tile edit operations, neither of which has a component to hand. The returned
 * field keeps the grid alive for exactly as long as the caller does, and no longer: it must not
 * outlive `tiles`.
 *
 * `height_step` defaults to 1, which makes a level in metres and a terrace in steps the same
 * number -- what a caller working in whole terraces wants.
 */
TerrainField borrow_tiles(TileMapData& tiles, float height_step = 1.0f, float tile_size = 1.0f);

/**
 * @struct TerrainFieldView
 * @brief A field plus where it sits in the world: the world-space query surface GroundProbe,
 *        the particle scatter and gameplay use without knowing about the component.
 */
struct TerrainFieldView {
    const TerrainField* field = nullptr;
    glm::vec3 origin{0.0f}; /**< @brief World position of the local (0, 0, 0) corner. */

    explicit operator bool() const { return field != nullptr; }
    glm::vec2 local(glm::vec3 world) const { return glm::vec2(world) - glm::vec2(origin); }
    bool contains(glm::vec3 world) const { return field && field->contains(local(world)); }
    /** @brief World height of the ground under a world position (the bed, under water). */
    float height(glm::vec3 world) const { return origin.z + field->ground_at(local(world)); }
    glm::vec3 normal(glm::vec3 world) const { return field->normal_at(local(world)); }
    float layer_weight(int layer, glm::vec3 world) const { return field->layer_weight_at(layer, local(world)); }
    /** @brief The water over a world position, its `level` in world space. */
    WaterCell water(glm::vec3 world) const;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_FIELD_H

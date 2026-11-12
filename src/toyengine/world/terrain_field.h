/**
 * @file terrain_field.h
 * @brief One terrain's data: a grid of tiles where each tile owns a sub-grid of heights and a
 *        sub-grid of layer weights, plus the water that stands over it.
 *
 * A thin, storage-owning wrapper over ONE `TerraData` (terra_data.h) -- the actual grid, height
 * plane, layer planes and change tracking live there. This type adds the local<->world geometry
 * helpers and the water-as-a-surface-level view every consumer (a brush, the foliage scatter,
 * GroundProbe, gameplay) already asks for, so none of them need to know whether the terrain under
 * them has a height plane (a "smooth" meshing style) or not (a "terraced" one):
 *
 * | | no height plane (`height_sub == 1`) | a height plane (`height_sub > 1`) |
 * |---|---|---|
 * | height | one step per tile, quantised to `height_step` | continuous metres, bilinear |
 * | layers | one layer index per tile (`TileCell::kinds`) | splat weights, up to eight per tile |
 * | drawn as | a welded mesh of tile-set pieces, per chunk | one shared patch per chunk, GPU-displaced |
 *
 * Which meshing style a terrain actually uses is `TerrainComponent::mode`'s business, not this
 * type's -- `TerrainField` answers the same way regardless.
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

#include <toyengine/world/terra_data.h>
#include <toyengine/world/terra_geometry.h>
#include <toyengine/world/terrain_types.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

/**
 * @struct WaterCell
 * @brief The water standing over one tile: what it is, where its surface is, and which way it
 *        runs.
 *
 * Depth is never stored -- it is `level - ground`, so the same cell describes water over a
 * terrace and water over smooth ground. `level` is metres, exact, in both.
 */
struct WaterCell {
    WaterKind    kind = WaterKind::None; /**< @brief Dry when None; `level` and `flow` are then unused. */
    float        level = 0.0f;           /**< @brief The water SURFACE height, metres. */
    std::uint8_t flow = 0;               /**< @brief A river's downstream octant, 1..8 (k_flow_directions); 0 none. */

    bool wet() const { return kind != WaterKind::None; }
};

/**
 * @struct TerrainFieldCursor
 * @brief How far a consumer has read each of a field's three planes (tiles, height, layers).
 *
 * Mirrors `TerraCursor` one-for-one; a thin wrapper so `TerrainField`'s own type stays the public
 * spelling consumers hold onto.
 */
struct TerrainFieldCursor {
    std::uint64_t tiles = 0;
    std::uint64_t heights = 0;
    std::uint64_t layers = 0;

    bool operator==(const TerrainFieldCursor& o) const {
        return tiles == o.tiles && heights == o.heights && layers == o.layers;
    }
    bool operator!=(const TerrainFieldCursor& o) const { return !(*this == o); }

    TerraCursor terra() const { return TerraCursor{tiles, heights, layers}; }
    static TerrainFieldCursor from_terra(const TerraCursor& c) {
        return TerrainFieldCursor{c.tiles, c.heights, c.layers};
    }
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
 * `n` is `height_sub + 1`: the tile's own corners, shared with its neighbours. A field with no
 * height plane has no height sub-grid at all -- its height is the tile's own `steps` -- so the
 * view is falsy there and TerrainField::ground_at() is the way to ask.
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
 * when a height plane exists, and in the tile's own `kinds` nibble otherwise (where `n` is 1 and
 * the tile's layer has the whole weight). Reading one is the same call either way, which is the
 * point -- a brush, a scatter mask and a query all speak one layer index.
 */
struct TileLayerWeights {
    const TerrainField* field = nullptr;
    int tx = 0, ty = 0;
    int n = 0; /**< @brief Texels per tile edge (`layer_sub`, or 1 without a height plane). */

    explicit operator bool() const { return field != nullptr; }

    /** @brief Weight of `layer` at texel (i, j), 0..255. */
    std::uint8_t weight(int i, int j, int layer) const;
    /** @brief The layer with the greatest weight at texel (i, j). */
    int dominant(int i, int j) const;
};

/**
 * @class TerrainField
 * @brief See the file comment. Owns one `TerraData`; carries no per-frame logic and no GPU state.
 */
class TerrainField {
public:
    TerrainField() = default;

    /** @brief Process-unique id; consumers key caches on it, never on the address. */
    std::uint64_t uid() const { return data_ ? data_->uid() : 0; }

    /** @brief Reshapes to `geometry`, allocating a fresh TerraData. Everything is marked changed. */
    void reset(const TerraGeometry& geometry);

    /**
     * @brief Takes a shared TerraData -- a component acquired it from a store, or a bake just
     *        finished one -- and reads this field's state off it from now on.
     *
     * The store outlives a component re-parse, which is what keeps unsaved editor strokes across
     * the editor's parse-on-every-change.
     */
    void attach(std::shared_ptr<TerraData> data);

    const TerraGeometry& geometry() const {
        static const TerraGeometry k_empty;
        return data_ ? data_->geometry() : k_empty;
    }
    /** @brief Whether this field is drawn with a continuous height plane or terraced `steps`. */
    bool has_height_plane() const { return data_ && data_->has_heights(); }
    /** @brief `Heightmap` when a height plane exists, `Tiles` otherwise -- a convenience for
     *         callers that still think in the old two-mode vocabulary (e.g. an epsilon that
     *         differs between a quantised and a continuous field). */
    TerrainMode mode() const { return has_height_plane() ? TerrainMode::Heightmap : TerrainMode::Tiles; }
    bool empty() const { return !data_ || data_->empty(); }

    /** @brief The backing store; null before reset()/attach(). */
    TerraData* data() { return data_.get(); }
    const TerraData* data() const { return data_.get(); }
    std::shared_ptr<TerraData> data_shared() const { return data_; }

    // ------------------------------------------------------------------ tiles

    bool contains(glm::ivec2 tile) const { return data_ && data_->contains(tile.x, tile.y); }
    /** @brief True when local xy lies on the terrain. */
    bool contains(glm::vec2 local) const { return data_ && data_->contains(local); }
    /** @brief The tile covering local xy (unclamped: may be off the grid). */
    glm::ivec2 tile_at(glm::vec2 local) const { return data_ ? data_->tile_at(local) : glm::ivec2(0); }
    /** @brief Local xy of tile (tx, ty)'s centre. */
    glm::vec2 tile_centre(glm::ivec2 tile) const { return data_ ? data_->tile_centre(tile) : glm::vec2(0.0f); }

    /** @brief The cell of a tile, clamped to the nearest edge tile; null on an empty field. */
    const TileCell* record_at(glm::ivec2 tile) const;

    // ------------------------------------------------------------------ ground

    /**
     * @brief Ground height in metres under local xy -- under water, the bed.
     *
     * Bilinear over the height plane when one exists; the tile's own `steps` otherwise. Clamped
     * to the terrain's edge.
     */
    float ground_at(glm::vec2 local) const { return data_ ? data_->ground_at(local) : 0.0f; }

    /**
     * @brief Ground height in metres at a tile, clamped to the nearest edge tile.
     *
     * The same answer as `ground_at(tile_centre(tile))` without the round trip through local
     * metres -- which matters because the water operations walk thousands of tiles per flood.
     */
    float ground_at_tile(glm::ivec2 tile) const { return data_ ? data_->ground_at_tile(tile) : 0.0f; }

    /** @brief Unit surface normal at local xy. No height plane: +Z, a column's top is flat. */
    glm::vec3 normal_at(glm::vec2 local) const {
        return data_ ? data_->normal_at(local) : glm::vec3(0.0f, 0.0f, 1.0f);
    }

    /** @brief Min and max ground height over chunk (cx, cy), its shared border included. */
    glm::vec2 chunk_height_range(int cx, int cy) const { return data_ ? data_->chunk_minmax(cx, cy) : glm::vec2(0.0f); }

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
    WaterCell water_at(glm::vec2 local) const { return water_at(tile_at(local)); }

    /**
     * @brief Sets the water over a tile. Does NOT mark dirty -- a caller editing a region marks
     *        it once (see toyengine/world/terrain_water.h, which is this as operations).
     *
     * The level is kept exact, whether or not this field has a height plane -- quantising to a
     * terraced mesh's steps is the mesher's business (terrain_chunk.h's column sampling), not the
     * data's. A level that does not clear the ground drains the tile.
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
    void mark_tiles_dirty(const IRect& tiles) { if (data_) data_->mark_tiles_dirty(tiles); }
    /** @brief Everything changed (a bake, a load, a resize). */
    void mark_all_dirty() { if (data_) data_->mark_all_dirty(); }

    /** @brief Where every plane stands now; a consumer keeps the one it last built from. */
    TerrainFieldCursor cursor() const {
        return data_ ? TerrainFieldCursor::from_terra(data_->cursor()) : TerrainFieldCursor{};
    }

    /**
     * @brief A revision for one chunk, so a consumer rebuilds only what actually moved.
     *
     * What the foliage scatter keys its per-chunk cache on: sculpting one hillside must not
     * re-scatter the whole terrain.
     */
    std::uint64_t chunk_revision(int cx, int cy) const { return data_ ? data_->chunk_revision(cx, cy) : 0; }

    /**
     * @brief The union, in TILES, of everything edited in any plane since `since`.
     *
     * A conservative cover, never an exact set: the backing journals coalesce nearby entries to
     * stay bounded, so a small edit beside a large one is reported with it.
     */
    IRect dirty_since(const TerrainFieldCursor& since) const {
        return data_ ? data_->dirty_since(since.terra()) : IRect{};
    }

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
    std::shared_ptr<TerraData> data_;
};

/**
 * @brief A field that BORROWS an existing TerraData instead of owning it.
 *
 * For code that holds a bare `TerraData` and wants the field operations over it -- the map bake,
 * and the tile edit operations, neither of which has a component to hand. The returned field
 * keeps the data alive for exactly as long as the caller does, and no longer: it must not outlive
 * `data`.
 */
TerrainField borrow(TerraData& data);

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

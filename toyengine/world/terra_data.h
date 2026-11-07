/**
 * @file terra_data.h
 * @brief `TerraData` -- the one CPU store a terrain's shape, layers, water, biome and climate
 *        live in, whichever meshing style draws it.
 *
 * ONE store with four planes, always allocated by the same rule regardless of meshing style:
 *
 * | plane | element | extent | always present? |
 * |---|---|---|---|
 * | tiles  | `TileCell` (24 B) | `tiles_x` x `tiles_y` | yes |
 * | height | `float`, metres   | `tiles_x*height_sub+1` x `tiles_y*height_sub+1` | only if `height_sub > 1` |
 * | layer0 | RGBA8 (layers 0-3) | `tiles_x*layer_sub` x `tiles_y*layer_sub` | yes |
 * | layer1 | RGBA8 (layers 4-7) | same | yes |
 *
 * A tile's `steps` is always the authoritative integer height a terraced/blocky meshing style
 * reads. On a field with a height plane, `steps` is a **maintained derived cache** of that
 * plane (see the private `refresh_steps_()` below, called from `mark_height_dirty()`) -- the
 * two are never independently authoritative, because a cliff has no representable shared
 * corner sample between two tiles at different heights.
 *
 * Pure CPU: no Vulkan, no Scene, no file I/O (that is `terra_codec.h`) -- unit-testable with
 * nothing else.
 *
 * ### Coordinates
 *
 * "Local" is metres from the grid's (0, 0) corner, z up. Tile (tx, ty) covers local x in
 * `[tx * tile_size, (tx + 1) * tile_size)`. A height sample sits on a tile *corner* (one more
 * sample per side than there are tiles); a layer texel is *cell-centred* inside its tile.
 *
 * ### Change tracking
 *
 * Three journals -- one per plane group (tiles, height, layers) -- each in that plane's own
 * native space, bumping one shared per-render-chunk revision table so a consumer (the foliage
 * scatter, the chunk streamer) can ask "did chunk (cx, cy) change at all" without caring which
 * plane moved. `cursor()` / `dirty_since()` report the union in TILE space (the one space every
 * consumer of `TerrainField` already reads in); `height_dirty_since()` / `layer_dirty_since()`
 * report their own plane's native texel space, for a GPU incremental upload.
 */

#ifndef TOYENGINE_WORLD_TERRA_DATA_H
#define TOYENGINE_WORLD_TERRA_DATA_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/world/terra_geometry.h>
#include <toyengine/world/terrain_types.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

/**
 * @struct TileCell
 * @brief Everything stored about one tile: 24 bytes.
 *
 * `steps` is always the ground -- under water, the bed. Water is a surface over it: never a
 * depth in whole steps (as `TileRecord::water_depth` was), but an absolute metres level, exact
 * in both a terraced and a smooth field. The climate fields are mapcoopa's own values,
 * quantised, exactly as `TileRecord`'s were.
 */
struct TileCell {
    std::int16_t  steps = 0;          /**< @brief Ground height in whole steps (the bed, under water). */
    std::uint8_t  kinds = 0x10;       /**< @brief Top kind low nibble, side kind high nibble. */
    std::uint8_t  flags = 0;          /**< @brief Coast | River | Road | Town (see Flag). */
    std::uint8_t  biome = 0;          /**< @brief coopa::maps::Biome. */
    std::uint8_t  temperature = 128;  /**< @brief mapcoopa temperature, 0..255 for 0..1. */
    std::uint8_t  moisture = 128;     /**< @brief mapcoopa moisture, 0..255 for 0..1. */
    std::uint8_t  water_kind = 0;     /**< @brief WaterKind, as a value -- not three lossy flag bits. */
    float         water_level = 0.0f; /**< @brief The water SURFACE, absolute metres. Unused when dry. */
    std::uint8_t  flow = 0;           /**< @brief A river's downstream octant, 1..8; 0 none. */
    std::uint8_t  flow_authored = 0;  /**< @brief 1: a human set `flow`; derive_water_flow() leaves it alone. */
    std::uint16_t region = 0xFFFFu;   /**< @brief mapcoopa region id; 0xFFFF for none. */
    std::uint16_t country = 0xFFFFu;  /**< @brief mapcoopa country id; 0xFFFF for none. */
    std::uint16_t reserved[3] = {0, 0, 0}; /**< @brief Zero; kept for later per-tile data. */

    /// Flag bits (water's kind moved to `water_kind`; `Water`/`Ocean`/`Flowing` are gone with it).
    enum Flag : std::uint8_t {
        Coast = 1u << 0, /**< @brief Land next to the ocean. */
        River = 1u << 1, /**< @brief A river channel cuts this tile. */
        Road  = 1u << 2, /**< @brief A roadway runs over this tile. */
        Town  = 1u << 3, /**< @brief This tile is inside a settlement. */
    };
    static constexpr std::uint16_t k_no_id = 0xFFFFu;

    TileKind top_kind() const { return static_cast<TileKind>(kinds & 0x0Fu); }
    TileKind side_kind() const { return static_cast<TileKind>(kinds >> 4); }
    void set_top_kind(TileKind k) { kinds = static_cast<std::uint8_t>((kinds & 0xF0u) | (static_cast<unsigned>(k) & 0x0Fu)); }
    void set_side_kind(TileKind k) { kinds = static_cast<std::uint8_t>((kinds & 0x0Fu) | ((static_cast<unsigned>(k) & 0x0Fu) << 4)); }
    bool has(Flag f) const { return (flags & f) != 0; }
    void set(Flag f, bool on) { flags = static_cast<std::uint8_t>(on ? (flags | f) : (flags & ~f)); }

    bool wet() const { return water_kind != 0; }

    /** @brief mapcoopa's 0..1 temperature / moisture, and the quantiser the other way. */
    float temperature01() const { return static_cast<float>(temperature) / 255.0f; }
    float moisture01() const { return static_cast<float>(moisture) / 255.0f; }
    static std::uint8_t quantise01(double v) {
        const double c = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        return static_cast<std::uint8_t>(c * 255.0 + 0.5);
    }

    bool operator==(const TileCell& o) const;
    bool operator!=(const TileCell& o) const { return !(*this == o); }
};
static_assert(sizeof(TileCell) == 24, "TileCell is the on-disk record: keep it 24 bytes");

/**
 * @brief The column a terraced/blocky mesher builds from one cell: the ground, and the water
 *        over it, quantised to whole steps.
 *
 * The one place a cell's exact-metres `water_level` is rounded down to the terrace it fills --
 * the mesher always reads a real integer plane (`TileCell::steps`), never a fresh quantisation
 * of a height plane, which is what keeps mesh_chunk_styled()'s exact-integer face comparisons
 * exact. Water in the pad: floor(level / height_step).
 */
inline TileColumn tile_column(const TileCell& c, float height_step) {
    TileColumn out;
    out.steps = c.steps < 0 ? 0 : c.steps;
    out.top_kind = c.top_kind();
    out.side_kind = c.side_kind();
    if (c.wet() && height_step > 0.0f) {
        out.water_level = static_cast<std::int32_t>(std::floor(c.water_level / height_step + 1e-4f));
        out.water = c.water_kind == static_cast<std::uint8_t>(WaterKind::River) ? k_water_flowing : k_water_still;
        out.flow = out.water == k_water_flowing ? c.flow : 0;
    }
    return out;
}

/**
 * @struct TileHit
 * @brief A ray's hit on the tile columns (`TerraData::raycast_columns()`).
 */
struct TileHit {
    glm::ivec2 tile{0};
    glm::vec3  point{0.0f};
    glm::vec3  normal{0.0f, 0.0f, 1.0f};
    float      t = 0.0f;
};

/**
 * @struct TerraCursor
 * @brief How far a consumer has read each of the three journals.
 */
struct TerraCursor {
    std::uint64_t tiles = 0;
    std::uint64_t heights = 0;
    std::uint64_t layers = 0;

    bool operator==(const TerraCursor& o) const { return tiles == o.tiles && heights == o.heights && layers == o.layers; }
    bool operator!=(const TerraCursor& o) const { return !(*this == o); }
};

/**
 * @class TerraData
 * @brief See the file comment.
 */
class TerraData {
public:
    TerraData();
    explicit TerraData(const TerraGeometry& geometry);

    /** @brief Process-unique id; consumers key caches on it, never on the address. */
    std::uint64_t uid() const { return uid_; }

    // ---------------------------------------------------------------- shape

    /** @brief Reallocates every plane to `geometry`: flat ground, layer 0 everywhere, no water. */
    void reset(const TerraGeometry& geometry);

    /**
     * @brief Changes the geometry, keeping the content: resamples the height and layer planes
     *        (bilinear) and rescales heights so the same normalised shape spans the new
     *        `height`; the tile plane is kept where it still exists and default elsewhere.
     */
    void reshape(const TerraGeometry& geometry);

    /**
     * @brief Replaces geometry and content with `src`'s, keeping this object's revisions moving
     *        forward (a restored snapshot must never look older to a consumer than what it
     *        built).
     */
    void assign_content(const TerraData& src);

    const TerraGeometry& geometry() const { return geometry_; }
    bool empty() const { return geometry_.tiles_x <= 0 || geometry_.tiles_y <= 0; }
    bool contains(int tx, int ty) const { return tx >= 0 && ty >= 0 && tx < geometry_.tiles_x && ty < geometry_.tiles_y; }
    /** @brief True when local xy lies on the terrain. */
    bool contains(glm::vec2 local) const;
    /** @brief The tile covering local xy (unclamped: may be off the grid). */
    glm::ivec2 tile_at(glm::vec2 local) const;
    /** @brief Local xy of tile (tx, ty)'s centre. */
    glm::vec2 tile_centre(glm::ivec2 tile) const;

    // ---------------------------------------------------------------- tile plane

    /** @brief Unchecked access. Mark what you write with mark_tiles_dirty(). */
    TileCell& at(int tx, int ty) { return tiles_[index_(tx, ty)]; }
    const TileCell& at(int tx, int ty) const { return tiles_[index_(tx, ty)]; }
    /** @brief Clamped read: outside the grid answers the nearest edge tile. */
    const TileCell& get(int tx, int ty) const;

    const std::vector<TileCell>& tiles() const { return tiles_; }

    /** @brief Copies `rect`'s tiles out, row-major (for undo). */
    std::vector<TileCell> capture(const IRect& rect) const;
    /** @brief Writes tiles captured from `rect` back and marks it dirty. */
    void restore(const IRect& rect, const std::vector<TileCell>& cells);

    // ---------------------------------------------------------------- height plane

    /** @brief True when a height plane exists (geometry().height_sub > 1). */
    bool has_heights() const { return !heights_.empty(); }
    int height_nx() const { return geometry_.height_nx(); }
    int height_ny() const { return geometry_.height_ny(); }

    std::vector<float>& heights() { return heights_; }
    const std::vector<float>& heights() const { return heights_; }
    float& height_at(int i, int j) { return heights_[static_cast<std::size_t>(j) * height_nx() + i]; }
    float height_at(int i, int j) const { return heights_[static_cast<std::size_t>(j) * height_nx() + i]; }
    float height_at_clamped(int i, int j) const {
        return height_at(std::clamp(i, 0, height_nx() - 1), std::clamp(j, 0, height_ny() - 1));
    }

    /** @brief Metres between adjacent height samples (uniform: tiles are square). */
    float cell() const { return geometry_.cell(); }
    /** @brief Height texel coordinates (fractional) of a local position. */
    glm::vec2 to_texel(glm::vec2 local) const { return local / cell(); }

    /**
     * @brief Ground height in metres under local xy -- under water, the bed.
     *
     * Bilinear over the height plane when one exists; the tile's own `steps` otherwise. Clamped
     * to the terrain's edge. This is the one query a meshing style's choice never shows through.
     */
    float ground_at(glm::vec2 local) const;
    /**
     * @brief Ground height in metres at a tile, clamped to the nearest edge tile.
     *
     * The same answer as `ground_at(tile_centre(tile))` without the round trip through local
     * metres -- which matters because the water operations walk thousands of tiles per flood.
     */
    float ground_at_tile(glm::ivec2 tile) const;
    /** @brief Unit surface normal at local xy. +Z with no height plane (a column's top is flat). */
    glm::vec3 normal_at(glm::vec2 local) const;

    /** @brief Min and max height inside a height-texel rectangle (inclusive of its far edge). */
    glm::vec2 minmax(const IRect& texels) const;
    /** @brief Min and max ground height over render chunk (cx, cy), shared border included. */
    glm::vec2 chunk_minmax(int cx, int cy) const;
    /** @brief Min and max height over the whole plane (cached per height revision). */
    glm::vec2 height_range() const;

    // ---------------------------------------------------------------- layer planes

    int layer_nx() const { return geometry_.layer_nx(); }
    int layer_ny() const { return geometry_.layer_ny(); }

    /** @brief Layer plane `which` (0 = layers 0-3, 1 = layers 4-7), RGBA8, row-major. */
    std::vector<std::uint8_t>& layer_plane(int which) { return layers_[static_cast<std::size_t>(which)]; }
    const std::vector<std::uint8_t>& layer_plane(int which) const { return layers_[static_cast<std::size_t>(which)]; }
    /** @brief Weight byte of `layer` at layer texel (i, j). */
    std::uint8_t& layer_at(int layer, int i, int j) {
        return layers_[static_cast<std::size_t>(layer / 4)][(static_cast<std::size_t>(j) * layer_nx() + i) * 4 + (layer % 4)];
    }
    std::uint8_t layer_at(int layer, int i, int j) const {
        return layers_[static_cast<std::size_t>(layer / 4)][(static_cast<std::size_t>(j) * layer_nx() + i) * 4 + (layer % 4)];
    }

    /** @brief Normalised weight (0..1) of `layer` at local xy, bilinear splat. With no height plane: 1 if `layer == the tile's top_kind()`, else 0. */
    float layer_weight(int layer, glm::vec2 local) const;
    /** @brief All eight splat-paintable weights at local xy, normalised. */
    std::array<float, k_heightmap_layer_count> layer_weights(glm::vec2 local) const;
    /** @brief Paints every layer texel 100% `layer`. */
    void fill_layer(int layer);
    /** @brief Renormalises a layer texel so its eight weights sum to 255, favouring `keep`. */
    void normalize_layer_texel(int i, int j, int keep = -1);

    // ---------------------------------------------------------------- change tracking

    /** @brief Journals a TILE rect, in every plane it touches; bumps the chunks it overlaps. */
    void mark_tiles_dirty(const IRect& tiles);
    /** @brief Journals a height-SAMPLE rect; bumps the render chunks it overlaps. No-op without a height plane. */
    void mark_height_dirty(const IRect& samples);
    /** @brief Journals a layer-TEXEL rect; bumps the render chunks it overlaps. */
    void mark_layer_dirty(const IRect& texels);
    /** @brief Everything changed (a bake, a load, a reshape): every consumer re-reads everything. */
    void mark_all_dirty();

    TerraCursor cursor() const;
    /** @brief The union, in TILES, of everything edited in any plane since `since`. A conservative cover. */
    IRect dirty_since(const TerraCursor& since) const;
    /** @brief Calls `fn(rect)` (TILE space) for every rect edited in any plane since `since`. */
    void for_each_dirty_since(const TerraCursor& since, const std::function<void(const IRect&)>& fn) const;
    /** @brief Height texels edited since `since` (native sample space; for a GPU incremental upload). */
    IRect height_dirty_since(std::uint64_t since) const;
    /** @brief Layer texels edited since `since` (native texel space; for a GPU incremental upload). */
    IRect layer_dirty_since(std::uint64_t since) const;

    /** @brief Bumped by every edit to the tile plane; the coarse "something changed" counter. */
    std::uint64_t revision() const { return tiles_revision_; }
    /** @brief Bumped by every edit to the height plane alone -- a collider rebuild's own debounce. */
    std::uint64_t height_revision() const { return height_revision_; }
    /** @brief Bumped by every edit to a layer plane alone. */
    std::uint64_t layer_revision() const { return layer_revision_; }
    /** @brief A revision for one render chunk, bumped by an edit to ANY plane inside it. */
    std::uint64_t chunk_revision(int cx, int cy) const;

    /** @brief True when there are edits not yet written to disk. */
    bool dirty_on_disk() const { return unsaved_; }
    void set_saved() { unsaved_ = false; }

    // ---------------------------------------------------------------- baking (owned by the terrain that fills it)

    /** @brief True until a bake (or a load) has filled the grid -- or after request_bake(). */
    bool needs_bake() const { return needs_bake_; }
    /** @brief Asks the owning terrain to bake the world again (the editor's Regenerate). */
    void request_bake() { needs_bake_ = true; ++bake_requests_; }
    /** @brief A load (or a hand sculpt starting from an edit) has committed real content: no bake needed. */
    void clear_needs_bake() { needs_bake_ = false; }
    std::uint64_t bake_requests() const { return bake_requests_; }
    std::uint64_t bakes_done() const { return bakes_done_; }
    /** @brief Takes `baked`'s content wholesale (a fresh bake): everything changes, the result is unsaved. */
    void assign_baked(TerraData&& baked);

    // ---------------------------------------------------------------- queries

    /** @brief Casts a ray (terrain-local space) against the tile columns: steps up to `max_t`. */
    bool raycast_columns(const glm::vec3& origin, const glm::vec3& dir, float max_t, TileHit& hit) const;
    /** @brief Ray against the height plane's surface. Requires has_heights(). */
    bool raycast_surface(glm::vec3 origin, glm::vec3 dir, float max_t, float& t) const;

private:
    /** @brief Bilinear height at local xy over the height plane directly. Requires has_heights(). */
    float bilinear_height_(glm::vec2 local) const;

    /** @brief Mean of a tile's corner samples, rounded to the nearest step. Requires has_heights(). */
    std::int16_t derive_steps_(int tx, int ty) const;
    /** @brief Writes derive_steps_() into every tile's `steps` in `tiles` -- the one place `steps`
     *         is written when a height plane exists (see the file comment). */
    void refresh_steps_(const IRect& tiles);

    std::size_t index_(int tx, int ty) const {
        return static_cast<std::size_t>(ty) * static_cast<std::size_t>(geometry_.tiles_x) + static_cast<std::size_t>(tx);
    }
    void bump_render_chunks_(const IRect& tile_rect);

    struct JournalEntry {
        std::uint64_t revision;
        IRect rect;
    };
    static void journal_(std::vector<JournalEntry>& journal, std::uint64_t& floor, std::uint64_t revision,
                         const IRect& rect);
    static IRect dirty_since_(const std::vector<JournalEntry>& journal, std::uint64_t floor, std::uint64_t since,
                              const IRect& full);
    static void for_each_dirty_since_(const std::vector<JournalEntry>& journal, std::uint64_t floor,
                                      std::uint64_t since, const IRect& full,
                                      const std::function<void(const IRect&)>& fn);

    std::uint64_t uid_ = 0;
    TerraGeometry geometry_;
    std::vector<TileCell> tiles_;
    std::vector<float> heights_;                    ///< Empty when !geometry_.has_height_plane().
    std::array<std::vector<std::uint8_t>, 2> layers_;

    std::vector<JournalEntry> tiles_journal_;
    std::vector<JournalEntry> height_journal_;
    std::vector<JournalEntry> layer_journal_;
    std::uint64_t tiles_floor_ = 0, height_floor_ = 0, layer_floor_ = 0;
    std::uint64_t tiles_revision_ = 1, height_revision_ = 1, layer_revision_ = 1;
    /** @brief Bumped by an edit to ANY plane; the stamp chunk_revisions_ entries are set to. */
    std::uint64_t revision_ = 1;
    std::vector<std::uint64_t> chunk_revisions_;     ///< One per render chunk (geometry_.chunk_tiles).

    bool unsaved_ = false;
    bool needs_bake_ = true;
    std::uint64_t bake_requests_ = 0;
    std::uint64_t bakes_done_ = 0;

    mutable glm::vec2 height_range_{0.0f};
    mutable std::uint64_t height_range_revision_ = 0;

    static std::uint64_t next_uid_();
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRA_DATA_H

/**
 * @file tile_map_data.h
 * @brief A TileTerrain's tiles, stored: one 16-byte TileRecord per tile, baked once from the
 *        mapcoopa world and edited from then on -- by the editor's Tile Mode and, through the
 *        same calls, by gameplay at runtime.
 *
 * Pure CPU, like HeightmapData (heightmap_data.h): no Vulkan, no Scene, so it is unit-tested
 * directly and the editor's brushes are plain functions on it.
 *
 * ### Editing
 *
 * Write records (`at()`), then `mark_dirty(rect)`. That bumps `revision()` and journals the
 * rect; every consumer keeps its own "seen" revision and asks `for_each_dirty_since()` which
 * rects changed -- TerrainSystem re-meshes the chunks they touch, the editor re-reads its
 * hover info. Consumers never drain each other's changes. The journal is bounded: a consumer
 * that fell further behind than it reaches is told to treat the whole grid as changed.
 *
 * ### Files
 *
 * `save_tile_map()` / `load_tile_map()` write a `.tiles` file: a header, a chunk index, then one
 * deflated block of records per 32 x 32 storage chunk. Blocks are kept compressed in memory
 * after a load or save, so a save recompresses only the chunks edited since.
 *
 * ### Threads
 *
 * Owner thread only. A chunk-meshing job never reads this: TerrainSystem copies the chunk's
 * columns out (sample_chunk_columns()) when it submits the job, so an edit while the job runs
 * can never tear what the job sees.
 */

#ifndef TOYENGINE_WORLD_TILE_MAP_DATA_H
#define TOYENGINE_WORLD_TILE_MAP_DATA_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/world/heightmap_data.h>   // IRect
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

/**
 * @struct TileRecord
 * @brief Everything stored about one tile: 16 bytes.
 *
 * `steps` is always the **ground** -- under water, the bed. Water is a surface over it:
 * `water_depth` steps of it, filling the column to the terrace `water_level()`, drawn
 * `TerrainParams::water_inset` below that terrace's top by the chunk's water surface (never as
 * blocks). A dry tile has `water_depth == 0`.
 *
 * The climate fields are mapcoopa's own values, quantised: `temperature` and `moisture` map
 * `[0, 255]` onto mapcoopa's `[0, 1]` (temperature 0 polar, 1 equatorial -- a static yearly
 * mean; mapcoopa has no seasons). `region` / `country` are mapcoopa ids, `k_no_id` for none.
 *
 * The first 12 bytes are the v1 record's, in its order (see TileMapData::load()).
 */
struct TileRecord {
    /// Flag bits.
    enum Flag : std::uint8_t {
        Water   = 1u << 0, /**< Water stands over this tile (`water_depth` > 0). */
        Ocean   = 1u << 1, /**< That water is the sea. */
        Coast   = 1u << 2, /**< Land next to the ocean. */
        River   = 1u << 3, /**< A river channel cuts this tile. */
        Road    = 1u << 4, /**< A roadway runs over this tile. */
        Town    = 1u << 5, /**< This tile is inside a settlement. */
        Flowing = 1u << 6, /**< That water is a river's, flowing toward `flow`. */
    };
    static constexpr std::uint16_t k_no_id = 0xFFFFu;

    std::int16_t  steps = 0;          /**< Ground height in whole steps (the bed, under water). */
    std::uint8_t  kinds = 0x10;       /**< Top kind in the low nibble, side kind in the high (Grass / Dirt). */
    std::uint8_t  flags = 0;          /**< Flag bits. */
    std::uint8_t  biome = 0;          /**< coopa::maps::Biome. */
    std::uint8_t  temperature = 128;  /**< mapcoopa temperature, 0..255 for 0..1. */
    std::uint8_t  moisture = 128;     /**< mapcoopa moisture, 0..255 for 0..1. */
    std::uint8_t  water_depth = 0;    /**< Steps of water over the ground; 0 when dry. */
    std::uint16_t region = k_no_id;   /**< mapcoopa region id. */
    std::uint16_t country = k_no_id;  /**< mapcoopa country id. */
    std::uint8_t  flow = 0;           /**< Flowing water's downstream octant, 1..8 (k_flow_directions); 0 none. */
    std::uint8_t  reserved[3] = {0, 0, 0}; /**< Zero; kept for later per-tile data. */

    TileKind top_kind() const { return static_cast<TileKind>(kinds & 0x0Fu); }
    TileKind side_kind() const { return static_cast<TileKind>(kinds >> 4); }
    void set_top_kind(TileKind k) { kinds = static_cast<std::uint8_t>((kinds & 0xF0u) | (static_cast<unsigned>(k) & 0x0Fu)); }
    void set_side_kind(TileKind k) { kinds = static_cast<std::uint8_t>((kinds & 0x0Fu) | ((static_cast<unsigned>(k) & 0x0Fu) << 4)); }
    bool has(Flag f) const { return (flags & f) != 0; }
    void set(Flag f, bool on) { flags = static_cast<std::uint8_t>(on ? (flags | f) : (flags & ~f)); }
    bool water() const { return water_depth > 0; }

    /** @brief The terrace the water fills to, in steps (the ground when dry). */
    int water_level() const { return static_cast<int>(steps) + static_cast<int>(water_depth); }

    /**
     * @brief Floods this tile to terrace `level` (or drains it, when `level <= steps`), keeping
     *        the flags in step. `ocean` / `flowing` say what kind of water it is.
     */
    void set_water(int level, bool ocean = false, bool flowing = false) {
        const int depth = level - static_cast<int>(steps);
        water_depth = static_cast<std::uint8_t>(depth <= 0 ? 0 : (depth > 255 ? 255 : depth));
        set(Water, water_depth > 0);
        set(Ocean, water_depth > 0 && ocean);
        set(Flowing, water_depth > 0 && flowing);
        if (water_depth == 0 || !flowing) flow = 0;
    }

    /** @brief mapcoopa's 0..1 temperature / moisture, and the reverse. */
    float temperature01() const { return static_cast<float>(temperature) / 255.0f; }
    float moisture01() const { return static_cast<float>(moisture) / 255.0f; }
    static std::uint8_t quantise01(double v) {
        const double c = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        return static_cast<std::uint8_t>(c * 255.0 + 0.5);
    }

    /** @brief The column the mesher builds: the ground, and the water over it. */
    TileColumn column() const {
        TileColumn c;
        c.steps = steps < 0 ? 0 : steps;
        c.top_kind = top_kind();
        c.side_kind = side_kind();
        if (water()) {
            c.water_level = water_level();
            c.water = has(Flowing) ? k_water_flowing : k_water_still;
            c.flow = has(Flowing) ? flow : 0;
        }
        return c;
    }

    bool operator==(const TileRecord& o) const;
    bool operator!=(const TileRecord& o) const { return !(*this == o); }
};
static_assert(sizeof(TileRecord) == 16, "TileRecord is the on-disk record: keep it 16 bytes");

/**
 * @struct TileHit
 * @brief A ray's hit on the tile columns (tile_map_data.h's raycast()).
 */
struct TileHit {
    glm::ivec2 tile{0};          /**< The tile hit. */
    glm::vec3  point{0.0f};      /**< Where, in the terrain's local space. */
    glm::vec3  normal{0.0f, 0.0f, 1.0f}; /**< The face hit: +z for a top, else a wall's outward normal. */
    float      t = 0.0f;         /**< Distance along the ray. */
};

/**
 * @class TileMapData
 * @brief See the file comment.
 */
class TileMapData {
public:
    /// Tiles per storage chunk edge (the file's compression and dirty-tracking unit).
    static constexpr int k_storage_chunk = 32;

    TileMapData();

    /** @brief Process-unique id: caches key on it, never on the address. */
    std::uint64_t uid() const { return uid_; }

    /** @brief Reshapes to `w` x `h` tiles of default records; marks everything changed. */
    void resize(int w, int h);

    int width() const { return width_; }
    int height() const { return height_; }
    bool empty() const { return width_ <= 0 || height_ <= 0; }
    bool contains(int x, int y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }

    /** @brief Unchecked access. Mark what you write with mark_dirty(). */
    TileRecord& at(int x, int y) { return records_[index_(x, y)]; }
    const TileRecord& at(int x, int y) const { return records_[index_(x, y)]; }

    /** @brief Clamped read: outside the grid answers the nearest edge tile (the world just continues). */
    const TileRecord& get(int x, int y) const;

    /** @brief All records, row-major. */
    const std::vector<TileRecord>& records() const { return records_; }

    /** @brief Copies `rect`'s records out, row-major (for undo). */
    std::vector<TileRecord> capture(const IRect& rect) const;
    /** @brief Writes records captured from `rect` back and marks it dirty. */
    void restore(const IRect& rect, const std::vector<TileRecord>& recs);

    // --- change tracking ---

    /** @brief Journals `rect` (clipped), bumps revision(), marks its storage chunks unsaved. */
    void mark_dirty(const IRect& rect);
    /** @brief The whole grid changed (a bake, a load): every consumer re-reads everything. */
    void mark_all_dirty();
    std::uint64_t revision() const { return revision_; }

    /**
     * @brief Calls `fn(rect)` for every rect edited after revision `since`, oldest first. When the
     *        journal no longer reaches back that far, calls it once with the whole grid.
     */
    void for_each_dirty_since(std::uint64_t since, const std::function<void(const IRect&)>& fn) const;
    /** @brief The union of for_each_dirty_since()'s rects. */
    IRect dirty_since(std::uint64_t since) const;

    /** @brief Edited since the last save or load. */
    bool dirty_on_disk() const { return unsaved_; }
    void set_saved();

    // --- baking (owned by the TileTerrain that fills it) ---

    /** @brief True until a bake (or a load) has filled the grid -- or after request_bake(). */
    bool needs_bake() const { return needs_bake_; }
    /** @brief Asks the owning terrain to bake the world again (the editor's Regenerate). */
    void request_bake() { needs_bake_ = true; ++bake_requests_; }
    /** @brief Bumped by every request_bake(): a terrain bakes once per request. */
    std::uint64_t bake_requests() const { return bake_requests_; }
    /** @brief Bumped every time a bake lands: the editor waits on it to record Regenerate's undo. */
    std::uint64_t bakes_done() const { return bakes_done_; }
    /** @brief Takes `baked`'s grid (a fresh bake): everything changes, the result is unsaved. */
    void assign_baked(TileMapData&& baked);

    // --- queries ---

    /**
     * @brief Casts a ray (terrain-local space) against the columns: steps up to `max_t`.
     *        A 2D grid walk over the tiles the ray crosses, testing each column's top and the
     *        walls it enters through.
     *
     * @param tile_size   Horizontal size of a tile.
     * @param height_step Vertical size of a step.
     */
    bool raycast(const glm::vec3& origin, const glm::vec3& dir, float tile_size, float height_step, float max_t,
                 TileHit& hit) const;

    // --- files (tile_map_data.cpp) ---

    /** @brief Writes a `.tiles` file; recompresses only the storage chunks edited since the last save/load. */
    bool save(const std::string& path);
    /** @brief Reads a `.tiles` file; on success the grid is clean and needs no bake. */
    bool load(const std::string& path);

private:
    std::size_t index_(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
    }
    int chunks_x_() const { return (width_ + k_storage_chunk - 1) / k_storage_chunk; }
    int chunks_y_() const { return (height_ + k_storage_chunk - 1) / k_storage_chunk; }
    std::vector<std::uint8_t> pack_chunk_(int cx, int cy) const;
    bool unpack_chunk_(int cx, int cy, const std::vector<std::uint8_t>& raw, std::size_t record_bytes = sizeof(TileRecord));

    struct JournalEntry {
        std::uint64_t revision;
        IRect rect;
    };

    std::uint64_t uid_;
    int width_ = 0, height_ = 0;
    std::vector<TileRecord> records_;
    std::vector<JournalEntry> journal_;
    std::uint64_t journal_floor_ = 0;   ///< Edits at or below this revision are no longer journalled.
    std::uint64_t revision_ = 1;
    bool unsaved_ = false;
    bool needs_bake_ = true;
    std::uint64_t bake_requests_ = 0;
    std::uint64_t bakes_done_ = 0;

    std::vector<std::vector<std::uint8_t>> compressed_;   ///< Per storage chunk, as last saved / loaded.
    std::vector<std::uint8_t> chunk_dirty_;               ///< Per storage chunk: edited since compressed_.
};

/**
 * @class TileMapStore
 * @brief Process-wide owner of every TileTerrain's tiles, keyed by its `.tiles` file -- so
 *        re-parsing a component (the editor does on every edit) hands back the same live data,
 *        unsaved edits included. Modelled on HeightmapStore.
 */
class TileMapStore {
public:
    static TileMapStore& instance();

    /**
     * @brief The live tiles for `path`. First call: loads the file when it exists, otherwise an
     *        empty grid that needs_bake(). Later calls return the same object -- reloaded from
     *        disk only when the file changed there and nothing is unsaved.
     */
    std::shared_ptr<TileMapData> acquire(const std::string& path);

    /** @brief Writes one terrain's tiles to its file. */
    bool save(TileMapData& data);
    /** @brief Writes every terrain with unsaved edits; returns how many were written. */
    int save_dirty();
    /** @brief The file `data` was acquired for, or empty. */
    std::string path_of(const TileMapData& data) const;
    /** @brief Forgets everything (a scene change). */
    void clear();

private:
    struct Entry {
        std::shared_ptr<TileMapData> data;
        std::int64_t mtime = 0;
    };
    std::vector<std::pair<std::string, Entry>> entries_;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TILE_MAP_DATA_H

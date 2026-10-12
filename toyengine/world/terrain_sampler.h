/**
 * @file terrain_sampler.h
 * @brief Turns a generated mapcoopa world into per-tile height and surface queries, on any
 *        number of threads at once.
 *
 * The bridge between `coopa::maps` (a Voronoi graph of cells carrying elevation, moisture,
 * temperature, biome, rivers) and the tile system (a regular grid of integer-height columns).
 * mapcoopa's own consumers rasterise the graph a polygon at a time; a chunk mesher needs the
 * opposite direction -- "what is under this exact point" -- so that lookup is built here.
 *
 * ### Point -> cell, and why it needs an index
 *
 * `MapGraph::elevation_at()` takes the cell to interpolate within as a hint, and mapcoopa has
 * no point-to-cell lookup of its own (nothing in that library needs one). A Voronoi cell *is*
 * the set of points nearest its generating site, so the lookup is a nearest-site query, and the
 * sites sit on a jittered lattice of roughly one grid unit. A uniform bucket grid at that
 * spacing therefore answers it in a handful of distance tests, expanding rings only until the
 * best candidate is provably closer than the next ring can reach.
 *
 * ### Thread safety
 *
 * build() runs once, on the owner thread. Afterwards every member is read-only and every query
 * is `const`, so any number of chunk-meshing jobs may sample concurrently -- which is the whole
 * point, since sampling is most of what a chunk job does.
 *
 * The class is neither copyable nor movable: `detail_` borrows from `surface_`, and
 * `channels_` indexes `graph_`, so relocating the object would leave both dangling. Same
 * reasoning as toy::core::Engine's own deleted copy/move.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_SAMPLER_H
#define TOYENGINE_WORLD_TERRAIN_SAMPLER_H

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <coopa/maps/biome.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/noise.h>
#include <coopa/maps/water_field.h>

#include <toyengine/world/tile_map_data.h>
#include <toyengine/world/tile_water.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

/**
 * @struct TerrainParams
 * @brief How the map's continuous world is cut into discrete tiles and chunks.
 *
 * Authored in scene YAML on the `Terrain` component (see terrain_component.h) and then passed
 * by value everywhere -- the sampler, the chunk mesher and the streamer all need it, and it is
 * small, immutable and copyable on purpose so a meshing job can hold its own copy.
 */
struct TerrainParams {
    /**
     * @brief Tiles per mapcoopa grid unit -- the resolution the map is sampled at.
     *
     * One grid unit is one Voronoi cell's nominal width, so this is "how many blocks wide a map
     * cell is". Raising it resolves coastlines and river channels more finely at a quadratic
     * cost in tiles; it does NOT add detail the map does not have, beyond the fractal
     * displacement `MapConfig::terrain_roughness` already carries.
     */
    std::int32_t tiles_per_grid_unit = 4;

    /**
     * @brief The world's size in tiles per side -- how many tiles the map is rasterised into,
     *        whatever its cell count. 0 derives it: `grid_size * tiles_per_grid_unit`.
     *
     * Set, it wins over `tiles_per_grid_unit`: the map is sampled at `world_tiles / grid_size`
     * tiles per cell (fractional is fine), so the world's size and the map's detail are chosen
     * separately. The world spans `world_tiles * tile_size` world units.
     */
    std::int32_t world_tiles = 0;

    /** @brief Tiles per side for a map of `grid_size` cells. */
    std::int32_t tiles_for_grid(std::int32_t grid_size) const {
        return world_tiles > 0 ? world_tiles : grid_size * tiles_per_grid_unit;
    }

    /** @brief Horizontal size of one tile, in world units. */
    float tile_size = 1.0f;

    /** @brief Vertical size of one height step, in world units; the quantisation of the terrain. */
    float height_step = 1.0f;

    /** @brief World height of the full `[0, 1]` elevation range -- the vertical scale of the world. */
    float height_scale = 16.0f;

    /** @brief Tiles per chunk edge; a chunk is `chunk_size * chunk_size` columns. */
    std::int32_t chunk_size = 16;

    /**
     * @brief Chunks of terrain kept loaded in every direction from the camera's chunk.
     *
     * The live region is a square of side `2 * view_radius + 1`. It sets how much geometry is
     * resident and meshed; what is DRAWN is narrower -- toy::render::ToyRenderPipeline
     * frustum-culls every chunk against the camera and each shadow view separately.
     */
    std::int32_t view_radius = 3;

    /**
     * @brief Longest wall a single column may emit, in steps.
     *
     * A cliff's exposed face is as many steps as the drop, and a sea cliff against the ocean
     * floor can be dozens. Past this many steps the wall stops: the result is a skirt whose
     * bottom is hidden inside the neighbouring column's own geometry at any viewing angle a
     * ground-level camera can reach, for a bounded cost per column.
     */
    std::int32_t max_wall_steps = 24;

    /** @brief Steps of `side_kind` under a top face before a wall switches to bare Stone. */
    std::int32_t soil_depth_steps = 3;

    /**
     * @brief How far the water surface sits under the top of the terrace it fills, in steps.
     *
     * Water fills a column to a whole terrace (TileRecord::water_level()), and its banks stand
     * at least that high; drawn exactly at the terrace top, the surface would lie flush with
     * every bank top and fight it. Half a step puts the shoreline halfway down the bank's wall,
     * where whatever shape the tile set gives that wall -- rounded, square -- is the shoreline.
     */
    float water_inset = 0.5f;

    /**
     * @brief The same for rivers, in steps. A river runs in a channel one tile wide, its banks
     *        the terrace it fills to; at the sea's half step it reads as sunk in a trench, so it
     *        sits higher, just under the bank tops.
     */
    float river_inset = 0.2f;

    /** @brief World-space size of one chunk edge. */
    float chunk_world_size() const {
        return static_cast<float>(chunk_size) * tile_size;
    }

    /** @brief The per-axis scale of one tile cell, for TileMeshLibrary::append_styled(). */
    glm::vec3 cell_scale() const {
        return glm::vec3(tile_size, tile_size, height_step);
    }

    /** @brief The chunk containing a world position (ignoring z). */
    ChunkCoord chunk_at(const glm::vec3& world_position) const {
        const float size = chunk_world_size();
        return ChunkCoord{static_cast<std::int32_t>(std::floor(world_position.x / size)),
                          static_cast<std::int32_t>(std::floor(world_position.y / size))};
    }
};

/**
 * @class TerrainSampler
 * @brief A generated map, plus the O(1) queries the tile system asks of it.
 */
class TerrainSampler {
public:
    TerrainSampler() = default;

    /// @brief Non-copyable, non-movable -- see this file's doc (borrowed interior pointers).
    TerrainSampler(const TerrainSampler&) = delete;
    TerrainSampler& operator=(const TerrainSampler&) = delete;
    TerrainSampler(TerrainSampler&&) = delete;
    TerrainSampler& operator=(TerrainSampler&&) = delete;

    /**
     * @brief Takes ownership of a generated world and indexes it for querying.
     *
     * Deliberately serial. The index is one pass over the sites -- roughly `(grid_size + 1)^2`
     * of them, a few thousand for a normal map -- against a generation pass that already ran
     * off-thread and a meshing pass that is genuinely parallel. Threading it would need
     * per-bucket synchronisation to save well under a millisecond, once.
     *
     * @param config The configuration the graph was generated from; supplies the vertical scale,
     *               the waterline and the fractal detail's amplitude.
     * @param graph  The generated world; MOVED FROM, since the sampler holds it for its lifetime.
     * @param params The tiling, which fixes how grid space maps onto tile space.
     */
    void build(const coopa::maps::MapConfig& config, coopa::maps::MapGraph&& graph,
               const TerrainParams& params) {
        config_ = config;
        graph_  = std::move(graph);
        params_ = params;

        // One sampler per Noise, held by unique_ptr so the address detail_ borrows survives (a
        // Noise wraps a FastNoiseLite and is not cheap to build -- see coopa::maps::Noise).
        // The ground every query samples -- classic or natural terrain, as the config says.
        surface_ = std::make_unique<coopa::maps::TerrainSurface>(graph_, config_);
        detail_  = surface_->detail();
        // Cut last, and indexed per cell, so a river bed survives the fractal displacement
        // rather than being filled back in by it -- see MapGraph::elevation_at()'s doc.
        channels_ = coopa::maps::make_river_channels(graph_, config_, detail_);
        // mapcoopa's own answer to "where is the water, and how high" -- seas and lakes at their
        // levels, rivers at their banked surfaces. Nothing about the water is decided here.
        water_ = std::make_unique<coopa::maps::WaterField>(graph_, config_, detail_, channels_);

        build_site_index_();
        built_ = !graph_.centers.empty();
    }

    /** @brief True once build() has been handed a non-empty world. */
    bool is_built() const { return built_; }

    /** @brief The configuration the world was generated from. */
    const coopa::maps::MapConfig& config() const { return config_; }

    /** @brief The generated world itself, for a consumer that wants towns, roads or rivers. */
    const coopa::maps::MapGraph& graph() const { return graph_; }

    /** @brief The tiling this sampler was built for. */
    const TerrainParams& params() const { return params_; }

    /** @brief Tiles along one axis of the whole world; tile indices span `[0, tiles_per_axis())`. */
    std::int32_t tiles_per_axis() const {
        return params_.tiles_for_grid(config_.grid_size);
    }

    /** @brief The height of the waterline, in tile steps -- the top of every water column. */
    std::int32_t sea_level_steps() const {
        return steps_from_elevation_(config_.sea_level);
    }

    /**
     * @brief The Voronoi cell containing a point in grid space.
     *
     * A nearest-site query over the bucket index. Rings are expanded outward from the point's
     * own bucket and the search stops as soon as the next ring cannot possibly hold anything
     * closer -- so a normal query touches one bucket's worth of sites.
     *
     * Precondition: is_built(). There is no meaningful cell to return from an empty graph and
     * no null MapCenter to hand back, so this asserts rather than inventing one; every caller
     * inside this module goes through sample(), which checks is_built() first.
     *
     * @param gx Horizontal grid position.
     * @param gy Vertical grid position.
     * @return The containing cell.
     */
    const coopa::maps::MapCenter& cell_at(double gx, double gy) const {
        assert(built_ && "TerrainSampler::cell_at() called before build()");
        const std::int32_t bucket_x = bucket_axis_(gx, origin_x_);
        const std::int32_t bucket_y = bucket_axis_(gy, origin_y_);

        std::size_t best_index = 0;
        double best_distance_sq = std::numeric_limits<double>::max();

        for (std::int32_t ring = 0; ring <= max_ring_; ++ring) {
            // Everything in ring r is at least (r - 1) buckets away, so once the best hit is
            // closer than that, no further ring can beat it. The -1 accounts for the query
            // sitting anywhere inside its own bucket rather than at its centre.
            if (ring > 0) {
                const double reach = static_cast<double>(ring - 1) * k_bucket_size;
                if (best_distance_sq <= reach * reach) break;
            }
            scan_ring_(bucket_x, bucket_y, ring, gx, gy, best_index, best_distance_sq);
        }
        return graph_.centers[best_index];
    }

    /**
     * @brief Samples one tile column: how tall it is and what it is made of.
     *
     * The column view of sample_record() -- what the mesher needs.
     */
    TileColumn sample(std::int32_t tile_x, std::int32_t tile_y) const {
        return sample_record(tile_x, tile_y).column();
    }

    /** @brief mapcoopa's water at a tile's centre -- the continuous answer the bake rounds. */
    coopa::maps::WaterSample water_at(std::int32_t tile_x, std::int32_t tile_y) const {
        const double per_unit = tiles_per_cell_();
        const double gx = (static_cast<double>(tile_x) + 0.5) / per_unit;
        const double gy = (static_cast<double>(tile_y) + 0.5) / per_unit;
        return water_->at(cell_at(gx, gy), gx, gy, k_river_min_half_tiles / per_unit);
    }

    /**
     * @brief Samples everything a baked tile stores (tile_map_data.h's TileRecord), before the
     *        bake's rounding pass.
     *
     * Sampled at the tile's CENTRE, not its corner, so a column's material and height describe
     * the ground it actually covers; sampling at the corner would bias every column toward its
     * lower-left neighbour and shift the whole world half a tile.
     *
     * `steps` is the ground rounded to the nearest step. Under water (mapcoopa's WaterField says
     * where, and how high) the record is flooded to the water's terrace; a frozen lake is ground
     * of ice at that terrace instead. bake() then reconciles each tile with its neighbours --
     * a lone tile cannot know that the water beside it needs a bank.
     */
    TileRecord sample_record(std::int32_t tile_x, std::int32_t tile_y) const {
        TileRecord r;
        if (!built_) return r;

        const double per_unit = tiles_per_cell_();
        const double gx = (static_cast<double>(tile_x) + 0.5) / per_unit;
        const double gy = (static_cast<double>(tile_y) + 0.5) / per_unit;
        const coopa::maps::MapCenter& cell = cell_at(gx, gy);

        r.biome       = static_cast<std::uint8_t>(cell.biome);
        r.temperature = TileRecord::quantise01(cell.temperature);
        r.moisture    = TileRecord::quantise01(cell.moisture);
        r.region      = cell.region >= 0 && cell.region < 0xFFFF ? static_cast<std::uint16_t>(cell.region) : TileRecord::k_no_id;
        r.country     = cell.country >= 0 && cell.country < 0xFFFF ? static_cast<std::uint16_t>(cell.country) : TileRecord::k_no_id;
        r.set(TileRecord::Coast, cell.coast);
        r.set(TileRecord::River, graph_.channel_cut(cell, gx, gy, channels_) > 0.0);

        // A river narrower than a tile would land on a tile centre only here and there -- a dotted
        // line of pits. Asked at the grid's own resolution, mapcoopa widens it to a continuous run.
        const coopa::maps::WaterSample water = water_->at(cell, gx, gy, k_river_min_half_tiles / per_unit);
        r.steps = static_cast<std::int16_t>(std::min<std::int32_t>(steps_from_elevation_(water.ground), 32767));
        TileKind top = tile_kind_for_biome(cell.biome);
        if (water.wet()) {
            const std::int32_t level = std::max(1, steps_from_elevation_(water.surface));
            if (cell.biome == coopa::maps::Biome::Ice && water.kind != coopa::maps::WaterKind::River) {
                // Frozen: solid ice at the water's terrace.
                r.steps = static_cast<std::int16_t>(level);
                r.set_top_kind(TileKind::Ice);
                r.set_side_kind(TileKind::Ice);
                return r;
            }
            const bool flowing = water.kind == coopa::maps::WaterKind::River;
            if (flowing) r.set(TileRecord::River, true);
            // The bed rounds down: at least a step under the water, however shallow mapcoopa's.
            r.steps = static_cast<std::int16_t>(std::min<std::int32_t>(r.steps, level - 1));
            r.set_water(level, water.kind == coopa::maps::WaterKind::Sea, flowing);
            if (flowing) r.flow = flow_octant(water.flow.x, water.flow.y);
            top = TileKind::Sand;
        }
        r.set_top_kind(top);
        r.set_side_kind(side_kind_for(top));
        return r;
    }

    /**
     * @brief Fills `out` (resized to the whole world) with every tile, then reconciles the
     *        rounding so water never stands above dry ground.
     *
     * mapcoopa's water already never stands over its own ground; rounding both to whole steps
     * independently could still put a bank a step under the terrace beside it. So the rounding
     * gives way on the GROUND's side, never the water's:
     *
     * - a dry tile beside water rounds UP to at least that water's terrace -- the bank;
     * - a flooded tile's bed rounds DOWN to at least one step under its terrace -- the bed.
     */
    void bake(TileMapData& out) const {
        const std::int32_t n = tiles_per_axis();
        out.resize(n, n);
        for (std::int32_t y = 0; y < n; ++y)
            for (std::int32_t x = 0; x < n; ++x) out.at(x, y) = sample_record(x, y);
        settle_water_rounding(out, IRect{0, 0, n, n});   // tile_water.h
    }

    /**
     * @brief Maps a mapcoopa biome onto the surface a tile face is textured with.
     *
     * Thirty-three biomes collapse onto twelve surfaces: a face needs to know which patch of the
     * atlas to sample, not which Whittaker cell it came from. A flat table indexed by the enum,
     * the same shape coopa::maps::BiomePalette uses for exactly the same reason.
     *
     * @param biome The cell's biome.
     * @return The surface to texture its top face with.
     */
    static TileKind tile_kind_for_biome(coopa::maps::Biome biome) {
        static const std::array<TileKind, coopa::maps::k_biome_count> table = {
            TileKind::Sand,  // Ocean (a shore or sea bed: the water itself is drawn over it)
            TileKind::Sand,  // Lake
            TileKind::Moss,  // Marsh
            TileKind::Ice,   // Ice
            TileKind::Sand,  // Beach
            TileKind::Snow,  // Snow
            TileKind::Moss,  // Tundra
            TileKind::Stone, // Bare
            TileKind::Ash,   // Scorched
            TileKind::Grass, // Taiga
            TileKind::Moss,  // Shrubland
            TileKind::Sand,  // TemperateDesert
            TileKind::Grass, // TemperateRainForest
            TileKind::Grass, // TemperateDeciduousForest
            TileKind::Grass, // Grassland
            TileKind::Grass, // TropicalRainForest
            TileKind::Grass, // TropicalSeasonalForest
            TileKind::Sand,  // SubtropicalDesert
            TileKind::Grass, // AlpineMeadow
            TileKind::Ice,   // Glacier
            TileKind::Clay,  // ColdDesert
            TileKind::Moss,  // Steppe
            TileKind::Clay,  // Savanna
            TileKind::Moss,  // Chaparral
            TileKind::Moss,  // Moorland
            TileKind::Moss,  // BorealWetland
            TileKind::Moss,  // Swamp
            TileKind::Moss,  // Mangrove
            TileKind::Grass, // CloudForest
            TileKind::Clay,  // Badlands
            TileKind::Salt,  // SaltFlat
            TileKind::Sand,  // Dunes
            TileKind::Ash    // VolcanicField
        };
        const std::size_t index = static_cast<std::size_t>(biome);
        return index < table.size() ? table[index] : TileKind::Stone;
    }

    /**
     * @brief What sits immediately under a given top surface, on a lateral face.
     *
     * The grass-over-dirt read every block game uses: a cut through turf shows soil, a cut
     * through sand shows sand. Below TerrainParams::soil_depth_steps the mesher overrides this
     * with bare stone regardless -- see build_chunk_mesh().
     *
     * @param top The column's top surface.
     * @return The surface its upper lateral faces are textured with.
     */
    static TileKind side_kind_for(TileKind top) {
        switch (top) {
            case TileKind::Grass:
            case TileKind::Moss:  return TileKind::Dirt;
            case TileKind::Snow:  return TileKind::Stone;
            case TileKind::Sand:  return TileKind::Sand;
            case TileKind::Clay:  return TileKind::Clay;
            case TileKind::Salt:  return TileKind::Clay;
            case TileKind::Ash:   return TileKind::Stone;
            case TileKind::Ice:   return TileKind::Ice;
            case TileKind::Water: return TileKind::Water;
            default:              return TileKind::Stone;
        }
    }

private:
    /** @brief Tiles per map cell, as sampled: the world's tiles over the map's cells. */
    double tiles_per_cell_() const {
        return static_cast<double>(tiles_per_axis()) / static_cast<double>(std::max(1, config_.grid_size));
    }

    /** @brief Half-width rivers are sampled at, at the least, in tiles: a 4-connected run of tiles. */
    static constexpr double k_river_min_half_tiles = 0.75;

    /** @brief Bucket edge in grid units. One grid unit is the nominal site spacing. */
    static constexpr double k_bucket_size = 1.0;

    /** @brief Quantises normalised elevation to whole tile steps, never below the ground plane. */
    std::int32_t steps_from_elevation_(double elevation) const {
        if (params_.height_step <= 0.0f) return 0;
        const double world_z = elevation * static_cast<double>(params_.height_scale);
        const std::int32_t steps =
            static_cast<std::int32_t>(std::lround(world_z / static_cast<double>(params_.height_step)));
        return steps < 0 ? 0 : steps;
    }

    /** @brief Builds the uniform bucket index over the generating sites. */
    void build_site_index_() {
        buckets_.clear();
        if (graph_.centers.empty()) return;

        double min_x = std::numeric_limits<double>::max();
        double min_y = std::numeric_limits<double>::max();
        double max_x = std::numeric_limits<double>::lowest();
        double max_y = std::numeric_limits<double>::lowest();
        for (const coopa::maps::MapCenter& center : graph_.centers) {
            min_x = std::min(min_x, center.point.x);
            min_y = std::min(min_y, center.point.y);
            max_x = std::max(max_x, center.point.x);
            max_y = std::max(max_y, center.point.y);
        }

        // Derived from the sites themselves, not from grid_size: the generator adds a boundary
        // ring of sites OUTSIDE [0, grid_size], and a query near the map edge must still find
        // them or it would snap to the wrong cell along every border.
        origin_x_ = min_x;
        origin_y_ = min_y;
        width_  = std::max(1, static_cast<std::int32_t>((max_x - min_x) / k_bucket_size) + 1);
        height_ = std::max(1, static_cast<std::int32_t>((max_y - min_y) / k_bucket_size) + 1);
        max_ring_ = std::max(width_, height_);

        buckets_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), {});
        for (std::size_t i = 0; i < graph_.centers.size(); ++i) {
            const coopa::maps::MapCenter& center = graph_.centers[i];
            const std::int32_t bx = bucket_axis_(center.point.x, origin_x_);
            const std::int32_t by = bucket_axis_(center.point.y, origin_y_);
            buckets_[bucket_index_(bx, by)].push_back(static_cast<std::int32_t>(i));
        }
    }

    /** @brief Bucket coordinate of a grid coordinate; may fall outside the index. */
    std::int32_t bucket_axis_(double value, double origin) const {
        return static_cast<std::int32_t>(std::floor((value - origin) / k_bucket_size));
    }

    /** @brief Flat index of a bucket, clamped into range. */
    std::size_t bucket_index_(std::int32_t bx, std::int32_t by) const {
        const std::int32_t cx = std::clamp(bx, 0, width_ - 1);
        const std::int32_t cy = std::clamp(by, 0, height_ - 1);
        return static_cast<std::size_t>(cy) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(cx);
    }

    /** @brief Tests every site in the square ring `ring` buckets out, keeping the nearest. */
    void scan_ring_(std::int32_t bucket_x, std::int32_t bucket_y, std::int32_t ring, double gx,
                    double gy, std::size_t& best_index, double& best_distance_sq) const {
        const std::int32_t min_x = bucket_x - ring;
        const std::int32_t max_x = bucket_x + ring;
        const std::int32_t min_y = bucket_y - ring;
        const std::int32_t max_y = bucket_y + ring;

        for (std::int32_t by = min_y; by <= max_y; ++by) {
            if (by < 0 || by >= height_) continue;
            for (std::int32_t bx = min_x; bx <= max_x; ++bx) {
                if (bx < 0 || bx >= width_) continue;
                // Ring, not block: interior buckets were already scanned by a smaller ring.
                const bool on_edge = (bx == min_x || bx == max_x || by == min_y || by == max_y);
                if (ring > 0 && !on_edge) continue;

                for (const std::int32_t site : buckets_[bucket_index_(bx, by)]) {
                    const coopa::maps::MapCenter& center = graph_.centers[static_cast<std::size_t>(site)];
                    const double dx = center.point.x - gx;
                    const double dy = center.point.y - gy;
                    const double distance_sq = dx * dx + dy * dy;
                    if (distance_sq < best_distance_sq) {
                        best_distance_sq = distance_sq;
                        best_index = static_cast<std::size_t>(site);
                    }
                }
            }
        }
    }

    coopa::maps::MapConfig  config_;
    coopa::maps::MapGraph   graph_;
    TerrainParams           params_;

    std::unique_ptr<coopa::maps::TerrainSurface> surface_; /**< Borrowed by detail_; hence the unique_ptr. */
    coopa::maps::TerrainDetail          detail_;   /**< How the ground is shaped over the control mesh. */
    coopa::maps::RiverChannels          channels_; /**< River beds cut after the displacement. */
    std::unique_ptr<coopa::maps::WaterField> water_; /**< Borrows graph_, detail_, channels_. */

    std::vector<std::vector<std::int32_t>> buckets_;  /**< Site indices, bucketed by position. */
    double       origin_x_ = 0.0;
    double       origin_y_ = 0.0;
    std::int32_t width_    = 0;
    std::int32_t height_   = 0;
    std::int32_t max_ring_ = 0;
    bool         built_    = false;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_SAMPLER_H

/**
 * @file terrain_sampler.h
 * @brief Turns a generated mapcoopa world into per-tile height and surface queries, on any
 *        number of threads at once.
 *
 * The bridge between `coopa::maps` (a Voronoi graph of cells carrying elevation, moisture,
 * temperature, biome, rivers) and a terrain's tiles. mapcoopa's own consumers rasterise the graph
 * a polygon at a time; a bake needs the opposite direction -- "what is under this exact point" --
 * so that lookup is built here.
 *
 * It lives beside the MapGenerator component rather than with the terrain, because it belongs to
 * the map: a terrain is handed the *result* of applying one (toyengine/mapgen/README.md).
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
 * is `const`, so any number of jobs may sample concurrently -- which is the whole point, since
 * sampling is most of what a bake does.
 *
 * The class is neither copyable nor movable: `detail_` borrows from `surface_`, and
 * `channels_` indexes `graph_`, so relocating the object would leave both dangling. Same
 * reasoning as toy::core::Engine's own deleted copy/move.
 */

#ifndef TOYENGINE_MAPGEN_TERRAIN_SAMPLER_H
#define TOYENGINE_MAPGEN_TERRAIN_SAMPLER_H

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
#include <coopa/maps/biome_field.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/noise.h>
#include <coopa/maps/cave_field.h>
#include <coopa/maps/water_field.h>

#include <toyengine/mapgen/world_bake.h>
#include <toyengine/world/terra_data.h>
#include <toyengine/world/terrain_params.h>
#include <toyengine/world/terrain_shore.h>
#include <toyengine/world/terrain_water.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

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
        // The per-point biome: classified from the continuous ground and blended
        // climate, so biome boundaries follow the terrain instead of the cell
        // mosaic, and beaches are a band at the actual waterline.
        biomes_ = std::make_unique<coopa::maps::BiomeField>(graph_, config_);
        // The open spans a cave leaves, and where each system was cut open. Built last because
        // it measures its mouths against the surface anyone can SEE -- detail and river cut
        // applied -- which is the same surface the generator clamped every ceiling under.
        caves_ = std::make_unique<coopa::maps::CaveField>(graph_, config_, detail_, channels_);

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

    /**
     * @brief The tile record at an arbitrary point of the map, with the cell search warm-started.
     *
     * What a replacement bake walk fills each of its tiles from (mapgen/world_bake.h): the flat
     * walk reads a tile's point off its grid index, and this answers the same question for a
     * point arrived at any other way.
     *
     * `cell_hint` is both read and written -- the cell search is a downhill walk over the map
     * graph, so handing back the cell it settled on makes the next nearby point a few steps
     * instead of a full descent. Start it at 0 and keep passing the same variable.
     *
     * @param p         The point on the map, in the graph's own frame.
     * @param per_unit  Tiles per map grid unit, which fixes how wide a river reads.
     * @param across    Unit map-space direction of the tile grid's x at `p`.
     * @param along     Unit map-space direction of the tile grid's y at `p`.
     * @param cell_hint In/out: where to start the cell search, left on the cell that won.
     * @return The tile, terraced and watered exactly as the flat walk would leave it.
     */
    TileCell cell_at(const coopa::maps::MapPoint& p, double per_unit, const glm::dvec3& across,
                     const glm::dvec3& along, std::size_t& cell_hint) const {
        cell_hint = nearest_cell_(p, cell_hint);
        return record_(graph_.centers[cell_hint], p, per_unit, across, along);
    }

    /**
     * @brief `cell_at()`, and the extra roofs the column needs if it has more than one.
     *
     * A `TileCell` holds one roof. A column under a multi-storey cave needs a list, and only the
     * world that owns the column has somewhere to keep it -- so a bake walk takes this form and
     * hands what comes back to `TerraData::set_spans()`.
     *
     * @param spans Cleared and filled with the column's roofs, bottom-up; left empty when one
     *              roof (or none) is enough, which is the overwhelmingly common case.
     */
    TileCell cell_at(const coopa::maps::MapPoint& p, double per_unit, const glm::dvec3& across,
                     const glm::dvec3& along, std::size_t& cell_hint,
                     std::vector<world::TileSpanRecord>& spans) const {
        spans.clear();
        cell_hint = nearest_cell_(p, cell_hint);
        // Through `record_()`, which is the one place a cave is applied: it has to run LAST of
        // everything that writes the column (the water, the biome, the kinds), so applying it a
        // second time out here would hand it a column whose ground is already a cave floor.
        return record_(graph_.centers[cell_hint], p, per_unit, across, along, false, &spans);
    }

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
        return tile_column(sample_cell(tile_x, tile_y), static_cast<float>(params_.height_step));
    }

    /** @brief mapcoopa's water at a tile's centre -- the continuous answer the bake rounds. */
    coopa::maps::WaterSample water_at(std::int32_t tile_x, std::int32_t tile_y) const {
        const double per_unit = tiles_per_cell_();
        const double gx = (static_cast<double>(tile_x) + 0.5) / per_unit;
        const double gy = (static_cast<double>(tile_y) + 0.5) / per_unit;
        return water_->at(cell_at(gx, gy), gx, gy, k_river_min_half_tiles / per_unit);
    }

    /**
     * @brief Samples everything a baked tile stores (terra_data.h's TileCell), before the
     *        bake's rounding pass.
     *
     * Sampled at the tile's CENTRE, not its corner, so a column's material and height describe
     * the ground it actually covers; sampling at the corner would bias every column toward its
     * lower-left neighbour and shift the whole world half a tile.
     *
     * `steps` is the ground rounded to the nearest step. Under water (mapcoopa's WaterField says
     * where, and how high) the cell is flooded to the water's terrace; a frozen lake is ground
     * of ice at that terrace instead. bake() then reconciles each tile with its neighbours --
     * a lone tile cannot know that the water beside it needs a bank.
     *
     * @param tile_x Tile column.
     * @param tile_y Tile row.
     * @param continuous_water_level Keeps `water_level` at mapcoopa's exact surface instead of
     *        rounding it to a terrace -- for a field whose ground is a continuous height plane
     *        rather than steps. See record_().
     */
    TileCell sample_cell(std::int32_t tile_x, std::int32_t tile_y,
                         bool continuous_water_level = false) const {
        TileCell r;
        if (!built_) return r;

        const double per_unit = tiles_per_cell_();
        const double gx = (static_cast<double>(tile_x) + 0.5) / per_unit;
        const double gy = (static_cast<double>(tile_y) + 0.5) / per_unit;
        const coopa::maps::MapCenter& cell = cell_at(gx, gy);
        return record_(cell, coopa::maps::MapPoint{gx, gy}, per_unit, glm::dvec3(1.0, 0.0, 0.0),
                       glm::dvec3(0.0, 1.0, 0.0), continuous_water_level);
    }

    /**
     * @struct SurfaceSample
     * @brief The continuous world at one point: metres of ground and water, and the cell's own
     *        surface, climate and ids.
     *
     * What a **smooth** terrain is written from. `sample_record()` is the same world quantised
     * into a terraced column; this is the step before that quantisation, which a height plane
     * wants instead -- the whole reason a generated world can be applied to either mode.
     */
    struct SurfaceSample {
        float ground = 0.0f;        /**< @brief Ground height in metres -- under water, the bed. */
        float water_surface = 0.0f; /**< @brief The water's surface in metres; only set when wet. */
        coopa::maps::WaterKind water = coopa::maps::WaterKind::None;
        std::uint8_t flow = 0;      /**< @brief A river's downstream octant, 1..8; 0 otherwise. */
        TileKind top = TileKind::Grass;
        std::uint8_t biome = 0;
        std::uint8_t temperature = 128;
        std::uint8_t moisture = 128;
        std::uint16_t region = TileCell::k_no_id;
        std::uint16_t country = TileCell::k_no_id;
        bool coast = false;
        bool river = false;

        bool wet() const { return water != coopa::maps::WaterKind::None; }
    };

    /**
     * @brief The continuous world at a point in FRACTIONAL tile coordinates.
     *
     * Fractional because a height plane samples finer than the tile grid: sample (i, j) of a
     * plane with `sub` samples per tile sits at tile coordinate `i / sub`.
     */
    SurfaceSample sample_surface(double tile_x, double tile_y) const {
        SurfaceSample out;
        if (!built_) return out;

        const double per_unit = tiles_per_cell_();
        const double gx = tile_x / per_unit;
        const double gy = tile_y / per_unit;
        const coopa::maps::MapCenter& cell = cell_at(gx, gy);
        const coopa::maps::MapPoint p{gx, gy};

        out.temperature = TileCell::quantise01(cell.temperature);
        out.moisture    = TileCell::quantise01(cell.moisture);
        out.region      = cell.region >= 0 && cell.region < 0xFFFF ? static_cast<std::uint16_t>(cell.region) : TileCell::k_no_id;
        out.country     = cell.country >= 0 && cell.country < 0xFFFF ? static_cast<std::uint16_t>(cell.country) : TileCell::k_no_id;
        out.coast       = cell.coast;
        out.river       = graph_.channel_cut(cell, p, channels_) > 0.0;

        const coopa::maps::WaterSample water = water_->at(cell, p, k_river_min_half_tiles / per_unit);
        const coopa::maps::Biome biome = biomes_->at(cell, p, water);
        out.biome = static_cast<std::uint8_t>(biome);
        out.ground = static_cast<float>(water.ground * params_.height_scale);
        out.top = tile_kind_for_biome(biome);
        if (water.wet()) {
            out.water = water.kind;
            out.water_surface = static_cast<float>(water.surface * params_.height_scale);
            // MOLTEN DOES NOT FREEZE. An ice biome locks its still water into a crust, and a
            // vent's crater standing in the middle of one is exactly where the exception has to
            // hold -- otherwise a frozen world is where the lava silently disappears.
            const bool molten = water.fluid == coopa::maps::SurfaceFluid::Lava;
            if (biome == coopa::maps::Biome::Ice && !molten &&
                water.kind != coopa::maps::WaterKind::River) {
                // Frozen: solid ice up at the water's surface, and no water over it.
                out.ground = out.water_surface;
                out.water = coopa::maps::WaterKind::None;
                out.top = TileKind::Ice;
            } else {
                if (water.kind == coopa::maps::WaterKind::River) {
                    const glm::dvec3 f(water.flow.x, water.flow.y, water.flow.z);
                    out.flow = flow_octant(f.x, f.y);
                }
                // The bed graded by depth, exactly as the terraced bake does: both measure it
                // in metres -- except under lava, which leaves basalt however deep it stands.
                const float depth = std::max(0.0f, out.water_surface - out.ground);
                out.top = molten ? TileKind::Basalt : bed_kind_for(water.kind, depth);
            }
        }
        return out;
    }

private:
    /**
     * @brief A tile's record from the cell under it and its point on the map (`p`: 2D on a plane,
     *        on whatever surface a shaped world's grid lies on). `across` and `along` are the
     *        tile grid's x and y in the
     *        map's frame, which a river's flow is measured against.
     *
     * `continuous_water_level` keeps `water_level` at mapcoopa's exact surface. mapcoopa
     * guarantees that surface stays under both banks of the CONTINUOUS ground; rounding it to a
     * terrace moves it by up to half a step either way, which a terraced world absorbs (the
     * shoreline hides inside a bank's wall) and a height plane does not -- there the water is a
     * flat sheet against ground that keeps falling, so half a step of lift is half a step of
     * water standing proud of the bank. The terraced readings this function also emits (`steps`,
     * `bed_kind_for`) keep using the rounded level either way: they describe a column, which is
     * terraced by definition.
     */
    TileCell record_(const coopa::maps::MapCenter& cell, const coopa::maps::MapPoint& p, double per_unit,
                     const glm::dvec3& across, const glm::dvec3& along,
                     bool continuous_water_level = false,
                     std::vector<world::TileSpanRecord>* spans_out = nullptr) const {
        TileCell r;
        r.temperature = TileCell::quantise01(cell.temperature);
        r.moisture    = TileCell::quantise01(cell.moisture);
        r.region      = cell.region >= 0 && cell.region < 0xFFFF ? static_cast<std::uint16_t>(cell.region) : TileCell::k_no_id;
        r.country     = cell.country >= 0 && cell.country < 0xFFFF ? static_cast<std::uint16_t>(cell.country) : TileCell::k_no_id;
        r.set(TileCell::Coast, cell.coast);
        r.set(TileCell::River, graph_.channel_cut(cell, p, channels_) > 0.0);

        // A river narrower than a tile would land on a tile centre only here and there -- a dotted
        // line of pits. Asked at the grid's own resolution, mapcoopa widens it to a continuous run.
        const coopa::maps::WaterSample water = water_->at(cell, p, k_river_min_half_tiles / per_unit);
        const coopa::maps::Biome biome = biomes_->at(cell, p, water);
        r.biome = static_cast<std::uint8_t>(biome);
        const std::int32_t ground_fine = fine_from_elevation_(water.ground);
        set_ground_fine_(r, ground_fine);
        TileKind top = tile_kind_for_biome(biome);
        if (water.wet()) {
            const std::int32_t level_steps = std::max(1, steps_from_elevation_(water.surface));
            const bool lava = water.fluid == coopa::maps::SurfaceFluid::Lava;
            if (biome == coopa::maps::Biome::Ice && !lava &&
                water.kind != coopa::maps::WaterKind::River) {
                // Frozen: solid ice at the water's terrace. Lava is exempt -- see the column
                // sampler: a vent's crater on an ice world stays molten.
                set_ground_fine_(r, level_steps * world::k_sub_steps);
                r.set_top_kind(TileKind::Ice);
                r.set_side_kind(TileKind::Ice);
                return r;
            }
            // A lava channel is still, not flowing: the viscosity that stops it running is the
            // whole character of the stuff, and a flowing surface would advect ripples down it
            // at a river's pace. So every body of lava, however it was shaped, meshes as one
            // still surface -- which is also why `water_is_still()` counts it.
            const bool flowing = !lava && water.kind == coopa::maps::WaterKind::River;
            if (flowing) r.set(TileCell::River, true);
            // The bed rounds down: at least a step under the water, however shallow mapcoopa's.
            set_ground_fine_(r, std::min(ground_fine, (level_steps - 1) * world::k_sub_steps));
            r.water_kind = static_cast<std::uint8_t>(
                lava ? WaterKind::Lava
                     : (water.kind == coopa::maps::WaterKind::Sea
                            ? WaterKind::Ocean
                            : (flowing ? WaterKind::River : WaterKind::Lake)));
            r.water_level = continuous_water_level
                                ? static_cast<float>(water.surface * params_.height_scale)
                                : static_cast<float>(level_steps) * params_.height_step;
            if (flowing) {
                const glm::dvec3 f(water.flow.x, water.flow.y, water.flow.z);
                r.flow = flow_octant(glm::dot(f, across), glm::dot(f, along));
                r.flow_authored = 1;   // mapcoopa's own flow, not a fallback to derive over later
            }
            top = lava ? TileKind::Basalt
                       : bed_kind_for(water.kind,
                                      static_cast<float>(level_steps - r.steps) * params_.height_step);
        }
        r.set_top_kind(top);
        r.set_side_kind(side_kind_for(top));
        // Last, because it rewrites the column it is given: a roof keeps the surface kinds the
        // ground just chose, and the ground becomes the floor of the span under it.
        if (caves_ && !caves_->empty()) {
            const coopa::maps::CaveSample cave = caves_->at(cell, p);
            if (cave.inside()) apply_cave_(r, cave, spans_out);
        }
        return r;
    }

    /**
     * @brief Turns the open spans at a column into the second solid span the record carries.
     *
     * The record holds the ground from 0 to `steps` and a roof from `roof_steps` to `roof_top`,
     * so what is written is: the span's floor as the ground, its ceiling as the roof's underside,
     * and the surface the column already had as the roof's top. Seen from outside the hill is
     * exactly what it was -- which is the invariant the whole feature rests on, and what the
     * `roof_top` rounding below protects.
     *
     * The shallowest span wins where a column has several. Daylight enters at a mouth, a mouth is
     * cut into the uppermost storey, and a lower storey hidden under a higher one is unreachable
     * either way -- so this is the only choice that cannot disconnect the part anyone can get to
     * from the part they came in through. A column that needs more than one span does not fit this
     * record and is not written here.
     *
     * @param r    The cell as the ground and water passes left it; rewritten in place.
     * @param cave Its open spans, and whether a mouth was cut through this column.
     */
    void apply_cave_(TileCell& r, const coopa::maps::CaveSample& cave,
                     std::vector<world::TileSpanRecord>* spans_out) const {

        // Not under water, and this is not a detail. A roofed column is written dry, because one
        // water surface over one bed cannot describe a surface standing ON a roof with air under
        // it. But leaving a column out of the wet set is not a local change: the coast distance
        // field is seeded from exactly that set, and the beach band and the talus relaxation
        // read it out to `cone_reach_tiles`. Measured on the shipped globe, 521 columns that
        // would have been wet were being roofed, and the surface moved on 1109 columns of real
        // terrain up to 31 steps away as a result. So a span under a river or a lake bed stays
        // shut -- which also leaves the generator's roof clearance honest, since it measured
        // that clearance against a surface with water standing on it.
        if (r.wet()) return;

        const coopa::maps::CaveSpanSample& span = cave.outermost();

        // Rounded UP, for the reason the coarse mesher rounds a block up: the outer surface must
        // never land BELOW the real ground, or the roof is a step short of the hillside beside it
        // and daylight leaks along the seam.
        const std::int32_t surface =
            (r.ground_fine() + world::k_sub_steps - 1) / world::k_sub_steps;
        // Up as well. A ceiling may be higher than the generator said, never lower -- lower eats
        // the rock clearance it guaranteed.
        const std::int32_t under = steps_from_elevation_(span.roof);
        // Down, by the same argument from the other side: a floor may be lower, never higher,
        // or the rounding eats the headroom.
        const std::int32_t floor_fine = fine_from_elevation_(span.floor);

        // A mouth's cut: the hillside here has been taken away, so the column's head is open to
        // the sky and no roof may be written over it. Without this a cave is a sealed pocket.
        if (cave.open) {
            // A cut takes rock AWAY; it never adds any. The cut is a swept disc around a
            // centreline the adit probed, so a column out at its rim can sit on ground well
            // below the floor the centreline holds -- and writing that floor there would stand a
            // pillar of cut rock out of the hillside rather than opening a way into it.
            // Measured before this clamp: 18 columns raised, one of them by 73 steps.
            set_ground_fine_(r, std::min(floor_fine, r.ground_fine()));
            r.set(TileCell::Roofed, false);
            r.set_top_kind(TileKind::Stone);
            r.set_side_kind(side_kind_for(TileKind::Stone));
            r.water_kind = 0;
            r.water_level = 0.0f;
            return;
        }

        // A one-step gap is not a cave and a one-step lid is not a roof, and both read as a seam
        // of half-formed cells rather than as geometry. Refusing them is also what makes a
        // passage taper SHUT at its rim instead of ending in one.
        if (under - floor_fine / world::k_sub_steps < k_min_cave_headroom_steps) return;
        if (surface - under < 1) return;
        if (under >= 32767 || surface >= 32767) return;

        r.roof_steps = static_cast<std::int16_t>(under);
        r.roof_top = static_cast<std::int16_t>(surface);
        r.roof_kinds = r.kinds;   // the hill keeps the surface it had
        r.set(TileCell::Roofed, true);

        // Every storey, where a caller has somewhere to put them. A roof's TOP is the floor of
        // the open span above it -- or the hill itself for the outermost -- so the list is built
        // by walking the spans shallowest-last and roofing the gap between each pair.
        if (spans_out != nullptr && cave.count > 1) {
            spans_out->clear();
            for (std::size_t i = 0; i < cave.count; ++i) {
                const coopa::maps::CaveSpanSample& below = cave.spans[i];
                const std::int32_t lid_under = steps_from_elevation_(below.roof);
                const std::int32_t lid_top = i + 1 < cave.count
                                                 ? fine_from_elevation_(cave.spans[i + 1].floor) /
                                                       world::k_sub_steps
                                                 : surface;
                if (lid_top - lid_under < 1) continue;          // too thin to be a roof
                if (lid_under >= 32767 || lid_top >= 32767) continue;
                world::TileSpanRecord rec;
                rec.under = static_cast<std::int16_t>(lid_under);
                rec.top = static_cast<std::int16_t>(lid_top);
                rec.kinds = i + 1 < cave.count
                                ? static_cast<std::uint8_t>(
                                      (static_cast<unsigned>(TileKind::Stone) & 0x0Fu) |
                                      ((static_cast<unsigned>(TileKind::Stone) & 0x0Fu) << 4))
                                : r.kinds;   // the outermost lid is the hill
                set_span_water_(rec, below, fine_from_elevation_(below.floor) / world::k_sub_steps);
                spans_out->push_back(rec);
            }
            // The deepest span's floor becomes the column's ground; the rest are lids over it.
            set_ground_fine_(r, fine_from_elevation_(cave.spans[0].floor));
            r.set_top_kind(TileKind::Stone);
            r.set_side_kind(side_kind_for(TileKind::Stone));
            r.water_kind = 0;
            r.water_level = 0.0f;
            r.flow = 0;
            r.flow_authored = 0;
            return;
        }

        set_ground_fine_(r, floor_fine);
        // Cut rock, both faces. The floor of a cave is not the biome that happens to be overhead.
        r.set_top_kind(TileKind::Stone);
        r.set_side_kind(side_kind_for(TileKind::Stone));
        // The column's OWN water is cleared, and stays cleared: `TileCell::water_level` is the
        // level of the water standing in the open air over the column, and there is no open air
        // over a roofed one. Water standing INSIDE the span is the span's, written below.
        r.water_kind = 0;
        r.water_level = 0.0f;
        r.flow = 0;
        r.flow_authored = 0;

        // A phreatic passage was cut AT its storey's table and a sump ends against it, so a
        // flooded span is the common case rather than an edge one. One roof is otherwise free of
        // the span planes entirely -- but a table has nowhere else to live, so a flooded band
        // spends one record to carry it.
        if (spans_out != nullptr) {
            world::TileSpanRecord rec;
            rec.under = static_cast<std::int16_t>(under);
            rec.top = static_cast<std::int16_t>(surface);
            rec.kinds = r.kinds;
            set_span_water_(rec, span, floor_fine / world::k_sub_steps);
            if (rec.water_kind != 0) {
                spans_out->clear();
                spans_out->push_back(rec);
            }
        }
    }

    /**
     * @brief Writes the table standing in the open band below `rec`'s roof, if any stands there.
     *
     * Rounded DOWN, which is the opposite of how a roof rounds and for the mirrored reason: a
     * surface a step higher than the generator said would stand inside the ceiling it was cut
     * under.
     *
     * A table AT or above the ceiling is a full pipe -- a phreatic passage drowned to its roof,
     * which is the commonest flooded case, since a passage cut at the table has the table at its
     * ceiling by construction. That is water, not an error, so it is clamped onto the last step
     * of air rather than refused: the passage comes out full. A table that lands on the floor
     * instead really is no water, and the band is left dry -- a sheet with no depth under it
     * reads as a glitch rather than as a sump.
     */
    void set_span_water_(world::TileSpanRecord& rec, const coopa::maps::CaveSpanSample& band,
                         std::int32_t floor_steps) const {
        if (!band.flooded || params_.height_step <= 0.0f) return;
        const double world_z = band.water * static_cast<double>(params_.height_scale);
        std::int32_t table = static_cast<std::int32_t>(
            std::floor(world_z / static_cast<double>(params_.height_step)));
        if (table >= rec.under) table = rec.under - 1;
        // A passage cut AT its own table has its floor on it, so the table rounds onto the floor
        // and a literal reading leaves it dry. What such a passage actually carries is the stream
        // at the water table -- a sheet along the floor -- so it gets one step of it.
        if (table <= floor_steps) table = floor_steps + 1;
        if (table >= rec.under) return;   // no air left to stand water in
        // Still, always: a sump has no slope to run down, and a phreatic passage is full to its
        // table rather than flowing along one. A river's channel is cut in the open air.
        rec.water_kind = static_cast<std::uint8_t>(world::WaterKind::Lake);
        rec.water_steps = static_cast<std::int16_t>(table);
    }

    /** @brief The cell whose site is nearest an arbitrary map point, walking out from `hint`. */
    std::size_t nearest_cell_(const coopa::maps::MapPoint& p, std::size_t hint) const {
        std::size_t best = hint < graph_.centers.size() ? hint : 0;
        double distance = graph_.centers[best].point.distance_to(p);
        for (int guard = 0; guard < 100000; ++guard) {
            std::size_t next = best;
            for (const coopa::maps::CenterId n : graph_.centers[best].neighbors) {
                const double d = graph_.centers[static_cast<std::size_t>(n)].point.distance_to(p);
                if (d < distance) {
                    distance = d;
                    next = static_cast<std::size_t>(n);
                }
            }
            if (next == best) break;
            best = next;
        }
        return best;
    }

public:

    /**
     * @brief Fills `out` (reshaped to the whole world, keeping whatever height_sub/layer_sub/
     *        tile_size/chunk_tiles it already carries) with every tile -- and, on a field with a
     *        height plane, the continuous height too -- then reconciles the rounding so water
     *        never stands above dry ground.
     *
     * mapcoopa's water already never stands over its own ground; rounding both to whole steps
     * independently could still put a bank a step under the terrace beside it. So on a field
     * with no height plane the rounding gives way on the GROUND's side, never the water's:
     *
     * - a dry tile beside water rounds UP to at least that water's terrace -- the bank;
     * - a flooded tile's bed rounds DOWN to at least one step under its terrace -- the bed.
     *
     * A field WITH a height plane instead shapes the plane itself (grade_water_shore()): its
     * shoreline is simply where the continuous ground crosses the water's level, so there is no
     * terrace to round toward -- and for the same reason its water levels are baked at
     * mapcoopa's exact surface rather than rounded to one (record_()'s
     * `continuous_water_level`), so a river keeps falling with the ground it runs over instead
     * of standing in flat terraces across it.
     *
     * Layer (splat) weights are NOT filled here -- they need the terrain's own layer-name mapping
     * (`MapGeneratorComponent::layers`), which this sampler has no idea exists.
     */
    void bake(TerraData& out) const {
        // A world that is not one flat square of tiles walks its own grid (mapgen/world_bake.h).
        if (const WorldBakeHook& hook = world_bake_hook(); hook && hook(*this, out)) return;
        const std::int32_t n = tiles_per_axis();
        TerraGeometry g = out.geometry();
        g.tiles_x = g.tiles_y = n;
        out.reset(g);
        const bool continuous = out.has_heights();
        for (std::int32_t y = 0; y < n; ++y)
            for (std::int32_t x = 0; x < n; ++x) out.at(x, y) = sample_cell(x, y, continuous);

        TerrainField field = borrow(out);
        if (continuous) {
            const int sub = std::max(1, g.height_sub);
            for (int j = 0; j < out.height_ny(); ++j) {
                for (int i = 0; i < out.height_nx(); ++i) {
                    const SurfaceSample s = sample_surface(static_cast<double>(i) / sub, static_cast<double>(j) / sub);
                    out.height_at(i, j) = s.ground;
                }
            }
            // Refreshes `steps` too (TerraData::mark_height_dirty()), so the terraced reading of
            // this same field (a voxel/soft-tile style switched onto it) agrees with the smooth one.
            out.mark_height_dirty(IRect{0, 0, out.height_nx(), out.height_ny()});
            grade_water_shore(field, IRect{0, 0, n, n},
                              ShoreGrade{params_.shore_clearance, params_.shore_grade_depth, params_.shore_grade_band});
        } else {
            settle_water_rounding(field, IRect{0, 0, n, n});   // terrain_water.h
            // The terraced counterpart of grade_water_shore() above, and it must follow the
            // rounding: the fill cone grades the lip that pass leaves, and the ring it pins is
            // what stops the cut cone lowering a waterline tile. Off unless the terrain asks
            // (`shore.enabled`), so a world that wants its bare terraces keeps them.
            grade_tile_shore(field, IRect{0, 0, n, n}, params_);   // terrain_shore.h
        }
    }

    /**
     * @brief Maps a mapcoopa biome onto the surface a tile face is textured with.
     *
     * Thirty-seven biomes collapse onto twelve surfaces: a face needs to know which patch of the
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
            TileKind::Basalt, // VolcanicField (open ground on a restless world: cooled, not molten)
            TileKind::Stone,  // Regolith
            TileKind::Basalt, // BasaltPlain
            TileKind::Lava,   // LavaField -- the one biome whose ground emits light
            TileKind::Clay    // SulfurFlat (clay's ochre reads as sulphur; sand reads as snow)
        };
        const std::size_t index = static_cast<std::size_t>(biome);
        return index < table.size() ? table[index] : TileKind::Stone;
    }

    /**
     * @brief What the floor of a body of water is made of, by the body and how deep it stands.
     *
     * The biome table above answers for the *shore* of a sea or lake (both map to sand), which is
     * the wrong answer for the floor under a hundred metres of water: taken unconditionally it
     * surfaces a whole ocean bed in beach sand, however deep.
     *
     * Graded by depth instead, because a bed is sorted by what the water does to it: the surf
     * zone keeps sand, below the wave base the fines settle out as silt, and the open floor is
     * scoured back to rock. A lake has no surf to speak of, so it is muddy from its margin in.
     *
     * Depth is METRES, not the terraced world's steps. A step is only a depth on a terrain whose
     * `height_step` happens to be one metre: halve the quantum and "two steps down" is a metre
     * of water, which would put the scoured floor a hand's breadth off the beach. The shipped
     * figures are the same ones a one-metre world always had.
     *
     * @param kind What kind of water stands over the tile; a river keeps its gravel bed.
     * @param depth_m Metres from the water's surface down to the bed.
     * @return The surface to texture the bed's top face with.
     */
    static TileKind bed_kind_for(coopa::maps::WaterKind kind, float depth_m) {
        if (kind == coopa::maps::WaterKind::River) return TileKind::Sand;
        if (kind == coopa::maps::WaterKind::Lake) {
            return depth_m <= k_bed_lake_margin_m ? TileKind::Sand : TileKind::Clay;
        }
        if (depth_m <= k_bed_surf_m) return TileKind::Sand;
        if (depth_m <= k_bed_silt_m) return TileKind::Clay;
        return TileKind::Stone;
    }

private:
    /** @brief Tiles per map cell, as sampled: the world's tiles over the map's cells. */
    double tiles_per_cell_() const {
        return static_cast<double>(tiles_per_axis()) / static_cast<double>(std::max(1, config_.grid_size));
    }

    /** @brief Half-width rivers are sampled at, at the least, in tiles: a 4-connected run of tiles. */
    static constexpr double k_river_min_half_tiles = 0.75;

    /**
     * @brief Least headroom an open span must have to be written, in whole steps.
     *
     * A roof's two ends are stored on whole steps -- there is no sub-step byte left for them --
     * so a span narrower than this quantises to a seam between two solids rather than to air.
     * Two is the smallest that is unambiguously a gap.
     */
    static constexpr std::int32_t k_min_cave_headroom_steps = 2;

    /** @brief Depth, in metres, to which a lake is still sandy -- its margin. */
    static constexpr float k_bed_lake_margin_m = 1.0f;
    /** @brief Depth, in metres, to which a sea floor is still sand -- the surf zone. */
    static constexpr float k_bed_surf_m = 2.0f;
    /** @brief Depth, in metres, to which a sea floor is silt; past it, scoured rock. */
    static constexpr float k_bed_silt_m = 5.0f;

    /** @brief Bucket edge in grid units. One grid unit is the nominal site spacing. */
    static constexpr double k_bucket_size = 1.0;

    /**
     * @brief Quantises normalised elevation to sub-step quanta, never below the ground plane.
     *
     * With `TerrainParams::height_fine` off this is `steps_from_elevation_()` scaled up, so the
     * result still lands exactly on a terrace. On, it floors to the finer grid instead and the
     * remainder survives as the column's sub-step lift.
     */
    std::int32_t fine_from_elevation_(double elevation) const {
        if (params_.height_step <= 0.0f) return 0;
        const std::int32_t cuts = world::sub_steps_for(params_.height_fine);
        if (cuts <= 1) return steps_from_elevation_(elevation) * world::k_sub_steps;
        const double world_z = elevation * static_cast<double>(params_.height_scale);
        const double in_steps = world_z / static_cast<double>(params_.height_step);
        return in_steps <= 0.0 ? 0 : world::quantise_fine(in_steps, cuts);
    }

    /** @brief Writes a sub-step ground height into a cell, clamped to what the record holds. */
    static void set_ground_fine_(TileCell& r, std::int32_t fine) {
        if (fine < 0) fine = 0;
        const std::int32_t steps = fine / world::k_sub_steps;
        if (steps >= 32767) {
            r.steps = 32767;
            r.ground_sub = 0;
            return;
        }
        r.steps = static_cast<std::int16_t>(steps);
        r.ground_sub = static_cast<std::uint8_t>(fine % world::k_sub_steps);
    }

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
    std::unique_ptr<coopa::maps::WaterField> water_;
    std::unique_ptr<coopa::maps::BiomeField> biomes_; /**< Borrows graph_, detail_, channels_. */
    std::unique_ptr<coopa::maps::CaveField> caves_;   /**< Borrows graph_, detail_, channels_. */

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

#endif // TOYENGINE_MAPGEN_TERRAIN_SAMPLER_H

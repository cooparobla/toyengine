/**
 * @file water_field.h
 * @brief Where the water is, and how high it stands, at any point of a generated map.
 *
 * The counterpart of `MapGraph::elevation_at()` for water, and the one answer a consumer meshing
 * a map in 3D needs: is this point under water, which body is it, how high is the surface, and
 * which way does it flow. Built on the same data the passes leave -- each body's flat
 * `MapCenter::water_level`, and the rivers' `RiverFit::Banked` surfaces -- so nothing about the
 * water is invented by the caller.
 *
 * ### The rule
 *
 * A point is wet when the ground there is **below** a surface that reaches it:
 *
 * - **A sea or lake reaches** every point whose Delaunay triangle -- the one `elevation_at()`
 *   interpolates the ground over -- has one of the body's cells as a vertex. Every shore cell
 *   stands above its body's surface (`assign_water_levels()`), so the part of such a triangle
 *   under the surface is exactly the part sloping down into the body: the shoreline is where
 *   the ground crosses the surface, wherever that falls, and never a cliff of water at a cell
 *   boundary.
 * - **A river reaches** the points inside its channel, at its banked height interpolated along
 *   the course. Banked surfaces stay under both banks, so a river is wet only in its channel.
 *
 * - **A lava pool reaches** the points inside a vent's crater, at the surface the volcano pass
 *   recorded for it. A crater is a basin like any other, so the same rule decides its shore:
 *   wet where the ground is under the surface, dry on the rim. These pools are molten whatever
 *   `MapConfig::surface_fluid` says, which is how a water world gets lava in its calderas.
 *
 * Where several reach a point, the highest wet one wins: a lake standing over the last stretch
 * of its inflow floods that channel, which is what a drowned inlet is.
 *
 * Build one per map, like `RiverChannels`; `at()` is const and safe to call from many threads.
 */

#ifndef COOPA_MAPS_WATER_FIELD_H
#define COOPA_MAPS_WATER_FIELD_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/** @brief What kind of water covers a point. */
enum class WaterKind : std::uint8_t {
    None = 0, /**< Dry ground. */
    Sea,      /**< The ocean, at `MapConfig::sea_level`. */
    Lake,     /**< An inland body, at its own spill level. */
    River,    /**< A watercourse, at its banked surface. */
};

/** @brief One answer of `WaterField::at()`. Heights are normalised like all elevation. */
struct WaterSample {
    WaterKind kind = WaterKind::None;
    /**
     * @brief What the body is filled with, from `MapConfig::surface_fluid`.
     *
     * Orthogonal to `kind` on purpose: what flooded a basin and what SHAPE the basin is are
     * two different questions, and a consumer wants both. A lava river is still a `River`,
     * with its downstream direction intact.
     */
    SurfaceFluid fluid = SurfaceFluid::Water;
    double surface = 0.0;   /**< @brief Height of the water surface; the ground when dry. */
    double ground = 0.0;    /**< @brief Height of the ground (the bed, under water). */
    MapPoint flow{0.0, 0.0};/**< @brief Unit downstream direction for a river; zero otherwise. */

    bool wet() const { return kind != WaterKind::None; }
    double depth() const { return wet() ? surface - ground : 0.0; }
};

/**
 * @class WaterField
 * @brief The water query; see the file doc for the rule it applies.
 */
class WaterField {
public:
    /**
     * @param graph The generated map. Must outlive the field.
     * @param config The config it was generated with.
     * @param detail The detail field the ground is sampled with. Must outlive the field.
     * @param channels The channels cut into that ground. Must outlive the field.
     */
    WaterField(const MapGraph& graph, const MapConfig& config, const TerrainDetail& detail,
               const RiverChannels& channels)
        : graph_(graph), detail_(detail), channels_(channels),
          rivers_(make_river_surfaces(graph, config, detail, channels, RiverFit::Banked)),
          min_depth_(meters_to_height(config, config.river_depth_m)),
          fluid_(config.surface_fluid) {
        build_pools_(graph, detail, channels);
    }

    /**
     * @brief The water at a point.
     * @param center The cell containing the point (the nearest site), as `elevation_at()` takes.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @param min_half_width For a consumer sampling coarser than a river is wide (a tile grid):
     *        rivers are treated as at least this wide either side of their centreline, in grid
     *        units, and the whole band is channel -- wherever the ground stands at or above the
     *        river there, the ground reported is a bed a river's depth under the water. 0
     *        samples the channels as they are.
     */
    WaterSample at(const MapCenter& center, double x, double y, double min_half_width = 0.0) const {
        return at(center, MapPoint{x, y}, min_half_width);
    }

    /** @brief `at()` at a point given whole -- a sphere map's form (`p` on its globe). */
    WaterSample at(const MapCenter& center, const MapPoint& p, double min_half_width = 0.0) const {
        const double x = p.x;
        const double y = p.y;
        const bool sphere = graph_.is_sphere();
        WaterSample sample;
        sample.ground = graph_.elevation_at(center, p, detail_, channels_);
        sample.surface = sample.ground;

        // Seas and lakes: any body cell among the triangle's vertices reaches the point.
        std::array<CenterId, 3> cells{};
        std::array<double, 3> weights{};
        const bool found = graph_.triangle_at(center, p, cells, weights);
        const std::size_t count = found ? 3 : 1;
        if (!found) {
            cells[0] = center.index;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const MapCenter& cell = graph_.centers[static_cast<std::size_t>(cells[i])];
            if (cell.water_level <= sample.surface) {
                continue;
            }
            sample.kind = (cell.ocean || !cell.water) ? WaterKind::Sea : WaterKind::Lake;
            sample.surface = cell.water_level;
        }

        // Rivers: the channels filed under this cell.
        const std::size_t index = static_cast<std::size_t>(center.index);
        if (index < channels_.cells.size()) {
            const RiverChannels::Cell& group = channels_.cells[index];
            const bool in_box = x >= group.min.x - min_half_width && x <= group.max.x + min_half_width &&
                                y >= group.min.y - min_half_width && y <= group.max.y + min_half_width &&
                                (!sphere || (p.z >= group.min.z - min_half_width && p.z <= group.max.z + min_half_width));
            if (group.count > 0 && in_box) {
                for (std::size_t i = group.first; i < group.first + group.count; ++i) {
                    const ChannelSegment& segment = channels_.segments[i];
                    const double distance = sphere ? distance_to_segment(p, segment.a, segment.b)
                                                   : distance_to_segment(x, y, segment.a, segment.b);
                    if (segment.river < 0 || distance >= std::max(segment.half_width, min_half_width)) {
                        continue;
                    }
                    const double height = river_height_(segment, p);
                    if (sample.wet() && height <= sample.surface) {
                        continue;   // a higher water already covers it
                    }
                    if (height <= sample.ground) {
                        // Sampled coarsely, the whole band is channel: a point on the channel's
                        // sloping side (or past it, in the widened band) is bed under the water.
                        if (min_half_width <= 0.0) continue;
                        sample.ground = height - min_depth_;
                    }
                    sample.kind = WaterKind::River;
                    sample.surface = height;
                    const double dx = segment.b.x - segment.a.x;
                    const double dy = segment.b.y - segment.a.y;
                    if (sphere) {
                        const double dz = segment.b.z - segment.a.z;
                        const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
                        sample.flow = length > 0.0 ? MapPoint{dx / length, dy / length, dz / length} : MapPoint{};
                    } else {
                        const double length = std::sqrt(dx * dx + dy * dy);
                        sample.flow = length > 0.0 ? MapPoint{dx / length, dy / length} : MapPoint{0.0, 0.0};
                    }
                }
            }
        }
        // Lava pools: the craters filed under this cell.
        bool molten = false;
        if (index < pool_cells_.size()) {
            const Bucket& bucket = pool_cells_[index];
            for (std::size_t i = bucket.first; i < bucket.first + bucket.count; ++i) {
                const LavaPool& pool = pools_[pool_refs_[i]];
                if (pool.surface <= sample.surface) {
                    continue;   // a higher water already covers it, or the rim stands over it
                }
                if (p.distance_to(pool.point) >= pool.radius) {
                    continue;
                }
                sample.kind = WaterKind::Lake;
                sample.surface = pool.surface;
                sample.flow = MapPoint{};
                molten = true;
            }
        }

        // What is in it: the map's fluid, except where a crater overrides it. A dry point keeps
        // `Water`, which is meaningless there and never read -- `wet()` gates every consumer.
        if (sample.wet()) {
            sample.fluid = molten ? SurfaceFluid::Lava : fluid_;
        }
        return sample;
    }

    /** @brief The banked river surfaces the field was built with. */
    const RiverSurfaces& rivers() const { return rivers_; }

private:
    /** @brief One vent's molten crater, in the field's own absolute terms. */
    struct LavaPool {
        MapPoint point;        /**< @brief The vent, in grid units. */
        double radius = 0.0;   /**< @brief How far the pool reaches from it. */
        double surface = 0.0;  /**< @brief Where the lava stands, in normalised elevation. */
    };

    /** @brief A slice of `pool_refs_`: the pools one cell can see. */
    struct Bucket {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };

    /**
     * @brief Turns the volcano pass's relative records into absolute surfaces, and indexes them.
     *
     * `MapVolcano::lava_rise` is measured from the vent's own ground because the pass cannot
     * sample the surface a consumer bakes -- it has no detail field. Here both are in hand, so
     * the floor is read once per vent and the lake gets a flat surface, exactly as a lake's
     * `water_level` is flat.
     *
     * The index is what keeps `at()` cheap: a vent is filed under every cell whose polygon could
     * contain a point inside its pool, which is every cell within the pool's radius plus that
     * cell's own reach -- so a query scans the handful of pools that can possibly cover it
     * rather than all of them. One pass per vent over the cells, once per map.
     */
    void build_pools_(const MapGraph& graph, const TerrainDetail& detail,
                      const RiverChannels& channels) {
        for (const MapVolcano& vent : graph.volcanoes) {
            if (vent.lava_rise <= 0.0 || vent.lava_radius <= 0.0 || vent.center == k_invalid_id) {
                continue;
            }
            const MapCenter& home = graph.centers[static_cast<std::size_t>(vent.center)];
            const double floor = graph.elevation_at(home, vent.point, detail, channels);
            pools_.push_back(LavaPool{vent.point, vent.lava_radius, floor + vent.lava_rise});
        }
        if (pools_.empty()) {
            return;
        }
        pool_cells_.assign(graph.centers.size(), Bucket{});
        for (std::size_t pass = 0; pass < 2; ++pass) {
            if (pass == 1) {
                std::uint32_t total = 0;
                for (Bucket& bucket : pool_cells_) {
                    bucket.first = total;
                    total += bucket.count;
                    bucket.count = 0;
                }
                pool_refs_.resize(total);
            }
            for (std::size_t ci = 0; ci < graph.centers.size(); ++ci) {
                const MapCenter& cell = graph.centers[ci];
                double reach = 0.0;
                for (const CornerId corner : cell.corners) {
                    if (corner == k_invalid_id) continue;
                    reach = std::max(reach, cell.point.distance_to(
                                                graph.corners[static_cast<std::size_t>(corner)].point));
                }
                for (std::size_t pi = 0; pi < pools_.size(); ++pi) {
                    if (cell.point.distance_to(pools_[pi].point) > pools_[pi].radius + reach) {
                        continue;
                    }
                    if (pass == 0) {
                        ++pool_cells_[ci].count;
                    } else {
                        pool_refs_[pool_cells_[ci].first + pool_cells_[ci].count++] =
                            static_cast<std::uint32_t>(pi);
                    }
                }
            }
        }
    }

    /**
     * @brief A river's surface at a point of one segment, blended toward its neighbours.
     *
     * The table holds one height per segment; read as constant, a river would descend in steps.
     * Treated as the height at each segment's midpoint and interpolated between them, it falls
     * smoothly -- and stays falling, since a blend of a falling sequence falls.
     */
    double river_height_(const ChannelSegment& segment, const MapPoint& p) const {
        const double x = p.x;
        const double y = p.y;
        const std::size_t river = static_cast<std::size_t>(segment.river);
        const std::size_t i = static_cast<std::size_t>(segment.segment);
        const double dx = segment.b.x - segment.a.x;
        const double dy = segment.b.y - segment.a.y;
        double t = 0.5;
        if (graph_.is_sphere()) {
            const double dz = segment.b.z - segment.a.z;
            const double ls = dx * dx + dy * dy + dz * dz;
            if (ls > 0.0) {
                t = std::clamp(((x - segment.a.x) * dx + (y - segment.a.y) * dy + (p.z - segment.a.z) * dz) / ls,
                               0.0, 1.0);
            }
        } else {
            const double length_squared = dx * dx + dy * dy;
            t = length_squared > 0.0
                    ? std::clamp(((x - segment.a.x) * dx + (y - segment.a.y) * dy) / length_squared, 0.0, 1.0)
                    : 0.5;
        }
        const double here = rivers_.at(river, i);
        if (segment.mouth) return here;   // the body's own level, all the way
        const std::size_t spans = river < rivers_.heights.size() ? rivers_.heights[river].size() : 0;
        if (t < 0.5) {
            const double before = i > 0 ? rivers_.at(river, i - 1) : here;
            return before + (here - before) * (t + 0.5);
        }
        const double after = i + 1 < spans ? rivers_.at(river, i + 1) : here;
        return here + (after - here) * (t - 0.5);
    }

    const MapGraph& graph_;
    const TerrainDetail& detail_;
    const RiverChannels& channels_;
    RiverSurfaces rivers_;
    double min_depth_ = 0.0; ///< A widened channel's bed under its water: `river_depth_m`.
    std::vector<LavaPool> pools_;
    std::vector<Bucket> pool_cells_;
    std::vector<std::uint32_t> pool_refs_;
    SurfaceFluid fluid_ = SurfaceFluid::Water; ///< What fills every body on this map.
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_WATER_FIELD_H

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
          min_depth_(meters_to_height(config, config.river_depth_m)) {}

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
        return sample;
    }

    /** @brief The banked river surfaces the field was built with. */
    const RiverSurfaces& rivers() const { return rivers_; }

private:
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
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_WATER_FIELD_H

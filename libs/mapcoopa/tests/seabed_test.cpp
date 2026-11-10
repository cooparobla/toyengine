/**
 * @file seabed_test.cpp
 * @brief The ground under the water: sea floors with relief, lakes with basins, and a shoreline
 *        neither of them moves.
 *
 * The flat bed these cases exist to rule out. Every landform of `TerrainModel::Natural` is gated
 * on height *above* the waterline, so all of them are exactly zero below it; with no second set
 * for the bed, and with one single depth for every cell of a lake, a sea floor is a hundredth of
 * the land's local relief and a kilometre-wide lake is a pan of exactly zero bed spread, on every
 * seed.
 *
 * Where the water is and how high it stands is water_field_test; this is what the water stands on.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <queue>
#include <vector>

#include <coopa/maps/map_presets.h>
#include <coopa/maps/passes/water_levels.h>
#include <coopa/maps/water_field.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("seabed");

namespace {

/** @brief A world with natural terrain, as a 3D host generates one. */
MapConfig bed_config(int seed) {
    MapConfig config = world_config(seed);
    config.terrain_model = TerrainModel::Natural;
    return derived_config(config);
}

/** @brief The cell whose site is nearest, by brute force. */
const MapCenter& nearest(const MapGraph& graph, const MapPoint& point) {
    std::size_t best = 0;
    double distance = 1e30;
    for (std::size_t i = 0; i < graph.centers.size(); ++i) {
        const double d = graph.centers[i].point.distance_to(point);
        if (d < distance) {
            distance = d;
            best = i;
        }
    }
    return graph.centers[best];
}

/** @brief One generated world, with everything needed to sample its ground and water. */
struct World {
    explicit World(int seed)
        : config(bed_config(seed)), generator(config, maps_logger()) {
        generator.generate();
        surface = std::make_unique<TerrainSurface>(generator.graph(), config);
        channels = make_river_channels(generator.graph(), config, surface->detail());
        water = std::make_unique<WaterField>(generator.graph(), config, surface->detail(), channels);
    }
    const MapGraph& graph() const { return generator.graph(); }

    MapConfig config;
    MapGenerator generator;
    std::unique_ptr<TerrainSurface> surface;
    RiverChannels channels;
    std::unique_ptr<WaterField> water;
};

/** @brief Every cell of one lake, flood-filled from `seed`. */
std::vector<CenterId> lake_body(const MapGraph& graph, CenterId seed, std::vector<bool>& visited) {
    std::vector<CenterId> body;
    std::queue<CenterId> pending;
    pending.push(seed);
    visited[static_cast<std::size_t>(seed)] = true;
    while (!pending.empty()) {
        const CenterId current = pending.front();
        pending.pop();
        body.push_back(current);
        for (const CenterId id : graph.centers[static_cast<std::size_t>(current)].neighbors) {
            const MapCenter& neighbor = graph.centers[static_cast<std::size_t>(id)];
            if (neighbor.water && !neighbor.ocean && !visited[static_cast<std::size_t>(id)]) {
                visited[static_cast<std::size_t>(id)] = true;
                pending.push(id);
            }
        }
    }
    return body;
}

/** @brief The median of a sample, which is what a bed's typical relief wants rather than a mean. */
double median(std::vector<double>& values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace

/**
 * @brief A sea floor has relief, on the same order as the land beside it.
 *
 * Measured as the median height change between two samples a tenth of a cell apart -- a tile
 * grid's own resolution -- in metres, so the comparison against the land is one of terrain
 * against terrain rather than of one normalised field against another.
 *
 * The number this guards: with no bed landforms the floor moves 0.1 m per 3.75 m of ground where
 * the land moves 1.9 m, an eighteenth. A bed is smoother than a hillside, so the bar here is a
 * fifth of the land's relief, not parity.
 */
COOPA_TEST(sea_floors_are_not_flat) {
    std::size_t measured = 0;
    for (const int seed : {17, 31, 61}) {
        const World world(seed);
        const MapGraph& graph = world.graph();
        const double step = 0.1;   // of a grid unit
        std::vector<double> wet;
        std::vector<double> dry;
        const int n = world.config.grid_size * 4;
        const double span = static_cast<double>(world.config.grid_size) / n;
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const MapPoint a{x * span, y * span};
                const MapPoint b{x * span + step, y * span};
                const WaterSample sa = world.water->at(nearest(graph, a), a);
                const WaterSample sb = world.water->at(nearest(graph, b), b);
                if (sa.kind == WaterKind::River || sb.kind == WaterKind::River) continue;
                const double change = height_to_meters(world.config, std::abs(sa.ground - sb.ground));
                (sa.wet() && sb.wet() ? wet : (!sa.wet() && !sb.wet() ? dry : wet)).push_back(change);
            }
        }
        ASSERT_TRUE(wet.size() > 100 && dry.size() > 100);
        const double bed = median(wet);
        const double land = median(dry);
        std::printf("seabed: seed %d -- bed %.3f m, land %.3f m per %.2f grid units\n", seed, bed, land, step);
        ASSERT_TRUE(bed * 5.0 >= land);
        ++measured;
    }
    ASSERT_TRUE(measured == 3);
}

/**
 * @brief The bed relief is confined to the water: above the waterline the ground is untouched.
 *
 * The shoreline is where the ground crosses the water's surface, and every pass downstream is
 * derived from where that falls -- so turning the bed shaping off must not move a single dry
 * sample. Checked by sampling one world twice, with the shaping on and off, which also proves
 * the two are the *same* surface rather than two that happen to agree near the shore.
 */
COOPA_TEST(bed_relief_leaves_dry_ground_alone) {
    MapConfig shaped = bed_config(31);
    MapConfig bare = shaped;
    bare.natural.seabed = 0.0;
    bare.natural.seabed_ridges = 0.0;

    MapGenerator generator(shaped, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    const TerrainSurface with(graph, shaped);
    const TerrainSurface without(graph, bare);

    const int n = graph.centers.empty() ? 0 : shaped.grid_size * 4;
    const double span = static_cast<double>(shaped.grid_size) / std::max(1, n);
    std::size_t dry = 0;
    std::size_t deeper = 0;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const MapPoint p{x * span, y * span};
            const MapCenter& cell = nearest(graph, p);
            const double a = graph.elevation_at(cell, p, with.detail());
            const double b = graph.elevation_at(cell, p, without.detail());
            // Dry: the unshaped surface -- which is the ground the water levels were set
            // against -- stands at or above every water its triangle reaches.
            std::array<CenterId, 3> cells{};
            std::array<double, 3> weights{};
            double water = 0.0;
            if (graph.triangle_at(cell, p, cells, weights)) {
                for (const CenterId id : cells) {
                    water = std::max(water, graph.centers[static_cast<std::size_t>(id)].water_level);
                }
            }
            if (b >= water) {
                ++dry;
                ASSERT_TRUE(a == b);
            } else if (a != b) {
                ++deeper;
            }
        }
    }
    std::printf("seabed: %zu dry samples unmoved, %zu wet samples reshaped\n", dry, deeper);
    ASSERT_TRUE(dry > 1000);
    ASSERT_TRUE(deeper > 1000);
}

/**
 * @brief Once a point's carved depth reaches `lake_depth_m`, shaping may not erase most of it.
 *
 * `lake_beds_are_basins` proves the basin is cut correctly at the cell centres
 * `assign_water_levels()` works on; nothing ever samples a cell centre directly, though -- every
 * consumer queries a point through `natural_elevation_()`'s land and bed shaping instead. The
 * land terms are gated on distance from water, not on water depth itself, so they apply at full
 * amplitude across a lake's interior, and only `raise` (never `lower`) applies near one -- a
 * one-directional leak with no size limit of its own, left unchecked by the older seabed-relief
 * clamp (which only ever halves what's left, and only after the land leak has already run).
 *
 * Checked the same way `bed_relief_leaves_dry_ground_alone` checks dry invariance: one generated
 * world sampled through two `TerrainSurface`s built from it, one with every land and bed
 * amplitude zero. The zeroed one reduces `natural_elevation_()` to its bare carved interpolation
 * (`base`), which gives `headroom0 = water - base` -- the point's true carved depth, independent
 * of shaping -- without duplicating that formula here.
 */
COOPA_TEST(shaping_cannot_erase_a_carved_lake_floor) {
    std::size_t protected_points = 0;
    for (const int seed : {17, 31, 61, 67}) {
        MapConfig shaped = bed_config(seed);
        MapConfig bare = shaped;
        bare.natural.hills = 0.0;
        bare.natural.mountains = 0.0;
        bare.natural.plateaus = 0.0;
        bare.natural.seabed = 0.0;
        bare.natural.seabed_ridges = 0.0;

        MapGenerator generator(shaped, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();
        const TerrainSurface with(graph, shaped);
        const TerrainSurface without(graph, bare);
        const double least_depth = meters_to_height(shaped, shaped.lake_depth_m);
        ASSERT_TRUE(least_depth > 0.0);

        const int n = shaped.grid_size * 4;
        const double span = static_cast<double>(shaped.grid_size) / n;
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const MapPoint p{x * span, y * span};
                const MapCenter& cell = nearest(graph, p);
                std::array<CenterId, 3> cells{};
                std::array<double, 3> weights{};
                if (!graph.triangle_at(cell, p, cells, weights)) continue;
                double water = 0.0;
                for (const CenterId id : cells) {
                    water = std::max(water, graph.centers[static_cast<std::size_t>(id)].water_level);
                }
                if (water <= 0.0) continue;
                const double base = graph.elevation_at(cell, p, without.detail());
                const double headroom0 = water - base;
                // Only points the carving already called fully deep -- half credit near a shore
                // is lake_beds_are_basins's job, not this one's; the floor's own taper means a
                // point just past the shelf is deliberately not held to the far-reach guarantee.
                if (headroom0 < least_depth + 1e-9) continue;
                const double actual = graph.elevation_at(cell, p, with.detail());
                const double depth = water - actual;
                ASSERT_TRUE(depth >= 0.75 * headroom0);
                ++protected_points;
            }
        }
    }
    std::printf("seabed: %zu points at or past the carved minimum depth, none eroded past a quarter\n",
                protected_points);
    ASSERT_TRUE(protected_points > 100);
}

/**
 * @brief Every lake is a basin: its bed falls away from its shoreline by the configured grade.
 *
 * One flat depth for the whole body gives a bed spread of exactly zero on every lake of every
 * seed -- a kilometre across and a pan. What is asserted is the rule itself, cell by cell: no
 * part of a bed is shallower than the grade asks for at its own distance from the shore. That
 * holds for a narrow lake, whose every cell is a shore cell and which therefore has no middle to
 * be deep in; the spread below is asked only of a lake that reaches a cell clear of its own
 * shore.
 *
 * Asserted on the cell heights, which is where the basin is cut; the finer relief sampled over
 * it is this suite's first case.
 */
COOPA_TEST(lake_beds_are_basins) {
    std::size_t wide_lakes = 0;
    for (const int seed : {17, 31, 61, 67}) {
        const World world(seed);
        const MapGraph& graph = world.graph();
        const MapConfig& config = world.config;
        const double least = meters_to_height(config, config.lake_depth_m);
        const double cap = std::max(least, meters_to_height(config, config.lake_depth_max_m));
        const double grade = meters_to_height(config, config.lake_bed_grade * config.meters_per_grid_unit);
        std::vector<bool> visited(graph.centers.size(), false);
        for (const MapCenter& cell : graph.centers) {
            if (!cell.water || cell.ocean || visited[static_cast<std::size_t>(cell.index)]) continue;
            const std::vector<CenterId> body = lake_body(graph, cell.index, visited);
            const std::vector<double> from_shore = shoreline_distances(graph, body);
            const double surface = cell.water_level;

            double shallowest = 0.0;
            double deepest = 0.0;
            double reach = 0.0;
            for (std::size_t i = 0; i < body.size(); ++i) {
                const MapCenter& bed = graph.centers[static_cast<std::size_t>(body[i])];
                const double depth = surface - bed.elevation;
                // The basin, stated as the rule that cuts it: at least the configured least
                // depth, plus the grade's worth for every grid unit out from the shoreline --
                // except where the floor of the height field gets in the way first.
                const double asked = std::min({cap, least + grade * from_shore[i], surface});
                ASSERT_TRUE(depth >= asked - 1e-12);
                if (i == 0 || depth < shallowest) shallowest = depth;
                deepest = std::max(deepest, depth);
                reach = std::max(reach, from_shore[i]);
            }
            if (reach < 1.0) continue;   // a lake with no cell clear of its own shore
            ++wide_lakes;
            // A basin, not a pan: measured against the grade across the body's own reach, so
            // the bar says the same thing about a tarn as about an inland sea. Half of it,
            // because the shallowest cell is itself already a little way offshore.
            ASSERT_TRUE(deepest - shallowest > 0.5 * grade * reach);
        }
    }
    std::printf("seabed: %zu lakes reaching a cell clear of their shore, all basins\n", wide_lakes);
    ASSERT_TRUE(wide_lakes >= 3);
}

/**
 * @brief A lake's depth follows its size: a pond stays shallow where a wide lake does not.
 *
 * `lake_depth_m` on its own is one depth for every body on the map, which sounds a tarn and an
 * inland sea the same. The grade is what ties depth to how far the water reaches from its shore.
 */
COOPA_TEST(wide_lakes_are_deeper_than_ponds) {
    double widest = 0.0;
    double widest_depth = 0.0;
    double narrowest = 1e30;
    double narrowest_depth = 0.0;
    for (const int seed : {17, 31, 61, 67}) {
        const World world(seed);
        const MapGraph& graph = world.graph();
        std::vector<bool> visited(graph.centers.size(), false);
        for (const MapCenter& cell : graph.centers) {
            if (!cell.water || cell.ocean || visited[static_cast<std::size_t>(cell.index)]) continue;
            const std::vector<CenterId> body = lake_body(graph, cell.index, visited);
            const std::vector<double> from_shore = shoreline_distances(graph, body);
            double reach = 0.0;
            double depth = 0.0;
            for (std::size_t i = 0; i < body.size(); ++i) {
                reach = std::max(reach, from_shore[i]);
                depth = std::max(depth, cell.water_level
                                        - graph.centers[static_cast<std::size_t>(body[i])].elevation);
            }
            if (reach > widest) {
                widest = reach;
                widest_depth = depth;
            }
            if (reach < narrowest) {
                narrowest = reach;
                narrowest_depth = depth;
            }
        }
    }
    ASSERT_TRUE(widest > narrowest);
    const MapConfig config = bed_config(17);
    std::printf("seabed: widest lake reaches %.2f cells and is %.1f m deep; narrowest %.2f cells, %.1f m\n",
                widest, height_to_meters(config, widest_depth), narrowest,
                height_to_meters(config, narrowest_depth));
    ASSERT_TRUE(widest_depth > narrowest_depth * 1.5);
}

/**
 * @brief A body with no dry shore anywhere reads zero everywhere, and so keeps one flat depth.
 *
 * The degenerate case `shoreline_distances()` has to answer for without looping forever or
 * handing back the infinity it starts from: a flooded pocket ringed entirely by other water has
 * no shoreline to slope away from.
 */
COOPA_TEST(a_body_with_no_shore_has_no_slope) {
    const World world(31);
    const MapGraph& graph = world.graph();
    // Every water cell on the map as one "body": nothing in it touches dry land from the
    // inside, because its own fill already swallowed the shore.
    std::vector<CenterId> all_water;
    for (const MapCenter& cell : graph.centers) {
        if (cell.water) all_water.push_back(cell.index);
    }
    ASSERT_TRUE(!all_water.empty());
    const std::vector<double> distances = shoreline_distances(graph, all_water);
    ASSERT_EQ(distances.size(), all_water.size());
    // The sea reaches the border, whose cells have no dry neighbour at all -- so some read zero
    // while the shore cells read their half-step. None may come back as the sentinel.
    for (const double d : distances) {
        ASSERT_TRUE(std::isfinite(d) && d >= 0.0 && d < 1e6);
    }

    // And a body truly ringed by water: one cell, fed to the walk on its own.
    for (const MapCenter& cell : graph.centers) {
        if (!cell.water) continue;
        bool all_wet = true;
        for (const CenterId id : cell.neighbors) {
            all_wet = all_wet && graph.centers[static_cast<std::size_t>(id)].water;
        }
        if (!all_wet) continue;
        const std::vector<double> one = shoreline_distances(graph, {cell.index});
        ASSERT_EQ(one.size(), std::size_t{1});
        ASSERT_TRUE(one[0] == 0.0);
        return;
    }
}

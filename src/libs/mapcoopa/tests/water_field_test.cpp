/**
 * @file water_field_test.cpp
 * @brief The physical water: lakes stand under their shores, rivers under their banks, and
 *        `WaterField` never puts water above the dry ground beside it.
 *
 * The 2D water layer is water_test / river_surface_test. This suite is what a consumer meshing the
 * water over the ground in 3D relies on.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include <coopa/maps/map_presets.h>
#include <coopa/maps/water_field.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("water_field");

namespace {

/** @brief Five worlds, each in both terrain models: every guarantee holds on either surface. */
std::vector<MapConfig> worlds() {
    std::vector<MapConfig> out;
    for (int seed : {17, 31, 61, 67, 251}) {
        out.push_back(world_config(seed));
        MapConfig natural = world_config(seed);
        natural.terrain_model = TerrainModel::Natural;
        out.push_back(natural);
    }
    return out;
}

/** @brief The cell whose site is nearest, by brute force -- see SurfaceProbe for why. */
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

} // namespace

/**
 * @brief Every lake stands under every cell of its shore, over a bed at least `lake_depth_m` deep.
 *
 * It used to take the highest bed in the body, which put a lake on a slope up to a hundred metres
 * over the low side of its own shore.
 */
COOPA_TEST(lakes_stand_under_their_shores) {
    std::size_t lakes = 0;
    for (const MapConfig& config : worlds()) {
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();
        const double depth = meters_to_height(config, config.lake_depth_m);
        for (const MapCenter& cell : graph.centers) {
            if (!cell.water || cell.ocean) continue;
            ++lakes;
            ASSERT_TRUE(cell.elevation <= cell.water_level - depth + 1e-12);
            for (const CenterId id : cell.neighbors) {
                const MapCenter& shore = graph.centers[static_cast<std::size_t>(id)];
                if (shore.water) continue;
                // Strictly above, except a shore at the waterline itself, where the lake is the sea's level.
                ASSERT_TRUE(shore.elevation > cell.water_level || cell.water_level == config.sea_level);
            }
        }
    }
    ASSERT_TRUE(lakes > 0);
}

COOPA_TEST(banked_rivers_fall_and_hold_water_in_their_channels) {
    for (const MapConfig& config : worlds()) {
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();
        const TerrainSurface surface(graph, config);
        const TerrainDetail& detail = surface.detail();
        const RiverChannels channels = make_river_channels(graph, config, detail);
        const WaterField field(graph, config, detail, channels);

        std::size_t checked = 0;
        // The course falls. (Its last entry is the mouth, at the level of the body it runs into --
        // which a lake can hold above its inflow's last stretch: the lake floods that channel.)
        for (const std::vector<double>& profile : field.rivers().heights) {
            for (std::size_t i = 1; i + 1 < profile.size(); ++i) ASSERT_TRUE(profile[i] <= profile[i - 1]);
        }
        // Down the middle of every channel there is water: the cut was made to hold it.
        for (const ChannelSegment& segment : channels.segments) {
            if (segment.depth <= 0.0) continue;
            const MapPoint middle{(segment.a.x + segment.b.x) * 0.5, (segment.a.y + segment.b.y) * 0.5};
            const WaterSample sample = field.at(nearest(graph, middle), middle.x, middle.y);
            ASSERT_TRUE(sample.wet());
            ++checked;
        }
        ASSERT_TRUE(checked > 0);
    }
}

/**
 * @brief Seas and lakes never stand above the dry ground beside them.
 *
 * Sampled on a grid finer than the cells: wherever a sample is under a sea or lake, no dry
 * neighbour sample may lie below that water's surface. (Rivers are held by their banks instead,
 * and a waterfall is allowed to be steep, so they are the test above.) A sample a ridge apart can
 * still flag across the crest, so one in two thousand is allowed.
 */
COOPA_TEST(seas_and_lakes_never_stand_over_dry_ground) {
    MapConfig standard = derived_config(standard_config());
    set_render_size(standard, 256);
    std::vector<MapConfig> configs = worlds();
    configs.push_back(standard);
    standard.terrain_model = TerrainModel::Natural;
    configs.push_back(standard);
    for (const MapConfig& config : configs) {
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();
        const TerrainSurface surface(graph, config);
        const TerrainDetail& detail = surface.detail();
        const RiverChannels channels = make_river_channels(graph, config, detail);
        const WaterField field(graph, config, detail, channels);

        const int n = config.grid_size * 4;
        const double step = static_cast<double>(config.grid_size) / n;
        std::vector<WaterSample> samples(static_cast<std::size_t>((n + 1) * (n + 1)));
        for (int y = 0; y <= n; ++y) {
            for (int x = 0; x <= n; ++x) {
                const MapPoint p{x * step, y * step};
                samples[static_cast<std::size_t>(y * (n + 1) + x)] = field.at(nearest(graph, p), p.x, p.y);
            }
        }
        const double tolerance = meters_to_height(config, 0.5);
        std::size_t wet = 0;
        std::size_t over = 0;
        for (int y = 0; y <= n; ++y) {
            for (int x = 0; x <= n; ++x) {
                const WaterSample& a = samples[static_cast<std::size_t>(y * (n + 1) + x)];
                if (a.kind != WaterKind::Sea && a.kind != WaterKind::Lake) continue;
                ++wet;
                ASSERT_TRUE(a.ground < a.surface);
                const int dx[4] = {1, -1, 0, 0};
                const int dy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    const int bx = x + dx[k];
                    const int by = y + dy[k];
                    if (bx < 0 || by < 0 || bx > n || by > n) continue;
                    const WaterSample& b = samples[static_cast<std::size_t>(by * (n + 1) + bx)];
                    if (!b.wet() && b.ground < a.surface - tolerance) ++over;
                }
            }
        }
        ASSERT_TRUE(wet > 0);
        std::printf("water_field: %zu wet samples, %zu beside lower dry ground\n", wet, over);
        ASSERT_TRUE(over * 2000 <= wet);
    }
}

COOPA_TEST(water_field_names_its_bodies) {
    const MapGenerator& generator = shared_world();
    const MapGraph& graph = generator.graph();
    const MapConfig& config = generator.config();
    const TerrainSurface surface(graph, config);
    const TerrainDetail& detail = surface.detail();
    const RiverChannels channels = make_river_channels(graph, config, detail);
    const WaterField field(graph, config, detail, channels);
    std::size_t seas = 0;
    std::size_t lakes = 0;
    for (const MapCenter& cell : graph.centers) {
        if (!cell.water || cell.border) continue;
        const WaterSample sample = field.at(cell, cell.point.x, cell.point.y);
        if (sample.ground >= cell.water_level) continue;  // a coastal cell whose site is up on the shore
        ASSERT_TRUE(sample.kind == (cell.ocean ? WaterKind::Sea : WaterKind::Lake));
        ASSERT_TRUE(sample.surface == cell.water_level);
        ASSERT_TRUE(sample.depth() > 0.0);
        (cell.ocean ? seas : lakes) += 1;
    }
    ASSERT_TRUE(seas > 0 && lakes > 0);
}

/**
 * @brief Every river runs on into the water it feeds: from its last point to the body's own
 *        cell, the line is wet all the way.
 *
 * A river's course ends on the coastline's corner, but the sea starts only where the ground falls
 * under its level, often a strip of shore further out; the mouth segment carries the channel on
 * through it. Sampled at a tile-like coarseness, as a tile terrain asks.
 */
COOPA_TEST(rivers_run_into_the_water_they_feed) {
    std::size_t checked = 0;
    for (const MapConfig& config : worlds()) {
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();
        const TerrainSurface surface(graph, config);
        const TerrainDetail& detail = surface.detail();
        const RiverChannels channels = make_river_channels(graph, config, detail);
        const WaterField field(graph, config, detail, channels);
        for (const ChannelSegment& mouth : channels.segments) {
            const MapRiver& river = graph.rivers[static_cast<std::size_t>(mouth.river)];
            if (static_cast<std::size_t>(mouth.segment) + 1 != river.points.size()) continue;   // not a mouth
            for (int i = 0; i <= 10; ++i) {
                const double t = i / 10.0;
                const MapPoint p{mouth.a.x + (mouth.b.x - mouth.a.x) * t, mouth.a.y + (mouth.b.y - mouth.a.y) * t};
                ASSERT_TRUE(field.at(nearest(graph, p), p.x, p.y, 0.2).wet());
            }
            ++checked;
        }
    }
    ASSERT_TRUE(checked > 20);
}

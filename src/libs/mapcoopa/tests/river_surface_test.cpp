/**
 * @file river_surface_test.cpp
 * @brief The river channel cut into the sampled height field, and the water surface drawn over
 *        it: locality, seams, visibility, monotone fall, joins at mouths and confluences.
 *
 * These are the invariants a consumer meshing the elevation and water layers together relies
 * on. Routing (where rivers go) is rivers_test.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("river_surface");

/**
 * @brief The channel cut is strictly local -- away from a river it changes nothing.
 *
 * The cut lives in the sampling path, not the control mesh, so the guarantee that matters is
 * that it is *only* a channel: anywhere further than a river's own width from a centreline the
 * surface must be the one the detail overload already produced, bit for bit.
 */
COOPA_TEST(river_channels_are_zero_away_from_water) {
    const MapGenerator& generator = shared_river_world();
    const MapConfig& config = generator.config();
    const MapGraph& graph = generator.graph();

    const Noise terrain(config.noise_terrain);
    const TerrainDetail detail = make_terrain_detail(config, terrain);
    const RiverChannels channels = make_river_channels(graph, config);
    ASSERT_TRUE(!channels.empty());

    std::size_t checked = 0;
    for (const MapCenter& center : graph.centers) {
        // A cell site is over half a cell from its own boundary, and rivers run along
        // boundaries -- so no land site is ever inside a channel. (A water cell's site is where a
        // river's mouth segment runs to: the channel carried on into the body it feeds.)
        if (center.water) continue;
        const double x = center.point.x;
        const double y = center.point.y;
        ASSERT_TRUE(graph.elevation_at(center, x, y, detail, channels)
                    == graph.elevation_at(center, x, y, detail));
        ++checked;
    }
    ASSERT_TRUE(checked > 0);
}

/** @brief Zero depth leaves the sampled surface exactly as the control mesh describes it. */
COOPA_TEST(river_channel_zero_depth_is_the_uncut_surface) {
    const MapGenerator& generator = shared_river_world();
    const MapConfig& config = generator.config();
    const MapGraph& graph = generator.graph();

    MapConfig flat = config;
    flat.river_channel_depth_m = 0.0;
    flat.river_channel_depth_per_volume_m = 0.0;

    const Noise terrain(config.noise_terrain);
    const TerrainDetail detail = make_terrain_detail(config, terrain);
    const RiverChannels none = make_river_channels(graph, flat);
    ASSERT_TRUE(none.empty());

    for (const MapRiver& river : graph.rivers) {
        const std::size_t spans = river.points.size() - 1;
        for (std::size_t i = 0; i < river.points.size(); ++i) {
            const MapCorner& corner = graph.corners[static_cast<std::size_t>(
                river.corners[std::min(river.corners.size() - 1,
                                       i * river.corners.size() / std::max<std::size_t>(1, spans))])];
            if (corner.touches.empty()) {
                continue;
            }
            const MapCenter& center = graph.centers[static_cast<std::size_t>(corner.touches.front())];
            const MapPoint& point = river.points[i];
            ASSERT_TRUE(graph.elevation_at(center, point.x, point.y, detail, none)
                        == graph.elevation_at(center, point.x, point.y, detail));
        }
    }
}

/**
 * @brief The cut surface joins across a cell boundary, where it is deepest.
 *
 * The counterpart of the seam check in terrain_test `elevation_interpolates_and_joins`, run on
 * the channel path. A river runs *along* a boundary, so the two cells either side sample the
 * deepest part of the cut from opposite directions -- and if they disagreed about which segments
 * exist, every watercourse would be hemmed by a visible seam. They agree because a segment is
 * filed under every cell its corner touches, which is both of them.
 */
COOPA_TEST(river_channels_join_across_cells) {
    const MapGenerator& generator = shared_river_world();
    const MapConfig& config = generator.config();
    const MapGraph& graph = generator.graph();

    const Noise terrain(config.noise_terrain);
    const TerrainDetail detail = make_terrain_detail(config, terrain);
    const RiverChannels channels = make_river_channels(graph, config);

    std::size_t checked = 0;
    for (const MapEdge& edge : graph.edges) {
        if (edge.river <= 0 || edge.d0 == k_invalid_id || edge.d1 == k_invalid_id
            || edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
            continue;
        }
        const MapCenter& a = graph.centers[static_cast<std::size_t>(edge.d0)];
        const MapCenter& b = graph.centers[static_cast<std::size_t>(edge.d1)];
        const MapPoint& v0 = graph.corners[static_cast<std::size_t>(edge.v0)].point;
        const MapPoint& v1 = graph.corners[static_cast<std::size_t>(edge.v1)].point;

        // Sampled along the shared edge rather than only at its midpoint, since a mismatch
        // could sit anywhere the two cells' segment groups differ.
        for (double t = 0.1; t <= 0.9; t += 0.2) {
            const double x = v0.x + (v1.x - v0.x) * t;
            const double y = v0.y + (v1.y - v0.y) * t;
            const double from_a = graph.elevation_at(a, x, y, detail, channels);
            const double from_b = graph.elevation_at(b, x, y, detail, channels);
            ASSERT_TRUE(std::abs(from_a - from_b) < 1e-6);
            ++checked;
        }
    }
    ASSERT_TRUE(checked > 0);
}

/**
 * @brief A river reads as a river in the height field, not as a dip in the ground.
 *
 * The test the previous attempt at this needed and did not have. Carving cell heights produced
 * a measurably deep valley that was invisible to look at, because it compared the ground against
 * *itself uncarved* -- a comparison a 500 m-wide depression passes just as happily as a channel
 * does.
 *
 * What the eye actually needs is local contrast, so that is what is asserted here: the ground at
 * the centreline against the ground a short way to either side of it, at the same moment, on the
 * same surface. A uniform depression scores zero on this no matter how deep it is.
 */
COOPA_TEST(river_channels_are_visible_in_the_height_field) {
    const MapGenerator& generator = shared_river_world();
    const MapConfig& config = generator.config();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.rivers.empty());

    const Noise terrain(config.noise_terrain);
    const TerrainDetail detail = make_terrain_detail(config, terrain);
    const RiverChannels channels = make_river_channels(graph, config);

    // Half a cell out: far outside the channel itself, which is a river's width across, so this
    // measures the bank against the bed rather than one part of the bed against another.
    const double offset = 0.5;
    double total = 0.0;
    std::size_t sampled = 0;
    for (const MapRiver& river : graph.rivers) {
        const std::size_t spans = river.points.size() - 1;
        for (std::size_t i = 2; i + 2 < river.points.size(); ++i) {
            // The same proportional step `make_river_channels()` files segments by, so the cell
            // asked for is one that actually carries this stretch of the river. Any other cell
            // reports no channel at all, which is correct of it and useless here.
            const MapCorner& corner = graph.corners[static_cast<std::size_t>(
                river.corners[std::min(river.corners.size() - 1,
                                       i * river.corners.size() / spans)])];
            if (corner.touches.empty()) {
                continue;
            }
            const MapCenter& center = graph.centers[static_cast<std::size_t>(corner.touches.front())];
            const double dx = river.points[i + 2].x - river.points[i - 2].x;
            const double dy = river.points[i + 2].y - river.points[i - 2].y;
            const double length = std::hypot(dx, dy);
            if (length < 1e-9) {
                continue;
            }
            const double nx = -dy / length;
            const double ny = dx / length;
            const MapPoint& point = river.points[i];
            const double bed = graph.elevation_at(center, point.x, point.y, detail, channels);
            const double left = graph.elevation_at(center, point.x + nx * offset,
                                                   point.y + ny * offset, detail, channels);
            const double right = graph.elevation_at(center, point.x - nx * offset,
                                                    point.y - ny * offset, detail, channels);
            if (bed <= config.sea_level || left <= config.sea_level || right <= config.sea_level) {
                continue;
            }
            total += (left + right) * 0.5 - bed;
            ++sampled;
        }
    }
    ASSERT_TRUE(sampled > 0);

    // Ten metres is the bar: below about eight the channel is fewer than four grey levels at the
    // default vertical scale, which is where it stops being something a reader can pick out of
    // the relief.
    ASSERT_TRUE(height_to_meters(config, total / static_cast<double>(sampled)) > 10.0);
}

/**
 * @brief A river's water surface stands above the terrain the elevation layer draws.
 *
 * The invariant a consumer meshing the two layers together depends on. Computing the surface
 * here from `corner.elevation` and asserting it sits above `corner.elevation` would be trivially
 * true, and about the wrong surface.
 *
 * So this reads `RiverSurfaces`, the same table the renderer strokes from, and compares against
 * the ground the elevation layer actually draws, cut channel and all, sampled across the whole
 * width of the stroke. This is the invariant most at risk from settling the profile downward to
 * meet the sea, which is why it is checked over the footprint rather than along the centreline.
 */
COOPA_TEST(river_surface_sits_above_the_ground) {
    const MapGenerator& generator = shared_river_world();
    const MapConfig& config = generator.config();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.rivers.empty());

    const Noise terrain(config.noise_terrain);
    const TerrainDetail detail = make_terrain_detail(config, terrain);
    const RiverChannels channels = make_river_channels(graph, config);
    const RiverSurfaces surfaces = make_river_surfaces(graph, config, detail, channels);

    std::size_t checked = 0;
    for (std::size_t r = 0; r < graph.rivers.size(); ++r) {
        const MapRiver& river = graph.rivers[r];
        const std::size_t spans = river.points.size() - 1;
        for (std::size_t i = 0; i < spans; ++i) {
            const std::size_t slot =
                std::min(river.corners.size() - 1, i * river.corners.size() / spans);
            const MapCorner& corner = graph.corners[static_cast<std::size_t>(river.corners[slot])];
            if (corner.touches.empty()) {
                continue;
            }
            const MapCenter& center = graph.centers[static_cast<std::size_t>(corner.touches.front())];
            const double surface = surfaces.at(r, i);

            const MapPoint& from = river.points[i];
            const MapPoint& to = river.points[i + 1];
            const double reach =
                (river_width(config, corner.river)
                 + meters_to_grid(config, config.water_edge_overlap_m) * 2.0) * 0.5;
            const double dx = to.x - from.x;
            const double dy = to.y - from.y;
            const double length = std::hypot(dx, dy);
            const double nx = length > 0.0 ? -dy / length * reach : 0.0;
            const double ny = length > 0.0 ? dx / length * reach : 0.0;

            for (double along = 0.0; along <= 1.0; along += 0.5) {
                for (double across = -1.0; across <= 1.0; across += 1.0) {
                    const double x = from.x + dx * along + nx * across;
                    const double y = from.y + dy * along + ny * across;
                    const double ground = graph.elevation_at(center, x, y, detail, channels);
                    // Half a metre of slack: the surface is piecewise linear, so a triangle
                    // vertex inside the stroke can poke a hair above every point the probe grid
                    // samples. Far below the 2.35 m a single grey level covers, so it can never
                    // reach a pixel.
                    ASSERT_TRUE(ground <= surface + meters_to_height(config, 0.5));
                    ++checked;
                }
            }
        }
    }
    ASSERT_TRUE(checked > 0);
}

/**
 * @brief A river's surface falls, except where it backs up into what it feeds.
 *
 * Water does not flow uphill, and the surface computed per segment in isolation did: the highest
 * ground under the stroke set the height, so a bank beside the course lifted the sheet over it.
 * **48 of 55 rivers rose somewhere downstream**, by up to 21.6 m, and nothing caught it.
 *
 * The one place a rise is allowed is the last stretch into a body standing above the river -- a
 * lake's level is the highest bed in its body and can be a hundred metres over its own shore --
 * because the alternative is ending below the water it feeds, which is a visible notch at the
 * join. That is a drowned inlet, and it is bounded twice over: it may only happen within
 * `river_mouth_blend_m` of the mouth, and it may never carry the surface above the body's own
 * level. Both bounds are asserted, not assumed.
 */
COOPA_TEST(river_surface_only_falls) {
    for (int seed : {67, 31, 101}) {
        MapConfig config = world_config(seed);
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();
        ASSERT_TRUE(!graph.rivers.empty());

        const Noise terrain(config.noise_terrain);
        const TerrainDetail detail = make_terrain_detail(config, terrain);
        const RiverChannels channels = make_river_channels(graph, config, detail);
        const RiverSurfaces surfaces = make_river_surfaces(graph, config, detail, channels);
        const double blend = meters_to_grid(config, config.river_mouth_blend_m);

        std::size_t checked = 0;
        for (std::size_t r = 0; r < graph.rivers.size(); ++r) {
            const MapRiver& river = graph.rivers[r];
            const std::size_t spans = surfaces.heights[r].size();
            if (spans < 2) {
                continue;
            }
            const MapCorner& mouth = graph.corners[static_cast<std::size_t>(river.corners.back())];
            double target = -1.0;
            for (const CenterId center_id : mouth.touches) {
                const MapCenter& center = graph.centers[static_cast<std::size_t>(center_id)];
                if (center.water) {
                    target = center.water_level;
                }
            }

            std::vector<double> to_mouth(spans, 0.0);
            double run = 0.0;
            for (std::size_t i = spans; i-- > 0;) {
                to_mouth[i] = run;
                run += river.points[i].distance_to(river.points[i + 1]);
            }

            for (std::size_t i = 1; i < spans; ++i) {
                ++checked;
                if (surfaces.at(r, i) <= surfaces.at(r, i - 1) + 1e-12) {
                    continue;
                }
                // A rise, so both bounds must hold.
                ASSERT_TRUE(to_mouth[i] <= blend + 1e-9);
                ASSERT_TRUE(target >= 0.0);
                ASSERT_TRUE(surfaces.at(r, i) <= std::max(surfaces.at(r, i - 1), target) + 1e-9);
            }
        }
        ASSERT_TRUE(checked > 0);
    }
}

/**
 * @brief A river ends at or above the water it feeds -- never below it.
 *
 * The join, stated as the invariant a reader actually notices: where a river met a larger body
 * its ribbon was drawn *darker* than the body, because it ended lower. At the ocean that came
 * from the channel carve, which took the deeper of its ordinary depth and the depth needed to
 * reach the waterline -- so a mouth needing 2 m of cut got 22 m and finished a clear 20 m under
 * the sea. The median ocean mouth sat 13.3 m below sea level.
 *
 * Bounded from above as well, or "ends high enough" would be satisfied by ending anywhere at
 * all. A river arrives at the body's level, or rests on its own bed where the ground never gets
 * down to that level, and never more than its freeboard above whichever of the two is higher.
 */
COOPA_TEST(rivers_end_at_or_above_the_water_they_feed) {
    for (int seed : {67, 31, 101}) {
        MapConfig config = world_config(seed);
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();

        const Noise terrain(config.noise_terrain);
        const TerrainDetail detail = make_terrain_detail(config, terrain);
        const RiverChannels channels = make_river_channels(graph, config, detail);
        const RiverSurfaces surfaces = make_river_surfaces(graph, config, detail, channels);
        const double slack = meters_to_height(config, 1.0);

        std::size_t checked = 0;
        for (std::size_t r = 0; r < graph.rivers.size(); ++r) {
            const MapRiver& river = graph.rivers[r];
            if (surfaces.heights[r].empty()) {
                continue;
            }
            const MapCorner& mouth = graph.corners[static_cast<std::size_t>(river.corners.back())];
            double target = -1.0;
            for (const CenterId center_id : mouth.touches) {
                const MapCenter& center = graph.centers[static_cast<std::size_t>(center_id)];
                if (center.water) {
                    target = center.water_level;
                }
            }
            if (target < 0.0) {
                continue;
            }

            const std::size_t last = surfaces.heights[r].size() - 1;
            const double surface = surfaces.at(r, last);
            ASSERT_TRUE(surface >= target - 1e-9);

            const double bed =
                detail::river_ground_under(graph, river, last, config, detail, &channels);
            const double freeboard = detail::river_freeboard(graph, river, last, config);
            ASSERT_TRUE(surface <= std::max(target, bed) + freeboard + slack);
            ++checked;
        }
        ASSERT_TRUE(checked > 0);
    }
}

/** @brief Two rivers meeting at a corner are drawn at the same height. */
COOPA_TEST(rivers_agree_where_they_meet) {
    const MapGenerator& generator = shared_river_world();
    const MapConfig& config = generator.config();
    const MapGraph& graph = generator.graph();

    const Noise terrain(config.noise_terrain);
    const TerrainDetail detail = make_terrain_detail(config, terrain);
    const RiverChannels channels = make_river_channels(graph, config);
    const RiverSurfaces surfaces = make_river_surfaces(graph, config, detail, channels);

    std::map<CornerId, std::vector<double>> claimed;
    for (std::size_t r = 0; r < graph.rivers.size(); ++r) {
        const MapRiver& river = graph.rivers[r];
        const std::size_t spans = surfaces.heights[r].size();
        if (spans == 0) {
            continue;
        }
        for (std::size_t k = 0; k < river.corners.size(); ++k) {
            const std::size_t segment = std::min(
                spans - 1, k * spans / std::max<std::size_t>(1, river.corners.size() - 1));
            claimed[river.corners[k]].push_back(surfaces.at(r, segment));
        }
    }

    std::size_t confluences = 0;
    for (const auto& entry : claimed) {
        if (entry.second.size() < 2) {
            continue;
        }
        ++confluences;
        const auto range = std::minmax_element(entry.second.begin(), entry.second.end());
        // Under a metre, against 30.8 m when each river decided its own height.
        ASSERT_TRUE(height_to_meters(config, *range.second - *range.first) < 1.0);
    }
    ASSERT_TRUE(confluences > 0);
}

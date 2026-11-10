/**
 * @file rivers_test.cpp
 * @brief River routing and valley carving: courses only fall, every river reaches water, the
 *        source/length rules, and valleys cut into (and never below) the terrain.
 *
 * The drawn river *surface* and the channel cut in the sampled height field are
 * river_surface_test. Not tested: how visible a valley is in metres -- that is tuning, and the
 * structural claims (river corners below their banks; the height field changes only through the
 * carve) are asserted here instead.
 */

#include <coopa/testing/test.h>

#include <cmath>

#include <coopa/maps/map_renderer.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("rivers");

COOPA_TEST(rivers_terminate_on_hostile_terrain) {
    // A threshold below the noise field's minimum makes every corner water, so no source can
    // ever satisfy the elevation window. The original retried by decrementing its loop counter
    // and hung here forever.
    MapConfig config = small_config();
    config.threshold_water = -2.0;
    config.river_count = 50;

    MapGenerator generator(config, maps_logger());
    generator.generate();

    for (const MapEdge& edge : generator.graph().edges) {
        ASSERT_EQ(edge.river, 0);
    }
}

/**
 * @brief A watercourse only ever falls, and the edge and corner records agree on where it is.
 *
 * Widening a valley does not know which corner is upstream of which, so where a larger river
 * passes close by, one of its rings can land on an upstream corner and cut it below its own
 * downstream neighbour -- water running uphill in the middle of a river. `PassValleys` clamps
 * along each course afterwards; this is the check that it does.
 */
COOPA_TEST(river_courses_run_downhill) {
    const MapGraph& graph = shared_river_world().graph();
    ASSERT_TRUE(!graph.rivers.empty());

    for (const MapRiver& river : graph.rivers) {
        for (std::size_t i = 1; i < river.corners.size(); ++i) {
            const MapCorner& upstream =
                graph.corners[static_cast<std::size_t>(river.corners[i - 1])];
            const MapCorner& corner = graph.corners[static_cast<std::size_t>(river.corners[i])];
            ASSERT_TRUE(corner.elevation <= upstream.elevation + 1e-12);
        }
    }

    // A river edge is a river at both of its ends: the renderer strokes edges, the surface
    // pass walks corners, and the two must describe the same network.
    std::size_t river_edges = 0;
    for (const MapEdge& edge : graph.edges) {
        if (edge.river <= 0) {
            continue;
        }
        ++river_edges;
        const MapCorner& v0 = graph.corners[static_cast<std::size_t>(edge.v0)];
        const MapCorner& v1 = graph.corners[static_cast<std::size_t>(edge.v1)];
        ASSERT_TRUE(v0.river > 0 && v1.river > 0);
    }
    ASSERT_TRUE(river_edges > 0);
}

/**
 * @brief Every river ends in a lake or the sea, and the terrain guarantees it.
 *
 * Two properties, and the second is the one that makes the first hold rather than merely happen
 * to be true on this seed.
 *
 * A river is a walk down `downslope`, so where it ends is decided entirely by the height field.
 * Relief noise, the rank remap and two smoothing passes each move corners independently of
 * their neighbours, and any of them can leave a corner lower than everything around it. That
 * corner is a pit, and a river that reaches one stops in the middle of a field. Left unfilled it
 * is not a rare accident: on the default world some 80 of 11 438 land corners are pits, and 23
 * of 55 rivers end dry.
 *
 * `fill_depressions()` removes them, so the assertion here is on the terrain and not on the
 * rivers: *every* dry corner must have a strictly lower neighbour, which by induction gives it a
 * descending path to water. That is a much stronger statement than "the 55 rivers this seed
 * happened to place all found the sea", and it is what a caller adding rivers, changing their
 * sources or sampling flow directly can rely on.
 */
COOPA_TEST(every_river_ends_in_a_water_body) {
    const MapGraph& graph = shared_world().graph();
    ASSERT_TRUE(!graph.rivers.empty());

    const auto wet = [](const MapCorner& corner) { return corner.water || corner.coast; };

    // 1. No pit anywhere on dry land.
    std::size_t dry_corners = 0;
    for (const MapCorner& corner : graph.corners) {
        if (wet(corner)) {
            continue;
        }
        ++dry_corners;
        bool has_lower = false;
        for (const CornerId neighbor_id : corner.adjacent) {
            has_lower = has_lower
                || graph.corners[static_cast<std::size_t>(neighbor_id)].elevation
                       < corner.elevation;
        }
        ASSERT_TRUE(has_lower);
        // Which is exactly the condition under which `downslope` leaves.
        ASSERT_TRUE(corner.downslope != corner.index);
    }
    ASSERT_TRUE(dry_corners > 0);

    // 2. Therefore a downhill walk from any dry corner at all -- not just from the corners
    //    rivers were seeded on -- arrives at water.
    for (const MapCorner& start : graph.corners) {
        if (wet(start)) {
            continue;
        }
        CornerId current = start.index;
        std::size_t steps = 0;
        while (!wet(graph.corners[static_cast<std::size_t>(current)])) {
            const MapCorner& corner = graph.corners[static_cast<std::size_t>(current)];
            ASSERT_TRUE(corner.downslope != current);
            current = corner.downslope;
            ++steps;
            ASSERT_TRUE(steps <= graph.corners.size());
        }
    }

    // 3. And the rivers themselves land on it: a mouth in water, and no corner before the mouth
    //    already in water -- a river that ran on past a shoreline would be drawing a channel
    //    across a lake's surface.
    for (const MapRiver& river : graph.rivers) {
        ASSERT_TRUE(wet(graph.corners[static_cast<std::size_t>(river.corners.back())]));
        for (std::size_t i = 0; i + 1 < river.corners.size(); ++i) {
            ASSERT_TRUE(!wet(graph.corners[static_cast<std::size_t>(river.corners[i])]));
        }
    }
}

COOPA_TEST(rivers_are_long_and_smooth) {
    MapConfig config = world_config(31);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    ASSERT_TRUE(!graph.rivers.empty());
    for (const MapRiver& river : graph.rivers) {
        // Short trickles are rejected and redrawn, so every kept watercourse actually crosses
        // some country.
        ASSERT_TRUE(static_cast<int>(river.corners.size()) >= config.river_min_length);

        // Corner-cutting multiplies the point count; a run of N corners that came back with N
        // points was never smoothed.
        ASSERT_TRUE(river.points.size() > river.corners.size());
        ASSERT_TRUE(river.volume > 0);

        for (const MapPoint& point : river.points) {
            ASSERT_TRUE(std::isfinite(point.x) && std::isfinite(point.y));
            ASSERT_TRUE(point.x >= 0.0 && point.x <= static_cast<double>(config.grid_size));
            ASSERT_TRUE(point.y >= 0.0 && point.y <= static_cast<double>(config.grid_size));
        }
        // The course runs downhill, source to mouth.
        const MapCorner& source = graph.corners[static_cast<std::size_t>(river.corners.front())];
        const MapCorner& mouth = graph.corners[static_cast<std::size_t>(river.corners.back())];
        ASSERT_TRUE(source.elevation >= mouth.elevation);
    }

    // Sources are drawn from high ground -- but that is a statement about the terrain the river
    // pass *chose* from, and the valley pass has since cut the ground away beneath them.
    // Checking it against the carved field would be asserting that rivers do not erode their
    // own headwaters. So it is checked on the uncarved run, which is the surface the threshold
    // was applied to.
    MapConfig uncarved = config;
    uncarved.enable_valleys = false;
    MapGenerator before(uncarved, maps_logger());
    before.generate();
    const MapGraph& unworn = before.graph();
    ASSERT_TRUE(!unworn.rivers.empty());
    for (const MapRiver& river : unworn.rivers) {
        const MapCorner& source = unworn.corners[static_cast<std::size_t>(river.corners.front())];
        ASSERT_TRUE(source.elevation >= config.river_source_min_elevation - 1e-9);
    }
}

/**
 * @brief A watercourse lies below the ground either side of it.
 *
 * The point of the whole valley pass. Checked on the corner field, which is where the incision
 * is measured, against the corners one edge away that carry no river -- the bank. Trunk rivers
 * only: a volume-one trickle is still inside the headwater taper and is not meant to have
 * opened a valley yet.
 */
COOPA_TEST(river_corners_sit_below_their_banks) {
    const MapGenerator& generator = shared_river_world();
    const MapConfig& config = generator.config();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.rivers.empty());

    std::size_t checked = 0;
    for (const MapCorner& corner : graph.corners) {
        if (corner.river < 2 || corner.water || corner.coast || corner.border) {
            continue;
        }
        // A mouth is pinned at the waterline and cannot be cut below it, so a corner already at
        // sea level proves nothing either way.
        if (corner.elevation <= config.sea_level + 1e-9) {
            continue;
        }
        double bank = 0.0;
        std::size_t counted = 0;
        for (const CornerId neighbor_id : corner.adjacent) {
            const MapCorner& neighbor = graph.corners[static_cast<std::size_t>(neighbor_id)];
            if (neighbor.river > 0 || neighbor.water) {
                continue;
            }
            bank += neighbor.elevation;
            ++counted;
        }
        if (counted == 0) {
            continue;
        }
        ASSERT_TRUE(corner.elevation < bank / static_cast<double>(counted));
        ++checked;
    }
    ASSERT_TRUE(checked > 0);
}

/**
 * @brief Rivers reach the height field by carving it, and only by carving it.
 *
 * The elevation layer must not paint the river network into itself: that would make the layer a
 * picture of the terrain rather than the terrain itself, and a consumer flooding a mesh to those
 * values would find channels already cut.
 *
 * Rivers reach it through the valley pass, which lowers the ground. So the layer must differ
 * when rivers run -- otherwise the carve never reached the pixels -- and must be
 * *bit-identical* once the incision is set to zero, which is the guarantee that the only thing
 * moving the image is the terrain itself.
 */
COOPA_TEST(rivers_cut_valleys_into_the_height_field) {
    MapConfig with_rivers_config = small_config();
    with_rivers_config.subdivide_noisy_edges = false;

    MapConfig without_rivers_config = with_rivers_config;
    without_rivers_config.enable_rivers = false;

    MapGenerator with_rivers(with_rivers_config, maps_logger());
    MapGenerator without_rivers(without_rivers_config, maps_logger());
    with_rivers.generate();
    without_rivers.generate();
    ASSERT_TRUE(!with_rivers.graph().rivers.empty());

    // The height field notices, because the ground under a river is lower.
    const Image drawn = MapLayers::elevation(with_rivers.graph(), with_rivers_config);
    const Image base = MapLayers::elevation(without_rivers.graph(), without_rivers_config);
    ASSERT_EQ(drawn.pixels.size(), base.pixels.size());
    ASSERT_TRUE(drawn.pixels != base.pixels);

    // The water layer very much does too.
    const Image wet = MapLayers::water(with_rivers.graph(), with_rivers_config);
    const Image dry = MapLayers::water(without_rivers.graph(), without_rivers_config);
    ASSERT_EQ(wet.pixels.size(), dry.pixels.size());
    ASSERT_TRUE(wet.pixels != dry.pixels);

    // With nothing carved -- neither the valley in the mesh nor the channel in the surface --
    // the two are the same image again, so the difference above is the carving and not some
    // other thing the river pass touched.
    MapConfig uncarved_config = with_rivers_config;
    uncarved_config.river_incision_m = 0.0;
    uncarved_config.river_incision_per_volume_m = 0.0;
    uncarved_config.river_channel_depth_m = 0.0;
    uncarved_config.river_channel_depth_per_volume_m = 0.0;
    MapConfig uncarved_without_config = uncarved_config;
    uncarved_without_config.enable_rivers = false;

    MapGenerator uncarved(uncarved_config, maps_logger());
    MapGenerator uncarved_without(uncarved_without_config, maps_logger());
    uncarved.generate();
    uncarved_without.generate();

    const Image flat = MapLayers::elevation(uncarved.graph(), uncarved_config);
    const Image flat_base = MapLayers::elevation(uncarved_without.graph(), uncarved_without_config);
    ASSERT_TRUE(flat.pixels == flat_base.pixels);
}

/**
 * @brief Cutting a valley never digs dry ground below the waterline.
 *
 * A river mouth already sits at sea level, so the clamp in `lower_corners_()` binds on every
 * watercourse on the map rather than in some corner case. Without it, land would come out
 * submerged and every pass that reads the height field to decide what is wet would disagree
 * with the one that decides what is land.
 */
COOPA_TEST(incision_never_breaches_sea_level) {
    for (int seed = 1; seed <= 4; ++seed) {
        MapConfig config = world_config(seed * 53);
        // Far deeper than the default, so the clamp is doing the work rather than the incision
        // happening to be too shallow to reach.
        config.river_incision_m = 240.0;
        config.river_incision_per_volume_m = 48.0;
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();

        for (const MapCorner& corner : graph.corners) {
            if (!corner.ocean) {
                ASSERT_TRUE(corner.elevation >= config.sea_level);
            }
        }
        for (const MapCenter& center : graph.centers) {
            if (!center.water) {
                ASSERT_TRUE(center.elevation >= config.sea_level);
            }
        }
    }
}

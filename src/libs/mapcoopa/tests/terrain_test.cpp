/**
 * @file terrain_test.cpp
 * @brief The height field between the stored samples: barycentric interpolation and its seams,
 *        roughness tapering to the coast, and relief reshaping the land without breaking drainage.
 *
 * River channels cut into this surface are river_surface_test; the stored per-cell values and
 * their ranges are map_graph_test.
 */

#include <coopa/testing/test.h>

#include <cmath>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("terrain");

/**
 * @brief The sampled surface interpolates the control mesh, and joins across cells.
 *
 * Barycentric, not inverse-distance weighting, which reads as a plateau: every corner is roughly
 * equidistant from the middle of a cell, so most of the interior comes out near the mean of the
 * corners. What is asserted is that the surface passes through the values it interpolates and
 * meets itself at a shared edge.
 */
COOPA_TEST(elevation_interpolates_and_joins) {
    MapConfig config = world_config(23);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::size_t checked = 0;
    for (const MapCenter& center : graph.centers) {
        if (center.corners.size() < 3 || center.border) {
            continue;
        }
        // At a site the surface passes through that site's own height: the sites are the
        // interpolation vertices, not the corners. A Voronoi corner is interior to a Delaunay
        // triangle, so its own `elevation` is *not* what the surface reads there.
        ASSERT_TRUE(std::abs(graph.elevation_at(center, center.point.x, center.point.y)
                             - center.elevation) < 1e-9);
        // A sample anywhere in the cell stays in range. Deliberately not asserted against the
        // three sites around the nearest *corner*: a Delaunay triangle with an obtuse angle has
        // its circumcentre outside itself, so the triangle claiming a corner need not be the one
        // that corner belongs to, and bounding by that corner's own sites is not an invariant.
        for (const CornerId corner_id : center.corners) {
            const MapCorner& corner = graph.corners[static_cast<std::size_t>(corner_id)];
            const double sampled = graph.elevation_at(center, corner.point.x, corner.point.y);
            ASSERT_TRUE(sampled >= 0.0 && sampled <= 1.0);
        }
        if (++checked >= 40) {
            break;
        }
    }
    ASSERT_TRUE(checked > 0);

    // Continuity: along an edge two cells share, both sides interpolate between the same two
    // corner heights, so both must agree.
    std::size_t seams = 0;
    for (const MapEdge& edge : graph.edges) {
        if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id
            || edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
            continue;
        }
        const MapCenter& a = graph.centers[static_cast<std::size_t>(edge.d0)];
        const MapCenter& b = graph.centers[static_cast<std::size_t>(edge.d1)];
        if (a.corners.size() < 3 || b.corners.size() < 3) {
            continue;
        }
        const MapPoint& mid = edge.midpoint;
        const double from_a = graph.elevation_at(a, mid.x, mid.y);
        const double from_b = graph.elevation_at(b, mid.x, mid.y);
        ASSERT_TRUE(std::abs(from_a - from_b) < 1e-6);
        if (++seams >= 200) {
            break;
        }
    }
    ASSERT_TRUE(seams > 0);
}

/**
 * @brief Roughness displaces the surface inland and never at the coast.
 *
 * The taper is the whole point: scaled by the local height, so a shoreline stays exactly at sea
 * level and no land is nudged below it however rough the rest gets.
 */
COOPA_TEST(terrain_roughness_tapers_to_the_coast) {
    MapConfig config = world_config(29);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    const Noise terrain(config.noise_terrain);

    MapConfig smooth = config;
    smooth.terrain_roughness = 0.0;
    const TerrainDetail none = make_terrain_detail(smooth, terrain);

    MapConfig rough = config;
    rough.terrain_roughness = 0.6;
    const TerrainDetail detail = make_terrain_detail(rough, terrain);

    std::size_t displaced = 0;
    for (const MapCenter& center : graph.centers) {
        if (center.corners.size() < 3) {
            continue;
        }
        const double x = center.point.x;
        const double y = center.point.y;
        const double base = graph.elevation_at(center, x, y);

        // Zero roughness is the control mesh, exactly.
        ASSERT_TRUE(graph.elevation_at(center, x, y, none) == base);

        const double displaced_height = graph.elevation_at(center, x, y, detail);
        ASSERT_TRUE(displaced_height >= 0.0 && displaced_height <= 1.0);
        if (base == 0.0) {
            // At sea level the taper leaves nothing to displace.
            ASSERT_TRUE(displaced_height == 0.0);
        } else if (displaced_height != base) {
            ++displaced;
        }
    }
    // Inland, it actually does something -- otherwise the taper test above would pass on a knob
    // that did nothing at all.
    ASSERT_TRUE(displaced > 0);
}

/**
 * @brief Relief reshapes where the high ground is without breaking what depends on it.
 *
 * The distance-from-coast field puts every summit on the medial axis of the landmass, so relief
 * blends it toward noise. Three things have to survive that: the coast stays the lowest land,
 * land stays above water so rivers run the right way, and zero still means the untouched field.
 */
COOPA_TEST(terrain_relief_reshapes_without_breaking_drainage) {
    MapConfig flat = world_config(53);
    flat.terrain_relief = 0.0;
    MapGenerator plain(flat, maps_logger());
    plain.generate();

    MapConfig shaped = flat;
    shaped.terrain_relief = 0.7;
    MapGenerator hilly(shaped, maps_logger());
    hilly.generate();

    // It actually changes the terrain -- otherwise the invariants below would hold on a knob
    // that did nothing.
    std::size_t moved = 0;
    ASSERT_EQ(plain.graph().corners.size(), hilly.graph().corners.size());
    for (std::size_t i = 0; i < plain.graph().corners.size(); ++i) {
        if (plain.graph().corners[i].elevation != hilly.graph().corners[i].elevation) {
            ++moved;
        }
    }
    ASSERT_TRUE(moved * 4 > plain.graph().corners.size());

    // The drainage still drains, which is the property the relief blend was built around.
    // `apply_relief_` lifts land clear of water precisely so that water cannot outrank a coastal
    // corner -- but it is not asserted directly here, because `smooth_elevations_` runs
    // afterwards and deliberately relaxes corners across the shore, so the strict separation is
    // gone by the time the graph is observable. What survives is that no watercourse climbs,
    // and that every single one of them ends in water -- see rivers_test
    // `every_river_ends_in_a_water_body` for why that is not a majority.
    const MapGraph& graph = hilly.graph();
    ASSERT_TRUE(!graph.rivers.empty());

    for (const MapRiver& river : graph.rivers) {
        for (std::size_t i = 0; i + 1 < river.corners.size(); ++i) {
            const MapCorner& from = graph.corners[static_cast<std::size_t>(river.corners[i])];
            const MapCorner& to = graph.corners[static_cast<std::size_t>(river.corners[i + 1])];
            ASSERT_TRUE(to.elevation <= from.elevation);
        }
        const MapCorner& mouth = graph.corners[static_cast<std::size_t>(river.corners.back())];
        ASSERT_TRUE(mouth.coast || mouth.water);
    }
}

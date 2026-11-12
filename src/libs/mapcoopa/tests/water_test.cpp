/**
 * @file water_test.cpp
 * @brief Seas and lakes: ocean/lake separation, the waterline, flat water surfaces, and the
 *        water layer drawing those surfaces over (and slightly past) their beds.
 *
 * Rivers are rivers_test / river_surface_test; the shape of the landmass the sea surrounds is
 * shapes_test.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <queue>
#include <string>
#include <vector>

#include <coopa/maps/map_renderer.h>
#include <coopa/maps/passes/water_levels.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("water");

namespace {

/**
 * @brief A square lattice `MapGraph`, 4-connected, for testing the pure graph algorithms in
 *        `water_levels.h` without running a real generator.
 *
 * `rows` is one row-major string per row, one character per cell: `'#'` land, anything else
 * water. A water cell at the array's own edge (`x == 0`, `y == 0`, or the opposite edges) is
 * flagged `border`, the same forced-water band a real generated map always carries. `ocean` is
 * then a flood fill from the border cells over water, exactly as `PassWater` leaves it.
 */
coopa::maps::MapGraph build_lattice(const std::vector<std::string>& rows) {
    using namespace coopa::maps;
    MapGraph graph;
    const int height = static_cast<int>(rows.size());
    const int width = height > 0 ? static_cast<int>(rows[0].size()) : 0;
    const auto index_of = [&](int x, int y) { return y * width + x; };

    graph.centers.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            MapCenter& c = graph.centers[static_cast<std::size_t>(index_of(x, y))];
            c.index = static_cast<CenterId>(index_of(x, y));
            c.point = MapPoint{static_cast<double>(x), static_cast<double>(y)};
            c.water = rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] != '#';
            c.border = c.water && (x == 0 || y == 0 || x == width - 1 || y == height - 1);
            static constexpr int k_steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& step : k_steps) {
                const int nx = x + step[0];
                const int ny = y + step[1];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                c.neighbors.push_back(static_cast<CenterId>(index_of(nx, ny)));
            }
        }
    }

    // ocean = a flood fill from every border water cell, over water -- PassWater's own rule.
    std::vector<bool> reached(graph.centers.size(), false);
    std::queue<CenterId> pending;
    for (MapCenter& c : graph.centers) {
        if (!c.border || !c.water) continue;
        c.ocean = true;
        reached[static_cast<std::size_t>(c.index)] = true;
        pending.push(c.index);
    }
    while (!pending.empty()) {
        const CenterId current = pending.front();
        pending.pop();
        for (const CenterId nb : graph.centers[static_cast<std::size_t>(current)].neighbors) {
            MapCenter& neighbor = graph.centers[static_cast<std::size_t>(nb)];
            if (!neighbor.water || reached[static_cast<std::size_t>(nb)]) continue;
            reached[static_cast<std::size_t>(nb)] = true;
            neighbor.ocean = true;
            pending.push(nb);
        }
    }
    return graph;
}

/**
 * @brief An island with a sealed lagoon behind a one-cell strait, `extra_corridor_rows` of open
 *        water beyond a fixed baseline between the map's border and where the strait begins.
 *
 * Every dimension is sized relative to `coopa::maps::k_strait_half_width` itself (a margin of
 * 3x), rather than a fixed literal, so this stays a valid regression however that constant is
 * retuned: the corridor and the lagoon are both comfortably past `2 * k_strait_half_width` wide,
 * so a wide INTERIOR behind a genuinely narrow (1-cell) neck does not on its own read as open --
 * the regression this geometry pins, see `ocean_enclosure()`'s own doc.
 *
 * The baseline depth is `margin` rows, not 0: the open sea's own core needs room in BOTH
 * directions to establish -- a border only one cell deep has nothing to read wide against,
 * however wide the lattice is laterally, so `extra_corridor_rows == 0` already gives the open sea
 * enough depth to read as open. A large value pushes the strait's mouth many more cells inland
 * while the strait and lagoon themselves stay pixel-for-pixel identical -- which is the whole
 * point: `ocean_enclosure()` must flag the lagoon either way.
 *
 * @return The lattice, plus the (x, y) of a cell at the lagoon's own centre.
 */
std::pair<coopa::maps::MapGraph, std::pair<int, int>> build_island_with_lagoon(int extra_corridor_rows) {
    using coopa::maps::k_strait_half_width;
    const int margin = static_cast<int>(std::ceil(k_strait_half_width)) * 3;
    const int width = margin * 2 + 3;          // two land edges + the strait's own column
    const int strait_col = width / 2;
    const int corridor_rows = margin + extra_corridor_rows;
    // The strait and the room behind it both need enough DEPTH that the lagoon's own centre
    // doesn't outrun the carve's fade (`2 * k_strait_half_width` past the nearest core cell) --
    // a too-shallow strait/room reaches the centre so quickly that even an unfixed bug could
    // look "enclosed enough" by sheer proximity. 5 rows each verified by probe to clear the
    // fade with margin to spare, independent of `extra_corridor_rows`.
    const int strait_rows = 5;
    const int room_rows = 5;

    const std::string border_row(static_cast<std::size_t>(width), '.');
    const std::string corridor_row = "#" + std::string(static_cast<std::size_t>(width - 2), '.') + "#";
    std::string strait_row(static_cast<std::size_t>(width), '#');
    strait_row[static_cast<std::size_t>(strait_col)] = '.';
    std::string room_row(static_cast<std::size_t>(width), '#');
    for (int dx = -margin; dx <= margin; ++dx) room_row[static_cast<std::size_t>(strait_col + dx)] = '.';
    const std::string buffer_row(static_cast<std::size_t>(width), '#');

    std::vector<std::string> rows;
    rows.push_back(border_row);
    for (int i = 0; i < corridor_rows; ++i) rows.push_back(corridor_row);
    for (int i = 0; i < strait_rows; ++i) rows.push_back(strait_row);   // the strait, 1 cell wide
    for (int i = 0; i < room_rows; ++i) rows.push_back(room_row);       // the lagoon itself
    rows.push_back(buffer_row);   // land buffer, seals the lagoon off from the far edge
    const int lagoon_row = 1 + corridor_rows + strait_rows + room_rows / 2;   // the room's middle row
    return {build_lattice(rows), {strait_col, lagoon_row}};
}

} // namespace

COOPA_TEST(water_separates_ocean_from_lakes) {
    std::size_t ocean = 0;
    std::size_t lake = 0;
    for (const MapCenter& center : shared_small_world().graph().centers) {
        if (center.water && center.ocean) ++ocean;
        if (center.water && !center.ocean) ++lake;
    }
    ASSERT_TRUE(ocean > 0);
    // Inland water that never reached the border is what the flood fill exists to
    // distinguish; without it every lake would be classified as sea.
    ASSERT_TRUE(lake > 0);
}

/**
 * @brief The waterline is a real height: the sea below it, everything else above.
 *
 * This is what makes "the ground here is under water" a comparison worth making. With the whole
 * field starting at zero and the sea pinned to the bottom of it, nothing would ever be below sea
 * level and the sea's own surface would render as the same black as dry land.
 */
COOPA_TEST(waterline_separates_sea_from_land) {
    MapConfig config = world_config(61);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::size_t sea_corners = 0;
    std::size_t ground_corners = 0;
    for (const MapCorner& corner : graph.corners) {
        if (corner.ocean) {
            ASSERT_TRUE(corner.elevation < config.sea_level);
            ++sea_corners;
        } else {
            ASSERT_TRUE(corner.elevation >= config.sea_level);
            ++ground_corners;
        }
    }
    ASSERT_TRUE(sea_corners > 0);
    ASSERT_TRUE(ground_corners > 0);

    // Land-relative height is what every threshold describing land is phrased against, so it
    // has to put the shoreline at 0 and the summit at 1.
    ASSERT_TRUE(land_height(config, config.sea_level) == 0.0f);
    ASSERT_TRUE(std::abs(land_height(config, 1.0) - 1.0) < 1e-12);
    ASSERT_TRUE(land_height(config, 0.0) == 0.0);  // submerged clamps to the shore

    // And the vertical scale round-trips, which is what lets a depth be stated in metres at all.
    ASSERT_TRUE(std::abs(height_to_meters(config, meters_to_height(config, 42.0)) - 42.0) < 1e-9);
}

/**
 * @brief Every body of water has one flat surface, which `elevation` does not.
 *
 * Drawing the water layer from `elevation` -- the height of the *bed* -- renders the sea
 * mottled. Only `border` corners are pinned to zero, and the rank remap then spreads every
 * corner across [0, 1], so an ocean cell away from the map edge has a small but nonzero height.
 * Flat water has to be stated.
 */
COOPA_TEST(water_bodies_are_flat) {
    MapConfig config = world_config(17);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::size_t ocean_cells = 0;
    for (const MapCenter& center : graph.centers) {
        if (!center.ocean) {
            continue;
        }
        ++ocean_cells;
        ASSERT_TRUE(center.water_level == config.sea_level);
    }
    ASSERT_TRUE(ocean_cells > 0);

    // And the bug this guards: the bed really does vary, so a flat surface is not something
    // that would have fallen out by accident.
    double lowest_bed = 1.0;
    double highest_bed = 0.0;
    for (const MapCenter& center : graph.centers) {
        if (center.ocean) {
            lowest_bed = std::min(lowest_bed, center.elevation);
            highest_bed = std::max(highest_bed, center.elevation);
        }
    }
    ASSERT_TRUE(highest_bed > lowest_bed);

    // Each lake is one surface across every cell of the body, and that surface is at or above
    // the highest bed in it, so no basin pokes through.
    std::vector<bool> visited(graph.centers.size(), false);
    std::size_t lakes = 0;
    for (const MapCenter& seed : graph.centers) {
        const std::size_t seed_index = static_cast<std::size_t>(seed.index);
        if (!seed.water || seed.ocean || visited[seed_index]) {
            continue;
        }
        ++lakes;
        std::vector<CenterId> pending{seed.index};
        visited[seed_index] = true;
        const double level = seed.water_level;
        while (!pending.empty()) {
            const CenterId current = pending.back();
            pending.pop_back();
            const MapCenter& cell = graph.centers[static_cast<std::size_t>(current)];
            ASSERT_TRUE(cell.water_level == level);
            ASSERT_TRUE(level >= cell.elevation - 1e-12);
            for (const CenterId neighbor_id : cell.neighbors) {
                const std::size_t index = static_cast<std::size_t>(neighbor_id);
                const MapCenter& neighbor = graph.centers[index];
                if (!visited[index] && neighbor.water && !neighbor.ocean) {
                    visited[index] = true;
                    pending.push_back(neighbor_id);
                }
            }
        }
    }
    ASSERT_TRUE(lakes > 0);
}

/**
 * @brief Inside a body of water, the drawn ground never rises through the surface.
 *
 * The companion to the river invariant, and it holds exactly, but only where it can. The
 * boundary is sharper than "away from the shore", and worth stating precisely because a
 * consumer meshing the two layers has to know where the guarantee stops.
 *
 * `MapCenter::elevation` under water is the *bed*, and for a coastal water cell it is not below
 * the water: cell heights are the mean of their corners, and a cell the sea reaches into has
 * corners up on the land. About one water cell in ten carries a "bed" above its own surface for
 * that reason. The ground is drawn by interpolating between cell heights, so those cells pull
 * the surface up through the water inside themselves *and* one ring further in.
 *
 * So the invariant is over cells that, together with every neighbour, have a bed at or below
 * their surface -- there every vertex of every triangle the sampler can reach is under water,
 * and a barycentric blend of values under water is under water. Measured across five maps, that
 * is zero violations out of ~2 400 samples each; anywhere else is the consumer's to clip.
 */
COOPA_TEST(water_bodies_cover_their_interiors) {
    for (int seed : {67, 31, 251}) {
        MapConfig config = world_config(seed);
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();

        const Noise terrain(config.noise_terrain);
        const TerrainDetail detail = make_terrain_detail(config, terrain);
        const RiverChannels channels = make_river_channels(graph, config);

        const auto submerged = [](const MapCenter& cell) {
            return cell.water && cell.elevation <= cell.water_level;
        };

        std::size_t sampled = 0;
        for (const MapCenter& center : graph.centers) {
            if (center.corners.empty() || !submerged(center)) {
                continue;
            }
            bool interior = true;
            for (const CenterId neighbor_id : center.neighbors) {
                if (!submerged(graph.centers[static_cast<std::size_t>(neighbor_id)])) {
                    interior = false;
                }
            }
            if (!interior) {
                continue;
            }
            for (const CornerId corner_id : center.corners) {
                const MapPoint& corner = graph.corners[static_cast<std::size_t>(corner_id)].point;
                // Pulled well in from the corner, so this measures the interior and not the
                // blend across the cell's own boundary.
                const double x = corner.x + (center.point.x - corner.x) * 0.7;
                const double y = corner.y + (center.point.y - corner.y) * 0.7;
                ASSERT_TRUE(graph.elevation_at(center, x, y, detail, channels)
                            <= center.water_level);
                ++sampled;
            }
        }
        ASSERT_TRUE(sampled > 0);
    }
}

/**
 * @brief Inside a body of water the flat surface wins, and it overhangs its edge.
 *
 * Rivers are stroked at ground height plus a depth, so drawing them *after* the bodies -- which
 * is what happened -- gouged a channel across every flat lake a river ran into. And two surfaces
 * that share an edge exactly show a seam wherever their meshes disagree by a rounding error,
 * which along a coastline is everywhere, so the water is extended past its own edge.
 */
COOPA_TEST(water_bodies_win_inside_and_overhang_their_edge) {
    MapConfig config = small_config(71);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    const Image water = MapLayers::water(graph, config);
    const double scale =
        static_cast<double>(config.image_size) / static_cast<double>(config.grid_size);

    std::size_t checked = 0;
    for (const MapCenter& center : graph.centers) {
        if (!center.water) {
            continue;
        }
        const int expected = static_cast<int>(std::clamp(center.water_level, 0.0, 1.0) * 255.0);
        for (const CornerId corner_id : center.corners) {
            // Sampled well inside the cell, so the overlap rim of a neighbour cannot account for
            // a mismatch.
            const MapPoint& corner = graph.corners[static_cast<std::size_t>(corner_id)].point;
            const int x = static_cast<int>((corner.x + (center.point.x - corner.x) * 0.7) * scale);
            const int y = static_cast<int>((corner.y + (center.point.y - corner.y) * 0.7) * scale);
            if (x < 0 || y < 0 || x >= water.width || y >= water.height) {
                continue;
            }
            ASSERT_EQ(static_cast<int>(water.color_at(x, y).r), expected);
            ++checked;
        }
    }
    ASSERT_TRUE(checked > 0);

    // The overhang: some pixels that are dry land carry a water height, because the sheet
    // reaches past its own shoreline.
    MapConfig sharp = config;
    sharp.water_edge_overlap_m = 0.0;
    const Image tight = MapLayers::water(graph, sharp);
    std::size_t wider = 0;
    for (int y = 0; y < water.height; ++y) {
        for (int x = 0; x < water.width; ++x) {
            if (water.color_at(x, y).r > 0.0f && tight.color_at(x, y).r == 0.0f) {
                ++wider;
            }
        }
    }
    ASSERT_TRUE(wider > 0);
}

/**
 * @brief A lagoon behind a strait gets a real basin wherever it sits on the map -- the direct pin
 *        on the bug an earlier fix round left in place: `enclosed_ocean_cells()` used to compare
 *        a cell's water-only path to the map's BORDER against the straight-line distance to it,
 *        which only ever fires for a bay close to the edge. For a lagoon tens of cells inland,
 *        the detour through its own strait is a small fraction of an already-long common
 *        journey to the border, so the ratio test never climbed far enough to flag it -- exactly
 *        `unified_terrain_demo`'s reported "lake" (seed 7, a continent filling most of a map many
 *        times its own width). `ocean_enclosure()` instead measures distance in strait-widths
 *        from the open-sea core, so the lagoon's own width is what decides it, not its distance
 *        from the border.
 *
 * Two lattices, built from the SAME strait-and-lagoon geometry (`build_island_with_lagoon()`),
 * one with the strait's mouth directly on the border and one with 20 rows of open sea between
 * them. Both must flag the lagoon's centre.
 */
COOPA_TEST(a_lagoon_behind_a_strait_is_enclosed_wherever_it_sits) {
    using namespace coopa::maps;
    const int margin = static_cast<int>(std::ceil(k_strait_half_width)) * 3;
    const int width = margin * 2 + 3;
    for (const int corridor_rows : {0, 20}) {
        auto [graph, lagoon] = build_island_with_lagoon(corridor_rows);
        const std::size_t centre = static_cast<std::size_t>(lagoon.second * width + lagoon.first);
        ASSERT_TRUE(graph.centers[centre].water && graph.centers[centre].ocean);

        const std::vector<double> strength = ocean_enclosure(graph);
        ASSERT_TRUE(strength[centre] > 0.5);

        const std::vector<bool> enclosed = enclosed_ocean_cells(graph);
        ASSERT_TRUE(enclosed[centre]);
    }
}

/**
 * @brief A straight coastline, with nothing but a wide, shallow cove in it, flags nothing.
 *
 * The negative case `an_open_coastline_is_never_enclosed` -- a wide, straight stretch of coast
 * with no strait anywhere in it. Nothing in the middle of it should ever read as the far side of
 * one.
 *
 * Sized relative to `k_strait_half_width` (a 4x margin), like `build_island_with_lagoon()`, so
 * this stays valid if that constant is retuned. Only the inner half of the interior is checked,
 * both in x and in y: where this rectangular lattice's straight coast meets its own side walls
 * at a right angle, the corner is locally just as narrow as a real strait would be (close to
 * land on two sides at once), and `ocean_enclosure()` correctly reads a little enclosure right
 * in that corner. A real, organically-shaped coastline has no such right angle to manufacture
 * one; the corner here is a lattice-construction artifact, not a case this test is for.
 */
COOPA_TEST(an_open_coastline_is_never_enclosed) {
    using namespace coopa::maps;
    const int margin = static_cast<int>(std::ceil(k_strait_half_width)) * 4;
    const int width = margin * 2 + 3;
    const int corridor_rows = margin;

    std::vector<std::string> rows = {std::string(static_cast<std::size_t>(width), '.')};  // border
    for (int i = 0; i < corridor_rows; ++i) {
        rows.push_back("#" + std::string(static_cast<std::size_t>(width - 2), '.') + "#");
    }
    rows.push_back(std::string(static_cast<std::size_t>(width), '#'));   // the coast -- straight

    const MapGraph graph = build_lattice(rows);
    const std::vector<double> strength = ocean_enclosure(graph);
    const int lo = margin / 2;
    const int hi = width - 1 - margin / 2;
    for (int y = 1; y <= corridor_rows - margin / 2; ++y) {
        for (int x = lo; x <= hi; ++x) {
            ASSERT_EQ(strength[static_cast<std::size_t>(y * width + x)], 0.0);
        }
    }
}

/**
 * @brief A globe has no border band at all (`MapCenter::border` is false everywhere -- sphere
 *        maps flag large bodies `ocean` by size instead, see `seed_sphere_oceans_()`), so there
 *        is no open-sea core to measure enclosure against. `ocean_enclosure()` must read exactly
 *        0 everywhere rather than, say, treating "unreached from a border that doesn't exist" as
 *        "enclosed" -- which would flag every sea on every planet.
 */
COOPA_TEST(a_globe_with_no_border_water_flags_nothing) {
    using namespace coopa::maps;
    auto [graph, lagoon] = build_island_with_lagoon(0);
    (void)lagoon;
    for (MapCenter& c : graph.centers) c.border = false;   // a sphere map's own invariant

    const std::vector<double> strength = ocean_enclosure(graph);
    for (double s : strength) ASSERT_EQ(s, 0.0);
    const std::vector<bool> enclosed = enclosed_ocean_cells(graph);
    for (bool e : enclosed) ASSERT_TRUE(!e);
}

/**
 * @file caves_test.cpp
 * @brief The cave pass: systems stay under the terrain with roof clearance, open on the
 *        steepest slopes, only descend, grow several non-intersecting storeys, honour the count,
 *        and show on the surface layers only at their mouths.
 *
 * Caves surviving the YAML round trip is map_yaml_test.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>

#include <coopa/maps/map_renderer.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("caves");

/**
 * @brief The headline invariant: no cave anywhere ever breaks the surface.
 *
 * Checked on the *smoothed* passages as well as on the grown stations, and that is the point of
 * the case rather than a thoroughness flourish. Corner-cutting moves points, so a smoothed
 * midpoint over concave ground can rise above a surface both its neighbours sat safely beneath
 * -- and the smoothed path is what the layers draw and export, so it is the geometry the
 * guarantee has to hold on.
 *
 * Run against `--channel`-equivalent settings too, because those cut the drawn surface below the
 * control mesh: a clamp taken against the mesh passes at the defaults and fails here.
 */
COOPA_TEST(caves_stay_under_the_terrain) {
    for (const double channel_depth : {0.0, 18.0, 60.0}) {
        MapConfig config = cave_config(7);
        config.river_channel_depth_m = channel_depth;
        config.river_channel_depth_per_volume_m = channel_depth * 0.22;
        config.terrain_roughness = channel_depth > 0.0 ? 1.0 : 0.0;

        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();
        ASSERT_TRUE(!graph.caves.empty());

        const SurfaceProbe probe(graph, config);
        // The guarantee is not merely "under the ground" -- it is `roof_clearance_m` of rock
        // left above every ceiling, and that is what is asserted. Testing only against the
        // surface itself leaves the whole clearance as slack, which is enough to hide a missing
        // clamp entirely.
        const double clearance = meters_to_height(config, config.caves.roof_clearance_m);
        const double slack = 1e-9;

        std::size_t checked = 0;
        for (const MapCave& cave : graph.caves) {
            for (const CaveNode& node : cave.nodes) {
                ASSERT_TRUE(node.roof + clearance <= probe.at(node.point) + slack);
                ASSERT_TRUE(node.roof >= node.floor);
                ++checked;
            }
            for (const CavePassage& passage : cave.passages) {
                ASSERT_EQ(passage.floors.size(), passage.points.size());
                ASSERT_EQ(passage.roofs.size(), passage.points.size());
                ASSERT_EQ(passage.radii.size(), passage.points.size());
                for (std::size_t i = 0; i < passage.points.size(); ++i) {
                    ASSERT_TRUE(passage.roofs[i] + clearance
                                <= probe.at(passage.points[i]) + slack);
                    ASSERT_TRUE(passage.roofs[i] >= passage.floors[i]);
                    ++checked;
                }
            }
        }
        ASSERT_TRUE(checked > 100);
    }
}

/**
 * @brief Caves open on sharp slopes, and demonstrably the sharpest ones.
 *
 * Two claims, because only the second says the ranking works. That every mouth clears
 * `min_grade` would also be true of a pass that took the first qualifying edge it found; that
 * the mouths are far steeper than a typical qualifying edge is what says they were chosen.
 */
COOPA_TEST(caves_open_on_the_steepest_slopes) {
    MapConfig config = cave_config(19);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.caves.empty());

    double candidate_total = 0.0;
    std::size_t candidates = 0;
    for (const MapEdge& edge : graph.edges) {
        if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id) {
            continue;
        }
        const MapCenter& a = graph.centers[static_cast<std::size_t>(edge.d0)];
        const MapCenter& b = graph.centers[static_cast<std::size_t>(edge.d1)];
        if (a.water || a.ocean || a.border || b.water || b.ocean || b.border) {
            continue;
        }
        const double grade = edge_grade(graph, edge, config);
        if (grade < config.caves.min_grade) {
            continue;
        }
        candidate_total += grade;
        ++candidates;
    }
    ASSERT_TRUE(candidates > graph.caves.size());

    double chosen_total = 0.0;
    for (const MapCave& cave : graph.caves) {
        ASSERT_TRUE(cave.mouth_edge != k_invalid_id);
        const MapEdge& edge = graph.edges[static_cast<std::size_t>(cave.mouth_edge)];
        // The mouth is the edge midpoint -- between two cells, which is where a slope is in a
        // Voronoi map.
        ASSERT_TRUE(std::abs(cave.mouth.x - edge.midpoint.x) < 1e-9);
        ASSERT_TRUE(std::abs(cave.mouth.y - edge.midpoint.y) < 1e-9);
        ASSERT_TRUE(cave.mouth_grade >= config.caves.min_grade);
        chosen_total += cave.mouth_grade;
    }

    const double chosen_mean = chosen_total / static_cast<double>(graph.caves.size());
    const double candidate_mean = candidate_total / static_cast<double>(candidates);
    ASSERT_TRUE(chosen_mean > candidate_mean * 1.5);

    // And they are spread, rather than all opening on one cliff.
    const double spacing = meters_to_grid(config, config.caves.min_spacing_m);
    for (std::size_t i = 0; i < graph.caves.size(); ++i) {
        for (std::size_t j = i + 1; j < graph.caves.size(); ++j) {
            ASSERT_TRUE(graph.caves[i].mouth.distance_to(graph.caves[j].mouth) >= spacing);
        }
    }
}

/** @brief `cave_count` is a cap that is honoured at both ends. */
COOPA_TEST(cave_count_is_respected) {
    MapConfig none = cave_config(31);
    none.caves.cave_count = 0;
    MapGenerator without(none, maps_logger());
    without.generate();
    ASSERT_TRUE(without.graph().caves.empty());

    MapConfig few = cave_config(31);
    few.caves.cave_count = 3;
    MapGenerator some(few, maps_logger());
    some.generate();
    ASSERT_EQ(static_cast<int>(some.graph().caves.size()), 3);

    // More than the terrain can space out: fewer caves, and no crash or hang.
    MapConfig many = cave_config(31);
    many.caves.cave_count = 4000;
    MapGenerator lots(many, maps_logger());
    lots.generate();
    ASSERT_TRUE(static_cast<int>(lots.graph().caves.size()) < many.caves.cave_count);
    ASSERT_TRUE(!lots.graph().caves.empty());
}

/**
 * @brief A cave only ever expands downwards, and never past its own floor.
 *
 * Non-increasing rather than strictly decreasing, because the half of a system at a water table
 * is deliberately level -- "expands downwards" is a statement about what a passage may never
 * do, which is climb. It holds across storeys too: a descent to the next table only ever falls,
 * so the whole system stays monotone from the mouth down however many levels it has.
 */
COOPA_TEST(caves_descend_from_their_mouths) {
    MapConfig config = cave_config(23);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.caves.empty());

    const double depth_limit = meters_to_height(config, config.caves.max_depth_m);
    for (const MapCave& cave : graph.caves) {
        ASSERT_TRUE(!cave.nodes.empty());
        ASSERT_TRUE(cave.nodes.front().parent == -1);
        for (const CaveNode& node : cave.nodes) {
            ASSERT_TRUE(node.floor >= 0.0);
            ASSERT_TRUE(node.floor <= cave.nodes.front().floor);
            ASSERT_TRUE(cave.surface_at_mouth - node.floor <= depth_limit + 1e-9);
            if (node.parent >= 0) {
                const CaveNode& parent = cave.nodes[static_cast<std::size_t>(node.parent)];
                ASSERT_TRUE(node.floor <= parent.floor + 1e-9);
            }
            // The zone a station records has to agree with where it actually is, or the two
            // halves of the model are decoration. Measured against the station's *own* table and
            // not the system's first: a run descending to the second storey is below the first
            // table and still vadose, because it is water falling toward the level it has not
            // reached yet.
            ASSERT_TRUE(node.level >= 0);
            ASSERT_TRUE(static_cast<std::size_t>(node.level) < cave.levels.size());
            const double table = cave.levels[static_cast<std::size_t>(node.level)];
            const bool below = node.floor <= table + 1e-6;
            ASSERT_TRUE((node.zone == CaveZone::Phreatic) == below);
        }
        ASSERT_TRUE(cave.deepest <= cave.nodes.front().floor);
        ASSERT_TRUE(cave.deepest >= 0.0);
    }
}

/**
 * @brief A cave grows more than one storey, and its storeys never intersect.
 *
 * Guards what the level model exists for. Without it a system is one near-planar sheet -- an
 * entrance series down to a single water table and a network spread along it -- and
 * "multi-level cave" is a phrase the geometry does not support. Checked on a population rather
 * than on one lucky cave, because how many storeys a system gets is derived from the relief
 * beneath its mouth.
 *
 * And `level_spacing_m` is load bearing: set it under `chamber_height_m` and a chamber cut at
 * one table reaches through the rock into the level below. The system still reports several
 * storeys and still draws plausibly from above, but there is no floor between them. The gap is
 * checked against the tallest space the configuration can cut, which is a chamber.
 */
COOPA_TEST(caves_grow_several_separate_storeys) {
    MapConfig config = cave_config(11);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.caves.empty());

    std::size_t multi = 0;
    std::size_t deepest = 0;
    for (const MapCave& cave : graph.caves) {
        ASSERT_TRUE(!cave.levels.empty());
        // `phreatic_level` is the shallowest table, which is where the entrance series stops
        // falling -- the meaning it had before there were storeys.
        ASSERT_TRUE(std::abs(cave.phreatic_level - cave.levels.front()) < 1e-12);
        if (cave.levels.size() > 1) {
            ++multi;
        }
        deepest = std::max(deepest, cave.levels.size());
    }
    ASSERT_TRUE(deepest >= 2);
    ASSERT_TRUE(multi * 4 >= graph.caves.size());

    // Stations are spread across those storeys rather than piled on the first, which is what
    // says the lower levels were actually dug and not merely planned. A system that listed four
    // tables and put every station on the top one would pass every claim above.
    std::size_t on_lower = 0;
    std::size_t stations = 0;
    for (const MapCave& cave : graph.caves) {
        for (const CaveNode& node : cave.nodes) {
            ASSERT_TRUE(node.level >= 0);
            ASSERT_TRUE(static_cast<std::size_t>(node.level) < cave.levels.size());
            ++stations;
            if (node.level > 0) {
                ++on_lower;
            }
        }
    }
    ASSERT_TRUE(on_lower * 10 >= stations);

    // Every passage agrees with the stations it is made of, so a consumer can select a storey
    // from either and get the same answer.
    for (const MapCave& cave : graph.caves) {
        for (const CavePassage& passage : cave.passages) {
            ASSERT_TRUE(!passage.nodes.empty());
            const CaveNode& tail = cave.nodes[static_cast<std::size_t>(passage.nodes.back())];
            ASSERT_TRUE(passage.level == tail.level);
        }
    }

    // Shallowest first, so each table stands above the next -- by more than a chamber is tall.
    const double chamber = meters_to_height(config, config.caves.chamber_height_m);
    std::size_t gaps = 0;
    for (const MapCave& cave : graph.caves) {
        for (std::size_t i = 0; i + 1 < cave.levels.size(); ++i) {
            ASSERT_TRUE(cave.levels[i] > cave.levels[i + 1]);
            ASSERT_TRUE(cave.levels[i] - cave.levels[i + 1] > chamber);
            ++gaps;
        }
    }
    ASSERT_TRUE(gaps > 0);
}

/**
 * @brief Only a cave's mouth reaches the surface layers.
 *
 * Two claims, and the second is the one that needs a test. A passage is underground, so no
 * surface layer may show it -- the composite is assembled by hand rather than from a list of
 * participating layers, so the only thing keeping passages out of it is that nobody added the
 * call. A mouth is a hole in a hillside, so the composite *must* show it.
 *
 * Those pull in opposite directions, and byte-identity cannot express the first while the
 * second holds. So the differing pixels are bounded instead: every pixel the caves change on the
 * composite has to lie within a mouth marker's reach of an actual mouth. Draw a passage there by
 * accident and the pixels land hundreds of metres from any mouth and this fails, which is
 * exactly the guarantee a byte-identity check would give.
 */
COOPA_TEST(only_cave_mouths_reach_the_surface_layers) {
    MapConfig with = cave_config(3);
    set_render_size(with, 192);
    MapGenerator generator(with, maps_logger());
    generator.generate();
    ASSERT_TRUE(!generator.graph().caves.empty());

    MapConfig without = with;
    without.enable_caves = false;
    MapGenerator bare(without, maps_logger());
    bare.generate();
    ASSERT_TRUE(bare.graph().caves.empty());

    // Untouched entirely: the cave pass writes only `MapGraph::caves`, and none of these layers
    // draws a marker of any kind.
    for (const MapLayer layer : {MapLayer::Elevation, MapLayer::Water, MapLayer::Biomes,
                                 MapLayer::Roads, MapLayer::Structures, MapLayer::Regions}) {
        const Image lit = MapLayers::render(layer, generator.graph(), with);
        const Image plain = MapLayers::render(layer, bare.graph(), without);
        ASSERT_TRUE(lit.pixels == plain.pixels);
    }

    // The three that must differ. `Caves` differing also says the comparison below is not
    // passing because nothing was rendered either way.
    for (const MapLayer layer : {MapLayer::Composite, MapLayer::Landmarks, MapLayer::Caves}) {
        const Image lit = MapLayers::render(layer, generator.graph(), with);
        const Image plain = MapLayers::render(layer, bare.graph(), without);
        ASSERT_TRUE(lit.pixels != plain.pixels);
    }

    // And every changed pixel of the composite is at a mouth. The marker size mirrors
    // `MapLayers::k_cave_mouth_marker_m`, which is private -- if the two drift the ring outgrows
    // this bound and the test says so, which is the right moment to look at it again.
    static constexpr double k_mouth_marker_m = 8.0;
    const Image lit = MapLayers::render(MapLayer::Composite, generator.graph(), with);
    const Image plain = MapLayers::render(MapLayer::Composite, bare.graph(), without);
    const double scale = static_cast<double>(with.image_size) / static_cast<double>(with.grid_size);
    const int marker = std::max(1, static_cast<int>(meters_to_grid(with, k_mouth_marker_m) * scale));
    // The marker's own radius, plus two pixels: one for the truncation to integer pixels that
    // `draw_ring_()` does to the centre, and one of slack.
    const double reach = static_cast<double>(marker) + 2.0;

    std::size_t changed = 0;
    for (int y = 0; y < with.image_size; ++y) {
        for (int x = 0; x < with.image_size; ++x) {
            if (lit.color_at(x, y) == plain.color_at(x, y)) {
                continue;
            }
            ++changed;
            bool at_a_mouth = false;
            for (const MapCave& cave : generator.graph().caves) {
                const double dx = static_cast<double>(x) - cave.mouth.x * scale;
                const double dy = static_cast<double>(y) - cave.mouth.y * scale;
                if (dx * dx + dy * dy <= reach * reach) {
                    at_a_mouth = true;
                    break;
                }
            }
            ASSERT_TRUE(at_a_mouth);
        }
    }
    ASSERT_TRUE(changed > 0);
}

/** @brief The cave vocabulary's on-disk names survive a round trip. */
COOPA_TEST(cave_zone_and_feature_names_round_trip) {
    for (std::size_t i = 0; i < k_cave_zone_count; ++i) {
        const CaveZone zone = static_cast<CaveZone>(i);
        ASSERT_TRUE(cave_zone_from_name(cave_zone_name(zone)) == zone);
    }
    for (std::size_t i = 0; i < k_cave_feature_count; ++i) {
        const CaveFeature feature = static_cast<CaveFeature>(i);
        ASSERT_TRUE(cave_feature_from_name(cave_feature_name(feature)) == feature);
    }
    ASSERT_TRUE(cave_zone_from_name("nonsense") == CaveZone::Vadose);
    ASSERT_TRUE(cave_feature_from_name("nonsense") == CaveFeature::Passage);
    ASSERT_TRUE(is_open_feature(CaveFeature::Chamber));
    ASSERT_TRUE(!is_open_feature(CaveFeature::Passage));
}

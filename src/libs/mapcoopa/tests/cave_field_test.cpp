/**
 * @file cave_field_test.cpp
 * @brief The field builder: it reports the spans a cave really leaves, it reports them all, it
 *        cuts each system a way in, and asking it is cheap.
 *
 * The pass's own invariants are `caves_test.cpp`. This is about the query -- the thing a consumer
 * turning a cave into geometry actually calls, and the only place the multi-storey model survives
 * or is lost.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <coopa/maps/cave_field.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("cave_field");

namespace {

/** @brief A generated world with caves in it, plus everything the field needs to index them. */
struct Fixture {
    MapConfig config;
    MapGenerator generator;
    const MapGraph* graph = nullptr;
    std::unique_ptr<TerrainSurface> surface;
    RiverChannels channels;
    std::unique_ptr<CaveField> field;

    explicit Fixture(MapConfig c) : config(std::move(c)), generator(config, maps_logger()) {
        generator.generate();
        graph = &generator.graph();
        surface = std::make_unique<TerrainSurface>(*graph, config);
        channels = make_river_channels(*graph, config, surface->detail());
        field = std::make_unique<CaveField>(*graph, config, surface->detail(), channels);
    }

    /** @brief The cell nearest a point, by linear scan -- deliberately not the field's own walk. */
    const MapCenter& cell_at(const MapPoint& p) const {
        const MapCenter* best = &graph->centers.front();
        double best_d = best->point.distance_to(p);
        for (const MapCenter& cell : graph->centers) {
            const double d = cell.point.distance_to(p);
            if (d < best_d) {
                best_d = d;
                best = &cell;
            }
        }
        return *best;
    }
};

} // namespace

/**
 * @brief Every span the field reports is really a cave, at the height the cave is at.
 *
 * Sampled at the smoothed points themselves, which is where a passage is by definition, so a
 * miss here is the index rejecting a point inside its own segment -- the one failure mode that
 * makes a cave vanish in patches rather than entirely.
 */
COOPA_TEST(every_passage_point_reports_the_span_it_sits_in) {
    Fixture f(cave_config(7));
    ASSERT_TRUE(!f.graph->caves.empty());   // the fixture world grows caves
    ASSERT_TRUE(!f.field->empty());   // and the field indexes them

    int sampled = 0, hit = 0;
    for (const MapCave& cave : f.graph->caves) {
        for (const CavePassage& run : cave.passages) {
            for (std::size_t i = 0; i < run.points.size(); ++i) {
                ++sampled;
                const CaveSample s = f.field->at(f.cell_at(run.points[i]), run.points[i]);
                if (!s.inside()) continue;
                ++hit;
                // Some span of the answer has to contain the MIDDLE of the cross-section asked
                // about. Not necessarily one span -- a junction merges two -- so it is
                // containment, not equality; and the middle rather than the two faces, because
                // `CaveConfig::surface_relief` deliberately moves both faces inward by a share
                // of the span's own height. Checking the faces would be checking that the
                // roughness is absent. What this still catches is the failure it exists for:
                // the index answering with a segment the point is not in, which misses the
                // middle by far more than any share of a height.
                const double middle = 0.5 * (run.floors[i] + run.roofs[i]);
                bool covered = false;
                for (std::size_t j = 0; j < s.count; ++j) {
                    if (s.spans[j].floor <= middle + 1e-9 && s.spans[j].roof >= middle - 1e-9) {
                        covered = true;
                    }
                    ASSERT_TRUE(s.spans[j].roof > s.spans[j].floor);   // a span has air in it
                }
                ASSERT_TRUE(covered);   // the span this point's own cross-section sits in is reported
            }
        }
    }
    ASSERT_TRUE(sampled > 100);   // there is a real population of passage points to sample
    // Not every point: a smoothed endpoint can sit a hair outside the disc its own radius
    // describes once the radius is interpolated to zero-length at a run's tail.
    ASSERT_TRUE(hit * 100 / sampled >= 95);   // nearly every passage point is inside the field
}

/** @brief Solid rock reports nothing, so a consumer does not roof the whole world. */
COOPA_TEST(rock_away_from_any_passage_is_solid) {
    Fixture f(cave_config(7));

    // Every cell centre far from every segment of every cave. A cave covers a fraction of a
    // percent of a map, so the overwhelming majority of these must come back empty.
    int probed = 0, empty = 0;
    for (const MapCenter& cell : f.graph->centers) {
        double nearest = 1.0e9;
        for (const MapCave& cave : f.graph->caves) {
            for (const CaveNode& node : cave.nodes) {
                nearest = std::min(nearest, cell.point.distance_to(node.point));
            }
        }
        if (nearest < 2.0) continue;   // grid units: well clear of any passage or chamber
        ++probed;
        if (!f.field->at(cell, cell.point).inside()) ++empty;
    }
    ASSERT_TRUE(probed > 20);   // the map has cells well away from every cave
    ASSERT_TRUE(probed == empty);   // none of them reports a span
}

/**
 * @brief Two storeys crossing in plan are BOTH reported.
 *
 * The whole reason `at()` returns a list. A field that collapsed to one span would pass every
 * other case in this file and silently delete the lower level at exactly the crossings that
 * make a system read as multi-level.
 */
COOPA_TEST(crossing_storeys_both_survive) {
    Fixture f(cave_config(7));

    int multi_level_systems = 0;
    for (const MapCave& cave : f.graph->caves) {
        if (cave.levels.size() > 1) ++multi_level_systems;
    }
    ASSERT_TRUE(multi_level_systems > 0);   // the fixture world has a system with more than one storey

    // A station of an upper storey whose plan position another storey also passes under. Sampled
    // at the station, so the upper span is certain to be there and the question is only whether
    // the lower one survived.
    int crossings = 0;
    for (const MapCave& cave : f.graph->caves) {
        if (cave.levels.size() < 2) continue;
        for (const CaveNode& node : cave.nodes) {
            const CaveSample s = f.field->at(f.cell_at(node.point), node.point);
            if (s.count < 2) continue;
            ++crossings;
            // Sorted deepest first, and genuinely disjoint: mapcoopa holds its storeys further
            // apart than the tallest space cut at either of them, so a merged pair here would be
            // the field's own doing.
            for (std::size_t j = 1; j < s.count; ++j) {
                ASSERT_TRUE(s.spans[j].roof > s.spans[j - 1].roof);   // spans come back shallowest last
                ASSERT_TRUE(s.spans[j].floor > s.spans[j - 1].roof);   // and do not overlap -- overlapping ones are merged, not listed
            }
        }
    }
    ASSERT_TRUE(crossings > 0);   // some point of a multi-storey system really has two spans over it
}

/**
 * @brief Each system gets a mouth cut for it, and the ones on steep ground break out.
 *
 * The pass seals every station including the first, so without this a cave is a sealed pocket.
 * The cut is probed rather than configured, so what is asserted is the relationship -- a steeper
 * mouth daylights in fewer steps -- not a length.
 */
COOPA_TEST(every_system_gets_a_mouth_and_steep_ones_open) {
    Fixture f(cave_config(7));
    const std::vector<CaveField::Mouth>& mouths = f.field->mouths();
    ASSERT_TRUE(mouths.size() == f.graph->caves.size());   // one mouth record per system

    int daylit = 0;
    for (std::size_t i = 0; i < mouths.size(); ++i) {
        const CaveField::Mouth& m = mouths[i];
        const MapCave& cave = f.graph->caves[i];
        ASSERT_TRUE(m.cave == static_cast<std::int32_t>(i));   // a mouth names its own system
        ASSERT_TRUE(m.roof > m.floor);   // and carries the passage section it was cut at
        if (!m.daylit) {
            // A cut that never broke out is DISCARDED, not left half-dug: a trench across a
            // hillside leading nowhere is worse than the sealed cave it was trying to open,
            // because it is visible. Zero stations is how that is reported.
            ASSERT_TRUE(m.stations == 0);
            continue;
        }
        ASSERT_TRUE(m.stations > 0);   // a cut that broke out took at least one step
        ++daylit;
        // The thing the cut exists to produce: a column the consumer must leave open.
        const CaveSample s = f.field->at(f.cell_at(cave.mouth), cave.mouth);
        ASSERT_TRUE(s.inside());   // the mouth itself is inside the cave once the adit is cut
    }
    ASSERT_TRUE(daylit > 0);   // at least one system on this map reaches daylight

    // The cut is SHORT -- which is the regression this guards. The run a cut needs is
    // `(roof_clearance + passage_height) / grade`, and this world's mouth grades put that at a
    // handful of tiles; walking it at the grown passage's own station spacing instead dug a
    // trench nearly two hundred tiles long across the hillside before breaking out.
    //
    // Measured against the CAP rather than against an absolute figure, and the cap is the run
    // `min_grade` promises -- the same expression `CaveField::adit_reach_()` bounds the walk by,
    // restated here because it is private. An absolute figure is a figure about this map's
    // particular mouths: how steep the slopes a given seed's systems happen to open on are moves
    // the mean around, and a bound that moves with the seed is not a bound on the walk. What a
    // walk that is too coarse looks like is a cut that spends most of the cap, every time.
    double total = 0.0;
    int cuts = 0;
    for (const CaveField::Mouth& m : mouths) {
        if (!m.daylit) continue;
        total += m.stations;
        ++cuts;
    }
    ASSERT_TRUE(cuts > 0);
    const CaveConfig& cc = f.config.caves;
    const double cap = meters_to_grid(f.config, (cc.roof_clearance_m + cc.passage_height_m) /
                                                    cc.min_grade) /
                       meters_to_grid(f.config, cc.passage_width_m * 0.25);
    maps_logger().info("adits: " + std::to_string(cuts) + " daylit of " +
                       std::to_string(mouths.size()) + ", mean cut " +
                       std::to_string(total / cuts) + " steps of a " +
                       std::to_string(static_cast<int>(cap)) + "-step cap");
    ASSERT_TRUE(total / cuts <= cap / 3.0);   // a cut stops where the hill thins, not at the cap
}

/**
 * @brief A phreatic passage and a sump hold water; an adit never does.
 *
 * Phreatic passage is cut AT its storey's table and a sump ends against it, so flooded passage
 * is the common half of the model rather than an edge case. The table is the SYSTEM's, well
 * above sea level under high ground.
 */
COOPA_TEST(water_stands_where_the_model_says_it_does) {
    Fixture f(cave_config(7));

    int flooded = 0, dry_adit = 0;
    for (const MapCave& cave : f.graph->caves) {
        for (const CaveNode& node : cave.nodes) {
            const CaveSample s = f.field->at(f.cell_at(node.point), node.point);
            for (std::size_t j = 0; j < s.count; ++j) {
                if (!s.spans[j].flooded) continue;
                ++flooded;
                ASSERT_TRUE(s.spans[j].water > s.spans[j].floor);   // flooded water stands above its own floor
                ASSERT_TRUE(s.spans[j].water <= s.spans[j].roof + 1e-9);   // and never above the ceiling over it
            }
        }
        // The mouth, where the adit is: dry by construction, so there is no air/water interface
        // on the one plane where the cave meets the sky.
        const CaveSample m = f.field->at(f.cell_at(cave.mouth), cave.mouth);
        if (m.inside() && m.open) {
            ++dry_adit;
            ASSERT_TRUE(!m.outermost().flooded);   // a daylit mouth is dry
        }
    }
    ASSERT_TRUE(flooded > 0);   // some passage on this map is under its own water table
    ASSERT_TRUE(dry_adit > 0);   // and some mouth is open and dry
}

/** @brief A map with the pass off indexes nothing and answers nothing, for one null compare. */
COOPA_TEST(a_map_without_caves_costs_nothing) {
    MapConfig config = cave_config(7);
    config.enable_caves = false;
    Fixture f(std::move(config));

    ASSERT_TRUE(f.graph->caves.empty());   // the pass did not run
    ASSERT_TRUE(f.field->empty());   // so the field is empty
    ASSERT_TRUE(f.field->mouths().empty());   // and there are no mouths
    for (const MapCenter& cell : f.graph->centers) {
        ASSERT_TRUE(!f.field->at(cell, cell.point).inside());   // and every query is solid rock
    }
}

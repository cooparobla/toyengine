/**
 * @file volcanoes_test.cpp
 * @brief The vent field and the pass that records it: the shape of a cone, the shape of a
 *        caldera, and that the two views of a vent cannot disagree.
 *
 * The assertions here are invariants and A/B deltas rather than tuned heights: a cone stands
 * above the ground it grew from, a caldera sinks below its own rim, the field only ever raises,
 * and the recorded list agrees with the surface. A threshold on "a vent is 63 m tall" would
 * fail the first time the profile is retuned and would be telling the truth about nothing.
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include <coopa/maps/map_data.h>
#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_presets.h>
#include <coopa/maps/biome.h>
#include <coopa/maps/water_field.h>
#include <coopa/testing/test.h>

#include "support/map_fixtures.h"

using namespace coopa::maps;
using namespace mapcoopa_test;

COOPA_TEST_SUITE("volcanoes");

namespace {

/**
 * @brief How tall a vent the cases ask for.
 *
 * Enough relief to measure through the landform noise, and deliberately short of the amplitude
 * that would push a summit into the top of the normalised height range: a clipped reading is
 * flat, and a flat reading passes an "is it taller than its surroundings" check for the wrong
 * reason.
 */
constexpr double k_amplitude = 0.3;

/** @brief A flat map with vents, and enough land for several of them. */
MapConfig flat_config(int seed = 251) {
    MapConfig config = derived_config([&] {
        MapConfig c = world_config(seed);
        c.natural.volcanoes = k_amplitude;
        c.terrain_model = TerrainModel::Natural;
        return c;
    }());
    return config;
}

/**
 * @brief A map whose base terrain is nearly flat, carrying one kind of volcano.
 *
 * The shape cases measure a cone against the ground around it, and ordinary landform noise at
 * the default `terrain_relief` is several times anything a vent adds -- so on a normal world
 * the measurement reads the mountains, not the volcano. Flattening the base is what isolates
 * the feature under test; the pipeline it travels through is otherwise the real one.
 */
MapConfig shape_config(VolcanoKind kind, int seed = 251) {
    MapConfig config = derived_config([&] {
        MapConfig c = world_config(seed);
        c.terrain_model = TerrainModel::Natural;
        c.terrain_relief = 0.05;
        c.terrain_ridges = 0.0;
        c.terrain_roughness = 0.0;
        c.erosion = 0.0;
        c.natural.hills = 0.0;
        c.natural.mountains = 0.0;
        c.natural.plateaus = 0.0;
        c.natural.volcanoes = 0.5;
        c.natural.volcano_size = 1.5;
        c.natural.volcano_kind = kind;
        return c;
    }());
    return config;
}

/** @brief The same, wrapped onto a globe -- the topology the planet project bakes. */
MapConfig globe_config(int seed = 251) {
    MapConfig config = derived_config([&] {
        MapConfig c = world_config(seed);
        c.topology = MapTopology::Sphere;
        c.grid_size = 40;
        c.natural.volcanoes = k_amplitude;
        c.terrain_model = TerrainModel::Natural;
        return c;
    }());
    return config;
}

/**
 * @brief Samples the natural surface at a point given whole, on either topology.
 *
 * `SurfaceProbe` drops `z`, which is fine for a plane and wrong for a globe. Nearest cell by
 * brute force for the same reason the fixture does it: a test that reuses the graph walk it is
 * checking can agree with a bug in it.
 */
class Probe {
public:
    Probe(const MapGraph& graph, const MapConfig& config)
        : graph_(graph), surface_(graph, config), detail_(surface_.detail()),
          channels_(make_river_channels(graph, config, detail_)) {}

    double at(const MapPoint& p) const {
        return graph_.elevation_at(nearest(p), p, detail_, channels_);
    }

    const MapCenter& nearest(const MapPoint& p) const {
        std::size_t best = 0;
        double closest = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < graph_.centers.size(); ++i) {
            const double distance = graph_.centers[i].point.distance_to(p);
            if (distance < closest) {
                closest = distance;
                best = i;
            }
        }
        return graph_.centers[best];
    }

private:
    const MapGraph& graph_;
    TerrainSurface surface_;
    TerrainDetail detail_;
    RiverChannels channels_;
};

/**
 * @brief A ring of points around a vent at `fraction` of its radius.
 *
 * Stepped by bearing through the graph's own `step_from()`, so the ring lies on the surface it
 * is measuring on either topology -- on a globe a ring built by adding to x and y would leave
 * the sphere and read the wrong ground.
 */
std::vector<MapPoint> ring(const MapGraph& graph, const MapVolcano& vent, double fraction,
                           int count = 16) {
    std::vector<MapPoint> out;
    const double distance = vent.radius * fraction;
    for (int i = 0; i < count; ++i) {
        const double bearing = 2.0 * k_pi * static_cast<double>(i) / static_cast<double>(count);
        out.push_back(graph.step_from(vent.point, bearing, distance));
    }
    return out;
}

/** @brief The median of a copy of `values`; empty reads as zero. */
double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle),
                     values.end());
    return values[middle];
}

/** @brief The median surface height over a ring around a vent. */
double ring_height(const MapGraph& graph, const Probe& probe, const MapVolcano& vent,
                   double fraction) {
    std::vector<double> heights;
    for (const MapPoint& p : ring(graph, vent, fraction)) {
        heights.push_back(probe.at(p));
    }
    return median(std::move(heights));
}

/** @brief The tallest recorded vent, which is the one with the most relief to measure. */
const MapVolcano& tallest(const MapGraph& graph) {
    const MapVolcano* best = &graph.volcanoes.front();
    for (const MapVolcano& vent : graph.volcanoes) {
        if (vent.height > best->height) best = &vent;
    }
    return *best;
}

} // namespace

COOPA_TEST(a_flat_map_places_vents_and_records_each_one) {
    MapGenerator generator(flat_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    ASSERT_TRUE(!graph.volcanoes.empty());
    for (const MapVolcano& vent : graph.volcanoes) {
        ASSERT_TRUE(vent.center != k_invalid_id);
        ASSERT_TRUE(static_cast<std::size_t>(vent.center) < graph.centers.size());
        ASSERT_TRUE(vent.radius > 0.0);
        ASSERT_TRUE(vent.height > 0.0);
        ASSERT_TRUE(vent.caldera >= 0.0 && vent.caldera <= 1.0);
    }
}

COOPA_TEST(zero_amplitude_records_nothing_and_cones_never_clip_the_ceiling) {
    // One graph, two surfaces. The vent field is sampled rather than stored, so the amplitude
    // cannot change the graph -- which is what makes this a clean A/B: the only difference
    // between the two readings is the term under test.
    const MapConfig on = flat_config();
    MapConfig off = on;
    off.natural.volcanoes = 0.0;

    MapGenerator generator(on, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.volcanoes.empty());

    MapGenerator bare(off, maps_logger());
    bare.generate();
    ASSERT_TRUE(bare.graph().volcanoes.empty());

    const Probe raised(graph, on);
    const Probe flat(graph, off);

    int lifted = 0;
    for (int i = 0; i <= 60; ++i) {
        for (int j = 0; j <= 60; ++j) {
            const MapPoint p{static_cast<double>(i) * on.grid_size / 60.0,
                             static_cast<double>(j) * on.grid_size / 60.0};
            const double with = raised.at(p);
            const double without = flat.at(p);
            // Two invariants. A vent may only ADD ground -- a field that could cut would
            // undercut a shoreline -- and it may not push the surface through the ceiling,
            // where it would be clamped into a flat plateau. The second is why the band is
            // squeezed around a vent before the cone is added (`natural_elevation_()`).
            ASSERT_TRUE(with >= without - 1e-9);
            ASSERT_TRUE(with <= 1.0 + 1e-9);
            if (with > without + 1e-6) ++lifted;
        }
    }
    maps_logger().info("volcano field raised " + std::to_string(lifted) + " of 3721 samples");
    ASSERT_TRUE(lifted > 0);
}

COOPA_TEST(a_vent_stands_above_the_ground_around_it) {
    MapGenerator generator(flat_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.volcanoes.empty());

    const Probe probe(graph, generator.config());
    const MapVolcano& vent = tallest(graph);
    // Outside the cone's base, so this is the ground it grew out of rather than its own flank.
    const double outside = ring_height(graph, probe, vent, 1.4);
    const double summit = probe.at(vent.point);
    maps_logger().info("tallest vent: summit " + std::to_string(summit) + " against "
                       + std::to_string(outside) + " outside its base, recorded height "
                       + std::to_string(vent.height));
    ASSERT_TRUE(summit > outside);
}

COOPA_TEST(a_collapsed_summit_sinks_below_its_own_rim) {
    // The near-flat base matters here: a caldera's rim is a few metres of ring, and ordinary
    // landform noise at the default relief is several times that, so on a normal world this
    // measures the mountains rather than the feature.
    MapConfig config = shape_config(VolcanoKind::Caldera);
    config.natural.volcano_caldera = 1.0;   // every summit fully collapsed
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.volcanoes.empty());

    const Probe probe(graph, config);
    const MapVolcano& vent = tallest(graph);
    const double crater = volcano_shape(VolcanoKind::Caldera).crater;
    const double floor = probe.at(vent.point);
    const double rim = ring_height(graph, probe, vent, crater);
    const double outside = ring_height(graph, probe, vent, 1.4);
    maps_logger().info("caldera: floor " + std::to_string(floor) + ", rim "
                       + std::to_string(rim) + ", ground outside " + std::to_string(outside));
    // A crater, not a dome: the rim stands over the floor inside it.
    ASSERT_TRUE(rim > floor);
    // And the rim is a real feature, not just the flank of a cone that happens to be there.
    ASSERT_TRUE(rim > outside);
}

COOPA_TEST(recorded_vents_are_not_crowded_on_top_of_one_another) {
    MapGenerator generator(flat_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(graph.volcanoes.size() > 1);

    for (std::size_t i = 0; i < graph.volcanoes.size(); ++i) {
        for (std::size_t j = i + 1; j < graph.volcanoes.size(); ++j) {
            const MapVolcano& a = graph.volcanoes[i];
            const MapVolcano& b = graph.volcanoes[j];
            ASSERT_TRUE(a.point.distance_to(b.point) > 0.0);
        }
    }
}

COOPA_TEST(the_vent_list_is_deterministic) {
    MapGenerator first(flat_config(), maps_logger());
    first.generate();
    MapGenerator second(flat_config(), maps_logger());
    second.generate();

    const MapGraph& a = first.graph();
    const MapGraph& b = second.graph();
    ASSERT_EQ(a.volcanoes.size(), b.volcanoes.size());
    for (std::size_t i = 0; i < a.volcanoes.size(); ++i) {
        ASSERT_EQ(a.volcanoes[i].center, b.volcanoes[i].center);
        ASSERT_TRUE(std::abs(a.volcanoes[i].point.x - b.volcanoes[i].point.x) < 1e-12);
        ASSERT_TRUE(std::abs(a.volcanoes[i].point.y - b.volcanoes[i].point.y) < 1e-12);
        ASSERT_TRUE(std::abs(a.volcanoes[i].height - b.volcanoes[i].height) < 1e-12);
    }
}

COOPA_TEST(a_globe_carries_vents_too_and_they_stand_up_on_it) {
    MapGenerator generator(globe_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    ASSERT_TRUE(graph.is_sphere());
    ASSERT_TRUE(!graph.volcanoes.empty());

    const Probe probe(graph, generator.config());
    const MapVolcano& vent = tallest(graph);
    // The vent is on the shell, not floating off it -- the pass projects it back out, and a
    // record that skipped that step would sample the wrong ground entirely.
    const double length = std::sqrt(vent.point.x * vent.point.x + vent.point.y * vent.point.y
                                    + vent.point.z * vent.point.z);
    ASSERT_TRUE(std::abs(length - graph.sphere_radius) < 1e-6);

    const double summit = probe.at(vent.point);
    const double outside = ring_height(graph, probe, vent, 1.4);
    maps_logger().info("globe vent: summit " + std::to_string(summit) + " against "
                       + std::to_string(outside) + " outside, " + std::to_string(graph.volcanoes.size())
                       + " vents on the globe");
    ASSERT_TRUE(summit > outside);
    // Relative to what the record claims, so a vent whose cone was lost to the shell geometry
    // fails here instead of passing on a millimetre. Generous slack: the ground under a cone is
    // not level, so the median ring is not the cone's own base.
    ASSERT_TRUE(summit - outside > 0.3 * vent.height);
}

COOPA_TEST(submarine_vents_are_built_only_when_they_are_asked_for) {
    MapConfig wanted = globe_config();
    wanted.natural.volcano_submarine = 1.0;
    MapGenerator with(wanted, maps_logger());
    with.generate();

    MapConfig refused = wanted;
    refused.natural.volcano_submarine = 0.0;
    MapGenerator without(refused, maps_logger());
    without.generate();

    const auto submerged = [](const MapGraph& graph) {
        std::size_t count = 0;
        for (const MapVolcano& vent : graph.volcanoes) {
            if (vent.submarine) ++count;
        }
        return count;
    };
    const std::size_t wet = submerged(with.graph());
    const std::size_t dry = submerged(without.graph());
    maps_logger().info("submarine vents: " + std::to_string(wet) + " with the field on, "
                       + std::to_string(dry) + " with it off");
    // The globe has sea for a seamount to stand in -- otherwise this measures nothing.
    ASSERT_TRUE(wet > 0);
    // Not zero: the gate fades over the first sliver of depth rather than switching at the
    // waterline, so a vent standing a hair under it still builds a little and is still filed
    // as submarine. What the key promises is that volcanism stops going to sea, not that the
    // waterline is a cliff.
    ASSERT_TRUE(dry * 4 < wet);
}


// --------------------------------------------------------------------------- the six kinds
//
// Every case below is DIFFERENTIAL: two kinds on the same seed, over the same near-flat base,
// compared against each other. A threshold on "a shield is 40 m across" would be a statement
// about today's tuning and would fail the first time the shape table moves, while saying
// nothing about whether the six are actually distinguishable -- which is the claim. The
// measured numbers are logged before every assertion, so a red run says what the shapes were.

namespace {

/** @brief One kind's profile, sampled outward from its tallest vent, as rises above the plain. */
struct Section {
    double summit = 0.0;   /**< @brief Rise at the vent itself. */
    double peak = 0.0;     /**< @brief The greatest rise anywhere along the section. */
    double peak_at = 0.0;  /**< @brief Where that greatest rise is, as a fraction of the radius. */
    double width = 0.0;    /**< @brief The edifice's radius, in grid units. */
};

/** @brief Measures a vent as rises above the ground outside its base. */
Section section_of(const MapGraph& graph, const Probe& probe, const MapVolcano& vent) {
    Section s;
    s.width = vent.radius;
    const double outside = ring_height(graph, probe, vent, 1.5);
    s.summit = probe.at(vent.point) - outside;
    s.peak = s.summit;
    for (int i = 1; i <= 24; ++i) {
        const double f = i / 24.0 * 1.2;
        const double h = ring_height(graph, probe, vent, f) - outside;
        if (h > s.peak) { s.peak = h; s.peak_at = f; }
    }
    return s;
}

/** @brief Generates one kind over the flat base and measures its tallest vent. */
Section measured(MapGenerator& gen, const char* what) {
    gen.generate();
    const MapGraph& graph = gen.graph();
    ASSERT_TRUE(!graph.volcanoes.empty());
    const Probe probe(graph, gen.config());
    const Section s = section_of(graph, probe, tallest(graph));
    maps_logger().info(std::string(what) + ": summit " + std::to_string(s.summit) + ", peak "
                       + std::to_string(s.peak) + " at " + std::to_string(s.peak_at)
                       + " of radius " + std::to_string(s.width));
    return s;
}

} // namespace

COOPA_TEST(a_shield_is_broader_and_lower_than_a_composite_cone) {
    MapGenerator shield_gen(shape_config(VolcanoKind::Shield), maps_logger());
    MapGenerator strato_gen(shape_config(VolcanoKind::Stratovolcano), maps_logger());
    const Section shield = measured(shield_gen, "shield");
    const Section strato = measured(strato_gen, "stratovolcano");

    // The two differ in BOTH axes, which is what makes them tell apart at a glance rather than
    // being one cone at two amplitudes.
    ASSERT_TRUE(shield.width > strato.width);
    ASSERT_TRUE(shield.summit < strato.summit);
    // Slope is what a reader actually sees, and it follows from the two above.
    ASSERT_TRUE(shield.summit / shield.width < strato.summit / strato.width);
}

COOPA_TEST(a_caldera_peaks_at_a_ring_and_a_composite_cone_at_its_middle) {
    MapGenerator caldera_gen(shape_config(VolcanoKind::Caldera), maps_logger());
    MapGenerator strato_gen(shape_config(VolcanoKind::Stratovolcano), maps_logger());
    const Section caldera = measured(caldera_gen, "caldera");
    const Section strato = measured(strato_gen, "stratovolcano");

    // The defining difference: a caldera's highest ground is a WALL out at its crater edge; a
    // composite cone's is its summit.
    ASSERT_TRUE(caldera.peak > caldera.summit + 1e-9);
    ASSERT_TRUE(caldera.peak_at > strato.peak_at);
}

COOPA_TEST(a_lava_dome_has_no_crater_at_all) {
    MapGenerator gen(shape_config(VolcanoKind::LavaDome), maps_logger());
    const Section dome = measured(gen, "lava dome");
    // Nothing is sunk into it, so the summit IS the maximum -- the one kind with no hole.
    ASSERT_TRUE(dome.summit >= dome.peak - 1e-9);
    ASSERT_TRUE(dome.summit > 0.0);
}

COOPA_TEST(a_fissure_is_longer_than_it_is_wide) {
    MapGenerator gen(shape_config(VolcanoKind::Fissure), maps_logger());
    gen.generate();
    const MapGraph& graph = gen.graph();
    ASSERT_TRUE(!graph.volcanoes.empty());

    const Probe probe(graph, gen.config());
    const MapVolcano& vent = tallest(graph);
    const double outside = ring_height(graph, probe, vent, 2.5);

    // A rift runs along a LINE, so the ground it raises reaches further one way than another.
    // The bearing is not recorded, so sweep a half turn (a line is symmetric) and compare the
    // best direction against the worst -- a rotationally symmetric cone cannot separate them.
    double most = -1e9, least = 1e9;
    for (int i = 0; i < 24; ++i) {
        const double bearing = k_pi * static_cast<double>(i) / 24.0;
        const MapPoint a = graph.step_from(vent.point, bearing, vent.radius * 1.6);
        const MapPoint b = graph.step_from(vent.point, bearing + k_pi, vent.radius * 1.6);
        const double h = 0.5 * (probe.at(a) + probe.at(b)) - outside;
        most = std::max(most, h);
        least = std::min(least, h);
    }
    maps_logger().info("fissure: along " + std::to_string(most) + ", across "
                       + std::to_string(least) + ", radius " + std::to_string(vent.radius));
    ASSERT_TRUE(most > least + 1e-6);
}

COOPA_TEST(asking_for_one_kind_gives_that_kind) {
    for (std::size_t i = 0; i < k_volcano_mixed_count; ++i) {
        const VolcanoKind kind = volcano_mixed_kind(i);
        MapGenerator generator(shape_config(kind), maps_logger());
        generator.generate();
        maps_logger().info(std::string(volcano_kind_name(kind)) + ": "
                           + std::to_string(generator.graph().volcanoes.size()) + " vents");
        ASSERT_TRUE(!generator.graph().volcanoes.empty());
        for (const MapVolcano& vent : generator.graph().volcanoes) {
            // Or a themed world would be a lottery.
            ASSERT_TRUE(vent.kind == kind);
        }
    }
}

COOPA_TEST(mixed_draws_more_than_one_kind) {
    MapGenerator generator(shape_config(VolcanoKind::Mixed), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(graph.volcanoes.size() > 3);
    std::set<VolcanoKind> seen;
    for (const MapVolcano& vent : graph.volcanoes) seen.insert(vent.kind);
    maps_logger().info("mixed drew " + std::to_string(seen.size()) + " kinds across "
                       + std::to_string(graph.volcanoes.size()) + " vents");
    ASSERT_TRUE(seen.size() > 1);
}


// --- Alive, dead, and what stands in a live crater -------------------------------------------

namespace {

/** @brief `shape_config`, with every vent alive (or none of them). */
MapConfig live_config(VolcanoKind kind, double live, int seed = 251) {
    MapConfig config = shape_config(kind, seed);
    config.natural.volcano_active = live;
    return config;
}

/** @brief The vents of a map carrying one kind, at a given share of them alive. */
struct Population {
    explicit Population(VolcanoKind kind, double live, int seed = 251)
        : generator(live_config(kind, live, seed), maps_logger()) {
        generator.generate();
    }
    const MapGraph& graph() const { return generator.graph(); }
    MapGenerator generator;
};

/** @brief How many of a map's vents pool lava. */
std::size_t molten_count(const MapGraph& graph) {
    std::size_t n = 0;
    for (const MapVolcano& vent : graph.volcanoes) {
        if (vent.lava_rise > 0.0) ++n;
    }
    return n;
}

} // namespace

COOPA_TEST(a_dead_volcano_is_the_same_landform_as_a_live_one) {
    // Activity must cost the GROUND nothing: the same seed with every vent dead has to record
    // the same cones in the same places at the same heights, or "alive" has quietly become a
    // second shape parameter and a world's terrain changes when you turn the lava off.
    const Population alive(VolcanoKind::Caldera, 1.0);
    const Population dead(VolcanoKind::Caldera, 0.0);
    ASSERT_TRUE(!alive.graph().volcanoes.empty());
    ASSERT_EQ(alive.graph().volcanoes.size(), dead.graph().volcanoes.size());
    for (std::size_t i = 0; i < alive.graph().volcanoes.size(); ++i) {
        const MapVolcano& a = alive.graph().volcanoes[i];
        const MapVolcano& d = dead.graph().volcanoes[i];
        ASSERT_TRUE(std::abs(a.height - d.height) < 1e-12);
        ASSERT_TRUE(std::abs(a.radius - d.radius) < 1e-12);
        ASSERT_TRUE(a.point.distance_to(d.point) < 1e-12);
        ASSERT_TRUE(a.kind == d.kind);
    }
    maps_logger().info("activity costs the ground nothing across "
                       + std::to_string(alive.graph().volcanoes.size()) + " calderas");
}

COOPA_TEST(only_a_living_crater_holds_lava) {
    // A LAKE NEEDS A BASIN AS WELL AS A PULSE. Being alive is necessary and not sufficient: the
    // summit also has to have collapsed far enough that its rim stands over its floor, and how
    // far each one collapsed is its own hashed draw around `volcano_caldera`. So the assertion
    // is one-directional -- every pool belongs to a live vent, and a dead map has none at all.
    const Population alive(VolcanoKind::Caldera, 1.0);
    const Population dead(VolcanoKind::Caldera, 0.0);
    maps_logger().info("calderas: " + std::to_string(molten_count(alive.graph())) + " of "
                       + std::to_string(alive.graph().volcanoes.size())
                       + " hold lava when every vent is alive");
    ASSERT_TRUE(molten_count(alive.graph()) > 0);
    ASSERT_EQ(molten_count(dead.graph()), static_cast<std::size_t>(0));
    for (const MapVolcano& vent : alive.graph().volcanoes) {
        ASSERT_TRUE(vent.alive());
        if (vent.lava_rise <= 0.0) continue;
        // The lake has to stand inside the hole it fills, never over the rim.
        ASSERT_TRUE(vent.lava_rise < vent.height);
        ASSERT_TRUE(vent.lava_radius > 0.0 && vent.lava_radius <= vent.radius);
    }
    for (const MapVolcano& vent : dead.graph().volcanoes) {
        ASSERT_TRUE(!vent.alive());
    }
}

COOPA_TEST(only_some_of_a_default_worlds_vents_are_alive) {
    // The default is a mostly extinct landscape, which is what a real one is. The point of the
    // assertion is that the hash actually splits the population rather than answering one way.
    const Population mixed(VolcanoKind::Mixed, 0.5, 977);
    ASSERT_TRUE(mixed.graph().volcanoes.size() > 4);
    std::size_t alive = 0;
    for (const MapVolcano& vent : mixed.graph().volcanoes) {
        if (vent.alive()) ++alive;
    }
    maps_logger().info("mixed: " + std::to_string(alive) + " of "
                       + std::to_string(mixed.graph().volcanoes.size()) + " vents alive");
    ASSERT_TRUE(alive > 0);
    ASSERT_TRUE(alive < mixed.graph().volcanoes.size());
}

COOPA_TEST(the_kinds_with_no_basin_pool_nothing_however_alive) {
    // A cinder cone is a pile of ash, a dome IS its own cooled lava, and a rift has no basin --
    // so "every vent alive" must still leave all three dry. This is the assertion that stops a
    // future retune from quietly giving every kind a lake.
    for (const VolcanoKind kind : {VolcanoKind::CinderCone, VolcanoKind::LavaDome,
                                   VolcanoKind::Fissure}) {
        const Population live(kind, 1.0);
        ASSERT_TRUE(!live.graph().volcanoes.empty());
        ASSERT_EQ(molten_count(live.graph()), static_cast<std::size_t>(0));
    }
    // And the three that do have one are wet on the same settings, so the difference is the
    // kind and not the map.
    for (const VolcanoKind kind : {VolcanoKind::Caldera, VolcanoKind::Stratovolcano,
                                   VolcanoKind::Shield}) {
        const Population live(kind, 1.0);
        ASSERT_TRUE(molten_count(live.graph()) > 0);
    }
}

COOPA_TEST(a_live_crater_reads_as_molten_even_on_a_water_world) {
    // The map's own fluid is water. A crater is lava anyway -- that is the whole point of the
    // per-sample `fluid`, and it is what lets a blue planet carry a glowing caldera.
    const Population live(VolcanoKind::Caldera, 1.0);
    const MapGraph& graph = live.graph();
    const MapConfig& config = live.generator.config();
    ASSERT_TRUE(config.surface_fluid == SurfaceFluid::Water);

    TerrainSurface surface(graph, config);
    const TerrainDetail& detail = surface.detail();
    const RiverChannels channels = make_river_channels(graph, config, detail);
    const WaterField water(graph, config, detail, channels);
    const Probe probe(graph, config);

    std::size_t molten = 0;
    std::size_t dry_rims = 0;
    for (const MapVolcano& vent : graph.volcanoes) {
        if (vent.lava_rise <= 0.0 || vent.submarine) continue;
        const WaterSample middle = water.at(probe.nearest(vent.point), vent.point);
        if (middle.wet() && middle.fluid == SurfaceFluid::Lava) {
            ++molten;
            // Molten, and standing in its own hole rather than sheeting over the landscape.
            ASSERT_TRUE(middle.depth() > 0.0);
        }
        // Out at the edifice's own base the ground has climbed over the lake and come back
        // down: nothing there is lava, which is what makes the pool a crater lake and not a
        // disc painted on a cone.
        bool lava_outside = false;
        for (const MapPoint& p : ring(graph, vent, 1.0)) {
            const WaterSample out = water.at(probe.nearest(p), p);
            if (out.wet() && out.fluid == SurfaceFluid::Lava) lava_outside = true;
        }
        if (!lava_outside) ++dry_rims;
    }
    maps_logger().info("water world: " + std::to_string(molten) + " molten craters, "
                       + std::to_string(dry_rims) + " with dry flanks");
    ASSERT_TRUE(molten > 0);
    ASSERT_TRUE(dry_rims > 0);
}

// --- An ice shell with an ocean under it ------------------------------------------------------

namespace {

/**
 * @brief A frozen ocean world with submarine volcanism: the ice-moon case.
 *
 * Mostly sea, classified by the glacial diagram, so every water cell freezes into a crust --
 * except wherever `volcano_warmth` says something underneath is melting it.
 */
MapConfig ice_config(double warmth, double live, int seed = 613) {
    return derived_config([&] {
        MapConfig c = world_config(seed);
        c.terrain_model = TerrainModel::Natural;
        c.biome_profile = BiomeProfile::Glacial;
        c.temperature_offset = -0.45;
        c.sea_level = 0.62;
        c.terrain_relief = 0.2;
        c.natural.volcanoes = 0.8;
        c.natural.volcano_density = 0.5;
        c.natural.volcano_size = 1.8;
        c.natural.volcano_submarine = 1.0;   // the vents stand on the SEA FLOOR
        c.natural.volcano_active = live;
        c.natural.volcano_warmth = warmth;
        return c;
    }());
}

/** @brief How many of a map's water cells are open water rather than ice. */
std::size_t open_water(const MapGraph& graph) {
    std::size_t open = 0;
    for (const MapCenter& center : graph.centers) {
        if (!center.water) continue;
        if (center.biome == Biome::Ocean || center.biome == Biome::Lake) ++open;
    }
    return open;
}

/** @brief How many water cells there are at all. */
std::size_t water_cells(const MapGraph& graph) {
    std::size_t n = 0;
    for (const MapCenter& center : graph.centers) {
        if (center.water) ++n;
    }
    return n;
}

} // namespace

COOPA_TEST(a_frozen_world_is_solid_ice_until_something_underneath_melts_it) {
    MapGenerator frozen(ice_config(0.0, 1.0), maps_logger());
    frozen.generate();
    MapGenerator melted(ice_config(1.0, 1.0), maps_logger());
    melted.generate();

    ASSERT_TRUE(water_cells(frozen.graph()) > 20);
    ASSERT_EQ(water_cells(frozen.graph()), water_cells(melted.graph()));
    // WITHOUT the key nothing changes at all -- which is what makes it safe to add to a
    // diagram every existing frozen world is already classified by.
    ASSERT_EQ(open_water(frozen.graph()), static_cast<std::size_t>(0));
    maps_logger().info("ice world: " + std::to_string(open_water(melted.graph())) + " of "
                       + std::to_string(water_cells(melted.graph()))
                       + " water cells open over live vents");
    ASSERT_TRUE(open_water(melted.graph()) > 0);
    // And it is a lid with holes in it, not a thaw: most of the sea is still ice.
    ASSERT_TRUE(open_water(melted.graph()) * 2 < water_cells(melted.graph()));
}

COOPA_TEST(a_dead_vent_melts_no_ice) {
    // The same warmth, the same seed, the same cones -- and every vent extinct. This is the
    // link the whole alive/dead distinction is for: the open water is the LIVE volcanism.
    MapGenerator dead(ice_config(1.0, 0.0), maps_logger());
    dead.generate();
    ASSERT_TRUE(!dead.graph().volcanoes.empty());
    for (const MapVolcano& vent : dead.graph().volcanoes) {
        ASSERT_TRUE(!vent.alive());
    }
    ASSERT_EQ(open_water(dead.graph()), static_cast<std::size_t>(0));
}

COOPA_TEST(the_open_water_stands_over_the_vents_that_opened_it) {
    MapGenerator melted(ice_config(1.0, 1.0), maps_logger());
    melted.generate();
    const MapGraph& graph = melted.graph();

    // Every melted cell has a live vent within the reach its own radius buys -- so the holes
    // are where the heat is and not an artefact of the climate noise.
    std::size_t checked = 0;
    for (const MapCenter& center : graph.centers) {
        if (!center.water) continue;
        if (center.biome != Biome::Ocean && center.biome != Biome::Lake) continue;
        bool near_a_vent = false;
        for (const MapVolcano& vent : graph.volcanoes) {
            if (!vent.alive()) continue;
            if (center.point.distance_to(vent.point) < vent.radius * 2.2) near_a_vent = true;
        }
        ASSERT_TRUE(near_a_vent);
        ++checked;
    }
    ASSERT_TRUE(checked > 0);
    maps_logger().info("every one of " + std::to_string(checked)
                       + " open cells stands over a live vent");
}

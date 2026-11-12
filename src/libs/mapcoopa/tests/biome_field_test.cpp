/**
 * @file biome_field_test.cpp
 * @brief The per-point biome query: the graded shore, agreement with the cell
 *        classification away from boundaries, and water reading as water.
 */

#include <coopa/testing/test.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include <coopa/maps/biome.h>
#include <coopa/maps/biome_field.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/water_field.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("biome_field");

/**
 * @brief The graded shore in `classify_biome(BiomeSample)`: beach is low ground
 *        near water, and nothing else.
 *
 * A flat shelf (low relief at the waterline) is beach; the same proximity over a
 * cliff lip is not, and low ground far from any water is not. The temperature
 * specialisations keep their old behaviour, and the cell-level overload is
 * exactly the struct call at full shore strength.
 */
COOPA_TEST(the_classifier_grades_the_shore) {
    BiomeSample shelf;
    shelf.elevation = 0.02;
    shelf.moisture = 0.4;
    shelf.temperature = 0.5;
    shelf.shore = 1.0;
    shelf.relief_m = 1.5;
    ASSERT_TRUE(classify_biome(shelf) == Biome::Beach);

    BiomeSample cliff = shelf;
    cliff.elevation = 0.3;
    cliff.relief_m = 25.0;
    ASSERT_TRUE(classify_biome(cliff) != Biome::Beach);

    BiomeSample inland = shelf;
    inland.shore = 0.0;
    inland.relief_m = 1.0e9;
    ASSERT_TRUE(classify_biome(inland) != Biome::Beach);

    // The band pinches out as proximity fades: half shore strength halves the
    // height a beach may stand at.
    BiomeSample fading = shelf;
    fading.shore = 0.5;
    fading.relief_m = 4.0;
    ASSERT_TRUE(classify_biome(fading) != Biome::Beach);
    fading.relief_m = 2.0;
    ASSERT_TRUE(classify_biome(fading) == Biome::Beach);

    BiomeSample tropical = shelf;
    tropical.temperature = 0.8;
    tropical.moisture = 0.7;
    ASSERT_TRUE(classify_biome(tropical) == Biome::Mangrove);

    BiomeSample frigid = shelf;
    frigid.temperature = 0.1;
    ASSERT_TRUE(classify_biome(frigid) == Biome::Tundra);

    // The legacy cell-level call is the struct call at full shore strength.
    ASSERT_TRUE(classify_biome(0.02, 0.4, 0.5, false, false, true) == Biome::Beach);
    ASSERT_TRUE(classify_biome(0.5, 0.4, 0.5, false, false, false)
                == classify_biome([] {
                       BiomeSample s;
                       s.elevation = 0.5;
                       s.moisture = 0.4;
                       s.temperature = 0.5;
                       return s;
                   }()));
}

namespace {

/** @brief The sampling stack every integration case below builds once. */
struct FieldFixture {
    const MapGraph& graph;
    MapConfig config;
    TerrainSurface surface;
    RiverChannels channels;
    WaterField water;
    BiomeField biomes;

    explicit FieldFixture(const MapGenerator& world, const MapConfig& used)
        : graph(world.graph()), config(used), surface(graph, config),
          channels(make_river_channels(graph, config, surface.detail())),
          water(graph, config, surface.detail(), channels), biomes(graph, config) {}

    /** @brief The moisture `BiomeField` classifies this point with, read off the field itself. */
    double moisture_at(const MapCenter& center, const MapPoint& p) const {
        return biomes.sample_at(center, p, water.at(center, p)).moisture;
    }
};

FieldFixture& shared_fields() {
    static FieldFixture fixture(shared_world(), world_config());
    return fixture;
}

} // namespace

/** @brief A point in the open sea classifies as ocean, every time. */
COOPA_TEST(open_sea_reads_ocean) {
    FieldFixture& f = shared_fields();
    std::size_t checked = 0;
    for (const MapCenter& center : f.graph.centers) {
        if (!center.ocean) continue;
        bool open = true;
        for (const CenterId n : center.neighbors) {
            open = open && f.graph.centers[static_cast<std::size_t>(n)].ocean;
        }
        if (!open) continue;
        const WaterSample water = f.water.at(center, center.point);
        ASSERT_TRUE(f.biomes.at(center, center.point, water) == Biome::Ocean);
        ++checked;
    }
    ASSERT_TRUE(checked > 0);
}

/**
 * @brief Away from every boundary, the point query agrees with the cell pass.
 *
 * Not bit-for-bit -- the point query reads the shaped ground and dithers its
 * thresholds, which is its purpose -- but an interior cell's own site must
 * usually land in the same biome the cell was assigned, or the two
 * classifications would be describing different worlds.
 */
COOPA_TEST(the_interior_agrees_with_the_cell_pass) {
    FieldFixture& f = shared_fields();
    std::size_t checked = 0;
    std::size_t agreed = 0;
    for (const MapCenter& center : f.graph.centers) {
        if (center.water || center.coast || center.border) continue;
        bool interior = true;
        for (const CenterId n : center.neighbors) {
            const MapCenter& cell = f.graph.centers[static_cast<std::size_t>(n)];
            interior = interior && !cell.water && !cell.coast;
        }
        if (!interior) continue;
        const WaterSample water = f.water.at(center, center.point);
        const Biome at_point = f.biomes.at(center, center.point, water);
        ++checked;
        agreed += at_point == center.biome;
    }
    ASSERT_TRUE(checked > 20);
    // Over half, well clear of what re-rolling every threshold would leave.
    ASSERT_TRUE(agreed * 2 > checked);
}

/**
 * @brief A river is a boundary: the climate the query reads jumps across it.
 *
 * The two cells a river runs between are its banks, and the Voronoi edge they
 * share is the watercourse. Sampling just inside each bank -- a tenth of the way
 * from each site toward the other, so both points are unambiguously on their own
 * side -- the blended moisture must differ by more than it does across an
 * ordinary edge between the same kind of neighbours. That difference is what
 * lands a biome transition on the river instead of near it.
 */
COOPA_TEST(a_river_separates_its_two_banks) {
    FieldFixture& f = shared_fields();

    // Mean |moisture difference| across a set of cell pairs, sampled just inside
    // each cell rather than at its site, which is where the blend does its work.
    const auto jump_across = [&](const std::vector<std::pair<CenterId, CenterId>>& pairs) {
        double total = 0.0;
        std::size_t counted = 0;
        for (const auto& [a_id, b_id] : pairs) {
            const MapCenter& a = f.graph.centers[static_cast<std::size_t>(a_id)];
            const MapCenter& b = f.graph.centers[static_cast<std::size_t>(b_id)];
            const auto toward = [&](const MapCenter& from, const MapCenter& to) {
                return f.graph.on_surface(MapPoint{from.point.x + 0.4 * (to.point.x - from.point.x),
                                                   from.point.y + 0.4 * (to.point.y - from.point.y),
                                                   from.point.z + 0.4 * (to.point.z - from.point.z)});
            };
            total += std::abs(f.moisture_at(a, toward(a, b)) - f.moisture_at(b, toward(b, a)));
            ++counted;
        }
        return counted > 0 ? total / static_cast<double>(counted) : 0.0;
    };

    std::vector<std::pair<CenterId, CenterId>> river_pairs;
    std::vector<std::pair<CenterId, CenterId>> plain_pairs;
    for (const MapEdge& edge : f.graph.edges) {
        if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id) continue;
        const MapCenter& a = f.graph.centers[static_cast<std::size_t>(edge.d0)];
        const MapCenter& b = f.graph.centers[static_cast<std::size_t>(edge.d1)];
        if (a.water || b.water) continue;   // dry land both sides, so only the river differs
        (edge.river > 0 ? river_pairs : plain_pairs).push_back({edge.d0, edge.d1});
    }
    ASSERT_TRUE(!river_pairs.empty());
    ASSERT_TRUE(!plain_pairs.empty());

    const double across_rivers = jump_across(river_pairs);
    const double across_plain = jump_across(plain_pairs);
    // A river edge holds a real step; an ordinary edge is smooth, so its two sides
    // read almost the same value.
    ASSERT_TRUE(across_rivers > across_plain * 2.0);
}

/**
 * @brief The coast grows real beaches: low shore points classify as beach, and
 *        every beach the field reports is genuinely low ground beside water.
 */
COOPA_TEST(flat_shores_grow_beaches) {
    FieldFixture& f = shared_fields();
    std::size_t beaches = 0;
    for (const MapCenter& center : f.graph.centers) {
        if (!center.coast) continue;
        // Walk from the cell site toward the ocean neighbour's site: the
        // midpoint lies near the waterline, where the band is widest.
        for (const CenterId n : center.neighbors) {
            const MapCenter& sea = f.graph.centers[static_cast<std::size_t>(n)];
            if (!sea.ocean) continue;
            const MapPoint mid = f.graph.on_surface(
                MapPoint{0.5 * (center.point.x + sea.point.x),
                         0.5 * (center.point.y + sea.point.y),
                         0.5 * (center.point.z + sea.point.z)});
            const WaterSample water = f.water.at(center, mid);
            if (water.wet()) continue;
            const Biome biome = f.biomes.at(center, mid, water);
            // Tundra is also an interior biome, so only the two the shore branch
            // alone can produce carry the band's height claim.
            if (biome != Biome::Beach && biome != Biome::Mangrove) {
                continue;
            }
            // The band's own claim: shore ground within the beach height of the sea.
            const double relief =
                height_to_meters(f.config, water.ground - f.config.sea_level);
            ASSERT_TRUE(relief < f.config.beach_height_m + 1.0e-9);
            ++beaches;
        }
    }
    ASSERT_TRUE(beaches > 0);
}

/**
 * @file planet_test.cpp
 * @brief Planet TileTerrains (toyengine/world/planet.h): the cube-sphere layout's mapping and its
 *        neighbours across face edges, the planet's size, and a planet bake from mapcoopa's globe
 *        keeping the water rule across every face.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <set>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_presets.h>
#include <toyengine/world/planet.h>
#include <toyengine/mapgen/terrain_sampler.h>

#include "engine/support/checks.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("planet");

using namespace toy::test;
using toy::world::PlanetLayout;

COOPA_TEST(every_tile_maps_to_its_own_direction_and_back) {
    PlanetLayout layout{24, 1.0f};
    for (int c = 0; c < 6 * layout.n; ++c) {
        for (int r = 0; r < layout.n; ++r) {
            const glm::ivec2 back = layout.tile_of(layout.tile_direction(c, r));
            if (back != glm::ivec2(c, r)) {
                expect(false, "tile " + std::to_string(c) + "," + std::to_string(r) + " maps back to itself");
                return;
            }
        }
    }
    expect(true, "all 6 x 24 x 24 tiles round-trip");
}

COOPA_TEST(neighbours_across_face_edges_are_adjacent) {
    PlanetLayout layout{16, 1.0f};
    const double tile_angle = PlanetLayout::k_pi * 0.5 / layout.n;
    int checked = 0, bad = 0;
    for (int c = 0; c < 6 * layout.n; ++c) {
        for (int r = 0; r < layout.n; ++r) {
            const glm::dvec3 here = layout.tile_direction(c, r);
            const int steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& s : steps) {
                const glm::ivec2 nb = layout.neighbour(c, r, s[0], s[1]);
                const double angle = std::acos(std::clamp(glm::dot(here, layout.tile_direction(nb.x, nb.y)), -1.0, 1.0));
                // A different tile, about one tile away, whose own neighbours lead back here.
                bool back = false;
                for (const auto& t : steps) back = back || layout.neighbour(nb.x, nb.y, t[0], t[1]) == glm::ivec2(c, r);
                if (nb == glm::ivec2(c, r) || angle > tile_angle * 1.9 || !back) ++bad;
                ++checked;
            }
        }
    }
    expect(bad == 0, "every cardinal neighbour is adjacent and leads back (" + std::to_string(bad) + " of " +
                         std::to_string(checked) + " not)");
}

COOPA_TEST(a_planet_tile_is_tile_size_across) {
    PlanetLayout layout{128, 1.0f};
    const float r = layout.radius();
    expect(std::abs(r - 2.0f * 128.0f / 3.14159265f) < 1e-3f, "radius = 2 N tile / pi");
    const glm::dvec3 a = layout.to_planet(0, 63.0, 64.0, 0.0), b = layout.to_planet(0, 64.0, 64.0, 0.0);
    expect(std::abs(glm::length(a - b) - 1.0) < 0.02, "a tile in the middle of a face spans its tile size");
    const glm::dvec3 c0 = layout.to_planet(4, 0.0, 0.0, 0.0), c1 = layout.to_planet(4, 1.0, 1.0, 0.0);
    expect(glm::length(c0 - c1) > 0.8, "a corner tile is not squeezed to nothing");
}

COOPA_TEST(a_planet_bakes_from_the_globe_with_water_never_above_its_banks) {
    using namespace toy::world;
    coopa::debug::Logger logger("planet_test");
    coopa::maps::MapConfig authored = coopa::maps::standard_config();
    authored.topology = coopa::maps::MapTopology::Sphere;
    authored.grid_size = 24;
    authored.terrain_model = coopa::maps::TerrainModel::Natural;
    const coopa::maps::MapConfig config = coopa::maps::derived_config(authored);
    coopa::maps::MapGenerator generator(config, logger);
    generator.generate();
    TerrainParams params = make_test_params(16);
    params.planet = true;
    params.face_tiles = 32;
    params.height_scale = 16.0f;
    TerrainSampler sampler;
    sampler.build(config, std::move(generator.graph()), params);
    TileMapData tiles;
    sampler.bake(tiles);
    expect(tiles.width() == 6 * 32 && tiles.height() == 32, "six faces side by side");
    const PlanetLayout layout = params.layout();
    int wet = 0, over = 0, dry = 0;
    for (int y = 0; y < tiles.height(); ++y) {
        for (int x = 0; x < tiles.width(); ++x) {
            const TileRecord& r = tiles.at(x, y);
            if (!r.water()) { ++dry; continue; }
            ++wet;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const glm::ivec2 n = layout.neighbour(x, y, dx, dy);
                    if (!tiles.at(n.x, n.y).water() && tiles.at(n.x, n.y).steps < r.water_level()) ++over;
                }
        }
    }
    expect(wet > 0 && dry > 0, "the globe has land and sea");
    expect(over == 0, "no dry tile on any face lies under the water beside it (" + std::to_string(over) + ")");
}

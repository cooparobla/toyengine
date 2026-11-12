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
#include <toyengine/env/world_frame.h>
#include <toyengine/world/planet.h>
#include <toyengine/mapgen/terrain_sampler.h>

#include "engine/support/checks.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("planet");

using namespace toy::test;
using toy::world::PlanetLayout;
using toy::env::WorldFrame;

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
    TerraData tiles;
    sampler.bake(tiles);
    expect(tiles.geometry().tiles_x == 6 * 32 && tiles.geometry().tiles_y == 32, "six faces side by side");
    const PlanetLayout layout = params.layout();
    const float height_step = tiles.geometry().height_step;
    int wet = 0, over = 0, dry = 0;
    for (int y = 0; y < tiles.geometry().tiles_y; ++y) {
        for (int x = 0; x < tiles.geometry().tiles_x; ++x) {
            const TileCell& r = tiles.at(x, y);
            if (!r.wet()) { ++dry; continue; }
            ++wet;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const glm::ivec2 n = layout.neighbour(x, y, dx, dy);
                    const TileCell& nc = tiles.at(n.x, n.y);
                    if (!nc.wet() && static_cast<float>(nc.steps) * height_step < r.water_level) ++over;
                }
        }
    }
    expect(wet > 0 && dry > 0, "the globe has land and sea");
    expect(over == 0, "no dry tile on any face lies under the water beside it (" + std::to_string(over) + ")");
}

// --- The planetary frame (toyengine/env/world_frame.h) ---

COOPA_TEST(a_default_frame_is_the_flat_z_up_world) {
    const WorldFrame flat{};
    expect(!flat.curved(), "a default frame is not curved");
    const glm::vec3 p(12.0f, -5.0f, 7.5f);
    expect(flat.up_at(p) == glm::vec3(0.0f, 0.0f, 1.0f), "up is +Z anywhere");
    expect(std::abs(flat.altitude_at(p) - 7.5f) < 1e-6f, "altitude is p.z");
    expect(flat.tangent_at(p) == glm::mat3(1.0f), "the tangent basis is identity");
    // The point at an altitude keeps xy and takes z from the altitude, so a flat caller's
    // "put this at height h over there" reads the same as it always did.
    const glm::vec3 at = flat.point_at(glm::vec3(3.0f, 4.0f, 0.0f), 9.0f);
    expect(at == glm::vec3(3.0f, 4.0f, 9.0f), "point_at lays the altitude on z");
    const glm::vec3 v(1.0f, 0.0f, 0.0f);
    expect(transport(flat, v, p, glm::vec3(-40.0f, 60.0f, 3.0f)) == v, "transport is a no-op when flat");
}

COOPA_TEST(a_planet_frame_answers_up_altitude_and_a_tangent_basis) {
    const WorldFrame planet{glm::vec3(10.0f, 0.0f, 0.0f), 100.0f};
    expect(planet.curved(), "a positive radius is curved");

    // Straight out along +X from a centre offset to (10,0,0): up is +X, altitude is the
    // distance past the datum. Both must be measured from the CENTRE, not the origin.
    const glm::vec3 p = glm::vec3(10.0f + 130.0f, 0.0f, 0.0f);
    expect(glm::length(planet.up_at(p) - glm::vec3(1.0f, 0.0f, 0.0f)) < 1e-5f, "up points away from the centre");
    expect(std::abs(planet.altitude_at(p) - 30.0f) < 1e-4f, "altitude is the height above the datum");
    expect(std::abs(planet.altitude_at(planet.centre + glm::vec3(0.0f, 0.0f, 100.0f))) < 1e-4f,
           "a point on the datum is at altitude 0");
    expect(planet.altitude_at(planet.centre) < 0.0f, "below the datum is negative");

    // point_at inverts altitude_at for any direction.
    const glm::vec3 dir = glm::normalize(glm::vec3(1.0f, 2.0f, -3.0f));
    const glm::vec3 q = planet.point_at(dir, 17.0f);
    expect(std::abs(planet.altitude_at(q) - 17.0f) < 1e-3f, "point_at and altitude_at invert");
    expect(glm::length(planet.up_at(q) - dir) < 1e-5f, "point_at lies along its direction");

    // The basis is orthonormal, right-handed, and its up agrees with up_at.
    const glm::mat3 b = planet.tangent_at(q);
    expect(glm::length(b[2] - planet.up_at(q)) < 1e-5f, "column 2 is up");
    expect(std::abs(glm::dot(b[0], b[1])) < 1e-5f && std::abs(glm::dot(b[0], b[2])) < 1e-5f &&
               std::abs(glm::dot(b[1], b[2])) < 1e-5f, "the basis is orthogonal");
    expect(std::abs(glm::length(b[0]) - 1.0f) < 1e-5f && std::abs(glm::length(b[1]) - 1.0f) < 1e-5f,
           "the basis is normalized");
    expect(glm::length(glm::cross(b[0], b[1]) - b[2]) < 1e-5f, "east x north == up (right-handed)");
}

COOPA_TEST(the_tangent_basis_survives_both_poles) {
    const WorldFrame planet{glm::vec3(0.0f), 50.0f};
    for (const float sign : {1.0f, -1.0f}) {
        // Exactly on the pole, where north has no bearing: the basis must still be usable
        // rather than a NaN, because a camera or a wind sample can land there.
        const glm::mat3 b = planet.tangent_at(glm::vec3(0.0f, 0.0f, sign * 60.0f));
        const bool finite = b[0] == b[0] && b[1] == b[1] && b[2] == b[2];
        expect(finite, "the pole's basis is finite");
        expect(std::abs(glm::length(b[0]) - 1.0f) < 1e-4f, "its east is still a unit vector");
        expect(glm::length(glm::cross(b[0], b[1]) - b[2]) < 1e-4f, "it is still right-handed");
    }
}

COOPA_TEST(transport_carries_a_tangent_wind_without_spinning_or_shrinking) {
    const WorldFrame planet{glm::vec3(0.0f), 100.0f};
    const glm::vec3 from = planet.point_at(glm::vec3(1.0f, 0.0f, 0.0f), 0.0f);
    // A wind blowing east at `from`, carried a quarter of the way round the globe.
    const glm::vec3 wind = planet.tangent_at(from)[0] * 7.0f;
    const glm::vec3 to = planet.point_at(glm::vec3(0.0f, 1.0f, 0.0f), 0.0f);
    const glm::vec3 moved = transport(planet, wind, from, to);
    expect(std::abs(glm::length(moved) - 7.0f) < 1e-3f, "transport keeps the wind's speed");
    expect(std::abs(glm::dot(moved, planet.up_at(to))) < 1e-3f, "it stays tangent at the far point");
    // Transporting back must return the original vector -- the property that stops a wind
    // from drifting as it is carried from sample to sample.
    const glm::vec3 back = transport(planet, moved, to, from);
    expect(glm::length(back - wind) < 1e-3f, "transport there and back is the identity");
}

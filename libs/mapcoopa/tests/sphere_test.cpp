/**
 * @file sphere_test.cpp
 * @brief Sphere maps (`MapTopology::Sphere`): a closed globe of cells -- every edge two-sided,
 *        every corner three cells, Euler's formula -- with oceans, rivers, climate by latitude,
 *        the water guarantees, and 3D queries over it.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <vector>

#include <coopa/maps/map_presets.h>
#include <coopa/maps/map_renderer.h>
#include <coopa/maps/water_field.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("sphere");

namespace {

MapConfig sphere_config(int seed = 251, MapShape shape = MapShape::Rectangle) {
    MapConfig config = derived_config([&] {
        MapConfig c = world_config(seed);
        c.topology = MapTopology::Sphere;
        c.grid_size = 40;
        c.shape.shape = shape;
        return c;
    }());
    return config;
}

/** @brief Points spread evenly over a globe of radius r (a Fibonacci spiral). */
std::vector<MapPoint> globe_samples(double r, int n) {
    std::vector<MapPoint> out;
    const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < n; ++i) {
        const double z = 1.0 - (2.0 * i + 1.0) / n;
        const double s = std::sqrt(1.0 - z * z);
        out.push_back({r * s * std::cos(golden * i), r * s * std::sin(golden * i), r * z});
    }
    return out;
}

const MapCenter& nearest(const MapGraph& graph, const MapPoint& p) {
    std::size_t best = 0;
    double d = 1e30;
    for (std::size_t i = 0; i < graph.centers.size(); ++i) {
        const double e = graph.centers[i].point.distance_to(p);
        if (e < d) { d = e; best = i; }
    }
    return graph.centers[best];
}

} // namespace

COOPA_TEST(a_globe_is_closed) {
    const MapConfig config = sphere_config();
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    const double r = sphere_radius(config);
    ASSERT_TRUE(graph.is_sphere() && std::abs(graph.sphere_radius - r) < 1e-12);
    ASSERT_EQ(static_cast<int>(graph.centers.size()), sphere_point_count(config));
    // V - E + F = 2: Voronoi corners, edges, cells.
    ASSERT_EQ(static_cast<long>(graph.corners.size()) - static_cast<long>(graph.edges.size())
                  + static_cast<long>(graph.centers.size()),
              2L);
    for (const MapCorner& corner : graph.corners) {
        ASSERT_EQ(corner.touches.size(), std::size_t{3});
        ASSERT_TRUE(std::abs(std::sqrt(corner.point.x * corner.point.x + corner.point.y * corner.point.y
                                       + corner.point.z * corner.point.z) - r) < 1e-6);
    }
    for (const MapCenter& center : graph.centers) {
        ASSERT_TRUE(center.corners.size() >= 3);
        ASSERT_TRUE(!center.border);   // no shape: no border anywhere
        ASSERT_TRUE(std::isfinite(center.elevation) && center.elevation >= 0.0 && center.elevation <= 1.0);
    }
}

COOPA_TEST(a_globe_has_seas_rivers_and_climate) {
    const MapConfig config = sphere_config(31);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    std::size_t ocean = 0, land = 0;
    double polar = 0.0, equator = 0.0;
    int polar_n = 0, equator_n = 0;
    for (const MapCenter& c : graph.centers) {
        ocean += c.ocean ? 1 : 0;
        land += c.water ? 0 : 1;
        const double lat = std::abs(c.point.z) / graph.sphere_radius;
        if (lat > 0.9) { polar += c.temperature; ++polar_n; }
        if (lat < 0.2) { equator += c.temperature; ++equator_n; }
    }
    ASSERT_TRUE(ocean > 0 && land > 0);
    ASSERT_TRUE(!graph.rivers.empty());
    ASSERT_TRUE(polar_n > 0 && equator_n > 0 && polar / polar_n < equator / equator_n);
}

COOPA_TEST(a_continent_globe_rings_its_land_with_sea) {
    const MapConfig config = sphere_config(17, MapShape::Archipelago);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    std::size_t border = 0, ocean = 0;
    for (const MapCenter& c : generator.graph().centers) {
        border += c.border ? 1 : 0;
        ocean += c.ocean ? 1 : 0;
    }
    ASSERT_TRUE(border > 0 && ocean >= border);
}

/** @brief The water guarantees on a globe, in both terrain models. */
COOPA_TEST(globe_water_never_stands_over_dry_ground) {
    for (const TerrainModel model : {TerrainModel::Classic, TerrainModel::Natural}) {
        MapConfig config = sphere_config(67);
        config.terrain_model = model;
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();
        const TerrainSurface surface(graph, config);
        const RiverChannels channels = make_river_channels(graph, config, surface.detail());
        const WaterField field(graph, config, surface.detail(), channels);
        const std::vector<MapPoint> points = globe_samples(graph.sphere_radius, 12000);
        std::vector<WaterSample> samples;
        samples.reserve(points.size());
        std::size_t wet = 0, rivers = 0;
        for (const MapPoint& p : points) {
            samples.push_back(field.at(nearest(graph, p), p));
            wet += samples.back().wet() ? 1 : 0;
            rivers += samples.back().kind == WaterKind::River ? 1 : 0;
            ASSERT_TRUE(!samples.back().wet() || samples.back().ground < samples.back().surface);
        }
        ASSERT_TRUE(wet > 0 && wet < points.size());
        // Bodies stand under every shore: no lake cell's level above a dry neighbour's site.
        for (const MapCenter& c : graph.centers) {
            if (!c.water || c.ocean) continue;
            for (const CenterId n : c.neighbors) {
                const MapCenter& shore = graph.centers[static_cast<std::size_t>(n)];
                if (!shore.water) ASSERT_TRUE(shore.elevation > c.water_level || c.water_level == config.sea_level);
            }
        }
        (void)rivers;
    }
}

COOPA_TEST(globe_queries_are_seamless) {
    const MapConfig config = sphere_config(5);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    // Every point of the globe is claimed by a triangle, and the surface is continuous: two
    // points a hair apart read heights a hair apart, wherever they are.
    int checked = 0;
    for (const MapPoint& p : globe_samples(graph.sphere_radius, 3000)) {
        std::array<CenterId, 3> cells{};
        std::array<double, 3> weights{};
        const MapCenter& c = nearest(graph, p);
        ASSERT_TRUE(graph.triangle_at(c, p, cells, weights));
        const MapPoint q = graph.step_from(p, 0.7, 1e-3);
        ASSERT_TRUE(std::abs(graph.elevation_at(c, p) - graph.elevation_at(nearest(graph, q), q)) < 1e-2);
        ++checked;
    }
    ASSERT_TRUE(checked > 0);
}

/** @brief A globe renders equirectangular -- twice as wide as tall -- and its seam meets itself. */
COOPA_TEST(a_globe_renders_without_a_seam) {
    MapConfig config = sphere_config(31);
    set_render_size(config, 256);
    config.image_size = 256;
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const Image image = MapLayers::composite(generator.graph(), config);
    ASSERT_EQ(image.width, 256);
    ASSERT_EQ(image.height, 128);
    // The first and last columns are neighbours across the antimeridian: they mostly agree.
    int agree = 0;
    for (int y = 0; y < image.height; ++y) {
        const glm::vec3 a = image.color_at(0, y);
        const glm::vec3 b = image.color_at(image.width - 1, y);
        agree += glm::length(a - b) < 40.0f ? 1 : 0;
    }
    ASSERT_TRUE(agree * 10 >= image.height * 8);
}

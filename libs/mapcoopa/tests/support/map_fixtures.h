#pragma once

/**
 * @file map_fixtures.h
 * @brief Shared fixtures for mapcoopa's suites: the standard configs, a logger and job engine,
 *        and the handful of generated worlds most suites read from.
 *
 * Generation is deterministic -- the same config produces the same graph, bit for bit, which
 * `determinism_test.cpp` pins -- and every check reads the graph without writing to it. So a
 * world that many tests would each regenerate identically is generated once per suite process
 * (a function-local static) and handed out `const`. A test that needs a *different* world, or
 * that compares two runs, still builds its own generator.
 */

#include <cstddef>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>

#include <coopa/debug/logger.h>
#include <coopa/job/engine.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/map_generator.h>

namespace mapcoopa_test {

using namespace coopa::maps;

inline constexpr double k_pi = 3.14159265358979323846;

/** @brief One logger for every map a suite builds, so its output stays attributable. */
inline coopa::debug::Logger& maps_logger() {
    static coopa::debug::Logger logger("maps_test");
    return logger;
}

/**
 * @brief One `JobEngine` shared by every test that needs one.
 *
 * A `JobEngine` starts a thread per core, and a suite that built one per case would spend its
 * runtime on thread creation. Shared is also the way it is meant to be used -- see the engine's
 * own note on long-lived subsystems sharing a single engine.
 */
inline coopa::job::JobEngine& maps_engine() {
    static coopa::job::JobEngine engine;
    return engine;
}

/**
 * @brief Sets a render resolution the way the loader and `--image-size` do.
 *
 * By back-computing the scale, so `image_size` and `meters_per_pixel` cannot disagree -- see
 * `MapConfig::image_size`. Assigning `image_size` alone leaves a fixture claiming a 960 m world
 * is 128 px across *at one pixel to the metre*, which these fixtures did for as long as they
 * existed.
 *
 * @param config Configured in place; `grid_size` and `meters_per_grid_unit` must already be set.
 * @param pixels Desired side length of the render.
 */
inline void set_render_size(MapConfig& config, int pixels) {
    const double world = static_cast<double>(config.grid_size) * config.meters_per_grid_unit;
    config.meters_per_pixel = world / static_cast<double>(pixels);
    config.image_size = derive_image_size(config);
}

/**
 * @brief Small enough that a full generate() is a few milliseconds, large enough that the
 *        passes have real terrain to work on.
 */
inline MapConfig small_config(int seed = 251) {
    MapConfig config;
    config.grid_size = 16;
    set_render_size(config, 128);
    config.seed = seed;
    config.noise_island.seed = seed;
    return config;
}

/** @brief Regions, names, landmarks and climate range need more land than a 16-cell map offers. */
inline MapConfig world_config(int seed = 251) {
    MapConfig config = small_config(seed);
    config.grid_size = 48;
    config.regions.country_count = 4;
    config.regions.regions_per_country = 2;
    return config;
}

/** @brief A config big enough for the road network to have somewhere to go. */
inline MapConfig road_config(int seed = 77) {
    MapConfig config;
    config.grid_size = 32;
    set_render_size(config, 256);
    config.seed = seed;
    config.noise_island.seed = seed;
    return config;
}

/**
 * @brief Builds a map whose terrain is steep enough to bear caves.
 *
 * `small_config()` is a 16-cell grid, which has too few land edges clearing `min_grade` for a
 * spacing rule to place many mouths on. This is the smallest map that reliably opens several
 * systems, so the cave cases assert on a population rather than on one lucky cave.
 */
inline MapConfig cave_config(int seed = 251) {
    MapConfig config = world_config(seed);
    config.caves.cave_count = 8;
    return config;
}

namespace fixture_detail {
inline const MapGenerator& generated_once(MapGenerator& generator) {
    generator.generate();
    return generator;
}
} // namespace fixture_detail

/** @brief `small_config()` (seed 251), generated once per process. Read-only. */
inline const MapGenerator& shared_small_world() {
    static MapGenerator generator(small_config(), maps_logger());
    static const MapGenerator& done = fixture_detail::generated_once(generator);
    return done;
}

/** @brief `world_config()` (seed 251), generated once per process. Read-only. */
inline const MapGenerator& shared_world() {
    static MapGenerator generator(world_config(), maps_logger());
    static const MapGenerator& done = fixture_detail::generated_once(generator);
    return done;
}

/**
 * @brief `world_config(67)`, generated once per process. Read-only.
 *
 * The seed the river-channel and river-surface checks were all measured on: its rivers are long
 * enough, and run far enough above the waterline, for every sampling probe to find stretches.
 */
inline const MapGenerator& shared_river_world() {
    static MapGenerator generator(world_config(67), maps_logger());
    static const MapGenerator& done = fixture_detail::generated_once(generator);
    return done;
}

/** @brief Reads a whole file, or returns an empty string if it will not open. */
inline std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

/** @brief Writes `body` to `path` and returns the path. */
inline std::string write_file(const std::string& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary);
    out << body;
    return path;
}

/** @brief Samples the ground exactly as the cave pass and the renderer do. */
class SurfaceProbe {
public:
    SurfaceProbe(const MapGraph& graph, const MapConfig& config)
        : graph_(graph), surface_(std::make_unique<TerrainSurface>(graph, config)),
          detail_(surface_->detail()),
          channels_(make_river_channels(graph, config, detail_)) {}

    double at(const MapPoint& point) const {
        // The nearest site is the cell, which is what `elevation_at()` wants as a hint. Linear
        // here rather than the pass's graph walk on purpose: a test that reuses the machinery
        // it is checking can agree with a bug.
        std::size_t best = 0;
        double nearest = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < graph_.centers.size(); ++i) {
            const double distance = graph_.centers[i].point.distance_to(point);
            if (distance < nearest) {
                nearest = distance;
                best = i;
            }
        }
        return graph_.elevation_at(graph_.centers[best], point.x, point.y, detail_, channels_);
    }

private:
    const MapGraph& graph_;
    std::unique_ptr<TerrainSurface> surface_;
    TerrainDetail detail_;
    RiverChannels channels_;
};

} // namespace mapcoopa_test

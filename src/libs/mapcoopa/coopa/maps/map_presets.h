/**
 * @file map_presets.h
 * @brief The standard map -- what a bare `mapcoopa` run generates -- and the values every host
 *        derives from a config before generating.
 *
 * `MapConfig`'s member defaults are the library's neutral starting point (a 50-cell rectangle).
 * The map the tools present as *the* default is a different, deliberate one: an 80-cell world of
 * continents with 55 rivers and 28 towns. It used to exist only as the CLI's `main()` overrides
 * layered under `assets/config.yaml`, so a host embedding the library (a game engine, the viewer)
 * could only reproduce it by copying both. `standard_config()` states it once; the CLI, the
 * viewer and any host start from it, and `presets_test.cpp` holds it to the shipped config file.
 *
 * `derived_config()` is the other half every host needs: the values the CLI computes *after*
 * reading a config. Skip it and the same seed makes a different map -- the land/water field would
 * keep its own fixed seed, so changing `seed` would barely move a coastline.
 */

#ifndef COOPA_MAPS_MAP_PRESETS_H
#define COOPA_MAPS_MAP_PRESETS_H

#include <coopa/maps/map_config.h>

namespace coopa {
namespace maps {

/**
 * @brief The grid size the island field's frequency is authored at.
 *
 * `noise_island.frequency` is stated for a 40-cell grid and scaled by `40 / grid_size`, so the
 * grid size controls *detail* rather than the size of the world: holding the frequency fixed while
 * enlarging the map would shrink every landmass into an archipelago of the same small islands.
 */
inline constexpr double k_reference_grid_size = 40.0;

/**
 * @brief The standard map's settings: what `mapcoopa` with no arguments generates.
 *
 * Authored values only -- pass the result through `derived_config()` before generating, exactly
 * as the CLI does after reading its config file.
 */
inline MapConfig standard_config() {
    MapConfig c;
    // Large enough that climate bands, several nations and a spread of landmarks all have room.
    c.seed = 42;
    c.grid_size = 80;
    c.river_count = 55;
    c.towns.town_count = 28;
    c.landmarks.max_natural = 70;
    c.landmarks.max_abandoned = 45;

    c.elevation_surface = ElevationSurface::Blended;
    c.elevation_blend = 0.1;
    c.elevation_blend_variation = 0.5;
    c.elevation_smoothing_iterations = 3;

    c.shape.shape = MapShape::Continent;
    c.shape.diameter_m = 4600.0;
    c.shape.edge_length_m = 4500.0;
    c.shape.rotation = 45.0;
    // Derived from the world extent at one metre a pixel, as loading a config file derives it.
    c.image_size = derive_image_size(c);
    return c;
}

/**
 * @brief A copy of `authored` with every value the CLI derives after loading a config.
 *
 * - The per-pass noise seeds follow the master seed, so one `seed` reproduces the whole map.
 * - The island frequency (and natural terrain's landform fields) are scaled by
 *   `k_reference_grid_size / grid_size`.
 * - `image_size` is re-derived from the world extent and `meters_per_pixel`.
 *
 * Takes the *authored* config and returns a new one rather than adjusting in place, so calling it
 * twice can never scale the frequency twice.
 */
inline MapConfig derived_config(MapConfig authored) {
    authored.noise_island.seed = authored.seed;
    authored.noise_temperature.seed = authored.seed + 1;
    authored.noise_blend.seed = authored.seed + 2;
    authored.noise_cave.seed = authored.seed + 3;
    authored.noise_hills.seed = authored.seed + 4;
    authored.noise_ridge.seed = authored.seed + 5;
    authored.noise_plateau.seed = authored.seed + 6;
    authored.noise_warp.seed = authored.seed + 7;
    authored.noise_seabed.seed = authored.seed + 8;
    authored.noise_biome.seed = authored.seed + 9;
    const double grid = authored.grid_size > 0 ? static_cast<double>(authored.grid_size) : k_reference_grid_size;
    authored.noise_island.frequency *= k_reference_grid_size / grid;
    // The natural landform fields keep their size against the world, not the cell, likewise.
    for (NoiseConfig* field : {&authored.noise_hills, &authored.noise_ridge, &authored.noise_plateau,
                               &authored.noise_warp, &authored.noise_seabed}) {
        field->frequency *= k_reference_grid_size / grid;
    }
    authored.image_size = derive_image_size(authored);
    return authored;
}

/**
 * @brief Sets the render resolution by back-computing the scale, as `--image-size` does.
 *
 * `image_size` and `meters_per_pixel` must always agree about how much ground a pixel covers
 * (see `MapConfig::image_size`), so a wanted resolution is applied through the scale.
 *
 * @param config Adjusted in place; `grid_size` and `meters_per_grid_unit` must be final.
 * @param pixels Wanted side length; values below 1 are ignored.
 */
inline void set_image_size(MapConfig& config, int pixels) {
    if (pixels < 1) {
        return;
    }
    const double world_meters = static_cast<double>(config.grid_size) * config.meters_per_grid_unit;
    config.meters_per_pixel = world_meters / static_cast<double>(pixels);
    config.image_size = derive_image_size(config);
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_PRESETS_H

/**
 * @file map_config_test.cpp
 * @brief Configuration files: load_config()'s error reporting and override semantics, a
 *        lossless round trip of every MapConfig field, the shipped assets/config.yaml, and the
 *        image_size / meters_per_pixel reconciliation.
 *
 * Not tested: the shipped file's individual tuning values (grid size, counts, cave knobs) --
 * those are documentation, not contract; what is checked is that it loads and ships with every
 * pass on. The scale arithmetic itself is world_scale_test.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <fstream>
#include <string>

#include <root_directory.h>

#include <coopa/maps/map_renderer.h>
#include <coopa/maps/map_yaml.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("map_config");

namespace {

/**
 * @brief A `MapConfig` with every field moved off its default.
 *
 * Deliberately exhaustive and deliberately not derived from the defaults: the
 * point is that a field left out of `config_to_node()` or `config_from_node()`
 * comes back as its default, so any field this function forgets to disturb is a
 * field the round-trip test cannot catch.
 */
MapConfig perturbed_config() {
    MapConfig config;
    config.grid_size = 37;
    config.jitter = 0.41;
    config.seed = 90210;
    config.border_length = 1.75;
    // The render scale, set from the metres end. `image_size` is not listed
    // separately: it is not independent of this, and the two are reconciled on
    // load -- see the assertions in `config_round_trips_every_field`.
    // Setting both here would invite a contradictory pair (say `image_size = 333`
    // beside 1 m per pixel on a 37 x 60 m world, 2220 m of ground claiming to be
    // 333 px) that load would silently overwrite.
    config.meters_per_pixel = 3.0;
    config.png_compression_level = 4;
    config.sea_level = 0.18;
    config.elevation_range_m = 777.0;
    config.river_depth_m = 2.5;
    config.river_depth_per_volume_m = 0.6;
    config.river_channel_depth_m = 23.0;
    config.river_channel_depth_per_volume_m = 7.0;
    config.river_mouth_blend_m = 175.0;
    config.river_incision_m = 88.0;
    config.river_incision_per_volume_m = 17.0;
    config.river_valley_width = 4;
    config.river_valley_falloff = 0.33;
    config.water_edge_overlap_m = 3.0;
    config.terrain_relief = 0.31;
    config.terrain_roughness = 0.44;
    config.noise_relief = {44, 0.11, FastNoiseLite::NoiseType_Perlin,
                           FastNoiseLite::FractalType_FBm, 3, 2.1, 0.55, 0.4};
    config.noise_terrain = {33, 0.77, FastNoiseLite::NoiseType_Value,
                            FastNoiseLite::FractalType_FBm, 4, 1.9, 0.45, 0.3};
    config.shape = {MapShape::Triangle, 111.0, 222.0, 333.0, 444.0, 0.55,
                    555.0, 6, 0.41, 0.22, 0.19};
    config.noise_shape = {66, 0.22, FastNoiseLite::NoiseType_Perlin,
                          FastNoiseLite::FractalType_FBm, 2, 2.3, 0.35, 0.25};
    config.noise_cave = {77, 0.61, FastNoiseLite::NoiseType_Perlin,
                         FastNoiseLite::FractalType_FBm, 3, 2.2, 0.4, 0.35};
    config.show_regions = false;
    config.draw_landmark_marks = true;
    config.composite_shading = CompositeShading::Hillshade;
    config.elevation_surface = ElevationSurface::Flat;
    config.elevation_blend = 0.37;
    config.region_tint = 0.42f;
    config.temperature_lapse_rate = 0.31;
    config.temperature_falloff = 2.4;
    config.temperature_offset = -0.18;
    config.polar_extent_north = 0.31;
    config.polar_extent_south = 0.07;
    config.elevation_smoothing_iterations = 9;
    config.elevation_smoothing_strength = 0.66;
    config.threshold_water = 0.44;
    config.threshold_water_count = 3;
    config.river_count = 17;
    config.river_width_base_m = 0.077;
    config.river_width_per_volume_m = 0.033;
    config.trail_width_m = 0.11;
    config.road_width_m = 0.22;
    config.highway_width_m = 0.33;
    config.subdivide_noisy_edges = false;

    config.noise_island = {11, 0.123, FastNoiseLite::NoiseType_Cellular,
                           FastNoiseLite::FractalType_Ridged, 3, 2.5, 0.6, 0.7};
    config.noise_temperature = {22, 0.456, FastNoiseLite::NoiseType_Perlin,
                                FastNoiseLite::FractalType_PingPong, 7, 1.5, 0.4, 0.2};

    // Positional, and therefore only correct while it covers *every* field in
    // declaration order: a knob added mid-struct silently shifts everything after
    // it, and a knob added at the end is silently left at its default and never
    // round-tripped. Six were, which is how they reached `assets/config.yaml`
    // without anything reading them back.
    config.towns = {13, 555.5, 2, 6, 0.55, 3.5, 0.15, 0.25, 0.3, 0.75, 0.05,
                    9, 5, 2, 41, 8.5, 17.5, 0.8, 0.5, 4.5, 1.25,
                    2, 9, 1.9, 1.4, 9.5, 16.5, 2.5, 0.5, 123,
                    21.5, 3, 6, 5, 1, 3.5};
    config.roads = {19, 444.5, 4.5, 2.5, 3.5, 1.25, 0.95, 44.0, 3, 0.65, 0.5, 0.2, 4};
    config.regions = {7, 4, 9.5, 3.25, 31.0};
    config.landmarks = {29, 31, 0.71, 0.088, 0.52, 12, 3.75, 2, 7, 5, 4};
    config.caves = {11, 0.55, 410.5, 33.5, 11.5, 27.5, 315.0, 0.72, 44.5, 3, 0.66,
                    41.5, 1250.0, 0.24, 0.45, 0.35, 0.09, 0.31, 0.62, 4, 275, 7.5,
                    21.5, 0.17, 55.5, 3};

    // The ground, above and below the waterline. Positional like the four above, and correct
    // only while it covers every field of NaturalTerrainConfig in declaration order:
    // smoothing, hills, mountains, mountain_start, plateaus, plateau_coverage, warp, then the
    // bed's own seabed, seabed_ridges, seabed_shelf.
    config.terrain_model = TerrainModel::Natural;
    config.natural = {0.4, 0.07, 0.6, 0.31, 0.11, 0.42, 2.25, 0.055, 0.09, 0.07};
    config.noise_hills = {55, 0.25, FastNoiseLite::NoiseType_Perlin,
                          FastNoiseLite::FractalType_FBm, 4, 2.2, 0.45, 0.3};
    config.noise_ridge = {56, 0.19, FastNoiseLite::NoiseType_Perlin,
                          FastNoiseLite::FractalType_Ridged, 6, 1.8, 0.55, 0.65};
    config.noise_plateau = {57, 0.07, FastNoiseLite::NoiseType_Value,
                            FastNoiseLite::FractalType_FBm, 2, 2.4, 0.5, 0.1};
    config.noise_warp = {58, 0.31, FastNoiseLite::NoiseType_Perlin,
                         FastNoiseLite::FractalType_FBm, 5, 2.0, 0.6, 0.2};
    config.noise_seabed = {59, 1.3, FastNoiseLite::NoiseType_Perlin,
                           FastNoiseLite::FractalType_FBm, 4, 2.1, 0.4, 0.5};
    config.lake_depth_m = 9.5;
    config.lake_bed_grade = 0.21;
    config.lake_depth_max_m = 140.0;

    config.enable_water = false;
    config.enable_coast = false;
    config.enable_elevation = false;
    config.enable_temperature = false;
    config.enable_rivers = false;
    config.enable_valleys = false;
    config.enable_moisture = false;
    config.enable_biomes = false;
    config.enable_roads = false;
    config.enable_regions = false;
    config.enable_towns = false;
    config.enable_landmarks = false;
    config.enable_caves = false;
    config.enable_noisy_edges = false;

    // Derived last, from the grid and the scale above, so this fixture satisfies
    // the `image_size * meters_per_pixel == grid_size * meters_per_grid_unit`
    // invariant. A fixture that did not could not round-trip: the loader
    // reconciles the pair, so writing out a contradiction and reading it back
    // necessarily changes one of the two.
    config.image_size = derive_image_size(config);
    return config;
}

} // namespace

COOPA_TEST(load_config_reports_a_missing_file) {
    MapConfig config;
    config.grid_size = 64;   // A caller's own default, which must survive.
    config.river_count = 9;

    const std::string missing = (coopa::test::scratch_dir() / "no_such_config_file.yaml").string();
    const ConfigLoadResult result = load_config(missing, config);
    ASSERT_TRUE(result.status == ConfigLoad::NotFound);
    ASSERT_TRUE(!result.has_seed);
    ASSERT_TRUE(!result.message.empty());
    // Nothing applied, so the caller's defaults are intact rather than reset.
    ASSERT_EQ(config.grid_size, 64);
    ASSERT_EQ(config.river_count, 9);
}

COOPA_TEST(load_config_rejects_a_malformed_file) {
    MapConfig config;
    config.grid_size = 64;

    const auto dir = coopa::test::scratch_dir();
    const std::string broken =
        write_file((dir / "broken_config.yaml").string(), "seed: 42\n  bad indent: [\n");
    const ConfigLoadResult result = load_config(broken, config);

    ASSERT_TRUE(result.status == ConfigLoad::Malformed);
    ASSERT_TRUE(!result.message.empty());
    ASSERT_EQ(config.grid_size, 64);

    // An empty file parses as null, not as an empty mapping. Reported rather than taken as "no
    // keys set": a caller asked for this file by name.
    const std::string empty = write_file((dir / "empty_config.yaml").string(), "");
    const ConfigLoadResult from_empty = load_config(empty, config);
    ASSERT_TRUE(from_empty.status == ConfigLoad::Malformed);
}

COOPA_TEST(config_round_trips_every_field) {
    const MapConfig original = perturbed_config();

    const std::string path = (coopa::test::scratch_dir() / "full_config.yaml").string();
    {
        std::ofstream out(path);
        out << config_to_node(original);
    }

    // Loaded onto a *default* config, so any field the writer or the reader forgets
    // comes back as its default and fails below.
    MapConfig loaded;
    const ConfigLoadResult result = load_config(path, loaded);

    ASSERT_TRUE(result.status == ConfigLoad::Ok);
    ASSERT_TRUE(result.has_seed);

    ASSERT_EQ(loaded.grid_size, original.grid_size);
    ASSERT_TRUE(std::abs(loaded.jitter - original.jitter) < 1e-9);
    ASSERT_EQ(loaded.seed, original.seed);
    ASSERT_TRUE(std::abs(loaded.border_length - original.border_length) < 1e-9);
    // Both ends of the render scale, and the invariant tying them together.
    ASSERT_TRUE(std::abs(loaded.meters_per_pixel - original.meters_per_pixel) < 1e-9);
    ASSERT_EQ(loaded.image_size, original.image_size);
    ASSERT_EQ(loaded.image_size, derive_image_size(loaded));
    ASSERT_EQ(loaded.png_compression_level, original.png_compression_level);
    ASSERT_TRUE(std::abs(loaded.sea_level - original.sea_level) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.elevation_range_m - original.elevation_range_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_depth_m - original.river_depth_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_depth_per_volume_m
                         - original.river_depth_per_volume_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_channel_depth_m - original.river_channel_depth_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_channel_depth_per_volume_m
                         - original.river_channel_depth_per_volume_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_mouth_blend_m - original.river_mouth_blend_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_incision_m - original.river_incision_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_incision_per_volume_m
                         - original.river_incision_per_volume_m) < 1e-9);
    ASSERT_TRUE(loaded.river_valley_width == original.river_valley_width);
    ASSERT_TRUE(std::abs(loaded.river_valley_falloff - original.river_valley_falloff) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.water_edge_overlap_m - original.water_edge_overlap_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.lake_depth_m - original.lake_depth_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.lake_bed_grade - original.lake_bed_grade) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.lake_depth_max_m - original.lake_depth_max_m) < 1e-9);
    ASSERT_TRUE(loaded.terrain_model == original.terrain_model);
    // The natural model's landforms, land and bed alike.
    ASSERT_TRUE(std::abs(loaded.natural.smoothing - original.natural.smoothing) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.natural.hills - original.natural.hills) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.natural.mountains - original.natural.mountains) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.natural.mountain_start - original.natural.mountain_start) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.natural.plateaus - original.natural.plateaus) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.natural.plateau_coverage - original.natural.plateau_coverage) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.natural.warp - original.natural.warp) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.natural.seabed - original.natural.seabed) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.natural.seabed_ridges - original.natural.seabed_ridges) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.natural.seabed_shelf - original.natural.seabed_shelf) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.terrain_relief - original.terrain_relief) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.terrain_roughness - original.terrain_roughness) < 1e-9);
    ASSERT_EQ(loaded.caves.cave_count, original.caves.cave_count);
    ASSERT_TRUE(std::abs(loaded.caves.min_grade - original.caves.min_grade) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.min_spacing_m - original.caves.min_spacing_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.roof_clearance_m - original.caves.roof_clearance_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.passage_height_m - original.caves.passage_height_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.chamber_height_m - original.caves.chamber_height_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.max_depth_m - original.caves.max_depth_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.vadose_share - original.caves.vadose_share) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.level_spacing_m - original.caves.level_spacing_m) < 1e-9);
    ASSERT_EQ(loaded.caves.max_levels, original.caves.max_levels);
    ASSERT_TRUE(std::abs(loaded.caves.level_budget - original.caves.level_budget) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.step_m - original.caves.step_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.passage_length_m - original.caves.passage_length_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.descent_grade - original.caves.descent_grade) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.massif_bias - original.caves.massif_bias) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.meander - original.caves.meander) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.branch_chance_vadose
                         - original.caves.branch_chance_vadose) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.branch_chance_phreatic
                         - original.caves.branch_chance_phreatic) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.branch_budget - original.caves.branch_budget) < 1e-9);
    ASSERT_EQ(loaded.caves.max_branches, original.caves.max_branches);
    ASSERT_EQ(loaded.caves.max_nodes, original.caves.max_nodes);
    ASSERT_TRUE(std::abs(loaded.caves.passage_width_m - original.caves.passage_width_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.chamber_radius_m - original.caves.chamber_radius_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.shaft_chance - original.caves.shaft_chance) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.caves.shaft_drop_m - original.caves.shaft_drop_m) < 1e-9);
    ASSERT_EQ(loaded.caves.smoothing_iterations, original.caves.smoothing_iterations);
    ASSERT_TRUE(loaded.enable_caves == original.enable_caves);
    ASSERT_EQ(loaded.noise_cave.seed, original.noise_cave.seed);
    ASSERT_TRUE(std::abs(loaded.noise_cave.frequency - original.noise_cave.frequency) < 1e-9);
    ASSERT_TRUE(loaded.shape.shape == original.shape.shape);
    ASSERT_TRUE(std::abs(loaded.shape.width_m - original.shape.width_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.height_m - original.shape.height_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.diameter_m - original.shape.diameter_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.edge_length_m - original.shape.edge_length_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.rotation - original.shape.rotation) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.continent_size_m - original.shape.continent_size_m) < 1e-9);
    ASSERT_TRUE(loaded.shape.continent_count == original.shape.continent_count);
    ASSERT_TRUE(std::abs(loaded.shape.irregularity - original.shape.irregularity) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.size_variance - original.shape.size_variance) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.coast_detail - original.shape.coast_detail) < 1e-9);
    ASSERT_TRUE(loaded.noise_shape.seed == original.noise_shape.seed);
    ASSERT_TRUE(std::abs(loaded.noise_shape.frequency - original.noise_shape.frequency) < 1e-9);
    ASSERT_TRUE(loaded.noise_shape.type == original.noise_shape.type);
    ASSERT_TRUE(loaded.show_regions == original.show_regions);
    ASSERT_TRUE(loaded.draw_landmark_marks == original.draw_landmark_marks);
    ASSERT_TRUE(loaded.composite_shading == original.composite_shading);
    ASSERT_TRUE(loaded.elevation_surface == original.elevation_surface);
    ASSERT_TRUE(std::abs(loaded.elevation_blend - original.elevation_blend) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.region_tint - original.region_tint) < 1e-6f);
    ASSERT_TRUE(std::abs(loaded.temperature_lapse_rate - original.temperature_lapse_rate) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.temperature_falloff - original.temperature_falloff) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.temperature_offset - original.temperature_offset) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.polar_extent_north - original.polar_extent_north) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.polar_extent_south - original.polar_extent_south) < 1e-9);
    ASSERT_EQ(loaded.elevation_smoothing_iterations, original.elevation_smoothing_iterations);
    ASSERT_TRUE(std::abs(loaded.elevation_smoothing_strength
                         - original.elevation_smoothing_strength) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.threshold_water - original.threshold_water) < 1e-9);
    ASSERT_EQ(loaded.threshold_water_count, original.threshold_water_count);
    ASSERT_EQ(loaded.river_count, original.river_count);
    ASSERT_TRUE(std::abs(loaded.river_width_base_m - original.river_width_base_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_width_per_volume_m - original.river_width_per_volume_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.trail_width_m - original.trail_width_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.road_width_m - original.road_width_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.highway_width_m - original.highway_width_m) < 1e-9);
    ASSERT_TRUE(loaded.subdivide_noisy_edges == original.subdivide_noisy_edges);

    // Both noise fields, which is the reason they share one pair of helpers: a
    // second hand-written copy is what falls behind.
    const NoiseConfig* noises[4][2] = {
        {&loaded.noise_island, &original.noise_island},
        {&loaded.noise_temperature, &original.noise_temperature},
        {&loaded.noise_relief, &original.noise_relief},
        {&loaded.noise_terrain, &original.noise_terrain},
    };
    for (const auto& pair : noises) {
        ASSERT_EQ(pair[0]->seed, pair[1]->seed);
        ASSERT_TRUE(std::abs(pair[0]->frequency - pair[1]->frequency) < 1e-9);
        ASSERT_TRUE(pair[0]->type == pair[1]->type);
        ASSERT_TRUE(pair[0]->fractal_type == pair[1]->fractal_type);
        ASSERT_EQ(pair[0]->octaves, pair[1]->octaves);
        ASSERT_TRUE(std::abs(pair[0]->lacunarity - pair[1]->lacunarity) < 1e-9);
        ASSERT_TRUE(std::abs(pair[0]->gain - pair[1]->gain) < 1e-9);
        ASSERT_TRUE(std::abs(pair[0]->weighted_strength - pair[1]->weighted_strength) < 1e-9);
    }

    ASSERT_EQ(loaded.towns.town_count, original.towns.town_count);
    ASSERT_TRUE(std::abs(loaded.towns.min_spacing_m - original.towns.min_spacing_m) < 1e-9);
    ASSERT_EQ(loaded.towns.capital_count, original.towns.capital_count);
    ASSERT_EQ(loaded.towns.town_tier_count, original.towns.town_tier_count);
    ASSERT_EQ(loaded.towns.buildings_per_town, original.towns.buildings_per_town);
    ASSERT_TRUE(std::abs(loaded.towns.building_size_min_m - original.towns.building_size_min_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.towns.building_size_max_m - original.towns.building_size_max_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.towns.water_clearance_m - original.towns.water_clearance_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.towns.street_clearance_m - original.towns.street_clearance_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.towns.street_width_m - original.towns.street_width_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.towns.plaza_radius_m - original.towns.plaza_radius_m) < 1e-9);
    ASSERT_EQ(loaded.towns.plaza_min_cells, original.towns.plaza_min_cells);
    ASSERT_EQ(loaded.towns.capital_civic_count, original.towns.capital_civic_count);
    ASSERT_EQ(loaded.towns.town_civic_count, original.towns.town_civic_count);
    ASSERT_EQ(loaded.towns.village_civic_count, original.towns.village_civic_count);
    ASSERT_EQ(loaded.towns.household_size_min, original.towns.household_size_min);
    ASSERT_EQ(loaded.towns.household_size_max, original.towns.household_size_max);
    ASSERT_EQ(loaded.towns.infill_attempts, original.towns.infill_attempts);

    ASSERT_EQ(loaded.roads.hub_count, original.roads.hub_count);
    ASSERT_TRUE(std::abs(loaded.roads.hub_min_spacing_m - original.roads.hub_min_spacing_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.roads.slope_cost - original.roads.slope_cost) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.roads.ford_cost - original.roads.ford_cost) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.roads.water_crossing_cost
                         - original.roads.water_crossing_cost) < 1e-9);
    ASSERT_EQ(loaded.roads.max_water_span, original.roads.max_water_span);
    ASSERT_TRUE(std::abs(loaded.roads.reuse_discount - original.roads.reuse_discount) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.roads.highway_traffic_share
                         - original.roads.highway_traffic_share) < 1e-9);
    ASSERT_EQ(loaded.roads.smoothing_iterations, original.roads.smoothing_iterations);

    // The two blocks that were missing entirely before configuration files existed.
    ASSERT_EQ(loaded.regions.country_count, original.regions.country_count);
    ASSERT_EQ(loaded.regions.regions_per_country, original.regions.regions_per_country);
    ASSERT_TRUE(std::abs(loaded.regions.min_country_spacing
                         - original.regions.min_country_spacing) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.regions.elevation_cost - original.regions.elevation_cost) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.regions.water_crossing_cost
                         - original.regions.water_crossing_cost) < 1e-9);

    ASSERT_EQ(loaded.landmarks.max_natural, original.landmarks.max_natural);
    ASSERT_EQ(loaded.landmarks.max_abandoned, original.landmarks.max_abandoned);
    ASSERT_TRUE(std::abs(loaded.landmarks.peak_elevation
                         - original.landmarks.peak_elevation) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.landmarks.waterfall_drop
                         - original.landmarks.waterfall_drop) < 1e-9);
    ASSERT_EQ(loaded.landmarks.great_lake_cells, original.landmarks.great_lake_cells);
    ASSERT_EQ(loaded.landmarks.kind_share_denominator, original.landmarks.kind_share_denominator);
    ASSERT_EQ(loaded.landmarks.cape_ocean_ratio_numerator,
              original.landmarks.cape_ocean_ratio_numerator);

    // Every pass toggle survives the round trip.
    ASSERT_TRUE(loaded.enable_water == original.enable_water);
    ASSERT_TRUE(loaded.enable_coast == original.enable_coast);
    ASSERT_TRUE(loaded.enable_elevation == original.enable_elevation);
    ASSERT_TRUE(loaded.enable_temperature == original.enable_temperature);
    ASSERT_TRUE(loaded.enable_rivers == original.enable_rivers);
    ASSERT_TRUE(loaded.enable_valleys == original.enable_valleys);
    ASSERT_TRUE(loaded.enable_moisture == original.enable_moisture);
    ASSERT_TRUE(loaded.enable_biomes == original.enable_biomes);
    ASSERT_TRUE(loaded.enable_roads == original.enable_roads);
    ASSERT_TRUE(loaded.enable_regions == original.enable_regions);
    ASSERT_TRUE(loaded.enable_towns == original.enable_towns);
    ASSERT_TRUE(loaded.enable_landmarks == original.enable_landmarks);
    ASSERT_TRUE(loaded.enable_noisy_edges == original.enable_noisy_edges);
}

COOPA_TEST(load_config_overrides_only_what_it_names) {
    MapConfig config;
    config.grid_size = 64;
    config.river_count = 9;
    config.roads.hub_count = 5;

    // An absent key leaves the caller's value alone; that is what makes a config file an
    // override rather than a replacement, and what lets the generator set its own scene
    // defaults first.
    const std::string path = write_file((coopa::test::scratch_dir() / "partial_config.yaml").string(),
                                        "grid_size: 12\nnot_a_real_key: 7\n");
    const ConfigLoadResult result = load_config(path, config);

    ASSERT_TRUE(result.status == ConfigLoad::Ok);
    ASSERT_TRUE(!result.has_seed);   // Never named, so a caller may still draw one.
    ASSERT_EQ(config.grid_size, 12);
    ASSERT_EQ(config.river_count, 9);
    ASSERT_EQ(config.roads.hub_count, 5);
}

/**
 * @brief The shipped assets/config.yaml loads, carries a seed, and ships with every pass on.
 *
 * It is what the `mapcoopa` tool reads by default, so a file that stopped parsing would make
 * the tool refuse to run, and shipping with a pass turned off would silently produce a map
 * missing a whole feature -- with a symptom that looks like a bug in the pass.
 */
COOPA_TEST(shipped_config_loads_with_every_pass_enabled) {
    MapConfig config;
    const ConfigLoadResult result =
        load_config(std::string(ROOT_DIR) + "/assets/config.yaml", config);

    ASSERT_TRUE(result.status == ConfigLoad::Ok);
    ASSERT_TRUE(result.has_seed);
    // The file omits image_size deliberately; the loader derives it from the scale.
    ASSERT_EQ(config.image_size, derive_image_size(config));

    ASSERT_TRUE(config.enable_water && config.enable_coast && config.enable_elevation);
    ASSERT_TRUE(config.enable_temperature && config.enable_rivers && config.enable_moisture);
    ASSERT_TRUE(config.enable_valleys);
    ASSERT_TRUE(config.enable_biomes && config.enable_roads && config.enable_regions);
    ASSERT_TRUE(config.enable_towns && config.enable_landmarks && config.enable_noisy_edges);
    ASSERT_TRUE(config.enable_caves);
}

/**
 * @brief `image_size` is an input, and setting it cannot leave the scale stale.
 *
 * The two render-scale fields are one knob with two ends, and the invariant
 * `image_size * meters_per_pixel == grid_size * meters_per_grid_unit` has to hold however a
 * caller arrives at it -- every feature is stroked in metres and converted through the scale, so
 * a contradictory pair draws features at a width the resolution does not agree with.
 *
 * `image_size: 2048` in a configuration file is honoured, not read and thrown away, and
 * honouring it means back-computing `meters_per_pixel`.
 */
COOPA_TEST(config_image_size_sets_the_scale) {
    const std::string path = (coopa::test::scratch_dir() / "image_size.yaml").string();

    // 1. A document naming image_size is honoured, and the scale follows it.
    //    40 cells x 60 m = 2400 m of world in 600 px is 4 m to the pixel.
    write_file(path, "grid_size: 40\nimage_size: 600\n");
    MapConfig from_pixels;
    ASSERT_TRUE(load_config(path, from_pixels).status == ConfigLoad::Ok);
    ASSERT_EQ(from_pixels.image_size, 600);
    ASSERT_TRUE(std::abs(from_pixels.meters_per_pixel - 4.0) < 1e-12);
    ASSERT_EQ(from_pixels.image_size, derive_image_size(from_pixels));

    // 2. The other end: naming the scale sizes the render.
    write_file(path, "grid_size: 40\nmeters_per_pixel: 3\n");
    MapConfig from_scale;
    ASSERT_TRUE(load_config(path, from_scale).status == ConfigLoad::Ok);
    ASSERT_EQ(from_scale.image_size, 800);
    ASSERT_TRUE(std::abs(from_scale.meters_per_pixel - 3.0) < 1e-12);

    // 3. Both given and disagreeing: image_size wins, being the more concrete statement of
    //    intent, and the scale is corrected rather than kept.
    write_file(path, "grid_size: 40\nimage_size: 1200\nmeters_per_pixel: 37\n");
    MapConfig both;
    ASSERT_TRUE(load_config(path, both).status == ConfigLoad::Ok);
    ASSERT_EQ(both.image_size, 1200);
    ASSERT_TRUE(std::abs(both.meters_per_pixel - 2.0) < 1e-12);
    ASSERT_EQ(both.image_size, derive_image_size(both));

    // 4. Neither given: meters_per_pixel is 1.0, always, so a render is a one-pixel-per-metre
    //    map and a pixel count off it is a measurement.
    write_file(path, "grid_size: 40\n");
    MapConfig neither;
    ASSERT_TRUE(load_config(path, neither).status == ConfigLoad::Ok);
    ASSERT_TRUE(std::abs(neither.meters_per_pixel - 1.0) < 1e-12);
    ASSERT_EQ(neither.image_size, 2400);

    // 5. A default-constructed config already satisfies the invariant, rather than starting out
    //    contradicting itself.
    const MapConfig fresh;
    ASSERT_TRUE(std::abs(fresh.meters_per_pixel - 1.0) < 1e-12);
    ASSERT_EQ(fresh.image_size, derive_image_size(fresh));

    // 6. And the renderer takes ONE scale: a resolution set either way round scales markers and
    //    roads together. Two configs describing the same world at the same resolution must
    //    render identically, whichever end they were written from.
    MapConfig by_pixels = small_config(5);
    set_render_size(by_pixels, 160);
    MapConfig by_scale = small_config(5);
    by_scale.meters_per_pixel =
        static_cast<double>(by_scale.grid_size) * by_scale.meters_per_grid_unit / 160.0;
    by_scale.image_size = derive_image_size(by_scale);
    ASSERT_EQ(by_pixels.image_size, by_scale.image_size);

    MapGenerator generator(by_pixels, maps_logger());
    generator.generate();
    for (std::size_t i = 0; i < k_map_layer_count; ++i) {
        const MapLayer layer = static_cast<MapLayer>(i);
        const Image a = MapLayers::render(layer, generator.graph(), by_pixels);
        const Image b = MapLayers::render(layer, generator.graph(), by_scale);
        ASSERT_EQ(a.pixels.size(), b.pixels.size());
        ASSERT_TRUE(a.pixels == b.pixels);
    }
}

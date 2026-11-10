/**
 * @file map_yaml.h
 * @brief Serialises a generated map to YAML and reads it back.
 */

#ifndef COOPA_MAPS_MAP_YAML_H
#define COOPA_MAPS_MAP_YAML_H

#include <cstddef>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

#include <fkYAML/node.hpp>
#include <coopa/yaml/document.h>
#include <glm/glm.hpp>

#include <coopa/collections/yaml_map.h>
#include <coopa/maps/biome.h>
#include <coopa/maps/landmark.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @brief Version stamped into every saved map.
 *
 * Written as `version:` at the document root so a loader can recognise a
 * schema it predates instead of silently misreading it.
 */
inline constexpr int k_map_yaml_version = 1;

/**
 * @brief Pixels per grid unit assumed when converting legacy pixel-valued widths.
 *
 * The old defaults rendered a 40-unit grid at 1024 pixels.
 */
inline constexpr double k_legacy_pixels_per_grid_unit = 1024.0 / 40.0;

namespace detail {

/** @brief Builds a YAML sequence node from a list of integer ids. */
template <typename IdType>
inline fkyaml::node id_sequence(const std::vector<IdType>& ids) {
    std::vector<fkyaml::node> nodes;
    nodes.reserve(ids.size());
    for (const IdType id : ids) {
        nodes.push_back(static_cast<int>(id));
    }
    return fkyaml::node::sequence(std::move(nodes));
}

/** @brief Reads a YAML sequence of integers back into a list of ids. */
template <typename IdType>
inline std::vector<IdType> read_id_sequence(const fkyaml::node& parent, const char* key) {
    std::vector<IdType> ids;
    if (!parent.contains(key)) {
        return ids;
    }
    const fkyaml::node& sequence = parent.at(key);
    if (!sequence.is_sequence()) {
        return ids;
    }
    ids.reserve(sequence.size());
    for (const fkyaml::node& item : sequence) {
        ids.push_back(static_cast<IdType>(item.get_value<int>()));
    }
    return ids;
}

/**
 * @brief Builds a YAML sequence node from a list of doubles.
 *
 * The scalar counterpart of `point_sequence()`, for the per-point arrays a cave
 * passage carries alongside its centreline.
 */
inline fkyaml::node scalar_sequence(const std::vector<double>& values) {
    std::vector<fkyaml::node> nodes;
    nodes.reserve(values.size());
    for (const double value : values) {
        nodes.push_back(fkyaml::node(value));
    }
    return fkyaml::node::sequence(std::move(nodes));
}

/** @brief Reads a YAML sequence of numbers back into a list of doubles. */
inline std::vector<double> read_scalar_sequence(const fkyaml::node& parent, const char* key) {
    std::vector<double> values;
    if (!parent.contains(key)) {
        return values;
    }
    const fkyaml::node& sequence = parent.at(key);
    if (!sequence.is_sequence()) {
        return values;
    }
    values.reserve(sequence.size());
    for (const fkyaml::node& item : sequence) {
        values.push_back(item.get_value<double>());
    }
    return values;
}

/** @brief Builds a YAML sequence of `[x, y]` pairs from a polyline. */
inline fkyaml::node point_sequence(const std::vector<MapPoint>& points) {
    std::vector<fkyaml::node> nodes;
    nodes.reserve(points.size());
    for (const MapPoint& point : points) {
        if (point.z != 0.0) {   // sphere maps only
            nodes.push_back(fkyaml::node::sequence({fkyaml::node(point.x), fkyaml::node(point.y), fkyaml::node(point.z)}));
        } else {
            nodes.push_back(fkyaml::node::sequence({fkyaml::node(point.x), fkyaml::node(point.y)}));
        }
    }
    return fkyaml::node::sequence(std::move(nodes));
}

/** @brief Reads a YAML sequence of `[x, y]` pairs back into a polyline. */
inline std::vector<MapPoint> read_point_sequence(const fkyaml::node& parent, const char* key) {
    std::vector<MapPoint> points;
    if (!parent.contains(key)) {
        return points;
    }
    const fkyaml::node& sequence = parent.at(key);
    if (!sequence.is_sequence()) {
        return points;
    }
    points.reserve(sequence.size());
    for (const fkyaml::node& pair : sequence) {
        if (pair.is_sequence() && pair.size() >= 2) {
            points.push_back({pair[0].get_value<double>(), pair[1].get_value<double>(),
                              pair.size() >= 3 ? pair[2].get_value<double>() : 0.0});
        }
    }
    return points;
}

/**
 * @brief Reads a key if present and convertible, otherwise returns the fallback.
 *
 * Tolerates a null node, which is what an empty string round-trips to: fkYAML
 * emits `key:` with nothing after it and parses that back as null, not as "".
 * Without this guard one unnamed place would abort a whole map load.
 */
template <typename T>
inline T read_or(const fkyaml::node& node, const char* key, T fallback) {
    if (!node.is_mapping() || !node.contains(key)) {
        return fallback;
    }
    const fkyaml::node& value = node.at(key);
    if (value.is_null()) {
        return fallback;
    }
    try {
        return value.get_value<T>();
    } catch (const std::exception&) {
        return fallback;
    }
}

/**
 * @brief Encodes a `NoiseConfig` as a mapping node.
 *
 * Factored out because `MapConfig` carries two noise fields -- the island field
 * and the temperature variation field -- and eight keys written twice by hand is
 * eight chances for the second copy to fall behind the first.
 *
 * @param noise The field parameters to encode.
 * @return A mapping node holding all eight parameters.
 */
inline fkyaml::node noise_to_node(const NoiseConfig& noise) {
    fkyaml::node node = fkyaml::node::mapping();
    node["seed"] = noise.seed;
    node["frequency"] = noise.frequency;
    node["type"] = static_cast<int>(noise.type);
    node["fractal_type"] = static_cast<int>(noise.fractal_type);
    node["octaves"] = noise.octaves;
    node["lacunarity"] = noise.lacunarity;
    node["gain"] = noise.gain;
    node["weighted_strength"] = noise.weighted_strength;
    return node;
}

/**
 * @brief Reads a `NoiseConfig` back from a parent mapping, if the key is present.
 *
 * The two enum-typed fields go through `int`: they are FastNoiseLite's own enums
 * and the document stores the underlying value, not a name.
 *
 * @param parent The mapping that may contain the block.
 * @param key The block's key, e.g. `"noise_island"`.
 * @param noise Updated in place; any absent key keeps its current value.
 */
inline void noise_from_node(const fkyaml::node& parent, const char* key, NoiseConfig& noise) {
    if (!parent.is_mapping() || !parent.contains(key)) {
        return;
    }
    const fkyaml::node& node = parent.at(key);
    noise.seed = read_or(node, "seed", noise.seed);
    noise.frequency = read_or(node, "frequency", noise.frequency);
    noise.type = static_cast<FastNoiseLite::NoiseType>(
        read_or(node, "type", static_cast<int>(noise.type)));
    noise.fractal_type = static_cast<FastNoiseLite::FractalType>(
        read_or(node, "fractal_type", static_cast<int>(noise.fractal_type)));
    noise.octaves = read_or(node, "octaves", noise.octaves);
    noise.lacunarity = read_or(node, "lacunarity", noise.lacunarity);
    noise.gain = read_or(node, "gain", noise.gain);
    noise.weighted_strength = read_or(node, "weighted_strength", noise.weighted_strength);
}

} // namespace detail

/**
 * @brief Serialises a `MapConfig` to a YAML mapping node.
 * @param config The configuration to encode.
 * @return A mapping node holding every generation parameter.
 */
inline fkyaml::node config_to_node(const MapConfig& config) {
    fkyaml::node towns = fkyaml::node::mapping();
    towns["town_count"] = config.towns.town_count;
    towns["min_spacing_m"] = config.towns.min_spacing_m;
    towns["capital_count"] = config.towns.capital_count;
    towns["town_tier_count"] = config.towns.town_tier_count;
    towns["capital_cells"] = config.towns.capital_cells;
    towns["town_cells"] = config.towns.town_cells;
    towns["village_cells"] = config.towns.village_cells;
    towns["buildings_per_town"] = config.towns.buildings_per_town;
    towns["building_size_min_m"] = config.towns.building_size_min_m;
    towns["building_size_max_m"] = config.towns.building_size_max_m;
    towns["town_building_scale"] = config.towns.town_building_scale;
    towns["village_building_scale"] = config.towns.village_building_scale;
    towns["water_clearance_m"] = config.towns.water_clearance_m;
    towns["street_clearance_m"] = config.towns.street_clearance_m;
    towns["street_width_m"] = config.towns.street_width_m;
    towns["plaza_radius_m"] = config.towns.plaza_radius_m;
    towns["plaza_min_cells"] = config.towns.plaza_min_cells;
    towns["capital_civic_count"] = config.towns.capital_civic_count;
    towns["town_civic_count"] = config.towns.town_civic_count;
    towns["village_civic_count"] = config.towns.village_civic_count;
    towns["household_size_min"] = config.towns.household_size_min;
    towns["household_size_max"] = config.towns.household_size_max;
    towns["capital_density"] = config.towns.capital_density;
    towns["town_density"] = config.towns.town_density;
    towns["street_offset_m"] = config.towns.street_offset_m;
    towns["street_spacing_m"] = config.towns.street_spacing_m;
    towns["position_jitter_m"] = config.towns.position_jitter_m;
    towns["rotation_jitter"] = config.towns.rotation_jitter;
    towns["infill_attempts"] = config.towns.infill_attempts;

    fkyaml::node regions = fkyaml::node::mapping();
    regions["country_count"] = config.regions.country_count;
    regions["regions_per_country"] = config.regions.regions_per_country;
    regions["min_country_spacing"] = config.regions.min_country_spacing;
    regions["elevation_cost"] = config.regions.elevation_cost;
    regions["water_crossing_cost"] = config.regions.water_crossing_cost;

    fkyaml::node landmarks = fkyaml::node::mapping();
    landmarks["max_natural"] = config.landmarks.max_natural;
    landmarks["max_abandoned"] = config.landmarks.max_abandoned;
    landmarks["peak_elevation"] = config.landmarks.peak_elevation;
    landmarks["waterfall_drop"] = config.landmarks.waterfall_drop;
    landmarks["canyon_elevation"] = config.landmarks.canyon_elevation;
    landmarks["great_lake_cells"] = static_cast<int>(config.landmarks.great_lake_cells);
    landmarks["town_clearance"] = config.landmarks.town_clearance;
    landmarks["kind_share_numerator"] = config.landmarks.kind_share_numerator;
    landmarks["kind_share_denominator"] = config.landmarks.kind_share_denominator;
    landmarks["cape_ocean_ratio_numerator"] = config.landmarks.cape_ocean_ratio_numerator;
    landmarks["cape_ocean_ratio_denominator"] = config.landmarks.cape_ocean_ratio_denominator;

    // The pass toggles live under one key rather than loose at the root: there are
    // fourteen of them, they are the coarsest thing in the file, and grouping them
    // is what lets a reader see the pipeline at a glance.
    fkyaml::node passes = fkyaml::node::mapping();
    passes["enable_water"] = config.enable_water;
    passes["enable_coast"] = config.enable_coast;
    passes["enable_elevation"] = config.enable_elevation;
    passes["enable_temperature"] = config.enable_temperature;
    passes["enable_rivers"] = config.enable_rivers;
    passes["enable_valleys"] = config.enable_valleys;
    passes["enable_moisture"] = config.enable_moisture;
    passes["enable_biomes"] = config.enable_biomes;
    passes["enable_roads"] = config.enable_roads;
    passes["enable_regions"] = config.enable_regions;
    passes["enable_towns"] = config.enable_towns;
    passes["enable_landmarks"] = config.enable_landmarks;
    passes["enable_caves"] = config.enable_caves;
    passes["enable_noisy_edges"] = config.enable_noisy_edges;

    fkyaml::node roads = fkyaml::node::mapping();
    roads["hub_count"] = config.roads.hub_count;
    roads["hub_min_spacing_m"] = config.roads.hub_min_spacing_m;
    roads["slope_cost"] = config.roads.slope_cost;
    roads["elevation_cost"] = config.roads.elevation_cost;
    roads["rough_ground_cost"] = config.roads.rough_ground_cost;
    roads["ford_cost"] = config.roads.ford_cost;
    roads["bridge_cost_per_volume"] = config.roads.bridge_cost_per_volume;
    roads["water_crossing_cost"] = config.roads.water_crossing_cost;
    roads["max_water_span"] = config.roads.max_water_span;
    roads["reuse_discount"] = config.roads.reuse_discount;
    roads["highway_traffic_share"] = config.roads.highway_traffic_share;
    roads["road_traffic_share"] = config.roads.road_traffic_share;
    roads["smoothing_iterations"] = config.roads.smoothing_iterations;

    fkyaml::node node = fkyaml::node::mapping();
    node["seed"] = config.seed;
    node["grid_size"] = config.grid_size;
    if (config.topology == MapTopology::Sphere) {   // written only for a globe: plane files are unchanged
        node["topology"] = std::string(map_topology_name(config.topology));
        node["sphere_cells"] = config.sphere_cells;
    }
    node["jitter"] = config.jitter;
    node["meters_per_grid_unit"] = config.meters_per_grid_unit;
    node["elevation_range_m"] = config.elevation_range_m;
    node["meters_per_pixel"] = config.meters_per_pixel;
    node["composite_shading"] = std::string(composite_shading_name(config.composite_shading));
    node["elevation_surface"] = std::string(elevation_surface_name(config.elevation_surface));
    node["elevation_blend"] = config.elevation_blend;
    node["elevation_blend_variation"] = config.elevation_blend_variation;
    node["image_size"] = config.image_size;
    node["png_compression_level"] = config.png_compression_level;
    node["border_length"] = config.border_length;
    node["sea_level"] = config.sea_level;
    node["terrain_relief"] = config.terrain_relief;
    node["terrain_roughness"] = config.terrain_roughness;
    node["threshold_water"] = config.threshold_water;
    node["threshold_water_count"] = config.threshold_water_count;
    node["river_count"] = config.river_count;
    node["river_min_length"] = config.river_min_length;
    node["river_source_min_elevation"] = config.river_source_min_elevation;
    node["river_source_max_elevation"] = config.river_source_max_elevation;
    node["river_smoothing_iterations"] = config.river_smoothing_iterations;
    node["river_width_base_m"] = config.river_width_base_m;
    node["river_width_per_volume_m"] = config.river_width_per_volume_m;
    node["river_depth_m"] = config.river_depth_m;
    node["river_depth_per_volume_m"] = config.river_depth_per_volume_m;
    node["river_channel_depth_m"] = config.river_channel_depth_m;
    node["river_channel_depth_per_volume_m"] = config.river_channel_depth_per_volume_m;
    node["river_mouth_blend_m"] = config.river_mouth_blend_m;
    node["river_incision_m"] = config.river_incision_m;
    node["river_incision_per_volume_m"] = config.river_incision_per_volume_m;
    node["river_valley_width"] = config.river_valley_width;
    node["river_valley_falloff"] = config.river_valley_falloff;
    node["water_edge_overlap_m"] = config.water_edge_overlap_m;
    node["lake_depth_m"] = config.lake_depth_m;
    node["lake_bed_grade"] = config.lake_bed_grade;
    node["lake_depth_max_m"] = config.lake_depth_max_m;
    node["terrain_model"] = std::string(terrain_model_name(config.terrain_model));
    {
        fkyaml::node natural = fkyaml::node::mapping();
        natural["smoothing"] = config.natural.smoothing;
        natural["hills"] = config.natural.hills;
        natural["mountains"] = config.natural.mountains;
        natural["mountain_start"] = config.natural.mountain_start;
        natural["plateaus"] = config.natural.plateaus;
        natural["plateau_coverage"] = config.natural.plateau_coverage;
        natural["warp"] = config.natural.warp;
        natural["seabed"] = config.natural.seabed;
        natural["seabed_ridges"] = config.natural.seabed_ridges;
        natural["seabed_shelf"] = config.natural.seabed_shelf;
        node["natural"] = std::move(natural);
    }
    node["noise_hills"] = detail::noise_to_node(config.noise_hills);
    node["noise_ridge"] = detail::noise_to_node(config.noise_ridge);
    node["noise_plateau"] = detail::noise_to_node(config.noise_plateau);
    node["noise_warp"] = detail::noise_to_node(config.noise_warp);
    node["noise_seabed"] = detail::noise_to_node(config.noise_seabed);
    node["trail_width_m"] = config.trail_width_m;
    node["road_width_m"] = config.road_width_m;
    node["highway_width_m"] = config.highway_width_m;
    node["temperature_lapse_rate"] = config.temperature_lapse_rate;
    node["temperature_falloff"] = config.temperature_falloff;
    node["temperature_offset"] = config.temperature_offset;
    node["polar_extent_north"] = config.polar_extent_north;
    node["polar_extent_south"] = config.polar_extent_south;
    node["elevation_smoothing_iterations"] = config.elevation_smoothing_iterations;
    node["elevation_smoothing_strength"] = config.elevation_smoothing_strength;
    node["subdivide_noisy_edges"] = config.subdivide_noisy_edges;
    node["show_regions"] = config.show_regions;
    node["draw_landmark_marks"] = config.draw_landmark_marks;
    node["region_tint"] = config.region_tint;
    node["noise_island"] = detail::noise_to_node(config.noise_island);
    node["noise_temperature"] = detail::noise_to_node(config.noise_temperature);
    node["noise_relief"] = detail::noise_to_node(config.noise_relief);
    node["noise_terrain"] = detail::noise_to_node(config.noise_terrain);
    node["noise_shape"] = detail::noise_to_node(config.noise_shape);
    node["noise_blend"] = detail::noise_to_node(config.noise_blend);
    node["noise_cave"] = detail::noise_to_node(config.noise_cave);
    fkyaml::node shape = fkyaml::node::mapping();
    shape["shape"] = std::string(map_shape_name(config.shape.shape));
    shape["width_m"] = config.shape.width_m;
    shape["height_m"] = config.shape.height_m;
    shape["diameter_m"] = config.shape.diameter_m;
    shape["edge_length_m"] = config.shape.edge_length_m;
    shape["rotation"] = config.shape.rotation;
    shape["continent_size_m"] = config.shape.continent_size_m;
    shape["continent_count"] = config.shape.continent_count;
    shape["irregularity"] = config.shape.irregularity;
    shape["size_variance"] = config.shape.size_variance;
    shape["coast_detail"] = config.shape.coast_detail;

    node["shape"] = std::move(shape);
    node["towns"] = std::move(towns);
    node["roads"] = std::move(roads);
    node["regions"] = std::move(regions);
    node["landmarks"] = std::move(landmarks);

    fkyaml::node caves = fkyaml::node::mapping();
    caves["cave_count"] = config.caves.cave_count;
    caves["min_grade"] = config.caves.min_grade;
    caves["min_spacing_m"] = config.caves.min_spacing_m;
    caves["roof_clearance_m"] = config.caves.roof_clearance_m;
    caves["passage_height_m"] = config.caves.passage_height_m;
    caves["chamber_height_m"] = config.caves.chamber_height_m;
    caves["max_depth_m"] = config.caves.max_depth_m;
    caves["vadose_share"] = config.caves.vadose_share;
    caves["level_spacing_m"] = config.caves.level_spacing_m;
    caves["max_levels"] = config.caves.max_levels;
    caves["level_budget"] = config.caves.level_budget;
    caves["step_m"] = config.caves.step_m;
    caves["passage_length_m"] = config.caves.passage_length_m;
    caves["descent_grade"] = config.caves.descent_grade;
    caves["massif_bias"] = config.caves.massif_bias;
    caves["meander"] = config.caves.meander;
    caves["branch_chance_vadose"] = config.caves.branch_chance_vadose;
    caves["branch_chance_phreatic"] = config.caves.branch_chance_phreatic;
    caves["branch_budget"] = config.caves.branch_budget;
    caves["max_branches"] = config.caves.max_branches;
    caves["max_nodes"] = config.caves.max_nodes;
    caves["passage_width_m"] = config.caves.passage_width_m;
    caves["chamber_radius_m"] = config.caves.chamber_radius_m;
    caves["shaft_chance"] = config.caves.shaft_chance;
    caves["shaft_drop_m"] = config.caves.shaft_drop_m;
    caves["smoothing_iterations"] = config.caves.smoothing_iterations;
    node["caves"] = std::move(caves);
    node["passes"] = std::move(passes);
    return node;
}

/**
 * @brief Applies a YAML mapping node on top of an existing `MapConfig`.
 *
 * Every key is optional; one that is absent leaves the corresponding field
 * exactly as it was. That is what makes a configuration file *override* a
 * caller's defaults rather than replace them wholesale, and it is also why a map
 * saved before a parameter existed still loads.
 *
 * The one field that does not simply pass through is `MapConfig::image_size`,
 * which is reconciled against `meters_per_pixel` at the end: naming either one
 * sets the other, so the pair cannot leave here disagreeing about how much
 * ground a pixel covers. See `MapConfig::image_size`.
 *
 * @param node The mapping node to decode; a non-mapping node applies nothing.
 * @param config Updated in place; its render scale is left self-consistent.
 */
inline void apply_config_node(const fkyaml::node& node, MapConfig& config) {
    if (!node.is_mapping()) {
        return;
    }

    config.seed = detail::read_or(node, "seed", config.seed);
    config.grid_size = detail::read_or(node, "grid_size", config.grid_size);
    config.topology = map_topology_from_name(
        detail::read_or(node, "topology", std::string(map_topology_name(config.topology))));
    config.sphere_cells = detail::read_or(node, "sphere_cells", config.sphere_cells);
    config.jitter = detail::read_or(node, "jitter", config.jitter);
    config.meters_per_grid_unit =
        detail::read_or(node, "meters_per_grid_unit", config.meters_per_grid_unit);
    config.elevation_range_m =
        detail::read_or(node, "elevation_range_m", config.elevation_range_m);
    config.meters_per_pixel = detail::read_or(node, "meters_per_pixel", config.meters_per_pixel);
    config.composite_shading = composite_shading_from_name(detail::read_or(
        node, "composite_shading", std::string(composite_shading_name(config.composite_shading))));
    config.elevation_surface = elevation_surface_from_name(detail::read_or(
        node, "elevation_surface", std::string(elevation_surface_name(config.elevation_surface))));
    config.elevation_blend =
        detail::read_or(node, "elevation_blend", config.elevation_blend);
    config.elevation_blend_variation = detail::read_or(node, "elevation_blend_variation",
                                                       config.elevation_blend_variation);
    config.image_size = detail::read_or(node, "image_size", config.image_size);
    config.png_compression_level =
        detail::read_or(node, "png_compression_level", config.png_compression_level);
    config.border_length = detail::read_or(node, "border_length", config.border_length);
    config.sea_level = detail::read_or(node, "sea_level", config.sea_level);
    config.terrain_relief = detail::read_or(node, "terrain_relief", config.terrain_relief);
    config.terrain_roughness =
        detail::read_or(node, "terrain_roughness", config.terrain_roughness);
    config.threshold_water = detail::read_or(node, "threshold_water", config.threshold_water);
    config.threshold_water_count = detail::read_or(node, "threshold_water_count", config.threshold_water_count);
    config.river_count = detail::read_or(node, "river_count", config.river_count);
    config.river_min_length = detail::read_or(node, "river_min_length", config.river_min_length);
    config.river_source_min_elevation =
        detail::read_or(node, "river_source_min_elevation", config.river_source_min_elevation);
    config.river_source_max_elevation =
        detail::read_or(node, "river_source_max_elevation", config.river_source_max_elevation);
    config.river_smoothing_iterations =
        detail::read_or(node, "river_smoothing_iterations", config.river_smoothing_iterations);
    config.river_width_base_m = detail::read_or(node, "river_width_base_m", config.river_width_base_m);
    config.river_width_per_volume_m =
        detail::read_or(node, "river_width_per_volume_m", config.river_width_per_volume_m);
    config.river_depth_m = detail::read_or(node, "river_depth_m", config.river_depth_m);
    config.river_depth_per_volume_m =
        detail::read_or(node, "river_depth_per_volume_m", config.river_depth_per_volume_m);
    config.river_channel_depth_m =
        detail::read_or(node, "river_channel_depth_m", config.river_channel_depth_m);
    config.river_channel_depth_per_volume_m = detail::read_or(
        node, "river_channel_depth_per_volume_m", config.river_channel_depth_per_volume_m);
    config.river_mouth_blend_m =
        detail::read_or(node, "river_mouth_blend_m", config.river_mouth_blend_m);
    config.river_incision_m = detail::read_or(node, "river_incision_m", config.river_incision_m);
    config.river_incision_per_volume_m =
        detail::read_or(node, "river_incision_per_volume_m", config.river_incision_per_volume_m);
    config.river_valley_width =
        detail::read_or(node, "river_valley_width", config.river_valley_width);
    config.river_valley_falloff =
        detail::read_or(node, "river_valley_falloff", config.river_valley_falloff);
    config.water_edge_overlap_m =
        detail::read_or(node, "water_edge_overlap_m", config.water_edge_overlap_m);
    config.lake_depth_m = detail::read_or(node, "lake_depth_m", config.lake_depth_m);
    config.lake_bed_grade = detail::read_or(node, "lake_bed_grade", config.lake_bed_grade);
    config.lake_depth_max_m = detail::read_or(node, "lake_depth_max_m", config.lake_depth_max_m);
    config.terrain_model = terrain_model_from_name(
        detail::read_or(node, "terrain_model", std::string(terrain_model_name(config.terrain_model))));
    if (node.contains("natural")) {
        const fkyaml::node& n = node.at("natural");
        NaturalTerrainConfig& c = config.natural;
        c.smoothing = detail::read_or(n, "smoothing", c.smoothing);
        c.hills = detail::read_or(n, "hills", c.hills);
        c.mountains = detail::read_or(n, "mountains", c.mountains);
        c.mountain_start = detail::read_or(n, "mountain_start", c.mountain_start);
        c.plateaus = detail::read_or(n, "plateaus", c.plateaus);
        c.plateau_coverage = detail::read_or(n, "plateau_coverage", c.plateau_coverage);
        c.warp = detail::read_or(n, "warp", c.warp);
        c.seabed = detail::read_or(n, "seabed", c.seabed);
        c.seabed_ridges = detail::read_or(n, "seabed_ridges", c.seabed_ridges);
        c.seabed_shelf = detail::read_or(n, "seabed_shelf", c.seabed_shelf);
    }
    detail::noise_from_node(node, "noise_hills", config.noise_hills);
    detail::noise_from_node(node, "noise_ridge", config.noise_ridge);
    detail::noise_from_node(node, "noise_plateau", config.noise_plateau);
    detail::noise_from_node(node, "noise_warp", config.noise_warp);
    detail::noise_from_node(node, "noise_seabed", config.noise_seabed);
    config.trail_width_m = detail::read_or(node, "trail_width_m", config.trail_width_m);
    config.road_width_m = detail::read_or(node, "road_width_m", config.road_width_m);
    config.highway_width_m = detail::read_or(node, "highway_width_m", config.highway_width_m);
    config.temperature_lapse_rate =
        detail::read_or(node, "temperature_lapse_rate", config.temperature_lapse_rate);
    config.temperature_falloff =
        detail::read_or(node, "temperature_falloff", config.temperature_falloff);
    config.temperature_offset =
        detail::read_or(node, "temperature_offset", config.temperature_offset);
    config.polar_extent_north =
        detail::read_or(node, "polar_extent_north", config.polar_extent_north);
    config.polar_extent_south =
        detail::read_or(node, "polar_extent_south", config.polar_extent_south);
    config.elevation_smoothing_iterations =
        detail::read_or(node, "elevation_smoothing_iterations", config.elevation_smoothing_iterations);
    config.elevation_smoothing_strength =
        detail::read_or(node, "elevation_smoothing_strength", config.elevation_smoothing_strength);

    // Legacy: widths were grid-unit fractions before they were metres. Scale them
    // by the world scale rather than ignoring them, so a map saved in between the
    // two conventions still draws its rivers and roads at the size it meant.
    const auto grid_key_as_meters = [&node, &config](const char* metre_key, const char* grid_key,
                                                     double& target) {
        if (!node.contains(metre_key) && node.contains(grid_key)) {
            target = detail::read_or(node, grid_key, 0.0) * config.meters_per_grid_unit;
        }
    };
    grid_key_as_meters("river_width_base_m", "river_width_base", config.river_width_base_m);
    grid_key_as_meters("river_width_per_volume_m", "river_width_per_volume",
                       config.river_width_per_volume_m);
    grid_key_as_meters("trail_width_m", "trail_width", config.trail_width_m);
    grid_key_as_meters("road_width_m", "road_width", config.road_width_m);
    grid_key_as_meters("highway_width_m", "highway_width", config.highway_width_m);

    // Legacy: widths used to be pixel counts at a nominal 1024 render of a
    // 40-unit grid. Convert rather than ignore, so a map saved before widths
    // became physical still draws its rivers and roads at the intended size.
    if (!node.contains("river_width_base_m") && node.contains("river_factor")) {
        const double pixels = static_cast<double>(detail::read_or(node, "river_factor", 1));
        config.river_width_base_m = (pixels * 2.0) / k_legacy_pixels_per_grid_unit;
        config.river_width_per_volume_m = 2.0 / k_legacy_pixels_per_grid_unit;
    }
    if (!node.contains("road_width_m") && node.contains("road_size")) {
        const double pixels = static_cast<double>(detail::read_or(node, "road_size", 1));
        config.road_width_m = (pixels * 2.0) / k_legacy_pixels_per_grid_unit;
    }
    config.subdivide_noisy_edges = detail::read_or(node, "subdivide_noisy_edges", config.subdivide_noisy_edges);

    config.show_regions = detail::read_or(node, "show_regions", config.show_regions);
    config.draw_landmark_marks =
        detail::read_or(node, "draw_landmark_marks", config.draw_landmark_marks);
    config.region_tint = detail::read_or(node, "region_tint", config.region_tint);

    detail::noise_from_node(node, "noise_cave", config.noise_cave);
    detail::noise_from_node(node, "noise_island", config.noise_island);
    detail::noise_from_node(node, "noise_temperature", config.noise_temperature);
    detail::noise_from_node(node, "noise_relief", config.noise_relief);
    detail::noise_from_node(node, "noise_terrain", config.noise_terrain);
    detail::noise_from_node(node, "noise_shape", config.noise_shape);
    detail::noise_from_node(node, "noise_blend", config.noise_blend);

    if (node.contains("regions")) {
        const fkyaml::node& regions = node.at("regions");
        RegionConfig& target = config.regions;
        target.country_count = detail::read_or(regions, "country_count", target.country_count);
        target.regions_per_country =
            detail::read_or(regions, "regions_per_country", target.regions_per_country);
        target.min_country_spacing =
            detail::read_or(regions, "min_country_spacing", target.min_country_spacing);
        target.elevation_cost = detail::read_or(regions, "elevation_cost", target.elevation_cost);
        target.water_crossing_cost =
            detail::read_or(regions, "water_crossing_cost", target.water_crossing_cost);
    }

    if (node.contains("caves")) {
        const fkyaml::node& caves = node.at("caves");
        CaveConfig& target = config.caves;
        target.cave_count = detail::read_or(caves, "cave_count", target.cave_count);
        target.min_grade = detail::read_or(caves, "min_grade", target.min_grade);
        target.min_spacing_m = detail::read_or(caves, "min_spacing_m", target.min_spacing_m);
        target.roof_clearance_m =
            detail::read_or(caves, "roof_clearance_m", target.roof_clearance_m);
        target.passage_height_m =
            detail::read_or(caves, "passage_height_m", target.passage_height_m);
        target.chamber_height_m =
            detail::read_or(caves, "chamber_height_m", target.chamber_height_m);
        target.max_depth_m = detail::read_or(caves, "max_depth_m", target.max_depth_m);
        target.vadose_share = detail::read_or(caves, "vadose_share", target.vadose_share);
        target.level_spacing_m =
            detail::read_or(caves, "level_spacing_m", target.level_spacing_m);
        target.max_levels = detail::read_or(caves, "max_levels", target.max_levels);
        target.level_budget = detail::read_or(caves, "level_budget", target.level_budget);
        target.step_m = detail::read_or(caves, "step_m", target.step_m);
        target.passage_length_m =
            detail::read_or(caves, "passage_length_m", target.passage_length_m);
        target.descent_grade = detail::read_or(caves, "descent_grade", target.descent_grade);
        target.massif_bias = detail::read_or(caves, "massif_bias", target.massif_bias);
        target.meander = detail::read_or(caves, "meander", target.meander);
        target.branch_chance_vadose =
            detail::read_or(caves, "branch_chance_vadose", target.branch_chance_vadose);
        target.branch_chance_phreatic =
            detail::read_or(caves, "branch_chance_phreatic", target.branch_chance_phreatic);
        target.branch_budget = detail::read_or(caves, "branch_budget", target.branch_budget);
        target.max_branches = detail::read_or(caves, "max_branches", target.max_branches);
        target.max_nodes = detail::read_or(caves, "max_nodes", target.max_nodes);
        target.passage_width_m =
            detail::read_or(caves, "passage_width_m", target.passage_width_m);
        target.chamber_radius_m =
            detail::read_or(caves, "chamber_radius_m", target.chamber_radius_m);
        target.shaft_chance = detail::read_or(caves, "shaft_chance", target.shaft_chance);
        target.shaft_drop_m = detail::read_or(caves, "shaft_drop_m", target.shaft_drop_m);
        target.smoothing_iterations =
            detail::read_or(caves, "smoothing_iterations", target.smoothing_iterations);
    }

    if (node.contains("landmarks")) {
        const fkyaml::node& landmarks = node.at("landmarks");
        LandmarkConfig& target = config.landmarks;
        target.max_natural = detail::read_or(landmarks, "max_natural", target.max_natural);
        target.max_abandoned = detail::read_or(landmarks, "max_abandoned", target.max_abandoned);
        target.peak_elevation = detail::read_or(landmarks, "peak_elevation", target.peak_elevation);
        target.waterfall_drop = detail::read_or(landmarks, "waterfall_drop", target.waterfall_drop);
        target.canyon_elevation =
            detail::read_or(landmarks, "canyon_elevation", target.canyon_elevation);
        target.great_lake_cells = static_cast<std::size_t>(detail::read_or(
            landmarks, "great_lake_cells", static_cast<int>(target.great_lake_cells)));
        target.town_clearance = detail::read_or(landmarks, "town_clearance", target.town_clearance);
        target.kind_share_numerator =
            detail::read_or(landmarks, "kind_share_numerator", target.kind_share_numerator);
        target.kind_share_denominator =
            detail::read_or(landmarks, "kind_share_denominator", target.kind_share_denominator);
        target.cape_ocean_ratio_numerator = detail::read_or(
            landmarks, "cape_ocean_ratio_numerator", target.cape_ocean_ratio_numerator);
        target.cape_ocean_ratio_denominator = detail::read_or(
            landmarks, "cape_ocean_ratio_denominator", target.cape_ocean_ratio_denominator);
    }

    if (node.contains("passes")) {
        const fkyaml::node& passes = node.at("passes");
        config.enable_water = detail::read_or(passes, "enable_water", config.enable_water);
        config.enable_coast = detail::read_or(passes, "enable_coast", config.enable_coast);
        config.enable_elevation = detail::read_or(passes, "enable_elevation", config.enable_elevation);
        config.enable_temperature =
            detail::read_or(passes, "enable_temperature", config.enable_temperature);
        config.enable_rivers = detail::read_or(passes, "enable_rivers", config.enable_rivers);
        config.enable_valleys = detail::read_or(passes, "enable_valleys", config.enable_valleys);
        config.enable_moisture = detail::read_or(passes, "enable_moisture", config.enable_moisture);
        config.enable_biomes = detail::read_or(passes, "enable_biomes", config.enable_biomes);
        config.enable_roads = detail::read_or(passes, "enable_roads", config.enable_roads);
        config.enable_regions = detail::read_or(passes, "enable_regions", config.enable_regions);
        config.enable_towns = detail::read_or(passes, "enable_towns", config.enable_towns);
        config.enable_landmarks =
            detail::read_or(passes, "enable_landmarks", config.enable_landmarks);
        config.enable_caves = detail::read_or(passes, "enable_caves", config.enable_caves);
        config.enable_noisy_edges =
            detail::read_or(passes, "enable_noisy_edges", config.enable_noisy_edges);
    }

    if (node.contains("roads")) {
        const fkyaml::node& roads = node.at("roads");
        RoadConfig& target = config.roads;
        target.hub_count = detail::read_or(roads, "hub_count", target.hub_count);
        target.hub_min_spacing_m = detail::read_or(roads, "hub_min_spacing_m", target.hub_min_spacing_m);
        target.slope_cost = detail::read_or(roads, "slope_cost", target.slope_cost);
        target.elevation_cost = detail::read_or(roads, "elevation_cost", target.elevation_cost);
        target.rough_ground_cost =
            detail::read_or(roads, "rough_ground_cost", target.rough_ground_cost);
        target.ford_cost = detail::read_or(roads, "ford_cost", target.ford_cost);
        target.bridge_cost_per_volume =
            detail::read_or(roads, "bridge_cost_per_volume", target.bridge_cost_per_volume);
        target.water_crossing_cost =
            detail::read_or(roads, "water_crossing_cost", target.water_crossing_cost);
        target.max_water_span = detail::read_or(roads, "max_water_span", target.max_water_span);
        target.reuse_discount = detail::read_or(roads, "reuse_discount", target.reuse_discount);
        target.highway_traffic_share =
            detail::read_or(roads, "highway_traffic_share", target.highway_traffic_share);
        target.road_traffic_share =
            detail::read_or(roads, "road_traffic_share", target.road_traffic_share);
        target.smoothing_iterations =
            detail::read_or(roads, "smoothing_iterations", target.smoothing_iterations);
    }

    if (node.contains("shape")) {
        const fkyaml::node& shape = node.at("shape");
        ShapeConfig& target = config.shape;
        target.shape = map_shape_from_name(
            detail::read_or(shape, "shape", std::string(map_shape_name(target.shape))));
        target.width_m = detail::read_or(shape, "width_m", target.width_m);
        target.height_m = detail::read_or(shape, "height_m", target.height_m);
        target.diameter_m = detail::read_or(shape, "diameter_m", target.diameter_m);
        target.edge_length_m = detail::read_or(shape, "edge_length_m", target.edge_length_m);
        target.rotation = detail::read_or(shape, "rotation", target.rotation);
        target.continent_size_m =
            detail::read_or(shape, "continent_size_m", target.continent_size_m);
        target.continent_count = detail::read_or(shape, "continent_count", target.continent_count);
        target.irregularity = detail::read_or(shape, "irregularity", target.irregularity);
        target.size_variance = detail::read_or(shape, "size_variance", target.size_variance);
        target.coast_detail = detail::read_or(shape, "coast_detail", target.coast_detail);
    }

    if (node.contains("towns")) {
        const fkyaml::node& towns = node.at("towns");
        config.towns.town_count = detail::read_or(towns, "town_count", config.towns.town_count);
        config.towns.min_spacing_m = detail::read_or(towns, "min_spacing_m", config.towns.min_spacing_m);
        config.towns.capital_count = detail::read_or(towns, "capital_count", config.towns.capital_count);
        config.towns.town_tier_count = detail::read_or(towns, "town_tier_count", config.towns.town_tier_count);
        config.towns.capital_cells = detail::read_or(towns, "capital_cells", config.towns.capital_cells);
        config.towns.town_cells = detail::read_or(towns, "town_cells", config.towns.town_cells);
        config.towns.village_cells = detail::read_or(towns, "village_cells", config.towns.village_cells);
        config.towns.buildings_per_town = detail::read_or(towns, "buildings_per_town", config.towns.buildings_per_town);
        config.towns.building_size_min_m =
            detail::read_or(towns, "building_size_min_m", config.towns.building_size_min_m);
        config.towns.building_size_max_m =
            detail::read_or(towns, "building_size_max_m", config.towns.building_size_max_m);
        // Legacy: one fixed size became a range. Collapse the range onto it so an
        // older map's buildings keep exactly the footprint they were saved with.
        if (!towns.contains("building_size_min_m") && towns.contains("building_size")) {
            const double fixed = detail::read_or(towns, "building_size", config.towns.building_size_min_m);
            config.towns.building_size_min_m = fixed;
            config.towns.building_size_max_m = fixed;
        }
        config.towns.town_building_scale =
            detail::read_or(towns, "town_building_scale", config.towns.town_building_scale);
        config.towns.village_building_scale =
            detail::read_or(towns, "village_building_scale", config.towns.village_building_scale);
        config.towns.water_clearance_m =
            detail::read_or(towns, "water_clearance_m", config.towns.water_clearance_m);
        config.towns.street_clearance_m =
            detail::read_or(towns, "street_clearance_m", config.towns.street_clearance_m);
        config.towns.street_width_m =
            detail::read_or(towns, "street_width_m", config.towns.street_width_m);
        config.towns.plaza_radius_m =
            detail::read_or(towns, "plaza_radius_m", config.towns.plaza_radius_m);
        config.towns.plaza_min_cells =
            detail::read_or(towns, "plaza_min_cells", config.towns.plaza_min_cells);
        config.towns.capital_civic_count =
            detail::read_or(towns, "capital_civic_count", config.towns.capital_civic_count);
        config.towns.town_civic_count =
            detail::read_or(towns, "town_civic_count", config.towns.town_civic_count);
        config.towns.village_civic_count =
            detail::read_or(towns, "village_civic_count", config.towns.village_civic_count);
        config.towns.household_size_min =
            detail::read_or(towns, "household_size_min", config.towns.household_size_min);
        config.towns.household_size_max =
            detail::read_or(towns, "household_size_max", config.towns.household_size_max);
        config.towns.capital_density =
            detail::read_or(towns, "capital_density", config.towns.capital_density);
        config.towns.town_density =
            detail::read_or(towns, "town_density", config.towns.town_density);
        config.towns.street_offset_m = detail::read_or(towns, "street_offset_m", config.towns.street_offset_m);
        config.towns.street_spacing_m = detail::read_or(towns, "street_spacing_m", config.towns.street_spacing_m);
        config.towns.position_jitter_m = detail::read_or(towns, "position_jitter_m", config.towns.position_jitter_m);
        config.towns.rotation_jitter = detail::read_or(towns, "rotation_jitter", config.towns.rotation_jitter);
        config.towns.infill_attempts = detail::read_or(towns, "infill_attempts", config.towns.infill_attempts);
    }

    // Last, because reconciling the two render-scale fields needs all of
    // `grid_size`, `meters_per_grid_unit`, `meters_per_pixel` and `image_size`
    // already read.
    //
    // The two ends of the scale must never disagree -- every feature is stroked
    // in METRES and converted through `meters_per_pixel`, so a document naming a
    // resolution and leaving a stale scale beside it would render a "10 m
    // highway" at whatever width the stale number implied. `image_size` wins
    // where both appear, because a pixel count is the more concrete statement of
    // intent, and the scale is back-computed to match it.
    //
    // Presence has to be tested rather than inferred: `image_size` has an
    // in-struct default, so a document naming that exact value is
    // indistinguishable from a document that said nothing -- the same problem
    // `ConfigLoadResult::has_seed` exists to solve.
    //
    // This lives here rather than in the generator so that *every* consumer of a
    // configuration gets a coherent pair, not just the one command-line tool that
    // happened to remember to derive it.
    const double world_meters =
        static_cast<double>(config.grid_size) * config.meters_per_grid_unit;
    if (node.contains("image_size") && config.image_size > 0 && world_meters > 0.0) {
        config.meters_per_pixel = world_meters / static_cast<double>(config.image_size);
    }
    config.image_size = derive_image_size(config);
}

/**
 * @brief Reads a `MapConfig` back from a YAML mapping node.
 *
 * Every key is optional and falls back to the in-struct default, so a map saved
 * before a parameter existed still loads.
 *
 * @param node The mapping node to decode.
 * @return The decoded configuration.
 */
inline MapConfig config_from_node(const fkyaml::node& node) {
    MapConfig config;
    apply_config_node(node, config);
    return config;
}

/**
 * @enum ConfigLoad
 * @brief How reading a configuration file turned out.
 */
enum class ConfigLoad {
    Ok,        /**< @brief The file parsed and was applied. */
    NotFound,  /**< @brief No file at that path; nothing was applied. */
    Malformed  /**< @brief The file exists but is not valid YAML; nothing was applied. */
};

/**
 * @struct ConfigLoadResult
 * @brief What `load_config()` managed to do, and what a caller should say about it.
 */
struct ConfigLoadResult {
    /** @brief The outcome. */
    ConfigLoad status = ConfigLoad::NotFound;
    /**
     * @brief The document set `seed:` explicitly, rather than inheriting a default.
     *
     * The decoded struct cannot answer this on its own: `MapConfig::seed` has an
     * in-struct default, so a configuration that named that exact value is
     * indistinguishable from one that said nothing. A generator that draws a
     * random seed when none was given needs the difference.
     */
    bool has_seed = false;
    /** @brief Why it failed, ready to print; empty when `status` is `Ok`. */
    std::string message;
};

/**
 * @brief Applies a configuration file on top of an existing `MapConfig`.
 *
 * Keys absent from the document keep whatever `out_config` already held, which is
 * what lets a caller establish its own defaults first and let the file override
 * only what it mentions. On `NotFound` or `Malformed` nothing is applied at all,
 * so a caller's defaults survive a missing or broken file intact.
 *
 * Reports the outcome rather than printing it. `load_map()` goes through
 * `coopa::collections::YAMLMap::load()`, which writes its own line to `std::cerr`
 * and rethrows on a parse error -- so it can neither tell a missing file from a
 * broken one nor keep quiet. A header-only library has no business owning the
 * program's stderr or deciding whether a bad config is fatal; that belongs to
 * whatever is running it.
 *
 * Unknown keys are ignored, so an older binary still reads a newer file.
 *
 * @param path Path to the configuration file, e.g. `assets/config.yaml`.
 * @param out_config Updated in place by whatever the document specifies.
 * @return The outcome, whether a seed was named, and a message on failure.
 */
inline ConfigLoadResult load_config(const std::string& path, MapConfig& out_config) {
    ConfigLoadResult result;

    const std::filesystem::path resolved = coopa::yaml::resolve_variant(path);
    if (!std::filesystem::exists(resolved)) {
        result.status = ConfigLoad::NotFound;
        result.message = "no configuration file at '" + path + "'";
        return result;
    }

    fkyaml::node root;
    try {
        root = coopa::yaml::load_document(resolved);
    } catch (const std::exception& error) {
        result.status = ConfigLoad::Malformed;
        result.message = "could not parse '" + path + "': " + error.what();
        return result;
    }

    if (!root.is_mapping()) {
        // An empty file parses as null rather than as an empty mapping. Treated as
        // malformed and not as "no keys set": a caller asked for this file, and
        // silently generating from defaults would hide the mistake.
        result.status = ConfigLoad::Malformed;
        result.message = "'" + path + "' is not a YAML mapping";
        return result;
    }

    result.has_seed = root.contains("seed");
    apply_config_node(root, out_config);
    result.status = ConfigLoad::Ok;
    return result;
}

/**
 * @brief Serialises a map and the configuration that produced it to a YAML document.
 *
 * The whole graph goes in -- cells, corners, edges, road runs, watercourses and
 * settlements, with every
 * adjacency list -- so the document is a save of generator state rather than a
 * derived export, and `map_from_node()` reconstructs it exactly.
 *
 * Keys are abbreviated (`i`, `x`, `d0`) on the bulk arrays. At a grid size of
 * 50 that is roughly 2,600 cells, 10,000 corners and 15,000 edges, and spelling
 * the keys out in full roughly doubles the file for no gain in legibility once
 * a document is that size.
 *
 * Floating-point fields are lossy: fkYAML's emitter formats them through a
 * default-precision `std::ostringstream`, so they survive to six significant
 * digits and no further. Positions therefore round-trip to about 1e-5 grid
 * units and normalised values to about 1e-7, which is far below the resolution
 * anything downstream distinguishes -- a reloaded map renders pixel-identical.
 * Connectivity, biomes, river volumes and flags are integral or boolean and
 * round-trip exactly. Anything needing bit-exact heights should re-run the
 * generator from the saved seed rather than read them back from here.
 *
 * @param graph The map to encode.
 * @param config The configuration it was generated from.
 * @return The document root node.
 */
inline fkyaml::node map_to_node(const MapGraph& graph, const MapConfig& config) {
    std::vector<fkyaml::node> centers;
    centers.reserve(graph.centers.size());
    for (const MapCenter& center : graph.centers) {
        fkyaml::node node = fkyaml::node::mapping();
        node["i"] = static_cast<int>(center.index);
        node["x"] = center.point.x;
        node["y"] = center.point.y;
        if (center.point.z != 0.0) node["z"] = center.point.z;   // sphere maps only
        node["elevation"] = center.elevation;
        node["water_level"] = center.water_level;
        node["moisture"] = center.moisture;
        node["temperature"] = center.temperature;
        node["biome"] = std::string(biome_name(center.biome));
        node["region"] = static_cast<int>(center.region);
        node["country"] = static_cast<int>(center.country);
        node["water"] = center.water;
        node["ocean"] = center.ocean;
        node["coast"] = center.coast;
        node["border"] = center.border;
        node["neighbors"] = detail::id_sequence(center.neighbors);
        node["corners"] = detail::id_sequence(center.corners);
        node["borders"] = detail::id_sequence(center.borders);
        centers.push_back(std::move(node));
    }

    std::vector<fkyaml::node> corners;
    corners.reserve(graph.corners.size());
    for (const MapCorner& corner : graph.corners) {
        fkyaml::node node = fkyaml::node::mapping();
        node["i"] = static_cast<int>(corner.index);
        node["x"] = corner.point.x;
        node["y"] = corner.point.y;
        if (corner.point.z != 0.0) node["z"] = corner.point.z;   // sphere maps only
        node["elevation"] = corner.elevation;
        node["moisture"] = corner.moisture;
        node["temperature"] = corner.temperature;
        node["river"] = corner.river;
        node["downslope"] = static_cast<int>(corner.downslope);
        node["water"] = corner.water;
        node["ocean"] = corner.ocean;
        node["coast"] = corner.coast;
        node["border"] = corner.border;
        node["touches"] = detail::id_sequence(corner.touches);
        node["protrudes"] = detail::id_sequence(corner.protrudes);
        node["adjacent"] = detail::id_sequence(corner.adjacent);
        corners.push_back(std::move(node));
    }

    std::vector<fkyaml::node> edges;
    edges.reserve(graph.edges.size());
    for (const MapEdge& edge : graph.edges) {
        fkyaml::node node = fkyaml::node::mapping();
        node["i"] = static_cast<int>(edge.index);
        node["d0"] = static_cast<int>(edge.d0);
        node["d1"] = static_cast<int>(edge.d1);
        node["v0"] = static_cast<int>(edge.v0);
        node["v1"] = static_cast<int>(edge.v1);
        node["mx"] = edge.midpoint.x;
        node["my"] = edge.midpoint.y;
        if (edge.midpoint.z != 0.0) node["mz"] = edge.midpoint.z;   // sphere maps only
        node["river"] = edge.river;
        node["road"] = edge.road;
        // Written only when there is something to say, as with noisy0/noisy1
        // below: most edges carry no road, and three keys apiece across every
        // edge of an 80-cell grid is megabytes spent saying "none".
        if (edge.road_class != RoadClass::None) {
            node["road_class"] = std::string(road_class_name(edge.road_class));
            node["traffic"] = edge.traffic;
        }
        if (edge.bridge) {
            node["bridge"] = true;
        }
        // Only written when subdivision actually produced a path worth keeping:
        // an unsubdivided edge's two points are recoverable from v0, v1 and the
        // midpoint, and writing them would inflate the document for nothing.
        if (edge.noisy_points0.size() > 2 || edge.noisy_points1.size() > 2) {
            node["noisy0"] = detail::point_sequence(edge.noisy_points0);
            node["noisy1"] = detail::point_sequence(edge.noisy_points1);
        }
        edges.push_back(std::move(node));
    }

    std::vector<fkyaml::node> roads;
    roads.reserve(graph.roads.size());
    for (const MapRoad& road : graph.roads) {
        fkyaml::node node = fkyaml::node::mapping();
        node["class"] = std::string(road_class_name(road.road_class));
        node["edges"] = detail::id_sequence(road.edges);
        node["points"] = detail::point_sequence(road.points);
        roads.push_back(std::move(node));
    }

    std::vector<fkyaml::node> rivers;
    rivers.reserve(graph.rivers.size());
    for (const MapRiver& river : graph.rivers) {
        fkyaml::node node = fkyaml::node::mapping();
        node["volume"] = river.volume;
        node["corners"] = detail::id_sequence(river.corners);
        node["points"] = detail::point_sequence(river.points);
        rivers.push_back(std::move(node));
    }

    std::vector<fkyaml::node> towns;
    towns.reserve(graph.towns.size());
    for (const MapTown& town : graph.towns) {
        std::vector<fkyaml::node> buildings;
        buildings.reserve(town.buildings.size());
        for (const MapBuilding& building : town.buildings) {
            fkyaml::node node = fkyaml::node::mapping();
            node["x"] = building.point.x;
            node["y"] = building.point.y;
            if (building.point.z != 0.0) node["z"] = building.point.z;   // sphere maps only
            node["w"] = building.width;
            node["h"] = building.height;
            node["r"] = building.rotation;
            // Written unconditionally, even for a dwelling: a reader that saw the
            // key only on civic buildings would have to guess whether its absence
            // meant "a house" or "saved before roles existed".
            node["role"] = std::string(building_role_name(building.role));
            buildings.push_back(std::move(node));
        }

        std::vector<fkyaml::node> streets;
        streets.reserve(town.streets.size());
        for (const MapStreet& street : town.streets) {
            fkyaml::node node = fkyaml::node::mapping();
            node["x0"] = street.from.x;
            node["y0"] = street.from.y;
            node["x1"] = street.to.x;
            node["y1"] = street.to.y;
            if (street.from.z != 0.0 || street.to.z != 0.0) {   // sphere maps only
                node["z0"] = street.from.z;
                node["z1"] = street.to.z;
            }
            node["clearance"] = street.clearance;
            streets.push_back(std::move(node));
        }

        fkyaml::node node = fkyaml::node::mapping();
        node["center"] = static_cast<int>(town.center);
        node["cells"] = detail::id_sequence(town.cells);
        node["x"] = town.point.x;
        node["y"] = town.point.y;
        if (town.point.z != 0.0) node["z"] = town.point.z;   // sphere maps only
        node["tier"] = std::string(town_tier_name(town.tier));
        node["score"] = town.score;
        if (!town.name.empty()) {
            node["name"] = town.name;
        }
        node["region"] = static_cast<int>(town.region);
        node["households"] = town.households;
        node["population"] = town.population;
        node["prosperity"] = town.prosperity;
        node["buildings"] = fkyaml::node::sequence(std::move(buildings));
        node["streets"] = fkyaml::node::sequence(std::move(streets));
        if (town.plaza.radius > 0.0) {
            fkyaml::node plaza = fkyaml::node::mapping();
            plaza["x"] = town.plaza.centre.x;
            plaza["y"] = town.plaza.centre.y;
            if (town.plaza.centre.z != 0.0) plaza["z"] = town.plaza.centre.z;   // sphere maps only
            plaza["radius"] = town.plaza.radius;
            node["plaza"] = std::move(plaza);
        }
        towns.push_back(std::move(node));
    }

    std::vector<fkyaml::node> regions;
    regions.reserve(graph.regions.size());
    for (const MapRegion& region : graph.regions) {
        fkyaml::node node = fkyaml::node::mapping();
        node["i"] = static_cast<int>(region.index);
        node["country"] = static_cast<int>(region.country);
        node["name"] = region.name;
        node["seed"] = static_cast<int>(region.seed);
        node["capital"] = static_cast<int>(region.capital);
        node["biome"] = std::string(biome_name(region.dominant_biome));
        node["population"] = region.population;
        node["area"] = region.area;
        // The renderer tints cells by this, so it is map data, not a derived
        // convenience -- a reloaded map must draw identically.
        node["r"] = region.color.r;
        node["g"] = region.color.g;
        node["b"] = region.color.b;
        node["cells"] = detail::id_sequence(region.cells);
        regions.push_back(std::move(node));
    }

    std::vector<fkyaml::node> countries;
    countries.reserve(graph.countries.size());
    for (const MapCountry& country : graph.countries) {
        fkyaml::node node = fkyaml::node::mapping();
        node["i"] = static_cast<int>(country.index);
        node["name"] = country.name;
        node["capital"] = static_cast<int>(country.capital);
        node["population"] = country.population;
        node["area"] = country.area;
        node["r"] = country.color.r;
        node["g"] = country.color.g;
        node["b"] = country.color.b;
        node["regions"] = detail::id_sequence(country.regions);
        countries.push_back(std::move(node));
    }

    std::vector<fkyaml::node> landmarks;
    landmarks.reserve(graph.landmarks.size());
    for (const MapLandmark& landmark : graph.landmarks) {
        fkyaml::node node = fkyaml::node::mapping();
        node["center"] = static_cast<int>(landmark.center);
        node["x"] = landmark.point.x;
        node["y"] = landmark.point.y;
        if (landmark.point.z != 0.0) node["z"] = landmark.point.z;   // sphere maps only
        node["kind"] = std::string(landmark_kind_name(landmark.kind));
        node["name"] = landmark.name;
        node["region"] = static_cast<int>(landmark.region);
        landmarks.push_back(std::move(node));
    }

    std::vector<fkyaml::node> caves;
    caves.reserve(graph.caves.size());
    for (const MapCave& cave : graph.caves) {
        // Stations and passages both, because they are different things rather
        // than one derived from the other: the stations are the system as it was
        // grown and carry its zones and features, while a passage is the smoothed
        // run that gets drawn and exported. Recomputing either from the other
        // needs the terrain and the pass's own random stream.
        std::vector<fkyaml::node> nodes;
        nodes.reserve(cave.nodes.size());
        for (const CaveNode& station : cave.nodes) {
            fkyaml::node node = fkyaml::node::mapping();
            node["x"] = station.point.x;
            node["y"] = station.point.y;
            if (station.point.z != 0.0) node["z"] = station.point.z;   // sphere maps only
            node["f"] = station.floor;
            node["r"] = station.roof;
            node["w"] = station.radius;
            node["c"] = static_cast<int>(station.center);
            node["zone"] = std::string(cave_zone_name(station.zone));
            node["feature"] = std::string(cave_feature_name(station.feature));
            node["l"] = static_cast<int>(station.level);
            node["p"] = static_cast<int>(station.parent);
            nodes.push_back(std::move(node));
        }

        std::vector<fkyaml::node> passages;
        passages.reserve(cave.passages.size());
        for (const CavePassage& passage : cave.passages) {
            fkyaml::node node = fkyaml::node::mapping();
            node["level"] = static_cast<int>(passage.level);
            node["nodes"] = detail::id_sequence(passage.nodes);
            node["points"] = detail::point_sequence(passage.points);
            node["floors"] = detail::scalar_sequence(passage.floors);
            node["roofs"] = detail::scalar_sequence(passage.roofs);
            node["radii"] = detail::scalar_sequence(passage.radii);
            passages.push_back(std::move(node));
        }

        fkyaml::node node = fkyaml::node::mapping();
        node["mouth_edge"] = static_cast<int>(cave.mouth_edge);
        node["x"] = cave.mouth.x;
        node["y"] = cave.mouth.y;
        if (cave.mouth.z != 0.0) node["z"] = cave.mouth.z;   // sphere maps only
        node["grade"] = cave.mouth_grade;
        node["surface"] = cave.surface_at_mouth;
        node["phreatic"] = cave.phreatic_level;
        node["levels"] = detail::scalar_sequence(cave.levels);
        node["deepest"] = cave.deepest;
        node["length_m"] = cave.length_m;
        node["region"] = static_cast<int>(cave.region);
        node["name"] = cave.name;
        node["nodes"] = fkyaml::node::sequence(std::move(nodes));
        node["passages"] = fkyaml::node::sequence(std::move(passages));
        caves.push_back(std::move(node));
    }

    fkyaml::node root = fkyaml::node::mapping();
    root["version"] = k_map_yaml_version;
    root["config"] = config_to_node(config);
    if (graph.is_sphere()) root["sphere_radius"] = graph.sphere_radius;
    root["centers"] = fkyaml::node::sequence(std::move(centers));
    root["corners"] = fkyaml::node::sequence(std::move(corners));
    root["edges"] = fkyaml::node::sequence(std::move(edges));
    root["roads"] = fkyaml::node::sequence(std::move(roads));
    root["rivers"] = fkyaml::node::sequence(std::move(rivers));
    root["towns"] = fkyaml::node::sequence(std::move(towns));
    root["regions"] = fkyaml::node::sequence(std::move(regions));
    root["countries"] = fkyaml::node::sequence(std::move(countries));
    root["landmarks"] = fkyaml::node::sequence(std::move(landmarks));
    root["caves"] = fkyaml::node::sequence(std::move(caves));
    return root;
}

/**
 * @brief Reconstructs a map and its configuration from a YAML document.
 *
 * Edges whose noisy paths were omitted have them rebuilt from the endpoints
 * and midpoint, so a round-tripped graph renders identically to the original
 * whether or not subdivision was enabled when it was saved.
 *
 * @param root The document root produced by `map_to_node()`.
 * @param out_graph Receives the decoded map.
 * @param out_config Receives the decoded configuration.
 * @return True if the document held a recognised map, false otherwise.
 */
inline bool map_from_node(const fkyaml::node& root, MapGraph& out_graph, MapConfig& out_config) {
    if (!root.is_mapping() || !root.contains("centers")) {
        return false;
    }

    out_graph.clear();
    out_config = root.contains("config") ? config_from_node(root.at("config")) : MapConfig{};
    out_graph.sphere_radius = detail::read_or(root, "sphere_radius", 0.0);

    if (root.at("centers").is_sequence()) {
        for (const fkyaml::node& node : root.at("centers")) {
            MapCenter center;
            center.index = static_cast<CenterId>(detail::read_or(node, "i", 0));
            center.point = {detail::read_or(node, "x", 0.0), detail::read_or(node, "y", 0.0), detail::read_or(node, "z", 0.0)};
            center.elevation = detail::read_or(node, "elevation", 0.0);
            center.water_level = detail::read_or(node, "water_level", 0.0);
            center.moisture = detail::read_or(node, "moisture", 0.0);
            center.temperature = detail::read_or(node, "temperature", 0.0);
            center.biome = biome_from_name(detail::read_or(node, "biome", std::string("ocean")));
            center.region = static_cast<RegionId>(
                detail::read_or(node, "region", static_cast<int>(k_invalid_id)));
            center.country = static_cast<CountryId>(
                detail::read_or(node, "country", static_cast<int>(k_invalid_id)));
            center.water = detail::read_or(node, "water", false);
            center.ocean = detail::read_or(node, "ocean", false);
            center.coast = detail::read_or(node, "coast", false);
            center.border = detail::read_or(node, "border", false);
            center.neighbors = detail::read_id_sequence<CenterId>(node, "neighbors");
            center.corners = detail::read_id_sequence<CornerId>(node, "corners");
            center.borders = detail::read_id_sequence<EdgeId>(node, "borders");
            out_graph.centers.push_back(std::move(center));
        }
    }

    if (root.contains("corners") && root.at("corners").is_sequence()) {
        for (const fkyaml::node& node : root.at("corners")) {
            MapCorner corner;
            corner.index = static_cast<CornerId>(detail::read_or(node, "i", 0));
            corner.point = {detail::read_or(node, "x", 0.0), detail::read_or(node, "y", 0.0), detail::read_or(node, "z", 0.0)};
            corner.elevation = detail::read_or(node, "elevation", 0.0);
            corner.moisture = detail::read_or(node, "moisture", 0.0);
            corner.temperature = detail::read_or(node, "temperature", 0.0);
            corner.river = detail::read_or(node, "river", 0);
            corner.downslope = static_cast<CornerId>(
                detail::read_or(node, "downslope", static_cast<int>(k_invalid_id)));
            corner.water = detail::read_or(node, "water", false);
            corner.ocean = detail::read_or(node, "ocean", false);
            corner.coast = detail::read_or(node, "coast", false);
            corner.border = detail::read_or(node, "border", false);
            corner.touches = detail::read_id_sequence<CenterId>(node, "touches");
            corner.protrudes = detail::read_id_sequence<EdgeId>(node, "protrudes");
            corner.adjacent = detail::read_id_sequence<CornerId>(node, "adjacent");
            out_graph.corners.push_back(std::move(corner));
        }
    }

    if (root.contains("edges") && root.at("edges").is_sequence()) {
        for (const fkyaml::node& node : root.at("edges")) {
            MapEdge edge;
            edge.index = static_cast<EdgeId>(detail::read_or(node, "i", 0));
            edge.d0 = static_cast<CenterId>(detail::read_or(node, "d0", static_cast<int>(k_invalid_id)));
            edge.d1 = static_cast<CenterId>(detail::read_or(node, "d1", static_cast<int>(k_invalid_id)));
            edge.v0 = static_cast<CornerId>(detail::read_or(node, "v0", static_cast<int>(k_invalid_id)));
            edge.v1 = static_cast<CornerId>(detail::read_or(node, "v1", static_cast<int>(k_invalid_id)));
            edge.midpoint = {detail::read_or(node, "mx", 0.0), detail::read_or(node, "my", 0.0), detail::read_or(node, "mz", 0.0)};
            edge.river = detail::read_or(node, "river", 0);
            edge.road = detail::read_or(node, "road", false);
            // A document written before roads had classes says only `road: true`.
            // Read that as a plain road rather than discarding it: the flag was
            // written because a road was there. Same courtesy the legacy
            // `road_size` conversion above extends to widths.
            edge.road_class = edge.road
                ? road_class_from_name(detail::read_or(node, "road_class", std::string("road")))
                : RoadClass::None;
            edge.traffic = detail::read_or(node, "traffic", 0);
            edge.bridge = detail::read_or(node, "bridge", false);
            edge.noisy_points0 = detail::read_point_sequence(node, "noisy0");
            edge.noisy_points1 = detail::read_point_sequence(node, "noisy1");
            edge.noisy = !edge.noisy_points0.empty();
            out_graph.edges.push_back(std::move(edge));
        }
    }

    if (root.contains("roads") && root.at("roads").is_sequence()) {
        for (const fkyaml::node& node : root.at("roads")) {
            MapRoad road;
            road.road_class =
                road_class_from_name(detail::read_or(node, "class", std::string("trail")));
            road.edges = detail::read_id_sequence<EdgeId>(node, "edges");
            road.points = detail::read_point_sequence(node, "points");
            out_graph.roads.push_back(std::move(road));
        }
    }

    if (root.contains("rivers") && root.at("rivers").is_sequence()) {
        for (const fkyaml::node& node : root.at("rivers")) {
            MapRiver river;
            river.volume = detail::read_or(node, "volume", 0);
            river.corners = detail::read_id_sequence<CornerId>(node, "corners");
            river.points = detail::read_point_sequence(node, "points");
            out_graph.rivers.push_back(std::move(river));
        }
    }

    if (root.contains("towns") && root.at("towns").is_sequence()) {
        for (const fkyaml::node& node : root.at("towns")) {
            MapTown town;
            town.cells = detail::read_id_sequence<CenterId>(node, "cells");
            town.center = static_cast<CenterId>(
                detail::read_or(node, "center", static_cast<int>(k_invalid_id)));
            town.point = {detail::read_or(node, "x", 0.0), detail::read_or(node, "y", 0.0), detail::read_or(node, "z", 0.0)};
            town.tier = town_tier_from_name(detail::read_or(node, "tier", std::string("village")));
            town.score = detail::read_or(node, "score", 0.0);
            town.name = detail::read_or(node, "name", std::string());
            town.region = static_cast<RegionId>(
                detail::read_or(node, "region", static_cast<int>(k_invalid_id)));
            town.households = detail::read_or(node, "households", 0);
            town.population = detail::read_or(node, "population", 0);
            town.prosperity = detail::read_or(node, "prosperity", 0.0);

            if (node.contains("buildings") && node.at("buildings").is_sequence()) {
                for (const fkyaml::node& building_node : node.at("buildings")) {
                    MapBuilding building;
                    building.point = {detail::read_or(building_node, "x", 0.0),
                                      detail::read_or(building_node, "y", 0.0), detail::read_or(building_node, "z", 0.0)};
                    building.width = detail::read_or(building_node, "w", 0.0);
                    building.height = detail::read_or(building_node, "h", 0.0);
                    building.rotation = detail::read_or(building_node, "r", 0.0);
                    building.role = building_role_from_name(
                        detail::read_or(building_node, "role", std::string("dwelling")));
                    town.buildings.push_back(building);
                }
            }
            if (node.contains("streets") && node.at("streets").is_sequence()) {
                for (const fkyaml::node& street_node : node.at("streets")) {
                    // Bearing and length are derived, not stored: two numbers that
                    // must agree with the endpoints are two numbers that can
                    // disagree with them.
                    town.streets.push_back(
                        make_street(MapPoint{detail::read_or(street_node, "x0", 0.0),
                                             detail::read_or(street_node, "y0", 0.0),
                                             detail::read_or(street_node, "z0", 0.0)},
                                    MapPoint{detail::read_or(street_node, "x1", 0.0),
                                             detail::read_or(street_node, "y1", 0.0),
                                             detail::read_or(street_node, "z1", 0.0)},
                                    detail::read_or(street_node, "clearance", 0.0)));
                }
            }
            if (node.contains("plaza")) {
                const fkyaml::node& plaza = node.at("plaza");
                town.plaza.centre = {detail::read_or(plaza, "x", 0.0),
                                     detail::read_or(plaza, "y", 0.0), detail::read_or(plaza, "z", 0.0)};
                town.plaza.radius = detail::read_or(plaza, "radius", 0.0);
            }
            out_graph.towns.push_back(std::move(town));
        }
    }

    if (root.contains("regions") && root.at("regions").is_sequence()) {
        for (const fkyaml::node& node : root.at("regions")) {
            MapRegion region;
            region.index = static_cast<RegionId>(detail::read_or(node, "i", 0));
            region.country = static_cast<CountryId>(
                detail::read_or(node, "country", static_cast<int>(k_invalid_id)));
            region.name = detail::read_or(node, "name", std::string());
            region.seed = static_cast<CenterId>(
                detail::read_or(node, "seed", static_cast<int>(k_invalid_id)));
            region.capital = static_cast<CenterId>(
                detail::read_or(node, "capital", static_cast<int>(k_invalid_id)));
            region.dominant_biome =
                biome_from_name(detail::read_or(node, "biome", std::string("grassland")));
            region.population = detail::read_or(node, "population", 0);
            region.area = detail::read_or(node, "area", 0.0);
            region.color = glm::vec3(detail::read_or(node, "r", 255.0f),
                                     detail::read_or(node, "g", 255.0f),
                                     detail::read_or(node, "b", 255.0f));
            region.cells = detail::read_id_sequence<CenterId>(node, "cells");
            out_graph.regions.push_back(std::move(region));
        }
    }

    if (root.contains("countries") && root.at("countries").is_sequence()) {
        for (const fkyaml::node& node : root.at("countries")) {
            MapCountry country;
            country.index = static_cast<CountryId>(detail::read_or(node, "i", 0));
            country.name = detail::read_or(node, "name", std::string());
            country.capital = static_cast<CenterId>(
                detail::read_or(node, "capital", static_cast<int>(k_invalid_id)));
            country.population = detail::read_or(node, "population", 0);
            country.area = detail::read_or(node, "area", 0.0);
            country.color = glm::vec3(detail::read_or(node, "r", 255.0f),
                                      detail::read_or(node, "g", 255.0f),
                                      detail::read_or(node, "b", 255.0f));
            country.regions = detail::read_id_sequence<RegionId>(node, "regions");
            out_graph.countries.push_back(std::move(country));
        }
    }

    if (root.contains("landmarks") && root.at("landmarks").is_sequence()) {
        for (const fkyaml::node& node : root.at("landmarks")) {
            MapLandmark landmark;
            landmark.center = static_cast<CenterId>(
                detail::read_or(node, "center", static_cast<int>(k_invalid_id)));
            landmark.point = {detail::read_or(node, "x", 0.0), detail::read_or(node, "y", 0.0), detail::read_or(node, "z", 0.0)};
            landmark.kind =
                landmark_kind_from_name(detail::read_or(node, "kind", std::string("ruins")));
            landmark.name = detail::read_or(node, "name", std::string());
            landmark.region = static_cast<RegionId>(
                detail::read_or(node, "region", static_cast<int>(k_invalid_id)));
            out_graph.landmarks.push_back(std::move(landmark));
        }
    }

    if (root.contains("caves") && root.at("caves").is_sequence()) {
        for (const fkyaml::node& node : root.at("caves")) {
            MapCave cave;
            cave.mouth_edge = static_cast<EdgeId>(
                detail::read_or(node, "mouth_edge", static_cast<int>(k_invalid_id)));
            cave.mouth = {detail::read_or(node, "x", 0.0), detail::read_or(node, "y", 0.0), detail::read_or(node, "z", 0.0)};
            cave.mouth_grade = detail::read_or(node, "grade", 0.0);
            cave.surface_at_mouth = detail::read_or(node, "surface", 0.0);
            cave.phreatic_level = detail::read_or(node, "phreatic", 0.0);
            cave.levels = detail::read_scalar_sequence(node, "levels");
            if (cave.levels.empty()) {
                // Written before caves had storeys. Its one water table is its one
                // level, which is exactly the system it was when it was saved.
                cave.levels.push_back(cave.phreatic_level);
            }
            cave.deepest = detail::read_or(node, "deepest", 0.0);
            cave.length_m = detail::read_or(node, "length_m", 0.0);
            cave.region = static_cast<RegionId>(
                detail::read_or(node, "region", static_cast<int>(k_invalid_id)));
            cave.name = detail::read_or(node, "name", std::string());

            if (node.contains("nodes") && node.at("nodes").is_sequence()) {
                for (const fkyaml::node& entry : node.at("nodes")) {
                    CaveNode station;
                    station.point = {detail::read_or(entry, "x", 0.0),
                                     detail::read_or(entry, "y", 0.0), detail::read_or(entry, "z", 0.0)};
                    station.floor = detail::read_or(entry, "f", 0.0);
                    station.roof = detail::read_or(entry, "r", 0.0);
                    station.radius = detail::read_or(entry, "w", 0.0);
                    station.center = static_cast<CenterId>(
                        detail::read_or(entry, "c", static_cast<int>(k_invalid_id)));
                    station.zone = cave_zone_from_name(
                        detail::read_or(entry, "zone", std::string("vadose")));
                    station.feature = cave_feature_from_name(
                        detail::read_or(entry, "feature", std::string("passage")));
                    // Defaults to the shallowest level, so a map written before
                    // caves had storeys loads as the single-level system it was.
                    station.level = detail::read_or(entry, "l", 0);
                    station.parent = detail::read_or(entry, "p", -1);
                    cave.nodes.push_back(station);
                }
            }
            if (node.contains("passages") && node.at("passages").is_sequence()) {
                for (const fkyaml::node& entry : node.at("passages")) {
                    CavePassage passage;
                    passage.nodes = detail::read_id_sequence<std::int32_t>(entry, "nodes");
                    passage.points = detail::read_point_sequence(entry, "points");
                    passage.level = detail::read_or(entry, "level", 0);
                    passage.floors = detail::read_scalar_sequence(entry, "floors");
                    passage.roofs = detail::read_scalar_sequence(entry, "roofs");
                    passage.radii = detail::read_scalar_sequence(entry, "radii");
                    cave.passages.push_back(std::move(passage));
                }
            }
            out_graph.caves.push_back(std::move(cave));
        }
    }

    // Rebuild the straight-edge paths the writer omitted.
    for (MapEdge& edge : out_graph.edges) {
        if (!edge.noisy_points0.empty()
            || edge.v0 == k_invalid_id || edge.v1 == k_invalid_id
            || static_cast<std::size_t>(edge.v0) >= out_graph.corners.size()
            || static_cast<std::size_t>(edge.v1) >= out_graph.corners.size()) {
            continue;
        }
        edge.noisy_points0 = {out_graph.corners[static_cast<std::size_t>(edge.v0)].point, edge.midpoint};
        edge.noisy_points1 = {out_graph.corners[static_cast<std::size_t>(edge.v1)].point, edge.midpoint};
        edge.noisy = true;
    }

    return true;
}

/**
 * @brief Writes a map to a YAML file.
 * @param graph The map to save.
 * @param config The configuration it was generated from.
 * @param filepath Destination path, including the `.yaml` extension.
 * @throws std::runtime_error If the file cannot be opened for writing.
 */
inline void save_map(const MapGraph& graph, const MapConfig& config, const std::string& filepath) {
    coopa::collections::YAMLMap document(map_to_node(graph, config));
    document.save(filepath);
}

/**
 * @brief Reads a map back from a YAML file.
 * @param filepath Path to a file previously written by `save_map()`.
 * @param out_graph Receives the decoded map.
 * @param out_config Receives the decoded configuration.
 * @return True on success, false if the file is missing or holds no map.
 */
inline bool load_map(const std::string& filepath, MapGraph& out_graph, MapConfig& out_config) {
    const coopa::collections::YAMLMap document = coopa::collections::YAMLMap::load(filepath);
    return map_from_node(document.get_raw_node(), out_graph, out_config);
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_YAML_H

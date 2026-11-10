/**
 * @file map_generator.cpp
 * @brief Standalone tool that generates a random map and writes it out as one
 *        PNG render per `MapLayer` and one YAML document.
 *
 * Build target `mapcoopa` (so `cplay` runs it). Settings come from four places,
 * each overriding the one before it:
 *
 *   1. `MapConfig`'s in-struct defaults, sized for the small map a bare
 *      `MapConfig` gives a library consumer.
 *   2. The overrides at the top of `main()` -- this tool's own defaults, which
 *      keep a run with no configuration file producing the documented 80-cell
 *      world.
 *   3. `assets/config.yaml`, or whatever `--config=` names.
 *   4. Command-line flags, so trying something never means editing a
 *      version-controlled file.
 *
 * A missing configuration file is a warning; one that exists but does not parse
 * is fatal. Generating a map that silently ignored the settings it was given is
 * worse than generating none.
 *
 * @code
 * ./build/mapcoopa                          # assets/config.yaml, written to ./map_out.*
 * ./build/mapcoopa --seed=251 --out=/tmp/m  # reproducible, written to /tmp/m.*
 * ./build/mapcoopa --config=/tmp/alt.yaml   # a different configuration entirely
 * @endcode
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <random>
#include <string>
#include <string_view>

#include <root_directory.h>

#include <coopa/debug/logger.h>
#include <coopa/job/engine.h>
#include <coopa/maps/map_export.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_presets.h>
#include <coopa/maps/map_renderer.h>
#include <coopa/maps/map_yaml.h>

namespace {

/** @brief Degrees to radians, for the shape rotation flag. */
constexpr double k_degrees_to_radians = 3.14159265358979323846 / 180.0;

/** @brief Prints the accepted flags and their defaults. */
void print_usage() {
    std::cout
        << "usage: coopa_mapgen [options]\n"
        << "\n"
        << "  --seed=N         master seed; random each run when omitted\n"
        << "  --grid-size=N    cells per axis (default 80)\n"
        << "  --image-size=N   render size in pixels, square; overrides meters_per_pixel\n"
        << "  --rivers=N       river sources to attempt (default 55)\n"
        << "  --towns=N        settlements to place (default 28)\n"
        << "  --road-hubs=N    places the road network is routed between (default 32)\n"
        << "  --countries=N    nations to carve out (default 5)\n"
        << "  --regions=N      provinces per nation (default 3)\n"
        << "  --no-regions     skip political geography entirely\n"
        << "  --no-landmarks   skip notable places\n"
        << "  --caves=N        cave systems to open (default 18)\n"
        << "  --cave-depth=M   how far below its mouth a system may reach, in metres\n"
        << "  --cave-grade=F   steepness a slope needs to bear a cave; rise over run\n"
        << "  --no-caves       skip caves entirely\n"
        << "  --no-roads       skip the road network\n"
        << "  --sphere         generate a globe (no edges, latitude climate); images equirectangular\n"
        << "  --no-subdivide   draw straight cell boundaries instead of wobbled ones\n"
        << "  --out=PATH       output prefix (default \"map_out\")\n"
        << "  --config=PATH    settings file (default assets/config.yaml)\n"
        << "  --shading=MODE   composite lighting: elevation (default) | hillshade\n"
        << "  --surface=MODE   height field between cells: interpolated (default) | flat |\n"
        << "                   blended. flat gives one height per cell with hard edges;\n"
        << "                   blended is flat, then blurred over the whole raster\n"
        << "  --blend=F        blended's blur radius in cells, 0 to 1; 0 is flat exactly\n"
        << "  --blend-variation=F  how much that radius varies from cell to cell, 0 to 1;\n"
        << "                   0 blurs every cell equally, 1 ranges from 0 to twice --blend\n"
        << "  --threads=N      worker threads; 0 = all cores (default), 1 = serial\n"
        << "  --png-level=N    PNG deflate effort 1-9; lower is faster and larger\n"
        << "  --shape=S        landmass outline: rect (default) | circle | triangle |\n"
        << "                   continent | archipelago -- the last two wander the coast\n"
        << "  --shape-size=M   width / diameter / edge / mean continent in metres;\n"
        << "                   0 fills the canvas\n"
        << "  --shape-height=M rectangle height in metres; defaults to --shape-size\n"
        << "  --shape-rot=DEG  rotation of the shape in degrees (triangle)\n"
        << "  --shape-count=N  landmasses to attempt (archipelago); a close pair fuses\n"
        << "  --shape-wobble=F how far the coast wanders from a circle, 0 to 0.6\n"
        << "  --relief=F       fractal reshaping of the height field, 0 to 1\n"
        << "  --incision=M     how deep rivers cut their valleys, in metres; 0 for none\n"
        << "  --channel=M      how deep the river channel itself is cut; 0 for none\n"
        << "  --no-valleys     leave the height field uncarved by the rivers\n"
        << "  --roughness=F    terrain detail amplitude, 0 (default) to 1\n"
        << "  --temperature=F  shifts the whole world warmer or colder, -1 to 1\n"
        << "  --polar=F        frozen fraction at each pole, 0 to 0.5; 0 for no ice caps\n"
        << "  --polar-north=F  frozen fraction at the north pole alone\n"
        << "  --polar-south=F  frozen fraction at the south pole alone\n"
        << "  --help           show this message\n"
        << "\n"
        << "writes PATH.yaml and one PNG per layer: elevation, water, biomes,\n"
        << "roads, structures, landmarks, regions, composite, and the caves --\n"
        << "a readable caves overview, plus a cave_floor_N and cave_roof_N pair\n"
        << "for every storey the map's systems reach, on the elevation layer's\n"
        << "own greyscale. How many pairs that is comes from the caves rather\n"
        << "than being fixed: a map of single-level systems writes one, and one\n"
        << "with three-storey systems writes three. The caves are underground\n"
        << "and are never drawn into the composite.\n";
}

/**
 * @brief Matches `--name=value` and extracts the value.
 * @param argument The raw command-line argument.
 * @param name The flag name, without the leading dashes.
 * @param out_value Receives the value when the argument matches.
 * @return True if the argument is this flag.
 */
bool match_option(std::string_view argument, std::string_view name, std::string_view& out_value) {
    const std::string prefix = "--" + std::string(name) + "=";
    if (argument.rfind(prefix, 0) != 0) {
        return false;
    }
    out_value = argument.substr(prefix.size());
    return true;
}

} // namespace

/**
 * @brief Parses the command line, generates a map, and writes the layer PNGs and YAML.
 * @param argc Argument count.
 * @param argv Argument values.
 * @return 0 on success, 1 on a bad argument or a failed write.
 */
int main(int argc, char** argv) {
    // The standard map (coopa/maps/map_presets.h), applied before the configuration file so
    // that a run without one still produces the world the README documents rather than quietly
    // dropping to MapConfig's library defaults of a grid of 50. Large enough that climate bands,
    // several nations and a spread of landmarks all have room to appear; the YAML lands around
    // 15 MB.
    //
    // No image_size here: it is derived from the world extent and `meters_per_pixel`, which
    // defaults to 1.0 so that a render is measurable at one pixel to the metre.
    coopa::maps::MapConfig config = coopa::maps::standard_config();

    std::string out_prefix = "map_out";
    bool seed_given = false;
    // 0 means "derive from the world scale". A --image-size is an instruction
    // about resolution, so it is applied by back-computing meters_per_pixel
    // rather than by setting image_size behind the scale's back -- the two must
    // never be able to disagree about how much ground a pixel covers.
    int image_size_override = 0;
    // 0 means one worker per core. 1 means no engine at all -- the serial path,
    // which is what the byte-for-byte comparison in docs/performance.md is run against.
    int threads = 0;
    // One flag drives whichever dimension the chosen shape actually uses, so a
    // caller does not have to know that a circle reads `diameter_m` and a
    // triangle `edge_length_m`. Negative means "not given".
    double shape_size_m = -1.0;
    double shape_height_m = -1.0;

    // --config has to be found before the file is read, and every other flag has
    // to be applied after -- so the argument list is walked twice. Doing it in one
    // pass would make a flag's effect depend on whether it happened to come before
    // or after --config on the line.
    std::string config_path = std::string(ROOT_DIR) + "/assets/config.yaml";
    for (int i = 1; i < argc; ++i) {
        std::string_view value;
        if (match_option(std::string_view(argv[i]), "config", value)) {
            config_path = std::string(value);
        }
    }

    const coopa::maps::ConfigLoadResult loaded =
        coopa::maps::load_config(config_path, config);
    switch (loaded.status) {
        case coopa::maps::ConfigLoad::Ok:
            std::cout << "coopa_mapgen: settings from " << config_path << "\n";
            break;
        case coopa::maps::ConfigLoad::NotFound:
            std::cerr << "coopa_mapgen: " << loaded.message << ", using built-in defaults\n";
            break;
        case coopa::maps::ConfigLoad::Malformed:
            std::cerr << "coopa_mapgen: " << loaded.message << "\n";
            return 1;
    }
    // A configuration that named a seed counts as having given one, so the
    // entropy draw below happens only when neither the file nor the command line
    // chose. Without this a config.yaml with `seed:` in it would still produce a
    // different map every run.
    seed_given = loaded.has_seed;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        std::string_view value;

        if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        } else if (argument == "--sphere") {
            config.topology = coopa::maps::MapTopology::Sphere;
        } else if (argument == "--no-subdivide") {
            config.subdivide_noisy_edges = false;
        } else if (argument == "--no-regions") {
            config.enable_regions = false;
            config.show_regions = false;
        } else if (argument == "--no-landmarks") {
            config.enable_landmarks = false;
        } else if (argument == "--no-caves") {
            config.enable_caves = false;
        } else if (argument == "--no-roads") {
            config.enable_roads = false;
        } else if (argument == "--no-valleys") {
            config.enable_valleys = false;
        } else if (match_option(argument, "seed", value)) {
            config.seed = std::atoi(std::string(value).c_str());
            seed_given = true;
        } else if (match_option(argument, "grid-size", value)) {
            config.grid_size = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "image-size", value)) {
            image_size_override = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "rivers", value)) {
            config.river_count = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "towns", value)) {
            config.towns.town_count = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "road-hubs", value)) {
            config.roads.hub_count = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "caves", value)) {
            config.caves.cave_count = std::atoi(std::string(value).c_str());
            if (config.caves.cave_count < 0) {
                std::cerr << "coopa_mapgen: --caves must not be negative\n";
                return 1;
            }
        } else if (match_option(argument, "cave-depth", value)) {
            config.caves.max_depth_m = std::atof(std::string(value).c_str());
            if (config.caves.max_depth_m < 0.0) {
                std::cerr << "coopa_mapgen: --cave-depth must not be negative\n";
                return 1;
            }
        } else if (match_option(argument, "cave-grade", value)) {
            config.caves.min_grade = std::atof(std::string(value).c_str());
            if (config.caves.min_grade < 0.0 || config.caves.min_grade > 4.0) {
                std::cerr << "coopa_mapgen: --cave-grade must be between 0 and 4\n";
                return 1;
            }
        } else if (match_option(argument, "countries", value)) {
            config.regions.country_count = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "regions", value)) {
            config.regions.regions_per_country = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "out", value)) {
            out_prefix = std::string(value);
        } else if (match_option(argument, "threads", value)) {
            threads = std::atoi(std::string(value).c_str());
            if (threads < 0) {
                std::cerr << "coopa_mapgen: --threads must be 0 or more\n";
                return 1;
            }
        } else if (match_option(argument, "png-level", value)) {
            config.png_compression_level = std::atoi(std::string(value).c_str());
            if (config.png_compression_level < 1 || config.png_compression_level > 9) {
                std::cerr << "coopa_mapgen: --png-level must be between 1 and 9\n";
                return 1;
            }
        } else if (match_option(argument, "shape", value)) {
            const std::string name(value);
            if (name != "rect" && name != "rectangle" && name != "circle"
                && name != "triangle" && name != "continent" && name != "archipelago") {
                std::cerr << "coopa_mapgen: --shape must be rect, circle, triangle, "
                             "continent or archipelago\n";
                return 1;
            }
            config.shape.shape = coopa::maps::map_shape_from_name(name);
        } else if (match_option(argument, "shape-size", value)) {
            shape_size_m = std::atof(std::string(value).c_str());
        } else if (match_option(argument, "shape-height", value)) {
            shape_height_m = std::atof(std::string(value).c_str());
        } else if (match_option(argument, "shape-rot", value)) {
            config.shape.rotation = std::atof(std::string(value).c_str()) * k_degrees_to_radians;
        } else if (match_option(argument, "shape-count", value)) {
            config.shape.continent_count = std::atoi(std::string(value).c_str());
            if (config.shape.continent_count < 1) {
                std::cerr << "coopa_mapgen: --shape-count must be at least 1\n";
                return 1;
            }
        } else if (match_option(argument, "shape-wobble", value)) {
            config.shape.irregularity = std::atof(std::string(value).c_str());
            if (config.shape.irregularity < 0.0 || config.shape.irregularity > 0.6) {
                std::cerr << "coopa_mapgen: --shape-wobble must be between 0 and 0.6\n";
                return 1;
            }
        } else if (match_option(argument, "channel", value)) {
            config.river_channel_depth_m = std::atof(std::string(value).c_str());
            if (config.river_channel_depth_m < 0.0) {
                std::cerr << "coopa_mapgen: --channel must not be negative\n";
                return 1;
            }
            // Scaled with the base for the same reason --incision is: one flag
            // should deepen the whole network, not flatten a trunk river toward
            // the stream feeding it.
            config.river_channel_depth_per_volume_m = config.river_channel_depth_m * 0.22;
        } else if (match_option(argument, "incision", value)) {
            config.river_incision_m = std::atof(std::string(value).c_str());
            if (config.river_incision_m < 0.0) {
                std::cerr << "coopa_mapgen: --incision must not be negative\n";
                return 1;
            }
            // The per-volume term scales with the base, so one flag deepens the
            // whole network rather than flattening the difference between a
            // stream and the trunk river it feeds.
            config.river_incision_per_volume_m = config.river_incision_m * 0.2;
        } else if (match_option(argument, "relief", value)) {
            config.terrain_relief = std::atof(std::string(value).c_str());
            if (config.terrain_relief < 0.0 || config.terrain_relief > 1.0) {
                std::cerr << "coopa_mapgen: --relief must be between 0 and 1\n";
                return 1;
            }
        } else if (match_option(argument, "roughness", value)) {
            config.terrain_roughness = std::atof(std::string(value).c_str());
            if (config.terrain_roughness < 0.0 || config.terrain_roughness > 1.0) {
                std::cerr << "coopa_mapgen: --roughness must be between 0 and 1\n";
                return 1;
            }
        } else if (match_option(argument, "temperature", value)) {
            config.temperature_offset = std::atof(std::string(value).c_str());
            if (config.temperature_offset < -1.0 || config.temperature_offset > 1.0) {
                std::cerr << "coopa_mapgen: --temperature must be between -1 and 1\n";
                return 1;
            }
        } else if (match_option(argument, "polar", value)
                   || match_option(argument, "polar-north", value)
                   || match_option(argument, "polar-south", value)) {
            const double extent = std::atof(std::string(value).c_str());
            if (extent < 0.0 || extent > 0.5) {
                std::cerr << "coopa_mapgen: polar extents must be between 0 and 0.5\n";
                return 1;
            }
            // `--polar` sets both; the two sided flags set one each, so a world can
            // carry an ice cap at one end only.
            if (argument.rfind("--polar=", 0) == 0) {
                config.polar_extent_north = extent;
                config.polar_extent_south = extent;
            } else if (argument.rfind("--polar-north=", 0) == 0) {
                config.polar_extent_north = extent;
            } else {
                config.polar_extent_south = extent;
            }
        } else if (match_option(argument, "surface", value)) {
            const std::string mode(value);
            if (mode != "interpolated" && mode != "flat" && mode != "blended") {
                std::cerr << "coopa_mapgen: --surface must be 'interpolated', 'flat' or "
                             "'blended'\n";
                return 1;
            }
            config.elevation_surface = coopa::maps::elevation_surface_from_name(mode);
        } else if (match_option(argument, "blend", value)) {
            config.elevation_blend = std::atof(std::string(value).c_str());
            if (config.elevation_blend < 0.0 || config.elevation_blend > 1.0) {
                std::cerr << "coopa_mapgen: --blend must be between 0 and 1\n";
                return 1;
            }
        } else if (match_option(argument, "blend-variation", value)) {
            config.elevation_blend_variation = std::atof(std::string(value).c_str());
            if (config.elevation_blend_variation < 0.0
                || config.elevation_blend_variation > 1.0) {
                std::cerr << "coopa_mapgen: --blend-variation must be between 0 and 1\n";
                return 1;
            }
        } else if (match_option(argument, "shading", value)) {
            const std::string mode(value);
            if (mode != "elevation" && mode != "hillshade") {
                std::cerr << "coopa_mapgen: --shading must be 'elevation' or 'hillshade'\n";
                return 1;
            }
            config.composite_shading = coopa::maps::composite_shading_from_name(mode);
        } else if (match_option(argument, "config", value)) {
            // Already read in the first pass above; accepted here so it is not
            // reported as an unrecognised argument.
        } else {
            std::cerr << "coopa_mapgen: unrecognised argument '" << argument << "'\n\n";
            print_usage();
            return 1;
        }
    }

    if (config.grid_size < 2) {
        std::cerr << "coopa_mapgen: --grid-size must be at least 2\n";
        return 1;
    }
    if (image_size_override > 0) {
        if (image_size_override < 16) {
            std::cerr << "coopa_mapgen: --image-size must be at least 16\n";
            return 1;
        }
        const double world_meters =
            static_cast<double>(config.grid_size) * config.meters_per_grid_unit;
        config.meters_per_pixel = world_meters / static_cast<double>(image_size_override);
    }
    config.image_size = coopa::maps::derive_image_size(config);

    if (shape_size_m >= 0.0) {
        switch (config.shape.shape) {
            case coopa::maps::MapShape::Circle:
                config.shape.diameter_m = shape_size_m;
                break;
            case coopa::maps::MapShape::Triangle:
                config.shape.edge_length_m = shape_size_m;
                break;
            case coopa::maps::MapShape::Continent:
            case coopa::maps::MapShape::Archipelago:
                // The MEAN diameter of one landmass, not the whole world: an
                // archipelago varies its continents about this and scatters them.
                config.shape.continent_size_m = shape_size_m;
                break;
            case coopa::maps::MapShape::Rectangle:
                config.shape.width_m = shape_size_m;
                config.shape.height_m = shape_size_m;
                break;
        }
    }
    if (shape_height_m >= 0.0) {
        config.shape.height_m = shape_height_m;
    }

    if (!seed_given) {
        std::random_device entropy;
        config.seed = static_cast<int>(entropy() & 0x7fffffffu);
    }
    // The per-pass noise seeds follow the master seed, so one --seed value reproduces the whole
    // map, and the island field scales with the grid so --grid-size controls detail, not the
    // size of the world. Shared with every other host through map_presets.h.
    config = coopa::maps::derived_config(config);

    coopa::debug::Logger logger("mapgen");

    // Owned here rather than inside the library: mapcoopa takes an engine, it
    // does not run one, so that a host with its own thread pool shares it instead
    // of competing with a second.
    std::unique_ptr<coopa::job::JobEngine> engine;
    if (threads != 1) {
        const unsigned int workers = threads > 0 ? static_cast<unsigned int>(threads)
                                                 : std::thread::hardware_concurrency();
        engine = std::make_unique<coopa::job::JobEngine>(std::max(1u, workers));
    }

    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [](std::chrono::steady_clock::time_point from) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from)
            .count();
    };

    coopa::maps::MapGenerator generator(config, logger);
    generator.set_job_engine(engine.get());
    generator.generate_async().wait();
    const double generate_ms = elapsed_ms(started);

    const coopa::maps::BiomePalette palette;
    const std::string yaml_path = out_prefix + ".yaml";

    const auto export_started = std::chrono::steady_clock::now();
    coopa::maps::MapExporter exporter;
    exporter.set_job_engine(engine.get());
    if (!exporter.export_layers(generator.graph(), config, out_prefix, palette, &logger)) {
        std::cerr << "coopa_mapgen: failed to write one or more layers\n";
        return 1;
    }
    const double export_ms = elapsed_ms(export_started);

    const auto yaml_started = std::chrono::steady_clock::now();
    try {
        coopa::maps::save_map(generator.graph(), config, yaml_path);
    } catch (const std::exception& e) {
        std::cerr << "coopa_mapgen: failed to write " << yaml_path << ": " << e.what() << "\n";
        return 1;
    }

    const double yaml_ms = elapsed_ms(yaml_started);

    int population = 0;
    for (const coopa::maps::MapTown& town : generator.graph().towns) {
        population += town.population;
    }

    double cave_metres = 0.0;
    double cave_depth_m = 0.0;
    std::size_t cave_storeys = 0;
    for (const coopa::maps::MapCave& cave : generator.graph().caves) {
        cave_metres += cave.length_m;
        cave_depth_m += coopa::maps::height_to_meters(
            config, cave.surface_at_mouth - cave.deepest);
        cave_storeys = std::max(cave_storeys, cave.levels.size());
    }
    if (!generator.graph().caves.empty()) {
        cave_depth_m /= static_cast<double>(generator.graph().caves.size());
    }

    const double world_km =
        static_cast<double>(config.grid_size) * config.meters_per_grid_unit / 1000.0;
    std::cout << "\nseed:       " << config.seed << "  (rerun with --seed=" << config.seed << ")\n"
              << "world:      " << world_km << " km square, " << config.meters_per_grid_unit
              << " m per cell, " << coopa::maps::map_shape_name(config.shape.shape)
              << " landmass\n"
              << "render:     " << config.image_size << " px square, "
              << config.meters_per_pixel << " m per pixel, "
              << coopa::maps::composite_shading_name(config.composite_shading) << " shading\n"
              << "cells:      " << generator.graph().centers.size() << "\n"
              << "countries:  " << generator.graph().countries.size()
              << "  regions: " << generator.graph().regions.size() << "\n"
              << "towns:      " << generator.graph().towns.size()
              << "  population: " << population << "\n"
              << "landmarks:  " << generator.graph().landmarks.size() << "\n"
              << "caves:      " << generator.graph().caves.size() << "  passage: "
              << static_cast<int>(cave_metres) << " m, mean depth "
              << static_cast<int>(cave_depth_m) << " m, up to " << cave_storeys
              << " storeys\n"
              << "written:    " << coopa::maps::k_map_layer_count << " layers as "
              << out_prefix << "_<layer>.png\n"
              << "            " << yaml_path << "\n"
              << "threads:    " << (engine ? engine->worker_count() : 1u) << "\n"
              << "timings:    generate " << static_cast<int>(generate_ms) << " ms, export "
              << static_cast<int>(export_ms) << " ms, yaml " << static_cast<int>(yaml_ms)
              << " ms" << std::endl;
    if (engine) {
        engine->shutdown();
    }
    return 0;
}

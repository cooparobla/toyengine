#include <toyengine/water/water_yaml.h>

#include <string>

#include <glm/glm.hpp>

#include <toyengine/io/yaml_read.h>

namespace toy {
namespace water {
namespace {

using io::yaml::read;
using io::yaml::read_rgb;

/** @brief An `{r, g, b}` mapping, leaving any channel the node omits at `fallback`. */
glm::vec3 parse_rgb(const fkyaml::node& n, const glm::vec3& fallback) {
    glm::vec3 v = fallback;
    if (n.contains("r")) v.r = n.at("r").get_value<float>();
    if (n.contains("g")) v.g = n.at("g").get_value<float>();
    if (n.contains("b")) v.b = n.at("b").get_value<float>();
    return v;
}

} // namespace

void parse_water_settings(const fkyaml::node& node, WaterBody& w) {
    // FIRST, so every key below overrides it. `fluid: lava` is a bundle of defaults -- colour,
    // density, waves, viscosity, the underwater volume -- and a scene that names one of those
    // as well means to change it, not to have it quietly put back.
    if (node.contains("fluid")) {
        apply_fluid_preset(w, fluid_kind_from_name(node.at("fluid").get_value<std::string>()));
    }
    if (node.contains("emissive_color")) {
        w.emissive_color = parse_rgb(node.at("emissive_color"), w.emissive_color);
    }
    read(node, "emissive_strength", w.emissive_strength);
    read(node, "viscosity", w.viscosity);
    read(node, "temperature_c", w.temperature_c);

    // Render granularity, not look: a body wider than one tile is drawn as several child
    // objects, each its own renderer, draw and per-frame CPU cost. A body whose extent is
    // already the granularity the rest of the world is culled at wants one tile, and only
    // this key can say so -- the 0 default splits at ~32 m however the body was produced.
    read(node, "tile_size", w.tile_size);

    read(node, "wave_amplitude", w.waves.amplitude);
    read(node, "wave_length", w.waves.wavelength);
    read(node, "wave_steepness", w.waves.steepness);
    read(node, "wind_response", w.wind_response);
    if (node.contains("wave_direction")) {
        w.waves.direction = glm::radians(node.at("wave_direction").get_value<float>());
    }

    read(node, "flow_speed", w.flow_speed);
    read(node, "flow_min_speed", w.flow_min_speed);
    read(node, "flow_slope_gain", w.flow_slope_gain);
    read(node, "obstacle_radius", w.obstacle_radius);
    read(node, "wake_length", w.wake_length);

    if (node.contains("deep_color")) w.deep_color = parse_rgb(node.at("deep_color"), w.deep_color);
    if (node.contains("foam_color")) w.foam_color = parse_rgb(node.at("foam_color"), w.foam_color);
    read(node, "foam_amount", w.foam_amount);
    read(node, "shore_foam_depth", w.shore_foam_depth);
    read(node, "edge_fade_depth", w.edge_fade_depth);
    read(node, "ripple_strength", w.ripple_strength);
    read(node, "ripple_scale", w.ripple_scale);
    read(node, "clarity", w.clarity);

    if (node.contains("underwater_color")) w.underwater_color = parse_rgb(node.at("underwater_color"), w.underwater_color);
    read(node, "underwater_visibility", w.underwater_visibility);
    if (node.contains("underwater_absorption")) {
        w.underwater_absorption = parse_rgb(node.at("underwater_absorption"), w.underwater_absorption);
    }
    read(node, "caustics", w.caustics);

    read(node, "density", w.density);
    read(node, "max_depth", w.max_depth);
}

} // namespace water
} // namespace toy

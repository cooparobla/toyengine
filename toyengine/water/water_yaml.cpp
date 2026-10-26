#include <toyengine/water/water_yaml.h>

#include <glm/glm.hpp>

#include <toyengine/core/yaml_read.h>

namespace toy {
namespace water {
namespace {

using core::yaml::read;
using core::yaml::read_rgb;

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

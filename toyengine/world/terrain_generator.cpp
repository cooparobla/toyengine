#include <toyengine/world/terrain_generator.h>

#include <string>

#include <coopa/maps/map_presets.h>
#include <coopa/maps/map_yaml.h>

namespace toy {
namespace world {

namespace {

/** @brief `node` minus every entry equal to `base`'s, recursing into nested mappings. */
fkyaml::node difference(const fkyaml::node& node, const fkyaml::node& base) {
    fkyaml::node out = fkyaml::node::mapping();
    for (const auto& entry : node.get_value<fkyaml::node::mapping_type>()) {
        const std::string key = entry.first.get_value<std::string>();
        const fkyaml::node& value = entry.second;
        if (!base.is_mapping() || !base.contains(key)) {
            out[key] = value;
            continue;
        }
        const fkyaml::node& other = base.at(key);
        if (value.is_mapping() && other.is_mapping()) {
            fkyaml::node nested = difference(value, other);
            if (!nested.get_value<fkyaml::node::mapping_type>().empty()) out[key] = nested;
        } else if (fkyaml::node::serialize(value) != fkyaml::node::serialize(other)) {
            out[key] = value;
        }
    }
    return out;
}

} // namespace

coopa::maps::MapConfig default_generator() {
    coopa::maps::MapConfig config = coopa::maps::standard_config();
    config.terrain_model = coopa::maps::TerrainModel::Natural;
    return config;
}

coopa::maps::MapConfig generator_config_from_node(const fkyaml::node& component) {
    coopa::maps::MapConfig config = default_generator();
    if (!component.is_mapping()) return config;
    // The flat keys of older scenes, read as mapcoopa reads its own.
    fkyaml::node legacy = fkyaml::node::mapping();
    fkyaml::node shape = fkyaml::node::mapping();
    for (const char* key : k_legacy_generator_keys) {
        if (!component.contains(key)) continue;
        const std::string k(key);
        if (k == "shape" || k == "continent_count" || k == "continent_size_m" || k == "irregularity" ||
            k == "coast_detail") {
            shape[k] = component.at(k);
        } else {
            legacy[k] = component.at(k);
        }
    }
    if (!shape.get_value<fkyaml::node::mapping_type>().empty()) legacy["shape"] = shape;
    coopa::maps::apply_config_node(legacy, config);
    if (component.contains("generator")) coopa::maps::apply_config_node(component.at("generator"), config);
    // Derived, never authored: mapcoopa recomputes it from the world's extent.
    config.image_size = coopa::maps::derive_image_size(config);
    return config;
}

fkyaml::node generator_node(const coopa::maps::MapConfig& authored) {
    return difference(coopa::maps::config_to_node(authored), coopa::maps::config_to_node(default_generator()));
}

} // namespace world
} // namespace toy

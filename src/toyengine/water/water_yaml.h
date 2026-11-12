/**
 * @file water_yaml.h
 * @brief A WaterBody's look and behaviour, read from scene YAML.
 *
 * Three things author water with the same keys, which is why this is not private to any of them:
 * a `WaterBody` component, a tiled terrain's `water:` block, and a smooth terrain's. A terrain's
 * water is built per chunk (toyengine/world/terrain_water_mesh.h) but looks the way the terrain
 * says, so all three go through here and cannot drift apart.
 */

#ifndef TOYENGINE_WATER_WATER_YAML_H
#define TOYENGINE_WATER_WATER_YAML_H

#include <fkYAML/node.hpp>

#include <toyengine/water/water_body.h>

namespace toy {
namespace water {

/**
 * @brief Reads a WaterBody's look and behaviour keys (waves, flow, foam, clarity, underwater,
 *        physics) from `node` into `w`, leaving anything the node does not mention alone.
 */
void parse_water_settings(const fkyaml::node& node, WaterBody& w);

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_WATER_YAML_H

/**
 * @file config.h
 * @brief Configuration parameters for scene loading.
 */

#ifndef COOPA_SCENE_CONFIG_H
#define COOPA_SCENE_CONFIG_H

#include <string>

namespace coopa {
namespace scene {

/**
 * @brief Default scene loading parameters.
 */
struct SceneConfig {
    std::string default_scene = "assets/scenes/gi_cornell_box/scene.yaml";
};

} // namespace scene
} // namespace coopa

#endif // COOPA_SCENE_CONFIG_H

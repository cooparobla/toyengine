/**
 * @file terrain_views.h
 * @brief Every terrain in a scene as one world-space query surface, whatever mode each is in.
 *
 * The narrow seam between the terrain components and the systems that only want to know where
 * the ground is: the weather's GroundProbe, the foliage scatter, gameplay. A caller walks the
 * views and asks each one for a height, a normal, a layer weight or the water over a point --
 * without naming a component type, and so without caring whether the ground under it is a
 * terraced tile world or a smooth heightmap.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_VIEWS_H
#define TOYENGINE_WORLD_TERRAIN_VIEWS_H

#include <vector>

#include <glm/glm.hpp>

#include <toyengine/world/terrain_field.h>

namespace coopa {
namespace scene {
class Scene;
class SceneObject;
} // namespace scene
} // namespace coopa

namespace toy {
namespace world {

/**
 * @struct TerrainSceneView
 * @brief One terrain's query surface, and the object that carries it.
 *
 * The owner travels with the view because what a caller needs it for is the components beside
 * the terrain -- `WeatherSurface`, a name, a transform -- and finding it again by matching data
 * pointers is a search the producer has already done.
 */
struct TerrainSceneView {
    TerrainFieldView view;
    const coopa::scene::SceneObject* owner = nullptr;

    explicit operator bool() const { return static_cast<bool>(view); }
};

/**
 * @brief Every active terrain in `scene`, in both modes, with its owner.
 *
 * A terrain whose field has no data yet (a tile world still baking, a grid still loading) is
 * left out, so a caller never has to test for it.
 */
std::vector<TerrainSceneView> terrain_views(const coopa::scene::Scene& scene);

/** @brief The first terrain `world` lies over, or a falsy view. */
TerrainSceneView terrain_view_at(const coopa::scene::Scene& scene, const glm::vec3& world);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_VIEWS_H

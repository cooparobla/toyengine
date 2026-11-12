/**
 * @file point_light.h
 * @brief Scene component representing a Point Light source.
 *
 * A PointLight emits light in all 360 degrees from its world-space position
 * (derived from the owning SceneObject's TransformComponent).
 */

#ifndef GFXCOOPA_ENGINE_COMPONENTS_POINT_LIGHT_H
#define GFXCOOPA_ENGINE_COMPONENTS_POINT_LIGHT_H

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <glm/glm.hpp>

namespace coopa {
namespace gfx {
namespace engine {
namespace components {

/**
 * @class PointLightComponent
 * @brief Component defining a point light source.
 *
 * Example YAML:
 * @code
 * - type: PointLight
 *   color:     { r: 1.0, g: 0.8, b: 0.4 }
 *   intensity: 2.0
 *   range:     15.0
 *   cast_shadows: true
 * @endcode
 */
class PointLightComponent : public coopa::scene::Component {
public:
    PointLightComponent() = default;

    std::string type_name() const override { return "PointLightComponent"; }

    // --- Light parameters ---

    glm::vec3 color        = glm::vec3(1.0f, 1.0f, 1.0f); /**< RGB color. */
    float     intensity    = 1.0f;                        /**< Light intensity multiplier. */
    float     range        = 10.0f;                       /**< Light radius/range of effect. */
    bool      cast_shadows = true;                        /**< Whether this point light casts shadows. */

    // Standard distance attenuation factors: 1 / (constant + linear * d + quadratic * d^2)
    float attenuation_constant  = 1.0f;
    float attenuation_linear    = 0.09f;
    float attenuation_quadratic = 0.032f;

    /**
     * @brief Computes and returns the light's world-space position.
     * @return World position vector (or zero if no owner).
     */
    glm::vec3 get_world_position() const {
        if (!owner) return glm::vec3(0.0f);
        const auto* tc = owner->get_transform();
        if (!tc) return glm::vec3(0.0f);
        glm::mat4 world = tc->get_world_matrix();
        return glm::vec3(world[3]);
    }
};

} // namespace components
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_COMPONENTS_POINT_LIGHT_H

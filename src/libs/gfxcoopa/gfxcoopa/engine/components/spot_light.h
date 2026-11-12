/**
 * @file spot_light.h
 * @brief Scene component representing a Spot Light source.
 *
 * A SpotLight emits light in a cone from its world-space position (derived from
 * the owning SceneObject's TransformComponent, same as PointLight), aimed along
 * an explicit world-space `direction` vector rather than the transform's rotation
 * -- see get_world_direction()'s doc for why.
 */

#ifndef GFXCOOPA_ENGINE_COMPONENTS_SPOT_LIGHT_H
#define GFXCOOPA_ENGINE_COMPONENTS_SPOT_LIGHT_H

#include <algorithm>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <glm/glm.hpp>

namespace coopa {
namespace gfx {
namespace engine {
namespace components {

/**
 * @class SpotLightComponent
 * @brief Component defining a spot (cone) light source.
 *
 * Example YAML:
 * @code
 * - type: SpotLight
 *   direction: { x: 0.4, y: -0.2, z: -1.0 }
 *   color:     { r: 1.0, g: 0.85, b: 0.55 }
 *   intensity: 140.0
 *   range:     20.0
 *   inner_angle: 14.0
 *   outer_angle: 26.0
 *   cast_shadows: true
 * @endcode
 */
class SpotLightComponent : public coopa::scene::Component {
public:
    SpotLightComponent() = default;

    std::string type_name() const override { return "SpotLightComponent"; }

    // --- Light parameters ---

    glm::vec3 color        = glm::vec3(1.0f, 1.0f, 1.0f); /**< RGB color. */
    float     intensity    = 1.0f;                        /**< Light intensity multiplier. */
    float     range        = 10.0f;                       /**< Light radius/range of effect. */

    /** Local-space direction the light rays travel, rotated by the owner's world
     *  rotation in get_world_direction() -- see that method's doc. Default is
     *  straight down, this engine's Z-up convention. */
    glm::vec3 direction = glm::vec3(0.0f, 0.0f, -1.0f);

    /** Half-angle, in degrees, of the cone's full-brightness core. Clamped to
     *  [0, outer_angle] at upload time so the inner cone can never exceed the
     *  outer one. */
    float inner_angle = 20.0f;

    /** Half-angle, in degrees, where the cone falls to zero. Clamped to [1, 89]
     *  at upload time -- 90 or above degenerates the perspective shadow
     *  projection (see ShadowMapTarget::get_spot_matrix()). */
    float outer_angle = 30.0f;

    bool cast_shadows = true; /**< Whether this spot light casts shadows. */

    // Standard distance attenuation factors: 1 / (constant + linear * d + quadratic * d^2)
    // (constant is repurposed as a falloff-sharpness exponent by the renderer's shading
    // loops -- see PointLightComponent's identical field for the same convention.)
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

    /**
     * @brief Computes and returns the light's world-space aim direction.
     *
     * `direction` is authored in the owner's local space (like a Transform's other
     * vector fields) and rotated into world space by the owner's world matrix here,
     * so a spotlight parented under a rotating object turns with it -- unlike
     * DirectionalLightComponent::direction, which is world-space already since a
     * directional light has no meaningful "local space" to rotate out of.
     *
     * Uses mat3(world) rather than the full mat4 (no translation for a direction),
     * and normalizes after multiplying so a non-uniformly scaled parent can't skew
     * the aim into a non-unit vector.
     *
     * @return Normalized world-space direction (falls back to normalize(direction)
     *         when there is no owner/transform).
     */
    glm::vec3 get_world_direction() const {
        if (!owner) return glm::normalize(direction);
        const auto* tc = owner->get_transform();
        if (!tc) return glm::normalize(direction);
        glm::mat3 world_rot = glm::mat3(tc->get_world_matrix());
        return glm::normalize(world_rot * direction);
    }

    /** @brief outer_angle clamped to a projectable range, in degrees. */
    float clamped_outer_angle() const {
        return std::clamp(outer_angle, 1.0f, 89.0f);
    }

    /** @brief inner_angle clamped to [0, clamped outer], in degrees. */
    float clamped_inner_angle() const {
        return std::clamp(inner_angle, 0.0f, clamped_outer_angle());
    }
};

} // namespace components
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_COMPONENTS_SPOT_LIGHT_H

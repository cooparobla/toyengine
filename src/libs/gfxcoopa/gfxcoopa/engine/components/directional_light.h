/**
 * @file directional_light.h
 * @brief Scene component representing a single directional (sun) light.
 *
 * A DirectionalLight has no position — only a direction, color, and intensity.
 * The renderer reads the first active DirectionalLightComponent in the scene
 * each frame and uploads it to the LightUBO.
 *
 * The direction is stored as the world-space vector pointing FROM the light
 * (i.e. the direction light rays travel). The shader negates it to get the
 * "to-light" direction for NdotL.
 */

#ifndef GFXCOOPA_ENGINE_COMPONENTS_DIRECTIONAL_LIGHT_H
#define GFXCOOPA_ENGINE_COMPONENTS_DIRECTIONAL_LIGHT_H

#include <coopa/scene/component.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace coopa {
namespace gfx {
namespace engine {
namespace components {

/**
 * @class DirectionalLightComponent
 * @brief Component that defines a directional (infinite) light source.
 *
 * Add to any SceneObject in the scene hierarchy. The renderer will pick up
 * the first active instance it finds and use it as the scene's primary light.
 *
 * Example YAML:
 * @code
 * - type: DirectionalLight
 *   direction: { x: -0.577, y: -0.577, z: -0.577 }
 *   color:     { r: 1.0, g: 0.95, b: 0.85 }
 *   intensity: 1.2
 *   shadow_intensity: 0.75
 * @endcode
 *
 * Ambient/indirect light is NOT configured per-light: it comes from the
 * renderer's sky gradient (see IndirectParams in render_features.h), scaled
 * and coloured engine-wide, not per directional light.
 */
class DirectionalLightComponent : public coopa::scene::Component {
public:
    // -------------------------------------------------------------------------
    // Light parameters (public for direct modification or scene loader access).
    // -------------------------------------------------------------------------

    /** World-space direction the light rays travel (FROM the light). Normalized. */
    glm::vec3 direction = glm::normalize(glm::vec3(-1.0f, -1.0f, -1.0f));

    /** RGB color of the light. */
    glm::vec3 color = glm::vec3(1.0f, 1.0f, 1.0f);

    /** Scalar intensity multiplier applied to color. */
    float intensity = 1.0f;

    /** Whether this directional light casts shadows. */
    bool cast_shadows = true;

    /**
     * @brief How dark this light's shadows are: 1.0 = full occlusion (the default),
     *        0.0 = the light is unshadowed. Values in between let some direct
     *        light through, which reads far better under a banded/cel-shaded style than a
     *        hard black cutoff. Has no effect when `cast_shadows` is false.
     */
    float shadow_intensity = 1.0f;

    // -------------------------------------------------------------------------
    // Component interface.
    // -------------------------------------------------------------------------

    /**
     * @brief Returns the component type name string.
     */
    std::string type_name() const override { return "DirectionalLightComponent"; }

    /**
     * @brief No per-frame update needed — data is read by the renderer directly.
     */
    void update(float /*dt*/) override {}
};

} // namespace components
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_COMPONENTS_DIRECTIONAL_LIGHT_H

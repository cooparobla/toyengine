/**
 * @file reflection_probe.h
 * @brief Scene component defining a parallax-corrected reflection capture probe.
 */

#ifndef GFXCOOPA_ENGINE_COMPONENTS_REFLECTION_PROBE_H
#define GFXCOOPA_ENGINE_COMPONENTS_REFLECTION_PROBE_H

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <glm/glm.hpp>
#include <string>

namespace coopa {
namespace gfx {
namespace engine {
namespace components {

/// A localized reflection capture probe.
class ReflectionProbeComponent : public coopa::scene::Component {
public:
    ReflectionProbeComponent() = default;
    std::string type_name() const override { return "ReflectionProbeComponent"; }
    void update(float /*dt*/) override {}

    glm::vec3 box_extent     = glm::vec3(5.0f);  // Half-extents of the parallax AABB.
    uint32_t  resolution     = 256;               // Cubemap face resolution.
    float     blend_distance = 1.0f;              // Blend falloff at AABB edges.
    int       importance     = 1;                 // Priority when overlapping probes.
    float     intensity      = 1.0f;               // Multiplier on this probe's indirect specular (ReflectionProbeUniforms::params.z).

    /// Returns world-space AABB min for parallax correction.
    glm::vec3 get_box_min() const {
        glm::vec3 pos = get_world_position();
        return pos - box_extent;
    }

    /// Returns world-space AABB max for parallax correction.
    glm::vec3 get_box_max() const {
        glm::vec3 pos = get_world_position();
        return pos + box_extent;
    }

    /// Returns world-space position from owner's transform.
    glm::vec3 get_world_position() const {
        if (!owner) return glm::vec3(0.0f);
        const auto* tc = owner->get_transform();
        if (!tc) return glm::vec3(0.0f);
        return glm::vec3(tc->get_world_matrix()[3]);
    }
};

} // namespace components
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_COMPONENTS_REFLECTION_PROBE_H

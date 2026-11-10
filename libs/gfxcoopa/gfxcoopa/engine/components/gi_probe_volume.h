/**
 * @file gi_probe_volume.h
 * @brief Scene component defining a 3D grid of SH light probes.
 */

#ifndef GFXCOOPA_ENGINE_COMPONENTS_GI_PROBE_VOLUME_H
#define GFXCOOPA_ENGINE_COMPONENTS_GI_PROBE_VOLUME_H

#include <coopa/scene/component.h>
#include <glm/glm.hpp>
#include <string>
#include <algorithm>

namespace coopa {
namespace gfx {
namespace engine {
namespace components {

/// Update mode for the probe volume.
enum class GiUpdateMode {
    Static,   // Baked once at scene load.
    Dynamic   // Re-baked periodically.
};

/// Defines a 3D grid of SH light probes in world space.
class GiProbeVolumeComponent : public coopa::scene::Component {
public:
    GiProbeVolumeComponent() = default;
    std::string type_name() const override { return "GiProbeVolumeComponent"; }
    void update(float /*dt*/) override {}

    glm::vec3  origin          = glm::vec3(-5.0f, -5.0f, 0.0f);
    glm::vec3  extent          = glm::vec3(10.0f, 10.0f, 4.0f);
    glm::ivec3 grid_resolution = glm::ivec3(8, 8, 4);
    GiUpdateMode update_mode   = GiUpdateMode::Static;
    float      gi_intensity    = 1.0f;

    /// Computed cell spacing: extent / (resolution - 1).
    glm::vec3 spacing() const {
        return glm::vec3(
            extent.x / float(std::max(grid_resolution.x - 1, 1)),
            extent.y / float(std::max(grid_resolution.y - 1, 1)),
            extent.z / float(std::max(grid_resolution.z - 1, 1))
        );
    }

    /// Total number of probes in the grid.
    int total_probes() const {
        return grid_resolution.x * grid_resolution.y * grid_resolution.z;
    }

    /// World position of probe at grid index (ix, iy, iz).
    glm::vec3 probe_position(int ix, int iy, int iz) const {
        glm::vec3 s = spacing();
        return origin + glm::vec3(float(ix) * s.x, float(iy) * s.y, float(iz) * s.z);
    }
};

} // namespace components
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_COMPONENTS_GI_PROBE_VOLUME_H

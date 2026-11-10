/**
 * @file listener.h
 * @brief The virtual listener's pose in world space.
 */

#ifndef SFXCOOPA_SPATIAL_LISTENER_H
#define SFXCOOPA_SPATIAL_LISTENER_H

#include <coopa/util/transform.h>

#include <glm/glm.hpp>

namespace coopa {
namespace sfx {
namespace spatial {

/**
 * @struct Listener
 * @brief World-space position, velocity, and orientation of the audio listener.
 *
 * `forward`/`up` use the same -Z-forward, +Y-up convention as gfxcoopa's
 * camera. basis() returns a rotation matrix whose columns are
 * (right, up, -forward) -- deliberately -forward, not forward, in the third
 * column: with that convention, transpose(basis()) * (source - position)
 * yields a local-space offset where negative Z means "in front of the
 * listener", matching the azimuth formula BinauralPanner uses.
 */
struct Listener {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};

    /** @brief Orthonormal basis (right, up, -forward) as columns. */
    glm::mat3 basis() const {
        const glm::vec3 f = glm::normalize(forward);
        const glm::vec3 right = glm::normalize(glm::cross(f, up));
        const glm::vec3 true_up = glm::cross(right, f);
        return glm::mat3(right, true_up, -f);
    }

    /**
     * @brief Sets position/forward/up from a Transform's current world matrix.
     *
     * Main-thread only: Transform::get_world_matrix() mutates a mutable
     * cache while walking the parent chain, and libcoopa documents that as
     * unsafe to read/write concurrently from separate threads. Velocity is
     * not derivable from a single transform snapshot and is left
     * unmodified -- the caller sets it separately, typically as
     * (position - previous_position) / delta_time (as AudioListener::update
     * does via listener_from_matrix()).
     */
    void set_from_transform(const coopa::util::Transform& transform) {
        const glm::mat4& m = transform.get_world_matrix();
        position = glm::vec3(m[3]);
        // Local +Z maps to m[2]; the -Z-forward convention means world
        // forward is the negation of that column.
        forward = glm::normalize(-glm::vec3(m[2]));
        up = glm::normalize(glm::vec3(m[1]));
    }
};

}  // namespace spatial
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_SPATIAL_LISTENER_H

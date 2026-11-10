/**
 * @file box_collider.h
 * @brief Box-shaped collider component.
 */

#ifndef PHYSXCOOPA_COMPONENTS_BOX_COLLIDER_H
#define PHYSXCOOPA_COMPONENTS_BOX_COLLIDER_H

#include <physxcoopa/components/collider.h>

#include <glm/glm.hpp>

#include <string>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class BoxCollider
 * @brief A box collider. `size` is full extents (Unity semantics) in the object's local,
 *        pre-scale space -- half-extents are computed at bind time.
 *
 * Scale handling: a box scales exactly per-axis from the object's world scale, unlike
 * sphere/capsule which take the max scale component -- a non-uniformly-scaled box is still a
 * (differently-proportioned) box, but a non-uniformly-scaled sphere is not a sphere.
 */
class BoxCollider : public Collider {
public:
    std::string type_name() const override { return "BoxCollider"; }

    const glm::vec3& size() const { return size_; }
    void set_size(const glm::vec3& s) { size_ = s; bump_revision_(); }

    collision::Shape make_shape(const glm::vec3& world_scale) const override {
        glm::vec3 half_extents = 0.5f * size_ * world_scale;
        return collision::Shape::make_box(half_extents, center() * world_scale);
    }

private:
    glm::vec3 size_{1.0f};
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_BOX_COLLIDER_H

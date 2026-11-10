/**
 * @file capsule_collider.h
 * @brief Capsule-shaped collider component.
 */

#ifndef PHYSXCOOPA_COMPONENTS_CAPSULE_COLLIDER_H
#define PHYSXCOOPA_COMPONENTS_CAPSULE_COLLIDER_H

#include <physxcoopa/components/collider.h>

#include <algorithm>
#include <string>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class CapsuleCollider
 * @brief A capsule collider. `height` is the total height including both hemispherical
 *        caps (Unity semantics). Default axis is Z (2), not Unity's Y, given the Z-up world.
 *
 * Takes the MAX component of the object's world scale (Unity's rule), same as SphereCollider.
 */
class CapsuleCollider : public Collider {
public:
    std::string type_name() const override { return "CapsuleCollider"; }

    float radius() const { return radius_; }
    void set_radius(float r) { radius_ = r; bump_revision_(); }

    float height() const { return height_; }
    void set_height(float h) { height_ = h; bump_revision_(); }

    int direction() const { return direction_; }
    void set_direction(int axis) { direction_ = axis; bump_revision_(); }

    collision::Shape make_shape(const glm::vec3& world_scale) const override {
        float scale = std::max({world_scale.x, world_scale.y, world_scale.z});
        float r = radius_ * scale;
        float total_half_height = 0.5f * height_ * scale;
        float cylinder_half_height = std::max(0.0f, total_half_height - r);
        return collision::Shape::make_capsule(r, cylinder_half_height, direction_, center() * world_scale);
    }

private:
    float radius_ = 0.5f;
    float height_ = 2.0f;
    int direction_ = 2; // Z, given the Z-up world
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_CAPSULE_COLLIDER_H

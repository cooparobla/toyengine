/**
 * @file sphere_collider.h
 * @brief Sphere-shaped collider component.
 */

#ifndef PHYSXCOOPA_COMPONENTS_SPHERE_COLLIDER_H
#define PHYSXCOOPA_COMPONENTS_SPHERE_COLLIDER_H

#include <physxcoopa/components/collider.h>

#include <algorithm>
#include <string>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class SphereCollider
 * @brief A sphere collider. Takes the MAX component of the object's world scale (Unity's
 *        rule) -- a non-uniformly-scaled sphere is no longer a sphere.
 */
class SphereCollider : public Collider {
public:
    std::string type_name() const override { return "SphereCollider"; }

    float radius() const { return radius_; }
    void set_radius(float r) { radius_ = r; bump_revision_(); }

    collision::Shape make_shape(const glm::vec3& world_scale) const override {
        float scale = std::max({world_scale.x, world_scale.y, world_scale.z});
        return collision::Shape::make_sphere(radius_ * scale, center() * world_scale);
    }

private:
    float radius_ = 0.5f;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_SPHERE_COLLIDER_H

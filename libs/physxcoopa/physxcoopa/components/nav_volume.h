/**
 * @file nav_volume.h
 * @brief NavVolume -- a box that relabels the area of every walkable surface inside it
 *        (mud, water, a hazard), or cuts it out entirely with area "NotWalkable".
 */

#ifndef PHYSXCOOPA_COMPONENTS_NAV_VOLUME_H
#define PHYSXCOOPA_COMPONENTS_NAV_VOLUME_H

#include <coopa/scene/component.h>

#include <glm/glm.hpp>

#include <string>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class NavVolumeComponent
 * @brief An oriented box in the object's local space (`center`, full `size`), scaled and
 *        rotated by the object's transform. Moving it rebuilds the tiles it leaves and enters.
 */
class NavVolumeComponent : public coopa::scene::Component {
public:
    std::string type_name() const override { return "NavVolume"; }

    glm::vec3 size{1.0f};
    glm::vec3 center{0.0f};
    /** @brief Area name or id; "NotWalkable" (id 0) removes the surface. */
    std::string area = "Walkable";
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_NAV_VOLUME_H

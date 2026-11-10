/**
 * @file nav_link.h
 * @brief NavLink -- an off-mesh link: a jump, drop, ladder or teleporter connecting two points
 *        the voxel surface can't connect on its own.
 */

#ifndef PHYSXCOOPA_COMPONENTS_NAV_LINK_H
#define PHYSXCOOPA_COMPONENTS_NAV_LINK_H

#include <coopa/scene/component.h>

#include <glm/glm.hpp>

#include <string>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class NavLinkComponent
 * @brief `start` and `end` are in the object's local space. Each snaps to the walkable surface
 *        within `snap_radius`; a link whose end can't snap is drawn red and ignored. Agents
 *        cross a link on a short arc (CrowdAgentParams::link_seconds_per_metre).
 */
class NavLinkComponent : public coopa::scene::Component {
public:
    std::string type_name() const override { return "NavLink"; }

    glm::vec3 start{0.0f};
    glm::vec3 end{0.0f, 1.0f, 0.0f};
    /** @brief False makes a one-way link (a drop you can't climb back up). */
    bool bidirectional = true;
    /** @brief Multiplier on the link's length when planning. */
    float cost = 1.0f;
    std::string area = "Walkable";
    float snap_radius = 1.0f;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_NAV_LINK_H

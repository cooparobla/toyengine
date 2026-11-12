/**
 * @file nav_modifier.h
 * @brief NavModifier -- how an object's colliders count for navigation: the area their
 *        walkable surfaces get, whether anything may stand on them at all, whether they are
 *        ignored, and whether a moving (dynamic) body carves the navmesh once it comes to rest.
 */

#ifndef PHYSXCOOPA_COMPONENTS_NAV_MODIFIER_H
#define PHYSXCOOPA_COMPONENTS_NAV_MODIFIER_H

#include <coopa/scene/component.h>

#include <string>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class NavModifierComponent
 * @brief Without one, a collider contributes to navigation when it is enabled, not a trigger,
 *        on a navigation layer and owned by a static or kinematic body; its flat-enough tops
 *        are "Walkable". Applies to the object's own colliders and, when `apply_to_children`,
 *        to every descendant's (a nearer NavModifier wins).
 */
class NavModifierComponent : public coopa::scene::Component {
public:
    std::string type_name() const override { return "NavModifier"; }

    /** @brief Area name (from navigation settings' `areas:`) or id for walkable surfaces. */
    std::string area = "Walkable";
    /** @brief False: the colliders block and carve, but their tops are never walkable. */
    bool walkable = true;
    /** @brief True: the colliders are invisible to navigation. */
    bool ignore = false;
    /** @brief Dynamic bodies only: carve the navmesh while the body sleeps (at rest). A moving
     *         body never carves -- crowds steer around it instead -- so a pushed crate updates
     *         the mesh once, where it stops. */
    bool carve = false;
    bool apply_to_children = true;

    /** @brief Bump after changing fields at runtime so NavSystem re-reads them. */
    void mark_changed() { ++revision_; }
    uint32_t revision() const { return revision_; }

private:
    uint32_t revision_ = 0;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_NAV_MODIFIER_H

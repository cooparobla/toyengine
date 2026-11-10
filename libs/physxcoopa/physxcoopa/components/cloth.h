/**
 * @file cloth.h
 * @brief ClothComponent -- Scene-authorable cloth sheet, resolved to a cloth::Cloth inside
 *        PhysicsWorld by PhysicsSystem at bind time.
 *
 * The component holds only AUTHORING data (grid dimensions, tunables, anchor descriptions by
 * object name). The simulated state -- particles, constraints, tethers -- lives in the
 * PhysicsWorld, reachable through cloth(). That split matters for two consumers: a renderer
 * needs the live particle positions every frame and gets them without a copy, and the solver
 * never has to walk the scene graph.
 *
 * Anchor object names are resolved by PhysicsSystem::regather_cloths_(), NOT by the YAML parser.
 * The parser runs while the hierarchy is still being built, so a sibling named in YAML may not
 * exist yet -- the same rule HingeJointComponent's `connected_object` follows.
 */

#ifndef PHYSXCOOPA_COMPONENTS_CLOTH_H
#define PHYSXCOOPA_COMPONENTS_CLOTH_H

#include <physxcoopa/cloth/cloth.h>
#include <physxcoopa/world.h>

#include <coopa/scene/component.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace coopa {
namespace physx {
namespace components {

/**
 * @struct ClothAnchorSpec
 * @brief Authoring description of one pinned region: "pin every particle within `radius` of
 *        `point` to the object named `object`".
 *
 * A region rather than an explicit particle list, because a particle index is not a stable thing
 * to author against -- changing the sheet's resolution would silently repoint every index, while
 * a world-space region keeps meaning the same thing. This is also how the common cases read
 * naturally: a cape is "the patch over the shoulders", a flag is "the left edge".
 */
struct ClothAnchorSpec {
    /** @brief SceneObject name whose Rigidbody the pinned particles follow (Scene::find_object()).
     *         Empty pins them in place instead, with no body to follow. */
    std::string object;

    /** @brief Centre of the pinned region, in `object`'s LOCAL frame when `object` is set (so
     *         `{0,0,1}` is "the top of a unit-radius ball" regardless of where the ball is), or
     *         in world space when it is empty. */
    glm::vec3 point{0.0f};

    /** @brief Radius of the pinned region in world units. */
    float radius = 0.25f;
};

/**
 * @class ClothComponent
 * @brief A rectangular XPBD cloth sheet owned by the object this is attached to.
 *
 * The sheet is built in the owner's local XY plane (normal +Z) at `size`, centred on the owner's
 * Transform, so moving/rotating that Transform in a scene file places the sheet -- exactly like
 * any other authored geometry. After that the particles live in WORLD space and the Transform is
 * no longer their parent: the owner's Transform is a spawn pose, not a per-frame one. A renderer
 * wanting object-space vertices divides back out by the owner's world matrix.
 *
 * Example YAML:
 * @code
 * - type: Cloth
 *   resolution: { x: 25, y: 25 }
 *   size: { x: 4.0, y: 4.0 }
 *   mass: 1.0
 *   bend_compliance: 0.00005
 *   damping: 0.08
 *   thickness: 0.03
 *   friction: 0.5
 *   self_collision: true
 *   wind: { x: 0.6, y: 0.0, z: 0.0 }
 *   wind_turbulence: 0.8
 *   anchors:
 *     - object: ball
 *       point: { x: 0.0, y: 0.0, z: 1.0 }
 *       radius: 0.4
 * @endcode
 *
 * Like HingeJointComponent, binding is creation-time only: editing `resolution`/`size`/`anchors`
 * after the cloth exists has no effect until the component is removed and re-added. The runtime
 * tunables in `params` are the exception -- they are read fresh every substep, so wind, friction,
 * damping and gravity_scale can all be driven live through cloth()->params.
 */
class ClothComponent : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Cloth"; }

    uint32_t columns = 21;          /**< Particles along the owner's local X. */
    uint32_t rows = 21;             /**< Particles along the owner's local Y. */
    float width = 2.0f;             /**< Sheet extent along local X, before the owner's scale. */
    float height = 2.0f;            /**< Sheet extent along local Y, before the owner's scale. */
    float mass = 1.0f;              /**< Total sheet mass in kg, split evenly across particles. */
    bool shear = true;              /**< Emit diagonal shear constraints; see GridClothDesc. */
    cloth::ClothParams params;      /**< Solver tunables; see cloth/cloth.h. */
    /** @brief How much of the host engine's weather wind (gusts included) blows this cloth, on
     *         top of `params.wind`: 1 = all of it (the default), 0 = only the authored wind. Read
     *         by the host (toyengine's WindSystem), which writes the live cloth's params.wind. */
    float weather_wind = 1.0f;
    std::vector<ClothAnchorSpec> anchors; /**< Pinned regions, resolved at bind time. */

    /** @brief Set only by PhysicsSystem's reconcile pass, once the cloth is actually created. */
    void set_cloth_binding(PhysicsWorld* world, cloth::ClothId id) {
        world_ = world;
        cloth_id_ = id;
    }

    /** @brief Handle of the simulated cloth, or an invalid id if this component never bound. */
    cloth::ClothId cloth_id() const { return cloth_id_; }

    /** @brief The world this cloth is simulated in, or nullptr if unbound. */
    PhysicsWorld* world() const { return world_; }

    /** @brief The live simulated cloth -- particle positions, triangles, bounds -- or nullptr if
     *         this component never bound. This is the renderer's read path: the particle array it
     *         returns is the solver's own, not a copy, and is current as of the last
     *         PhysicsWorld::step(). */
    const cloth::Cloth* cloth() const { return world_ ? world_->get_cloth(cloth_id_) : nullptr; }

    /** @brief Mutable access, for driving tunables (wind, friction, gravity_scale) at runtime --
     *         those are re-read every substep, unlike the creation-time grid/anchor fields. */
    cloth::Cloth* cloth_mut() { return world_ ? world_->get_cloth(cloth_id_) : nullptr; }

private:
    PhysicsWorld* world_ = nullptr;
    cloth::ClothId cloth_id_;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_CLOTH_H

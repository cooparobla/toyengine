/**
 * @file rigidbody.h
 * @brief Dynamics-configuration component -- mass, drag, kinematic flag, constraints -- plus
 *        the Unity-style force/velocity API gameplay code calls directly.
 *
 * A RigidbodyComponent only takes effect when a Collider is also present on the same
 * SceneObject or a descendant (a compound body): PhysicsSystem's reconcile pass is keyed on
 * Collider (a body needs a shape to exist in the binding layer, even though PhysicsWorld itself supports a shapeless body --
 * see Shape::enabled's doc). A bare Rigidbody with no Collider is a known v1 limitation, not
 * a bug: Unity's "Rigidbody with no Collider falls under gravity but hits nothing" case isn't
 * reachable from scene YAML today, only via PhysicsWorld::add_body() called directly.
 */

#ifndef PHYSXCOOPA_COMPONENTS_RIGIDBODY_H
#define PHYSXCOOPA_COMPONENTS_RIGIDBODY_H

#include <physxcoopa/world.h>
#include <physxcoopa/dynamics/body.h>
#include <physxcoopa/util/transform_bridge.h>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class RigidbodyComponent
 * @brief Configures a body's dynamics; also the gameplay-facing force/velocity API.
 *
 * `world_`/`body_id_` are set only by PhysicsSystem's reconcile pass (see set_body_binding()),
 * mirroring Collider::set_body_id() -- this is what lets add_force() etc. work without this
 * header depending on system/physics_system.h (which itself must depend on this header to
 * read mass/drag/kinematic during reconcile; a dependency the other way would cycle).
 */
class RigidbodyComponent : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Rigidbody"; }

    float mass = 1.0f;
    float drag = 0.0f;
    float angular_drag = 0.05f;
    bool use_gravity = true;
    bool is_kinematic = false;
    dynamics::Interpolation interpolation = dynamics::Interpolation::Interpolate;
    uint32_t constraints = dynamics::kConstraintNone;
    /** @brief How hard the host engine's weather wind pushes this body (kg/m: roughly 0.6 x the
     *         area it shows the wind, m^2). 0 = ignores the wind (the default). Read by the host
     *         (toyengine's WindSystem), which applies influence * |rel| * rel each substep, rel =
     *         the wind minus the body's horizontal velocity. */
    float wind_influence = 0.0f;

    /** @brief Initial linear/angular velocity, applied once when PhysicsSystem's reconcile
     *         pass creates this Rigidbody's body (Dynamic bodies only -- a Kinematic body's
     *         velocity is always derived from its transform's frame-to-frame delta instead,
     *         see PhysicsSystem::sync_transforms_in_()). Ignored after bind; use set_velocity()
     *         for runtime changes. */
    glm::vec3 initial_velocity{0.0f};
    glm::vec3 initial_angular_velocity{0.0f};

    /** @brief Local-space offset from the collider's own center (Collider::center(), already
     *         folded into body.position -- see PhysicsSystem::create_compound_body_()'s doc) to
     *         the body's true center of mass, applied ON TOP of that folding at bind time. Unset
     *         by default (no override -- the body's center of mass is exactly the collider's
     *         center). Matches Unity's
     *         Rigidbody.centerOfMass. Creation-time only, like initial_velocity above -- not
     *         reconciled by PhysicsSystem's per-frame runtime-property pass. */
    std::optional<glm::vec3> center_of_mass_override;

    /** @brief Diagonal local-space inverse inertia tensor to use instead of the one
     *         PhysicsSystem::create_compound_body_() would otherwise derive analytically from the
     *         collider's shape (dynamics::inertia_for_shape()). Matches Unity's
     *         Rigidbody.inertiaTensor, with the same INVERSE convention Body::inv_inertia_local
     *         already uses everywhere else in this engine (not Unity's own non-inverse
     *         convention -- invert a value copied from Unity before setting this). Creation-time
     *         only, like center_of_mass_override above. */
    std::optional<glm::vec3> inertia_tensor_override;

    /** @brief Called only by PhysicsSystem's reconcile pass. */
    void set_body_binding(PhysicsWorld* world, dynamics::BodyId id) {
        world_ = world;
        body_id_ = id;
    }

    dynamics::BodyId body_id() const { return body_id_; }

    /** @brief Called only by PhysicsSystem when it (re)computes this body's center offset --
     *         see local_center_of_mass(). */
    void set_local_center_of_mass(const glm::vec3& offset) { local_center_of_mass_ = offset; }

    /** @brief World-scaled, local-space (pre-rotation) offset from the owning Transform's pivot
     *         to the body's true center of mass -- exactly PhysicsSystem's Binding::center_offset
     *         for this body (collider center(s), compound composition and
     *         center_of_mass_override all folded in). Zero until bound. A point authored in the
     *         owner's (world-scaled) local frame maps to world space via the live body pose as
     *         `world_center_of_mass() + rotation() * (p_local - local_center_of_mass())`. */
    glm::vec3 local_center_of_mass() const { return local_center_of_mass_; }

    /** @brief The body's live (un-interpolated, current-substep) center of mass. Falls back to
     *         the owner's Transform pivot + offset when unbound. */
    glm::vec3 world_center_of_mass() const {
        if (const dynamics::Body* b = body_()) return b->position;
        if (!owner) return local_center_of_mass_;
        util::Trs trs = util::world_trs(owner->get_transform()->transform());
        return trs.position + trs.rotation * local_center_of_mass_;
    }

    /** @brief The body's live (un-interpolated) orientation; identity when unbound. */
    glm::quat rotation() const {
        if (const dynamics::Body* b = body_()) return b->orientation;
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }

    /** @brief See Body::velocity_at_point(). Zero when unbound. */
    glm::vec3 velocity_at_point(const glm::vec3& world_point) const {
        const dynamics::Body* b = body_();
        return b ? b->velocity_at_point(world_point) : glm::vec3(0.0f);
    }

    /** @brief See Body::apply_impulse_at_position(). */
    void add_impulse_at_position(const glm::vec3& impulse, const glm::vec3& world_point) {
        if (dynamics::Body* b = body_()) b->apply_impulse_at_position(impulse, world_point);
    }

    void add_force(const glm::vec3& f) {
        if (dynamics::Body* b = body_()) b->add_force(f);
    }
    void add_torque(const glm::vec3& t) {
        if (dynamics::Body* b = body_()) b->add_torque(t);
    }
    void add_force_at_position(const glm::vec3& f, const glm::vec3& world_point) {
        if (dynamics::Body* b = body_()) b->add_force_at_position(f, world_point);
    }

    /** @brief Instantaneous world-space impulse at the center of mass -- see
     *         Body::apply_impulse()'s doc for how this differs from add_force(). */
    void add_impulse(const glm::vec3& impulse) {
        if (dynamics::Body* b = body_()) b->apply_impulse(impulse);
    }
    /** @brief Instantaneous world-space angular impulse -- see Body::apply_angular_impulse(). */
    void add_torque_impulse(const glm::vec3& angular_impulse) {
        if (dynamics::Body* b = body_()) b->apply_angular_impulse(angular_impulse);
    }

    glm::vec3 velocity() const {
        const dynamics::Body* b = body_();
        return b ? b->linear_velocity : glm::vec3(0.0f);
    }
    /** @brief Wakes a sleeping Dynamic body, same reasoning as Body::add_force()'s doc -- an
     *         explicit velocity write is exactly the kind of script-driven change a sleeping
     *         body should react to, not silently ignore. */
    void set_velocity(const glm::vec3& v) {
        if (dynamics::Body* b = body_()) {
            b->linear_velocity = v;
            if (b->type == dynamics::BodyType::Dynamic) b->wake();
        }
    }

    glm::vec3 angular_velocity() const {
        const dynamics::Body* b = body_();
        return b ? b->angular_velocity : glm::vec3(0.0f);
    }
    /** @brief See set_velocity()'s doc. */
    void set_angular_velocity(const glm::vec3& w) {
        if (dynamics::Body* b = body_()) {
            b->angular_velocity = w;
            if (b->type == dynamics::BodyType::Dynamic) b->wake();
        }
    }

    /** @brief True if this Rigidbody's body is currently asleep (skipped by the solver/
     *         integrator until woken -- see Body::awake's doc). False if unbound. */
    bool is_sleeping() const {
        const dynamics::Body* b = body_();
        return b && !b->awake;
    }
    /** @brief Wakes the body immediately -- see Body::wake(). No-op if unbound. */
    void wake() {
        if (dynamics::Body* b = body_()) b->wake();
    }
    /** @brief Puts the body to sleep immediately -- see Body::sleep(). No-op if unbound. Note
     *         this can be immediately undone the next substep if the body's island still has an
     *         awake member touching it (same as every other sleep/wake rule in this engine --
     *         see update_islands_and_sleep()'s doc in dynamics/solver.h). */
    void sleep() {
        if (dynamics::Body* b = body_()) b->sleep();
    }

    /**
     * @brief Moves a kinematic body's transform smoothly -- for v1, this is a direct alias
     *        for writing the owning object's Transform position; PhysicsSystem's sync-in
     *        pass derives the correct kinematic velocity from the resulting frame-to-frame
     *        position delta, exactly as if a script had called Transform::set_position()
     *        directly. Provided under this name for Unity-API familiarity.
     */
    void move_position(const glm::vec3& world_position) {
        if (!owner) return;
        coopa::util::Transform& t = owner->get_transform()->transform();
        util::set_world_trs(t, world_position, t.rotation_quat());
    }

    /** @brief See move_position()'s doc -- the rotational equivalent. */
    void move_rotation(const glm::quat& world_rotation) {
        if (!owner) return;
        coopa::util::Transform& t = owner->get_transform()->transform();
        util::set_world_trs(t, util::world_trs(t).position, world_rotation);
    }

private:
    dynamics::Body* body_() {
        return world_ ? world_->get_body(body_id_) : nullptr;
    }
    const dynamics::Body* body_() const {
        return world_ ? world_->get_body(body_id_) : nullptr;
    }

    PhysicsWorld* world_ = nullptr;
    dynamics::BodyId body_id_;
    glm::vec3 local_center_of_mass_{0.0f};
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_RIGIDBODY_H

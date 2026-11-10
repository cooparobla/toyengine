/**
 * @file collider.h
 * @brief Base class for every collider component -- passive descriptor data only, no
 *        body-creation logic. PhysicsSystem (system/physics_system.h) is the only thing
 *        that ever creates or destroys a physics body from a Collider.
 */

#ifndef PHYSXCOOPA_COMPONENTS_COLLIDER_H
#define PHYSXCOOPA_COMPONENTS_COLLIDER_H

#include <physxcoopa/collision/shape.h>
#include <physxcoopa/dynamics/body.h>
#include <physxcoopa/dynamics/physics_material.h>

#include <coopa/scene/component.h>
#include <coopa/event/signal.h>
#include <coopa/asset/asset_handle.h>

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <memory>

namespace coopa {
namespace physx {
namespace components {

struct Collision; // collision.h -- only ever named by reference in a Signal below.

/**
 * @class Collider
 * @brief Base for BoxCollider/SphereCollider/CapsuleCollider/MeshCollider.
 *
 * A Collider is pure data until PhysicsSystem's reconcile pass binds it to a body -- it
 * overrides Component::start() with nothing, because SceneLoader::load() calls Scene::start()
 * itself, before the app has a chance to call install_physics_system() (see the plan's
 * "Physics components override start() with nothing").
 *
 * `revision_` is bumped by every setter; PhysicsSystem compares it against the value at last
 * reconcile to decide whether to rebuild this collider's shape (see PhysicsSystem's doc for
 * why a full rebuild-on-any-change, copying AnimationSystem's pattern, would be wrong here --
 * a collider owns a live broadphase proxy, warm-start impulses and a sleep timer that must
 * survive an unrelated collider's parameter change elsewhere in the scene).
 */
class Collider : public coopa::scene::Component {
public:
    /** @brief Fired when this trigger-collider-free collider starts touching another --
     *         carries contact geometry/impulse (see components/collision.h's Collision). */
    coopa::event::Signal<Collider&, const Collision&> on_collision_enter;
    /** @brief Fired every step this collider remains touching another. */
    coopa::event::Signal<Collider&, const Collision&> on_collision_stay;
    /** @brief Fired when this collider stops touching another -- Collision::contact_count == 0. */
    coopa::event::Signal<Collider&, const Collision&> on_collision_exit;
    /** @brief Fired when a trigger collider starts overlapping another. */
    coopa::event::Signal<Collider&, Collider&> on_trigger_enter;
    /** @brief Fired every step a trigger collider remains overlapping another. */
    coopa::event::Signal<Collider&, Collider&> on_trigger_stay;
    /** @brief Fired when a trigger collider stops overlapping another. */
    coopa::event::Signal<Collider&, Collider&> on_trigger_exit;

    void start() override { /* intentionally empty -- see class doc */ }

    bool is_trigger() const { return is_trigger_; }
    void set_is_trigger(bool v) { is_trigger_ = v; ++revision_; }

    /** @brief Unity's Collider.enabled -- false makes PhysicsSystem hand the body a disabled
     *         Shape (participates in dynamics, never in collision) without destroying the
     *         body/binding the way removing the Collider component entirely would. */
    bool is_enabled() const { return enabled_; }
    void set_enabled(bool v) { enabled_ = v; ++revision_; }

    uint32_t layer() const { return layer_; }
    void set_layer(uint32_t v) { layer_ = v; ++revision_; }

    const glm::vec3& center() const { return center_; }
    void set_center(const glm::vec3& v) { center_ = v; ++revision_; }

    /**
     * @brief The effective material for this collider, resolved asset-first: an
     *        AssetManager-backed material set via set_material_asset() (YAML `material: <name>`)
     *        takes priority over one set via set_material() (YAML inline `material: {...}` or a
     *        direct C++ call); nullptr (PhysicsMaterial::default_material()) if neither is set,
     *        or the asset handle hasn't finished loading yet.
     */
    const dynamics::PhysicsMaterial* material() const {
        if (material_asset_.is_valid() && material_asset_.is_loaded()) return material_asset_.get();
        if (inline_material_) return inline_material_.get();
        return nullptr;
    }

    /** @brief Points this collider at an AssetManager-owned, shareable PhysicsMaterial. */
    void set_material_asset(coopa::asset::AssetHandle<dynamics::PhysicsMaterial> handle) {
        material_asset_ = std::move(handle);
        ++revision_;
    }
    const coopa::asset::AssetHandle<dynamics::PhysicsMaterial>& material_asset() const { return material_asset_; }

    /** @brief Sets an inline (not AssetManager-tracked) material -- from a YAML inline mapping
     *         or direct C++ code. Ignored while a valid material_asset() is set. */
    void set_material(std::shared_ptr<const dynamics::PhysicsMaterial> m) {
        inline_material_ = std::move(m);
        ++revision_;
    }

    /** @brief Bumped by every setter above (and by a subclass's own shape-parameter setters). */
    uint32_t revision() const { return revision_; }

    /**
     * @brief This collider instance's identity, unique for the life of the process.
     *
     * Its ADDRESS is not an identity: a destroyed collider's block is routinely handed straight
     * back to the next one, which a world that streams collider objects in and out does many
     * times a second. Anything that caches state per collider across frames -- PhysicsSystem's
     * bindings -- must compare this, or it will mistake a new collider for the one that was
     * freed and keep bookkeeping (a body, an owning object) that belongs to a dead object.
     */
    std::uint64_t instance_id() const { return instance_id_; }

    /** @brief The physics body this collider is currently bound to, or an invalid id before
     *         PhysicsSystem's first reconcile pass. Set only by PhysicsSystem. */
    dynamics::BodyId body_id() const { return body_id_; }

    /** @brief Called only by PhysicsSystem's reconcile pass. */
    void set_body_id(dynamics::BodyId id) { body_id_ = id; }

    /**
     * @brief Builds this collider's world-scale Shape.
     *
     * @param world_scale The owning SceneObject's world-space scale (per-axis for boxes,
     *                     max-component for spheres/capsules -- Unity's rule, since a
     *                     non-uniformly-scaled sphere/capsule is no longer that shape).
     * @return A Shape ready to hand to PhysicsWorld::add_body()/set_shape().
     */
    virtual collision::Shape make_shape(const glm::vec3& world_scale) const = 0;

protected:
    /** @brief Increments revision_ -- exposed so a subclass's own setters can bump it too. */
    void bump_revision_() { ++revision_; }

private:
    /** @brief Hands out instance_id(); monotonic, and atomic because colliders are built on
     *         whatever thread loads or spawns their object. */
    static std::uint64_t next_instance_id_() {
        static std::atomic<std::uint64_t> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    bool is_trigger_ = false;
    bool enabled_ = true;
    uint32_t layer_ = 0;
    glm::vec3 center_{0.0f};
    coopa::asset::AssetHandle<dynamics::PhysicsMaterial> material_asset_;
    std::shared_ptr<const dynamics::PhysicsMaterial> inline_material_;

    uint32_t revision_ = 0;
    dynamics::BodyId body_id_;
    const std::uint64_t instance_id_ = next_instance_id_();
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_COLLIDER_H

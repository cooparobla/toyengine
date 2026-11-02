/**
 * @file damageable.h
 * @brief Marks an object as able to take typed damage: health, a localized DamageField, and the
 *        settings DestructionSystem reads to carry out what a saturated blow calls for.
 *
 * The scene-authored half, same split as Destructible/destruction_system.h: no geometry edits
 * happen here, so this stays a plain-ish data component a tool can edit and a test can drive.
 * `apply_damage()` only QUEUES a blow -- nothing mutates a mesh from inside a signal, the same
 * rule physxcoopa's own Vehicle README states, because a slot that edited geometry mid-solve
 * would do so halfway through a physics step. DestructionSystem drains the queue once per pass.
 *
 * **What is implemented today**: health tracking, the localized DamageField (so repeated hits
 * in one spot read as progressively more damaged), and two of the five responses
 * damage::DamageResponse can call for -- Cleave (via DestructionSystem::slice_object(), once the
 * field's saturated region reads flat enough) and Fracture (via a sibling Destructible's
 * shatter(), if one exists). **Dent, Chip and Hole are not yet implemented** -- they would
 * edit the mesh's own vertices in place, which needs a per-object private mesh clone (the
 * shared asset is not safe to deform) that this stage does not build; a blow that resolves to
 * one of those three still costs HP and marks the field, but changes no geometry.
 *
 * Example YAML:
 * @code
 * - type: Damageable
 *   material: wood           # damage_materials/<name>.yaml; empty uses the engine default
 *   hit_points: 0             # 0 takes the material's own
 *   damage_resolution: 16     # cells on the longest axis of the damage field
 *   cleave: true               # allow a saturated slash to cut the object in two
 *   fracture_on_death: true    # shatter when HP reaches 0 (needs a sibling Destructible)
 *   localized_fracture: false  # Destructible has no localized-region fracture yet; always whole-object
 *   min_impulse: 200           # N*s below which a contact is a settling touch, not a blow
 * @endcode
 */

#ifndef TOYENGINE_SCENE_DAMAGEABLE_H
#define TOYENGINE_SCENE_DAMAGEABLE_H

#include <physxcoopa/damage/damage_types.h>

#include <coopa/event/signal.h>
#include <coopa/scene/component.h>

#include <algorithm>
#include <string>
#include <vector>

#include <toyengine/destruct/damage_field.h>

namespace coopa { namespace physx { namespace damage { struct ResolvedDamage; } } }

namespace toy {
namespace scene {

/**
 * @class Damageable
 * @brief An object that accumulates typed damage; see this file's doc for what DestructionSystem
 *        actually carries out today.
 */
class Damageable : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Damageable"; }

    /** @brief damage_materials/<name>.yaml; empty uses DamageMaterial::default_material(). */
    std::string material;

    /** @brief Overrides the material's own hit_points; 0 takes the material's. */
    float hit_points = 0.0f;

    /** @brief Cells on the longest axis of the local DamageField, allocated on first hit. */
    int damage_resolution = 16;

    /** @brief Allow a saturated slash streak to cut the object in two (DestructionSystem::
     *         slice_object(), along DamageField::saturated_plane()). */
    bool cleave = true;

    /** @brief Shatter (via a sibling Destructible) once health reaches 0, or on a single blow
     *         that resolves to DamageResponse::Fracture. A Damageable with no Destructible
     *         sibling simply cannot fracture -- the blow still costs HP. */
    bool fracture_on_death = true;

    /** @brief Reserved: break off only the saturated region rather than the whole object.
     *         Not implemented -- Destructible's fracture has no localized-region mode yet, so
     *         this flag is currently inert. */
    bool localized_fracture = false;

    /** @brief Impulse (N*s) below which a contact is a settling touch, not a blow, and is
     *         dropped before it ever reaches resolve_damage(). Separate from
     *         resolve_damage()'s own internal floor, which exists for non-impulse-derived
     *         blows too (a scripted Slash with a tiny `amount`, say). */
    float min_impulse = 200.0f;

    /**
     * @brief Listens for impacts on the owner's collider; converts them to an Impact blow
     *         (physxcoopa's damage::impact_blow()) unless the OTHER collider carries a
     *         DamageDealer, in which case that declares the type instead (a swung axe reads as
     *         Slash, not Impact).
     */
    void start() override;

    /** @brief Queues a blow for DestructionSystem's next pass to resolve and carry out. */
    void apply_damage(const coopa::physx::damage::DamageEvent& blow) { queued_.push_back(blow); }

    float health() const { return current_hp_; }
    float health_fraction() const { return max_hp_ > 0.0f ? current_hp_ / max_hp_ : 0.0f; }
    const destruct::DamageField& field() const { return field_; }
    bool destroyed() const { return destroyed_; }

    coopa::event::Signal<const coopa::physx::damage::ResolvedDamage&> on_damaged;
    coopa::event::Signal<> on_destroyed;

    // --- DestructionSystem's own state; not for scenes to set ---

    std::vector<coopa::physx::damage::DamageEvent>& queued() { return queued_; }
    void clear_queue() { queued_.clear(); }
    destruct::DamageField& field_mut() { return field_; }
    bool field_ready() const { return field_ready_; }
    void mark_field_ready() { field_ready_ = true; }
    /** @brief Starts health at `hit_points` (or the material's own, if 0) the first time
     *         DestructionSystem resolves a blow against this object. */
    void start_health(float hp) {
        if (health_started_) return;
        health_started_ = true;
        max_hp_ = hp;
        current_hp_ = hp;
    }
    void apply_hp_loss(float loss) {
        current_hp_ = std::max(0.0f, current_hp_ - loss);
        if (current_hp_ <= 0.0f) destroyed_ = true;
    }

private:
    coopa::event::ScopedConnection impact_;
    std::vector<coopa::physx::damage::DamageEvent> queued_;
    destruct::DamageField field_;
    bool  field_ready_ = false;
    bool  health_started_ = false;
    float max_hp_ = 0.0f;
    float current_hp_ = 0.0f;
    bool  destroyed_ = false;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_DAMAGEABLE_H

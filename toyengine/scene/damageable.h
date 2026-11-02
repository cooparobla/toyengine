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
 * in one spot read as progressively more damaged), and four of the five responses
 * damage::DamageResponse can call for. Dent and Chip (`dent`) push vertices inward on a private
 * per-object mesh clone DestructionSystem builds the first time one lands -- the shared asset
 * stays untouched, since every other instance of the same mesh must not deform with it. Cleave
 * cuts the object in two (DestructionSystem::slice_object(), once the field's saturated region
 * reads flat enough) and Fracture shatters it (via a sibling Destructible's shatter(), if one
 * exists). **Hole is not implemented** -- punching all the way through is a topology change,
 * which the dent clone's fixed index buffer cannot express; the blow still costs HP and marks
 * the field. Three further limits, all deliberate: the object's collider does not follow a
 * dent (it is centimetres against a decimetre-scale authored shape); a fractured or sliced
 * object's chunks come from the mesh FILE, so a crushed object shatters into un-crushed debris;
 * and a dent is not saved (no component in toyengine/scene/ but CharacterController is
 * ISaveable, so this matches Destructible/Sliceable's own fracture state).
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
 *   dent: true                 # push vertices inward on Dent/Chip; off still costs HP
 *   max_dent: 0                # per-vertex push cap, metres; 0 derives one from the mesh
 *   dent_resolution: 0         # target mean edge for the dent clone, metres; 0 means 0.05
 *   impact_transfer: 0.35      # fraction of a DamageCursor click that pushes a sibling Rigidbody
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

    /** @brief Push vertices inward where a blow resolves to DamageResponse::Dent or Chip
     *         (DestructionSystem::apply_dent_()). Off still costs HP and marks the DamageField,
     *         same as a blow that resolves to Hole always does. */
    bool dent = true;

    /** @brief Cap on how far one vertex may be pushed in, accumulated across every hit it has
     *         taken, metres; 0 derives it from the mesh's own world bounds (0.35 of the
     *         shortest extent) -- what keeps a thin plate from being crushed through itself. */
    float max_dent = 0.0f;

    /** @brief Target mean triangle edge, in world metres, for the private mesh clone a dent
     *         builds on first impact; 0 means 0.05. A mesh already finer than this is left
     *         alone; a coarser one (a primitive like `cube`) is subdivided once, silently,
     *         before its first dent -- see toyengine/destruct/dent.h's subdivide_for_dent(). */
    float dent_resolution = 0.0f;

    /** @brief What fraction of a DamageCursor click's resistance-adjusted magnitude becomes an
     *         actual physical push on a sibling Rigidbody, via DestructionSystem::
     *         process_damage_cursors_() -- same name and default as Destructible::
     *         impact_transfer, which does the equivalent job for a chunk's debris scatter.
     *         Has no effect without a sibling Rigidbody, and does not apply to a blow that
     *         arrives from an actual physics contact (physxcoopa's impact_blow()) -- that blow
     *         already has a real collision response from the solver; adding a second impulse on
     *         top of every ordinary bump would be energy gained from nothing, not a dial to tune. */
    float impact_transfer = 0.35f;

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

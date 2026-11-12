/**
 * @file damage_types.h
 * @brief What a blow IS: its kind and its magnitude, in world space -- the vocabulary the rest
 *        of `damage/` is written against.
 *
 * No scene types, no mesh types: a projectile, a swung weapon, a contact or a script can all
 * produce the same DamageEvent. The response a material gives differs per DamageType, which is
 * the whole point of this being typed rather than a single scalar "damage" number -- an axe
 * cleaves what a bullet punches through, and the same impulse means neither.
 */

#ifndef PHYSXCOOPA_DAMAGE_DAMAGE_TYPES_H
#define PHYSXCOOPA_DAMAGE_DAMAGE_TYPES_H

#include <glm/glm.hpp>

#include <cstdint>

namespace coopa {
namespace physx {
namespace damage {

/** @brief What kind of blow this is. */
enum class DamageType : std::uint8_t {
    Impact,    ///< A blunt hit or a falling weight -- wide, shallow.
    Slash,     ///< A blade drawn across the surface -- a line, deep and thin.
    Pierce,    ///< A bullet or a spike -- very narrow, very deep.
    Explosive, ///< A blast -- very wide, medium depth.
    Fire,      ///< Sustained heat -- no geometric footprint of its own; see toyengine/fire/.
    Count,
};

/**
 * @struct DamageEvent
 * @brief One blow, in world space.
 */
struct DamageEvent {
    DamageType type = DamageType::Impact;

    /** @brief The blow's magnitude, in the type's own units: N*s for Impact/Explosive (an
     *         impulse -- see impact.h), otherwise an abstract designer-tuned amount. */
    float amount = 0.0f;

    glm::vec3 point{0.0f};     ///< Where it landed.
    glm::vec3 direction{0.0f}; ///< Unit; the way the blow travels (into the surface).

    /** @brief Slash only: the blade's edge direction, so the footprint is a line, not a dot.
     *         Zero (the default) means no preferred axis -- resolve_damage() then treats it as
     *         a dot, same as Impact's footprint. */
    glm::vec3 edge{0.0f};

    /** @brief Footprint radius, metres; 0 lets the material and type choose one. */
    float radius = 0.0f;
};

} // namespace damage
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_DAMAGE_DAMAGE_TYPES_H

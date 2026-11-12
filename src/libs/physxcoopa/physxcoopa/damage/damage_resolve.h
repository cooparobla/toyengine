/**
 * @file damage_resolve.h
 * @brief The one function that encodes "an axe cleaves what a bullet punches through, and the
 *        same impulse means neither" -- pure, and the thing worth unit-testing on its own.
 *
 * `resolve_damage()` takes a DamageEvent and a DamageMaterial and decides two things: how much
 * HP the blow costs, and what geometric RESPONSE it calls for (a dent, a chip, a hole, a clean
 * cleave, or a full fracture) -- the response toyengine/destruct/'s `Damageable` carries out.
 * No scene types, no mesh types: this function never touches geometry, only decides what should
 * happen to some.
 *
 * Shape of the policy (see each branch's comment for the numbers):
 *
 * | Type      | Footprint        | Depth        | Tends toward                              |
 * |-----------|------------------|--------------|--------------------------------------------|
 * | Impact    | wide, isotropic  | shallow      | Dent on tough/soft; Fracture on brittle    |
 * | Slash     | a line (`edge`)  | deep, thin   | Cleave when fibrous and strong enough; else Chip |
 * | Pierce    | very narrow      | very deep    | Hole; Chip (spalling) on brittle           |
 * | Explosive | very wide        | medium       | Fracture                                   |
 * | Fire      | n/a              | n/a          | None -- HP only; toyengine/fire/ owns the visual |
 */

#ifndef PHYSXCOOPA_DAMAGE_DAMAGE_RESOLVE_H
#define PHYSXCOOPA_DAMAGE_DAMAGE_RESOLVE_H

#include <physxcoopa/damage/damage_material.h>
#include <physxcoopa/damage/damage_types.h>

#include <algorithm>

namespace coopa {
namespace physx {
namespace damage {

/** @brief What a blow does to a material, geometrically. */
enum class DamageResponse {
    None,     ///< Too weak to mark the surface; HP (if any) is the only effect.
    Dent,     ///< Displace vertices inward within `radius`, falloff by distance.
    Chip,     ///< Carve a shallow, roughly round divot off the mesh.
    Hole,     ///< Carve all the way through -- a narrow punch-through.
    Cleave,   ///< Cut the object in two along the blow's plane (DestructionSystem::slice_object()).
    Fracture, ///< Shatter -- the whole object, or (localized_fracture) just the hit region.
};

/** @brief What resolve_damage() decided: the HP cost and the geometric response, sized for it. */
struct ResolvedDamage {
    float          hp_loss = 0.0f;
    DamageResponse response = DamageResponse::None;
    float          depth = 0.0f;      ///< How far into the surface, metres.
    float          radius = 0.0f;     ///< Footprint radius, metres.
    float          along_edge = 0.0f; ///< Slash only: footprint half-length along `edge`.
};

namespace detail {

/** @brief Below this, ANY type is a settling contact or a tap, not a blow -- the thing that
 *         lets a resting crate's steady 5-10 N*s of contact impulse resolve to None instead of
 *         slowly sanding it down to nothing over a thousand frames. */
constexpr float k_min_effective = 1.0f;

} // namespace detail

/**
 * @brief Resolves one blow against one material.
 * @param blow     The blow, already in the object's local space (direction, edge, point).
 * @param material The surface's resistance and failure character.
 */
inline ResolvedDamage resolve_damage(const DamageEvent& blow, const DamageMaterial& material) {
    ResolvedDamage out;
    const float resistance = std::max(1e-3f, material.resistance_to(blow.type));
    const float effective = std::max(0.0f, blow.amount) / resistance;
    out.hp_loss = effective;

    if (effective < detail::k_min_effective) return out; // DamageResponse::None, as constructed

    // toughness: how much the material deforms before it gives way outright. (1 - toughness)
    // is how brittle it reads -- glass and terracotta are near 1 here, mild steel near 0.
    const float brittle = 1.0f - std::clamp(material.toughness, 0.0f, 1.0f);

    switch (blow.type) {
        case DamageType::Impact: {
            // Wide and shallow: the footprint grows slowly with how hard the hit was, and a
            // soft material (low hardness) dents further for the same blow.
            out.radius = blow.radius > 0.0f ? blow.radius : std::clamp(0.03f + effective * 0.0015f, 0.03f, 0.6f);
            out.depth = out.radius * (1.0f - std::clamp(material.hardness, 0.0f, 1.0f)) * 0.3f;
            // A brittle material fails outright under a hit strong enough to have dented a
            // tough one deeply; a tough one just keeps denting.
            out.response = (brittle > 0.6f && effective > material.hit_points * 0.3f)
                               ? DamageResponse::Fracture
                               : DamageResponse::Dent;
            break;
        }
        case DamageType::Slash: {
            // Deep and thin, drawn out along the blade's edge.
            out.radius = blow.radius > 0.0f ? blow.radius : 0.015f;
            out.depth = out.radius * 4.0f;
            out.along_edge = 0.1f + effective * 0.002f;
            // Fibrous materials (wood, flesh) part cleanly along the grain once the cut clears
            // its own cost; otherwise the blade just chips a line off the surface.
            out.response = (material.fibrous > 0.5f && effective > material.hit_points * 0.5f)
                               ? DamageResponse::Cleave
                               : DamageResponse::Chip;
            break;
        }
        case DamageType::Pierce: {
            // Very narrow, very deep -- a spike or a bullet.
            out.radius = blow.radius > 0.0f ? blow.radius : 0.008f;
            out.depth = out.radius * 8.0f;
            // A brittle surface spalls (chips) around the entry instead of a clean hole.
            out.response = (brittle > 0.7f) ? DamageResponse::Chip : DamageResponse::Hole;
            break;
        }
        case DamageType::Explosive: {
            out.radius = blow.radius > 0.0f ? blow.radius : std::clamp(0.1f + effective * 0.001f, 0.1f, 3.0f);
            out.depth = out.radius * 0.5f;
            out.response = DamageResponse::Fracture;
            break;
        }
        case DamageType::Fire:
        case DamageType::Count:
        default:
            out.response = DamageResponse::None; // HP loss only; toyengine/fire/ owns the visual
            break;
    }
    return out;
}

} // namespace damage
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_DAMAGE_DAMAGE_RESOLVE_H

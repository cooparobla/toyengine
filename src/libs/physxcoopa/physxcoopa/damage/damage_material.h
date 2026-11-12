/**
 * @file damage_material.h
 * @brief How a surface responds to being hit -- per-DamageType resistance plus the three dials
 *        resolve_damage() reads to pick a response (Dent/Chip/Hole/Cleave/Fracture).
 *
 * Shared by pointer, loaded from `damage_materials/<name>.yaml`
 * (loaders/damage_material_loader.h) -- the same convention as dynamics::PhysicsMaterial.
 */

#ifndef PHYSXCOOPA_DAMAGE_DAMAGE_MATERIAL_H
#define PHYSXCOOPA_DAMAGE_DAMAGE_MATERIAL_H

#include <physxcoopa/damage/damage_types.h>

#include <array>

namespace coopa {
namespace physx {
namespace damage {

/**
 * @struct DamageMaterial
 * @brief A surface's resistance to each DamageType, plus how it fails.
 */
struct DamageMaterial {
    /** @brief Divides incoming `DamageEvent::amount` before it is turned into HP loss, one per
     *         DamageType. A steel plate resists Pierce poorly (a thin slug punches through) and
     *         Impact well (it dents instead of breaking); the index is `(int)DamageType`. */
    std::array<float, static_cast<std::size_t>(DamageType::Count)> resistance = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

    /** @brief 0..1. High resists denting (stone); low dents readily (lead, soft wood). */
    float hardness = 0.5f;

    /** @brief 0..1. High deforms instead of breaking at the hit site (steel); low shatters
     *         there instead (glass, terracotta). */
    float toughness = 0.5f;

    /** @brief 0..1. High cleaves cleanly along a slash's edge (wood, flesh); low just chips. */
    float fibrous = 0.0f;

    float hit_points = 100.0f;

    float resistance_to(DamageType type) const {
        return resistance[static_cast<std::size_t>(type)];
    }

    /** @brief The material used when a Damageable names none. */
    static const DamageMaterial& default_material() {
        static const DamageMaterial instance{};
        return instance;
    }
};

} // namespace damage
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_DAMAGE_DAMAGE_MATERIAL_H

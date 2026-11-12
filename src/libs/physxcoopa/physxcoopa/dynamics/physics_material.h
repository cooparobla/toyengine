/**
 * @file physics_material.h
 * @brief Surface friction/restitution properties, shared by pointer across colliders --
 *        Unity's PhysicMaterial equivalent.
 */

#ifndef PHYSXCOOPA_DYNAMICS_PHYSICS_MATERIAL_H
#define PHYSXCOOPA_DYNAMICS_PHYSICS_MATERIAL_H

#include <algorithm>

namespace coopa {
namespace physx {
namespace dynamics {

/**
 * @brief How two materials' friction or restitution values combine at a contact.
 *
 * When the two materials at a contact disagree on which mode to use, Unity's asymmetric
 * tie-break applies: Multiply > Maximum > Minimum > Average (the higher-priority mode wins).
 */
enum class CombineMode {
    Average,
    Minimum,
    Maximum,
    Multiply,
};

/**
 * @struct PhysicsMaterial
 * @brief Friction and restitution for one surface. Instances are shared by pointer; a
 *        Collider with no material assigned uses PhysicsMaterial::default_material().
 */
struct PhysicsMaterial {
    float dynamic_friction = 0.6f;
    float static_friction = 0.6f;
    float restitution = 0.0f;
    CombineMode friction_combine = CombineMode::Average;
    CombineMode restitution_combine = CombineMode::Average;

    /**
     * @brief Rolling-resistance coefficient a wheel pays on this surface, as a fraction of the
     *        load it carries. Read only by the vehicle sim (vehicle/vehicle_sim.h); contacts,
     *        casts and the solver ignore it entirely.
     *
     * Separate from `dynamic_friction` because the two are independent in the surfaces that
     * matter: mud is slippery AND draggy (low friction, high rolling resistance), ice is
     * slippery and not draggy at all. 0.015 is hard tarmac, 0.05 loose gravel, 0.15 deep mud
     * or soft snow.
     */
    float rolling_resistance = 0.015f;

    /** @brief The material used by a Collider with no PhysicsMaterial assigned. */
    static const PhysicsMaterial& default_material() {
        static const PhysicsMaterial instance{};
        return instance;
    }
};

/**
 * @brief Combines two scalar values (friction or restitution) per the higher-priority of the
 *        two materials' combine modes.
 *
 * @param a    First material's value.
 * @param b    Second material's value.
 * @param mode_a First material's combine mode.
 * @param mode_b Second material's combine mode.
 * @return Combined value.
 */
inline float combine(float a, float b, CombineMode mode_a, CombineMode mode_b) {
    CombineMode mode = std::max(mode_a, mode_b); // enum order IS the priority order below
    switch (mode) {
        case CombineMode::Multiply: return a * b;
        case CombineMode::Maximum:  return std::max(a, b);
        case CombineMode::Minimum:  return std::min(a, b);
        case CombineMode::Average:
        default:                    return 0.5f * (a + b);
    }
}

} // namespace dynamics
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_DYNAMICS_PHYSICS_MATERIAL_H

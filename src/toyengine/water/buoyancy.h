/**
 * @file buoyancy.h
 * @brief Buoyancy component: makes a Rigidbody float, in the pontoon style of Unreal's
 *        BuoyancyComponent / the common Unity floater setups.
 *
 * A body's displaced volume is split across a handful of pontoons -- sample points fixed in the
 * body, each owning a small box of water-displacing volume. Every physics substep WaterSystem
 * samples the water surface above each pontoon and applies, AT that pontoon:
 *  - an upward Archimedes impulse  rho * g * V_i * submerged_i * h, and
 *  - a drag impulse on the pontoon's velocity RELATIVE TO THE CURRENT, scaled by submersion --
 *    linear (`linear_drag`) plus quadratic form drag (`form_drag`) -- which is also what carries
 *    objects downstream.
 * Off-centre application gives righting torque and wave rocking for free; no separate stability
 * model is needed.
 *
 * Pontoons are generated from the body's colliders unless authored: a box becomes a
 * `subdivisions`^3 lattice, a sphere one pontoon, a capsule three along its axis. Volume defaults
 * to the colliders' solid volume (so floating height follows the Rigidbody's mass/volume ratio,
 * exactly like a real object); `volume` overrides it.
 *
 * All per-substep work happens in WaterSystem (one pass over every buoyant body, no per-component
 * signal connections); this component is configuration plus its resolved pontoons.
 */

#ifndef TOYENGINE_WATER_BUOYANCY_H
#define TOYENGINE_WATER_BUOYANCY_H

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/scene/component.h>

namespace toy {
namespace water {

/** @brief One resolved pontoon, in the owner's world-scaled local frame (pivot origin). */
struct Pontoon {
    glm::vec3 local{0.0f};          ///< Offset from the owner's pivot (world-scaled, unrotated).
    glm::vec3 half_extents{0.25f};  ///< Local half-size of the volume this pontoon stands for.
    float     volume = 0.0f;        ///< m^3 of water this pontoon displaces when fully submerged.
};

/** @brief An authored pontoon (YAML `pontoons:` entry), in the owner's UNSCALED local frame. */
struct PontoonDesc {
    glm::vec3 position{0.0f};
    float     radius = 0.25f;
};

/**
 * @class Buoyancy
 * @brief See the file doc. Requires a Rigidbody (and normally a Collider) on the same object.
 */
class Buoyancy : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Buoyancy"; }

    std::vector<PontoonDesc> pontoons;  ///< Empty = generate from colliders.
    int   subdivisions  = 2;            ///< Box lattice resolution per axis (1-4).
    float volume        = -1.0f;        ///< Displaced volume override (m^3); < 0 = from colliders.
    float linear_drag   = 1.5f;         ///< 1/s, on velocity relative to the water, when submerged.
    /// Quadratic form-drag coefficient (C_d in 0.5 rho C_d A |v| v; ~1 for a blunt body). Water is
    /// dense: this is what stops a dropped crate ploughing deep and bouncing back out, while
    /// barely touching slow drift (the linear term handles that).
    float form_drag     = 1.0f;
    float angular_drag  = 1.0f;         ///< 1/s, when fully submerged.
    float buoyancy_scale = 1.0f;        ///< Multiplier on the Archimedes force (gameplay tuning).

    // --- Resolved by WaterSystem ---
    std::vector<Pontoon> resolved;
    bool  initialized = false;
    int   resolved_subdivision_cap = -1; ///< WaterSettings::max_box_subdivisions resolved with.

    // --- Live state, for gameplay/debug: written every substep ---
    float submerged_fraction = 0.0f;    ///< Volume-weighted, 0..1.
    bool  in_water = false;
    /// Inside WaterSystem's simulation range (full pontoon buoyancy). Outside it the body is
    /// frozen: put to sleep where it floats (see WaterSystem::substep_()).
    bool  simulated = true;
    float frozen_rest_time = 0.0f;      ///< Out of range and awake: seconds spent near rest.
};

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_BUOYANCY_H

/**
 * @file tire.h
 * @brief The tyre model: a normalised slip curve and the friction circle that couples
 *        accelerating and cornering. Pure functions over plain floats -- no world, no body.
 *
 * Both longitudinal and lateral grip follow the same shape, which is what gives a tyre its
 * feel: force rises with slip to a peak, then falls away to a plateau. Below the peak the tyre
 * grips and a car tracks its steering; past it the tyre slides, so power-on oversteer, a
 * handbrake slide and wheelspin in mud are all the same curve on the far side of its peak.
 *
 * The two demands are then clamped together on a circle: a wheel spending all of its grip
 * accelerating has none left to corner with. That single rule is most of what separates a
 * vehicle that feels driveable from one that feels like a brick on rails.
 */

#ifndef PHYSXCOOPA_VEHICLE_TIRE_H
#define PHYSXCOOPA_VEHICLE_TIRE_H

#include <algorithm>
#include <cmath>

namespace coopa {
namespace physx {
namespace vehicle {

/**
 * @struct TireConfig
 * @brief One tyre compound, shared by every wheel of a vehicle.
 */
struct TireConfig {
    /** @brief Multiplier on the friction the ground offers. A road tyre on tarmac is a little
     *         over 1; a knobbly offroad tyre trades tarmac grip (below 1) for mud it can bite. */
    float grip = 1.0f;
    /** @brief Extra multiplier on the LATERAL limit only. Above 1 resists sliding sideways
     *         without adding traction -- a tracked vehicle's tracks, which dig in. */
    float lateral_grip = 1.0f;

    /** @brief Slip ratio at peak longitudinal force. Smaller is sharper and more
     *         snap-oversteery; larger is progressive and forgiving. */
    float peak_slip_ratio = 0.12f;
    /** @brief Slip angle at peak lateral force, radians (~8 deg by default). */
    float peak_slip_angle = 0.14f;
    /** @brief Fraction of the peak still available when fully sliding. Low values punish a
     *         slide hard (a road car); high values let it be held and steered on the throttle. */
    float plateau = 0.75f;
    /** @brief How far past the peak the force takes to decay to `plateau`, in multiples of the
     *         peak slip. */
    float falloff = 4.0f;

    /** @brief Rolling resistance coefficient of the tyre itself, multiplied by the surface's. */
    float rolling_resistance = 1.0f;

    /** @brief How much the friction coefficient drops as vertical load rises: at `nominal_load`
     *         the full coefficient is available, at twice that it is
     *         `1 - load_sensitivity` of it. Real tyres do this, and it is what stops a heavy
     *         vehicle cornering like a light one just because it presses down harder. */
    float load_sensitivity = 0.25f;
    /** @brief Load at which the tyre delivers its full coefficient, N. Zero: derived from the
     *         vehicle's static corner load by vehicle::simulate(). */
    float nominal_load = 0.0f;
};

/**
 * @brief Normalised force (0..1) for a slip magnitude, peaking at 1 at `peak`.
 *
 * Linear up to the peak, then decaying toward `plateau` over `falloff * peak` of further slip
 * and holding there. Straight lines rather than a Pacejka fit: the shape that matters for feel
 * is "rises, peaks, falls to a plateau", and this one is cheap, monotonic below the peak and
 * has no magic coefficients to mistune.
 *
 * @param slip     Slip ratio, or slip angle in radians.
 * @param peak     Slip at which the force peaks.
 * @param plateau  Fraction of the peak retained when fully sliding.
 * @param falloff  Width of the decay, in multiples of `peak`.
 * @return Normalised force magnitude in (0, 1].
 */
inline float slip_curve(float slip, float peak, float plateau, float falloff = 4.0f) {
    const float s = std::abs(slip);
    if (peak <= 1e-6f) return plateau;
    if (s <= peak) return s / peak;
    if (falloff <= 1e-6f) return plateau;
    const float t = std::min((s - peak) / (falloff * peak), 1.0f);
    return 1.0f - (1.0f - plateau) * t;
}

/**
 * @brief The friction coefficient available to one wheel, load sensitivity included.
 *
 * @param tire         The compound.
 * @param surface_grip The ground material's friction coefficient at the contact.
 * @param load         Vertical load on the wheel, N.
 * @param nominal_load Load at which the full coefficient is available, N.
 * @return The coefficient to multiply the load by for a force limit.
 */
inline float available_friction(const TireConfig& tire, float surface_grip, float load, float nominal_load) {
    float mu = std::max(tire.grip, 0.0f) * std::max(surface_grip, 0.0f);
    if (nominal_load > 1.0f && load > nominal_load) {
        const float excess = std::min((load - nominal_load) / nominal_load, 1.0f);
        mu *= 1.0f - std::clamp(tire.load_sensitivity, 0.0f, 0.95f) * excess;
    }
    return mu;
}

/**
 * @struct TireForces
 * @brief One wheel's tyre forces for a substep, after the friction circle.
 */
struct TireForces {
    float longitudinal = 0.0f;  ///< N, positive drives the vehicle forward.
    float lateral = 0.0f;       ///< N, positive pushes the vehicle to its right.
    bool sliding = false;       ///< Either axis is past its peak, or the circle clamped.
};

/**
 * @brief Longitudinal and lateral tyre force for one wheel, clamped on the friction circle.
 *
 * The lateral force opposes the lateral velocity and the longitudinal one follows the sign of
 * the slip ratio (a wheel spinning faster than the ground pushes the vehicle forward). Both are
 * then scaled together if their combined magnitude exceeds what the contact can hold, so the
 * split between accelerating and cornering is decided by which demand is larger -- not by one
 * axis silently winning.
 *
 * @param tire       The compound.
 * @param slip_ratio Longitudinal slip.
 * @param slip_angle Lateral slip, radians.
 * @param load       Vertical load, N.
 * @param mu         Available friction coefficient (see available_friction()).
 * @return The two forces, and whether the tyre is sliding.
 */
inline TireForces tire_forces(const TireConfig& tire, float slip_ratio, float slip_angle, float load, float mu) {
    TireForces out;
    if (load <= 0.0f) return out;

    const float limit = mu * load;
    const float n_long = slip_curve(slip_ratio, tire.peak_slip_ratio, tire.plateau, tire.falloff);
    const float n_lat = slip_curve(slip_angle, tire.peak_slip_angle, tire.plateau, tire.falloff);

    out.longitudinal = std::copysign(n_long * limit, slip_ratio);
    // Lateral force RESISTS the slip: a wheel sliding to its right is pushed left.
    out.lateral = -std::copysign(n_lat * limit * std::max(tire.lateral_grip, 0.0f), slip_angle);

    const float combined = std::sqrt(out.longitudinal * out.longitudinal + out.lateral * out.lateral);
    const float circle = limit * std::max(1.0f, tire.lateral_grip);
    if (combined > circle && combined > 1e-4f) {
        const float scale = circle / combined;
        out.longitudinal *= scale;
        out.lateral *= scale;
        out.sliding = true;
    }
    if (std::abs(slip_ratio) > tire.peak_slip_ratio || std::abs(slip_angle) > tire.peak_slip_angle) {
        out.sliding = true;
    }
    return out;
}

} // namespace vehicle
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_VEHICLE_TIRE_H

/**
 * @file wheel.h
 * @brief One wheel's tuning (WheelConfig) and its per-substep simulation output (WheelState).
 *
 * A wheel is a raycast, not a shape: the sim casts from `anchor` along the chassis' down axis
 * and treats the hit as the contact patch (see vehicle/vehicle_sim.h). Nothing here touches a
 * scene or a renderer -- `WheelState::hub_offset` and `spin_angle` are what a host engine needs
 * to place a visual wheel, and it is the only coupling between the two.
 *
 * Distances are metres, angles radians, forces newtons, torques newton-metres. `anchor` is in
 * the chassis body's local frame, with +X right, +Y forward and +Z up (toyengine's convention:
 * an object faces its local +Y), and is measured from the body's CENTRE OF MASS -- which is what
 * dynamics::Body::position is. A host authoring anchors against an object's Transform pivot
 * subtracts the body's local centre of mass first (components::RigidbodyComponent::
 * local_center_of_mass(), which VehicleComponent does for you).
 */

#ifndef PHYSXCOOPA_VEHICLE_WHEEL_H
#define PHYSXCOOPA_VEHICLE_WHEEL_H

#include <physxcoopa/dynamics/body.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace coopa {
namespace physx {
namespace vehicle {

/** @brief Which side of the vehicle a wheel is on -- what differential steering drives against
 *         and what the Ackermann inner/outer split is measured from. */
enum class WheelSide {
    Left,
    Right,
};

/**
 * @struct WheelConfig
 * @brief One wheel's geometry, suspension and role in the drivetrain.
 *
 * The suspension is a spring-damper of free length `rest_length` hanging from `anchor`, able to
 * compress by `travel`. A corner carrying mass `m` settles at `m * g / stiffness` of
 * compression, so a preset wants that to land near 40% of `travel`: less and the wheel tops out
 * over every bump, more and it rides on the bump stop.
 */
struct WheelConfig {
    /** @brief Suspension top mount, chassis-local, from the centre of mass (see the file doc). */
    glm::vec3 anchor{0.0f};

    float radius = 0.35f;
    /** @brief Visual/contact width. The sim is a single ray, so this only widens the reported
     *         contact patch for a host's debug draw -- it does not make the wheel catch a lip
     *         narrower than itself (see the README's limits). */
    float width = 0.25f;
    /** @brief Unsprung mass, used for the wheel's own spin inertia (0.5 * m * r^2). */
    float mass = 20.0f;

    /** @brief Suspension length at full extension, anchor to hub. */
    float rest_length = 0.30f;
    /** @brief How far above full extension the hub can travel before the bump stop. */
    float travel = 0.20f;

    /** @brief Spring rate, N/m. Zero derives it from `natural_frequency` and the corner's share
     *         of the chassis mass, which is the easier dial to tune: 1.0-1.3 Hz is a soft
     *         offroad truck, 1.5-2.0 Hz a road car, 2.5+ Hz a race car. */
    float stiffness = 0.0f;
    float natural_frequency = 1.6f;

    /** @brief Damping as a fraction of critical, on compression (`damping_ratio`) and on
     *         extension (`rebound_ratio`). Rebound above bump is what keeps a vehicle from
     *         pogoing after a kerb. */
    float damping_ratio = 0.45f;
    float rebound_ratio = 0.65f;

    /** @brief Turned by the steering (ignored entirely by SteerMode::Differential). */
    bool steered = false;
    /** @brief Share of the drivetrain's torque this wheel takes. The shares across all wheels
     *         are normalised, so {1,1,0,0} is front drive and {0,0,1,1} rear, whatever the
     *         absolute numbers. Zero: an undriven wheel. */
    float drive_share = 0.0f;
    /** @brief Share of `VehicleConfig::brake_torque` / `handbrake_torque` this wheel takes.
     *         A handbrake that only acts on the rear (`handbrake_share` 0 at the front) is what
     *         makes the back step out. */
    float brake_share = 1.0f;
    float handbrake_share = 0.0f;

    /** @brief Wheels sharing a non-negative group form an axle: the anti-roll bar acts across
     *         them and the differential couples them. -1 opts out of both. */
    int anti_roll_group = -1;

    WheelSide side = WheelSide::Left;

    /** @brief Spring rate actually used, resolving `stiffness` against `natural_frequency`.
     *
     *  @param corner_mass The chassis mass this corner carries (total / wheel count).
     *  @return N/m, never below a floor that would let the chassis sink through its own travel. */
    float resolved_stiffness(float corner_mass) const {
        if (stiffness > 0.0f) return stiffness;
        const float omega = 2.0f * 3.14159265358979f * std::max(natural_frequency, 0.05f);
        return std::max(corner_mass * omega * omega, 1.0f);
    }

    /** @brief Spin inertia of the wheel about its axle, kg m^2 (a solid disc). */
    float spin_inertia() const { return std::max(0.5f * mass * radius * radius, 1e-3f); }
};

/**
 * @struct WheelState
 * @brief What the last substep computed for one wheel. Written by vehicle::simulate(), read by
 *        gameplay, debug draw and the host's wheel visuals.
 */
struct WheelState {
    bool grounded = false;

    /** @brief Suspension compression, 0 at full extension up to `WheelConfig::travel`. */
    float compression = 0.0f;
    /** @brief Rate of change of `compression`, m/s -- positive while compressing. */
    float compression_rate = 0.0f;
    /** @brief Hub distance below the anchor along the chassis down axis: `rest_length -
     *         compression`. What a host positions the visual wheel at. */
    float hub_offset = 0.30f;
    /** @brief Spring + damper force along the contact normal, N. Zero when airborne. */
    float suspension_force = 0.0f;

    glm::vec3 contact_point{0.0f};
    glm::vec3 contact_normal{0.0f, 0.0f, 1.0f};
    /** @brief The ground's friction coefficient at the contact (the hit shape's material). */
    float surface_grip = 0.6f;
    /** @brief The ground's rolling-resistance coefficient at the contact. */
    float surface_rolling_resistance = 0.015f;
    dynamics::BodyId contact_body;

    /** @brief Wheel spin, rad/s; positive rolls the vehicle forward. */
    float spin = 0.0f;
    /** @brief Accumulated spin for the visual, radians. */
    float spin_angle = 0.0f;
    /** @brief Current steer angle, radians; positive steers right. */
    float steer_angle = 0.0f;

    /** @brief (rim speed - ground speed) / ground speed along the wheel's forward direction.
     *         Positive under power, negative under braking. */
    float slip_ratio = 0.0f;
    /** @brief Angle between where the wheel points and where it is going, radians. */
    float slip_angle = 0.0f;

    /** @brief Tyre force applied this substep, N, along the wheel's forward / right directions
     *         projected into the contact plane. */
    float force_long = 0.0f;
    float force_lat = 0.0f;
    /** @brief The tyre is past its peak and sliding -- what a skid sound or mark keys off. */
    bool slipping = false;
};

} // namespace vehicle
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_VEHICLE_WHEEL_H

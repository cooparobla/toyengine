/**
 * @file vehicle_sim.h
 * @brief vehicle::simulate() -- one substep of a raycast-wheel vehicle on a rigid chassis.
 *        Pure algorithm over PhysicsWorld, with no scene types and no state of its own (the
 *        caller owns the VehicleState), exactly like character::move().
 *
 * The chassis is an ordinary dynamic body with an ordinary collider; the wheels are not bodies
 * at all. Each substep, per wheel:
 *
 *   1. **Steer.** The steering actuator moves toward the input at a finite rate, limited by a
 *      speed-sensitive maximum (flicking the stick at 40 m/s must not ask for 35 degrees), and
 *      each steered wheel takes its Ackermann share of it -- the inner wheel of a turn points
 *      further in than the outer one, because they are tracing different radii about one centre.
 *   2. **Cast.** A ray from the wheel's chassis-local anchor along the chassis' down axis,
 *      `rest_length + radius` long, ignoring the chassis itself. Its hit is the contact patch,
 *      and the hit shape's PhysicsMaterial is the surface the tyre is on.
 *   3. **Suspend.** A spring-damper along the contact normal, plus an anti-roll term across the
 *      axle, clamped to never pull: suspension can push the chassis up, never suck it down.
 *   4. **Grip.** Slip ratio and slip angle from the contact point's velocity against the wheel's
 *      own rotation, through the tyre curve and friction circle (vehicle/tire.h), scaled by the
 *      surface's friction. Applied as an impulse in the contact plane.
 *   5. **Spin.** Drive torque (vehicle/drivetrain.h) minus brakes, minus the reaction to the
 *      tyre force, minus rolling resistance, integrated into the wheel's own rotation.
 *
 * Then once per vehicle: aerodynamic drag and downforce, and -- while every wheel is off the
 * ground -- an optional righting torque, so a vehicle that jumps lands on its wheels.
 *
 * **Call it from PhysicsWorld::on_substep.** That is after force integration and after the
 * broadphase refit, but before the solve (see PhysicsWorld::step_fixed()), which has two
 * consequences this file depends on:
 *
 *   - Everything here is applied as an IMPULSE (force * h). A force added from a substep slot
 *     is not integrated until the next one, so it would arrive a substep late -- see
 *     dynamics::Body::add_force()'s doc.
 *   - The wheel raycasts observe poses that are current and a broadphase that has just been
 *     refit, with only the velocities pre-solve. That is the correct state for a suspension
 *     cast; see the Queries section of world.h for the full statement.
 *
 * The continuous impulses deliberately do NOT wake a sleeping chassis (a parked vehicle must be
 * allowed to stay asleep under its own suspension); simulate() wakes the body itself the moment
 * a control input is non-zero.
 *
 * Conventions: the chassis faces its local +Y with +Z up (toyengine's object convention),
 * `VehicleControls::steer` is positive to the right, and every `WheelConfig::anchor` is measured
 * from the body's centre of mass (see wheel.h). Metres, radians, newtons, seconds.
 */

#ifndef PHYSXCOOPA_VEHICLE_VEHICLE_SIM_H
#define PHYSXCOOPA_VEHICLE_VEHICLE_SIM_H

#include <physxcoopa/vehicle/drivetrain.h>
#include <physxcoopa/vehicle/tire.h>
#include <physxcoopa/vehicle/wheel.h>
#include <physxcoopa/world.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace coopa {
namespace physx {
namespace vehicle {

/**
 * @struct VehicleConfig
 * @brief Everything that makes one vehicle handle the way it does. See vehicle_presets.h for
 *        the shipped tunings and vehicle/README.md for which dial to reach for.
 */
struct VehicleConfig {
    std::vector<WheelConfig> wheels;
    TireConfig tire;
    EngineConfig engine;
    GearboxConfig gearbox;
    DriveConfig drive;

    SteerMode steer_mode = SteerMode::Ackermann;
    /** @brief Steering lock at a standstill, degrees. */
    float max_steer_deg = 35.0f;
    /** @brief Steering lock at `steer_falloff_speed` and above, degrees. The gap between the two
     *         is what keeps a vehicle stable at speed without making it a bus at parking pace. */
    float min_steer_deg = 10.0f;
    float steer_falloff_speed = 30.0f;
    /** @brief How fast the steering rack moves toward the input, and back to centre when
     *         released, degrees per second. */
    float steer_rate_deg = 220.0f;
    float steer_return_rate_deg = 320.0f;

    /** @brief Total brake torque at full pedal, Nm, split across the wheels by
     *         `WheelConfig::brake_share`. */
    float brake_torque = 3000.0f;
    float handbrake_torque = 6000.0f;

    /** @brief Anti-roll bar rate across each axle, N/m of compression difference. Resists body
     *         roll in a corner without stiffening the springs -- a tall vehicle needs it. */
    float anti_roll = 0.0f;

    /** @brief Aerodynamic drag, N per (m/s)^2: `0.5 * rho * Cd * A`, so ~0.4 for a car and
     *         ~3 for a truck's slab front. */
    float drag = 0.4f;
    /** @brief Downforce, N per (m/s)^2, pressing along the chassis' down axis. */
    float downforce = 0.0f;

    /** @brief While airborne, how hard the chassis is torqued back level, rad/s^2 per unit of
     *         sine of the lean. Zero leaves it tumbling honestly. */
    float air_righting = 0.0f;
    /** @brief Angular damping applied with `air_righting`, 1/s. */
    float air_damping = 1.5f;

    /** @brief Reference load for the tyre's load sensitivity, N. Zero derives it from the
     *         chassis mass and wheel count, which is nearly always what is wanted. */
    float nominal_wheel_load = 0.0f;

    // --- Recovery: righting a vehicle that has ended up on its roof ----------------------
    /** @brief Seconds a recovery takes. The chassis floats clear, turns smoothly back upright
     *         about the heading it already had, and is handed back to its suspension. */
    float recover_time = 1.4f;
    /** @brief How far above its normal ride height the chassis floats while turning over, so it
     *         clears whatever it was lying against. */
    float recover_lift = 0.8f;
    /** @brief Seconds upside down and near-stationary before it rights ITSELF. 0 (the default)
     *         never does: recovery is the driver's to ask for. */
    float auto_recover_time = 0.0f;

    /** @brief Total wheel drive share, for normalising. Zero wheels driven: a trailer. */
    float total_drive_share() const {
        float sum = 0.0f;
        for (const WheelConfig& w : wheels) sum += std::max(w.drive_share, 0.0f);
        return sum;
    }
};

/**
 * @struct VehicleControls
 * @brief One frame of driver input, pushed in by the host (never read from an input device
 *        here -- same contract as toyengine's CharacterController).
 */
struct VehicleControls {
    float throttle = 0.0f;   ///< 0..1.
    float brake = 0.0f;      ///< 0..1. Held at a standstill, selects reverse -- see simulate().
    float steer = 0.0f;      ///< -1 (left) .. +1 (right).
    float handbrake = 0.0f;  ///< 0..1.
    /** @brief Manual gear change: -1 down, +1 up, consumed by the next simulate(). Ignored
     *         while `GearboxConfig::automatic` and `manual` is false. */
    int gear_request = 0;
    bool manual = false;
    /** @brief A press: start righting the vehicle (see VehicleConfig::recover_time). Consumed by
     *         the host once the recovery has begun. Harmless on a vehicle already upright -- it
     *         simply lifts and settles. */
    bool recover = false;
};

/**
 * @struct VehicleState
 * @brief Live simulation state. The caller owns it and must keep it across substeps; simulate()
 *        resizes `wheels` to match the config.
 */
struct VehicleState {
    std::vector<WheelState> wheels;

    float engine_rpm = 800.0f;
    /** @brief 0 neutral, -1 reverse, 1..n forward. */
    int gear = 1;
    /** @brief Seconds left of the current gear change (no torque passes while non-zero). */
    float shift_timer = 0.0f;
    /** @brief Seconds left before another gear change may be considered. */
    float shift_cooldown = 0.0f;

    /** @brief Speed along the chassis' forward axis, m/s -- negative going backwards. */
    float forward_speed = 0.0f;
    /** @brief Speed magnitude, m/s. */
    float speed = 0.0f;
    int wheels_on_ground = 0;
    bool airborne = false;
    /** @brief Current steering-rack angle, radians, positive right. */
    float steer_angle = 0.0f;
    /** @brief Seconds left of an active recovery; 0 when not recovering. */
    float recover_timer = 0.0f;
    /** @brief How long it has been lying on its side or roof and nearly still, seconds. */
    float upside_down_time = 0.0f;
    /** @brief More than 60 degrees off upright. */
    bool upside_down = false;
    bool is_recovering() const { return recover_timer > 0.0f; }

    /** @brief Largest |slip ratio| across the wheels, for a skid cue. */
    float max_slip = 0.0f;
    bool any_wheel_slipping = false;
    bool valid = false;

    // --- Transitions, for a host to turn into signals -------------------------------------
    //
    // simulate() only ever SETS these; the consumer clears them once it has acted. A frame can
    // run several substeps, and a gear change or a landing that happened in the first of them
    // must not be lost because the next one found nothing -- which is also why they are reported
    // here rather than inferred by comparing frames, where anything shorter than a frame
    // vanishes.
    /** @brief The gear changed during this frame. */
    bool gear_changed = false;
    /** @brief Speed along the chassis' down axis at the moment every-wheel-airborne ended, m/s.
     *         Zero if no landing happened. */
    float landing_impact = 0.0f;
    /** @brief Bit i set if wheel i started sliding (it was gripping on the previous substep). */
    uint32_t slip_started_mask = 0;
};

namespace detail {

/** @brief Moves `value` toward `target` by at most `max_delta`. */
inline float move_toward_(float value, float target, float max_delta) {
    const float d = target - value;
    if (std::abs(d) <= max_delta) return target;
    return value + std::copysign(max_delta, d);
}

/** @brief Unit-length `v`, or `fallback` if it is degenerate. */
inline glm::vec3 safe_normalize_(const glm::vec3& v, const glm::vec3& fallback) {
    const float len2 = glm::dot(v, v);
    if (len2 < 1e-12f) return fallback;
    return v / std::sqrt(len2);
}

/** @brief `v` with its component along `n` removed, renormalised. */
inline glm::vec3 project_to_plane_(const glm::vec3& v, const glm::vec3& n, const glm::vec3& fallback) {
    return safe_normalize_(v - n * glm::dot(v, n), fallback);
}

/** @brief Per-wheel scratch for the passes that have to run across all wheels at once (the
 *         anti-roll bar needs both sides' compression; the differential needs both sides'
 *         traction limit). */
struct WheelScratch {
    glm::vec3 anchor_world{0.0f};
    glm::vec3 axis{0.0f};        ///< Suspension direction, chassis down, world space.
    glm::vec3 forward{0.0f};     ///< Wheel heading in the contact plane.
    glm::vec3 right{0.0f};       ///< Lateral direction in the contact plane.
    float spring_force = 0.0f;   ///< Before the anti-roll term.
    float load = 0.0f;           ///< After it; what the tyre works against.
    float mu = 0.0f;             ///< Available friction coefficient.
    float traction_torque = 0.0f;///< Torque this wheel can transmit before it spins, Nm.
    float drive_command = 0.0f;  ///< Signed throttle reaching this wheel (differential steering).
};

/**
 * @brief One substep of righting a vehicle: float it clear, turn it smoothly back upright about
 *        the heading it already had, and bleed off the drift.
 *
 * Both the rotation and the lift are driven as exponential approaches (`velocity = error / tau`,
 * applied as an impulse that sets the velocity outright) rather than as a snap or a constant
 * rate. That is what makes it read as the vehicle rolling itself over rather than teleporting:
 * fastest while it is furthest from upright, easing as it arrives, and still a real rigid body
 * the whole time -- it collides on the way, and its suspension catches it at the end.
 *
 * Every wheel is reported airborne for the duration; no drive, brake or tyre force is applied.
 */
inline void recover_substep_(PhysicsWorld& world, dynamics::BodyId chassis, dynamics::Body& body,
                             const VehicleConfig& cfg, VehicleState& st, float h,
                             const glm::vec3& forward, const glm::vec3& right,
                             const glm::vec3& world_up) {
    for (WheelState& w : st.wheels) {
        w.grounded = false;
        w.compression = 0.0f;
        w.compression_rate = 0.0f;
        w.suspension_force = 0.0f;
        w.force_long = 0.0f;
        w.force_lat = 0.0f;
        w.slip_ratio = 0.0f;
        w.slip_angle = 0.0f;
        w.slipping = false;
        w.spin *= std::max(1.0f - 3.0f * h, 0.0f);
    }
    st.wheels_on_ground = 0;
    st.airborne = true;
    st.max_slip = 0.0f;
    st.any_wheel_slipping = false;
    st.engine_rpm += (cfg.engine.idle_rpm - st.engine_rpm) * std::min(4.0f * h, 1.0f);

    const float tau = std::max(cfg.recover_time * 0.30f, 0.08f);

    // --- Upright, about the heading it already had ---
    glm::vec3 heading = forward - world_up * glm::dot(forward, world_up);
    if (glm::dot(heading, heading) < 1e-4f) {
        // Nose straight up or down: the chassis' forward says nothing about which way it faces,
        // so take the heading from its side axis instead.
        const glm::vec3 flat_side = right - world_up * glm::dot(right, world_up);
        heading = glm::cross(world_up, flat_side);
    }
    heading = safe_normalize_(heading, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 side = safe_normalize_(glm::cross(heading, world_up), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::quat target = glm::quat_cast(glm::mat3(side, heading, world_up));

    glm::quat error = target * glm::inverse(body.orientation);
    if (error.w < 0.0f) error = -error;  // the short way round, never the long one
    const glm::vec3 rotation_vector = 2.0f * glm::vec3(error.x, error.y, error.z);

    const glm::vec3 desired_spin = rotation_vector / tau;
    body.apply_angular_impulse(glm::inverse(body.inv_inertia_world) *
                               (desired_spin - body.angular_velocity));

    // --- Float clear of whatever it is lying on ---
    float ride_height = 1.0f;
    if (!cfg.wheels.empty()) {
        ride_height = -cfg.wheels[0].anchor.z + cfg.wheels[0].rest_length + cfg.wheels[0].radius;
    }
    const float target_height = ride_height + cfg.recover_lift;

    geometry::Ray probe;
    probe.origin = body.position;
    probe.direction = -world_up;
    probe.max_distance = 60.0f;
    query::QueryFilter filter;
    filter.ignore = chassis;
    filter.include_triggers = false;
    query::RaycastHit hit;
    const float height = world.raycast(probe, hit, filter) ? hit.distance : target_height;

    const glm::vec3 velocity = body.linear_velocity;
    const float vertical = glm::dot(velocity, world_up);
    const glm::vec3 horizontal = velocity - world_up * vertical;
    const float climb = std::clamp((target_height - height) / tau, -3.0f, 4.0f);
    const glm::vec3 desired = world_up * climb + horizontal * std::exp(-5.0f * h);
    body.apply_impulse_at_position((desired - velocity) * body.mass, body.position);
}

} // namespace detail

/**
 * @brief Advances one vehicle by one physics substep.
 *
 * @param world   The world the chassis lives in. Queried (wheel raycasts) and mutated
 *                (impulses on the chassis) -- call from on_substep, see the file doc.
 * @param chassis The chassis body. Must be Dynamic; anything else is ignored.
 * @param cfg     Tuning. Must not change between substeps of one step (resolve presets first).
 * @param in      This frame's driver input.
 * @param st      Live state, carried by the caller across calls.
 * @param h       Substep length, seconds.
 */
inline void simulate(PhysicsWorld& world, dynamics::BodyId chassis, const VehicleConfig& cfg,
                     const VehicleControls& in, VehicleState& st, float h) {
    using namespace detail;

    dynamics::Body* body = world.get_body(chassis);
    if (!body || body->type != dynamics::BodyType::Dynamic || h <= 0.0f || cfg.wheels.empty()) return;

    const std::size_t n = cfg.wheels.size();
    st.wheels.resize(n);
    st.valid = true;

    // A sleeping chassis is a parked vehicle, and there is nothing to simulate on it. Running the
    // rest of this function anyway is actively wrong: the integrator skips a sleeping body, so the
    // suspension impulses below are never integrated into a position and never damped -- they just
    // accumulate in its velocity field, and a vehicle standing still for ever ends up reporting
    // most of a metre per second of climb. Any driver input wakes it, and the next substep is
    // ordinary.
    const bool driver_active = std::abs(in.throttle) > 0.01f || std::abs(in.brake) > 0.01f ||
                               std::abs(in.steer) > 0.01f || std::abs(in.handbrake) > 0.01f ||
                               in.gear_request != 0 || in.recover || st.recover_timer > 0.0f;
    if (!body->awake) {
        if (!driver_active) {
            st.speed = 0.0f;
            st.forward_speed = 0.0f;
            return;
        }
        body->wake();
    }

    // Captured before anything this substep changes them, for the transition reporting below.
    const int gear_before = st.gear;
    const bool was_airborne = st.airborne;
    const glm::vec3 velocity_before = body->linear_velocity;

    // --- Chassis frame and gravity -------------------------------------------------------
    const glm::quat rot = body->orientation;
    const glm::vec3 up = rot * glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 fwd = rot * glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 right = rot * glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 down = -up;

    const glm::vec3 gravity = world.gravity_at(body->position);
    const float g = std::max(glm::length(gravity), 0.01f);
    const glm::vec3 world_up = world.up_at(body->position);

    st.speed = glm::length(body->linear_velocity);
    st.forward_speed = glm::dot(body->linear_velocity, fwd);

    // --- Recovery -----------------------------------------------------------------------
    // Checked before anything else: a vehicle on its roof has its wheel rays pointing at the sky,
    // so none of the passes below would do anything useful anyway.
    st.upside_down = glm::dot(up, world_up) < 0.5f;  // more than 60 degrees off upright
    if (st.upside_down && st.speed < 2.0f) {
        st.upside_down_time += h;
    } else {
        st.upside_down_time = 0.0f;
    }
    const bool auto_recover =
        cfg.auto_recover_time > 0.0f && st.upside_down_time >= cfg.auto_recover_time;
    if ((in.recover || auto_recover) && st.recover_timer <= 0.0f) {
        st.recover_timer = cfg.recover_time;
        st.upside_down_time = 0.0f;
        st.gear = cfg.gearbox.forward_ratios.empty() ? 0 : 1;
        st.shift_timer = 0.0f;
        st.shift_cooldown = 0.0f;
        body->wake();
    }
    if (st.recover_timer > 0.0f) {
        st.recover_timer = std::max(st.recover_timer - h, 0.0f);
        recover_substep_(world, chassis, *body, cfg, st, h, fwd, right, world_up);
        return;
    }

    const float corner_mass = body->mass / static_cast<float>(n);
    const float nominal_load = cfg.nominal_wheel_load > 0.0f ? cfg.nominal_wheel_load : corner_mass * g;

    // --- Gears, and the reverse mapping -------------------------------------------------
    // Arcade convention (GTA's): brake held at a standstill selects reverse, and in reverse the
    // two pedals swap roles, so the same key both reverses and brakes depending on which way the
    // vehicle is already going. Everything below this point sees plain throttle/brake.
    const bool differential_steer = cfg.steer_mode == SteerMode::Differential;
    float throttle = std::clamp(in.throttle, 0.0f, 1.0f);
    float brake = std::clamp(in.brake, 0.0f, 1.0f);
    st.shift_timer = std::max(st.shift_timer - h, 0.0f);
    st.shift_cooldown = std::max(st.shift_cooldown - h, 0.0f);

    if (differential_steer) {
        // A skid-steer machine has no gearbox to speak of: it runs a single-range HYDROSTATIC
        // drive, where engine speed is set by the throttle and the tracks are driven through a
        // pump that does not care how fast they are turning. Modelling it as a geared drive
        // instead couples the engine rigidly to the tracks, and a pivot turn -- the two sides
        // counter-rotating at speed while the machine itself barely moves -- then reads as
        // 13000 rpm, the rev limiter cuts all torque, and the machine cannot turn at all.
        //
        // Top gear is used for its ratio alone: it sets the torque and the track speed the drive
        // tops out at (see the falloff in the wheel-spin pass).
        st.gear = std::max(cfg.gearbox.top_gear(), 1);
    } else {
        const bool crawling = std::abs(st.forward_speed) < 0.6f;
        if (cfg.gearbox.automatic && !in.manual) {
            if (st.gear >= 0 && crawling && brake > 0.1f && throttle < 0.1f) st.gear = -1;
            else if (st.gear < 0 && crawling && throttle > 0.1f) st.gear = 1;
        } else if (in.gear_request != 0) {
            st.gear = std::clamp(st.gear + in.gear_request, -1, cfg.gearbox.top_gear());
        }
        if (st.gear < 0) std::swap(throttle, brake);

        if (cfg.gearbox.automatic && !in.manual && st.shift_timer <= 0.0f &&
            st.shift_cooldown <= 0.0f) {
            // The engine speed the ROAD speed implies in this gear -- see auto_gear()'s doc for
            // why the real, wheelspin-inflated figure must not be used here.
            float driven_radius = 0.0f;
            float driven_count = 0.0f;
            for (const WheelConfig& w : cfg.wheels) {
                if (w.drive_share <= 0.0f) continue;
                driven_radius += w.radius;
                driven_count += 1.0f;
            }
            driven_radius = driven_count > 0.0f ? driven_radius / driven_count : 0.35f;
            const float road_rpm = std::abs(st.forward_speed) / std::max(driven_radius, 0.01f) *
                                   std::abs(cfg.gearbox.ratio(st.gear)) * k_rad_s_to_rpm;
            const int next = auto_gear(cfg.gearbox, st.gear, road_rpm, st.forward_speed, throttle);
            if (next != st.gear) {
                st.gear = next;
                st.shift_timer = cfg.gearbox.shift_time;
                st.shift_cooldown = cfg.gearbox.shift_time + cfg.gearbox.shift_cooldown;
            }
        }
    }

    if (st.gear != gear_before) st.gear_changed = true;

    const float handbrake = std::clamp(in.handbrake, 0.0f, 1.0f);
    const float steer_in = std::clamp(in.steer, -1.0f, 1.0f);
    if (driver_active) body->wake();  // keep it awake while it is being driven

    // --- Steering rack ------------------------------------------------------------------
    const float speed_t = cfg.steer_falloff_speed > 1e-3f
                              ? std::min(std::abs(st.forward_speed) / cfg.steer_falloff_speed, 1.0f)
                              : 0.0f;
    const float lock = glm::radians(cfg.max_steer_deg + (cfg.min_steer_deg - cfg.max_steer_deg) * speed_t);
    const float target_angle = steer_in * lock;
    const bool returning = std::abs(target_angle) < std::abs(st.steer_angle);
    const float rack_rate = glm::radians(returning ? cfg.steer_return_rate_deg : cfg.steer_rate_deg);
    st.steer_angle = move_toward_(st.steer_angle, target_angle, rack_rate * h);

    // Wheelbase and the rear axle, for Ackermann.
    float y_front = cfg.wheels[0].anchor.y;
    float y_rear = cfg.wheels[0].anchor.y;
    float max_half_track = 0.0f;
    for (const WheelConfig& w : cfg.wheels) {
        y_front = std::max(y_front, w.anchor.y);
        y_rear = std::min(y_rear, w.anchor.y);
        max_half_track = std::max(max_half_track, std::abs(w.anchor.x));
    }
    const float wheelbase = std::max(y_front - y_rear, 0.1f);
    float turn_radius = 0.0f;  // signed, about the chassis-local +X side; 0 = straight
    if (std::abs(st.steer_angle) > 1e-4f) {
        turn_radius = wheelbase / std::tan(st.steer_angle);
        // Never let the turn centre fall inside the track: that is a geometric singularity, not
        // a tighter turn, and it flips one wheel's Ackermann angle past 90 degrees.
        const float min_r = max_half_track + 0.2f;
        if (std::abs(turn_radius) < min_r) turn_radius = std::copysign(min_r, turn_radius);
    }

    // --- Differential steering's per-side command ---------------------------------------
    float left_cmd = throttle;
    float right_cmd = throttle;
    if (differential_steer) {
        // Each track takes drive +/- steer, so full steer with no drive counter-rotates the two
        // sides and the machine turns on the spot. The brake pedal drives the tracks BACKWARDS
        // rather than braking them: there is no reverse gear to select, and a lever pulled back
        // is how a tracked machine reverses. The handbrake still stops both tracks.
        const float drive_axis = throttle - brake;
        brake = 0.0f;
        // Steering right (positive) drives the LEFT track forward and the right one back.
        left_cmd = std::clamp(drive_axis + steer_in, -1.0f, 1.0f);
        right_cmd = std::clamp(drive_axis - steer_in, -1.0f, 1.0f);
    }

    // --- Pass 1: suspension casts -------------------------------------------------------
    std::vector<WheelScratch> scratch(n);
    st.wheels_on_ground = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const WheelConfig& wc = cfg.wheels[i];
        WheelState& ws = st.wheels[i];
        WheelScratch& sc = scratch[i];

        ws.steer_angle = 0.0f;
        if (cfg.steer_mode == SteerMode::Ackermann && wc.steered) {
            if (std::abs(turn_radius) > 1e-4f) {
                const float arm = wc.anchor.y - y_rear;
                ws.steer_angle = std::atan2(arm, turn_radius - wc.anchor.x);
                // atan2 returns the angle to the turn centre; keep it on the same side as the
                // rack and inside the lock, whatever the geometry says.
                ws.steer_angle = std::copysign(std::min(std::abs(ws.steer_angle), lock * 1.3f), st.steer_angle);
            }
        }
        sc.drive_command = wc.side == WheelSide::Left ? left_cmd : right_cmd;

        sc.anchor_world = body->position + rot * wc.anchor;
        sc.axis = down;

        geometry::Ray ray;
        ray.origin = sc.anchor_world;
        ray.direction = sc.axis;
        ray.max_distance = wc.rest_length + wc.radius;

        query::QueryFilter filter;
        filter.ignore = chassis;
        filter.include_triggers = false;

        query::RaycastHit hit;
        bool hit_ground = world.raycast(ray, hit, filter);

        // A hit only counts as ground if it faces up relative to the chassis AND sits a sane
        // distance below the anchor. Both guards matter:
        //
        //  - An obstacle taller than the anchor itself (a 0.9 m cone against a 0.72 m suspension
        //    mount) means the ray STARTS inside it, and the hit comes back at ~zero distance.
        //    Read as suspension travel that is full compression, so the spring fires at its
        //    limit and launches the vehicle backwards off a traffic cone -- an 8 kg cone
        //    reversing a 2 tonne truck at 13 m/s. Resolving that overlap is the chassis
        //    collider's job, not the suspension's.
        //  - A near-vertical surface is a wall, not a floor. Letting one act as ground lets a
        //    wheel climb it, since the cast only ever looks down.
        //
        // The floor is half a wheel radius, comfortably under the closest a fully compressed
        // suspension can legitimately report (`rest_length - travel + radius`).
        if (hit_ground && (hit.distance < wc.radius * 0.5f || glm::dot(hit.normal, up) < 0.1f)) {
            hit_ground = false;
        }

        const float prev_compression = ws.compression;
        if (!hit_ground) {
            ws.grounded = false;
            ws.compression = 0.0f;
            ws.compression_rate = (ws.compression - prev_compression) / h;
            ws.hub_offset = wc.rest_length;
            ws.suspension_force = 0.0f;
            ws.force_long = ws.force_lat = 0.0f;
            ws.slip_ratio = ws.slip_angle = 0.0f;
            ws.slipping = false;
            ws.contact_body = dynamics::BodyId{};
            continue;
        }

        ws.grounded = true;
        ++st.wheels_on_ground;
        ws.contact_point = hit.point;
        ws.contact_normal = hit.normal;
        ws.contact_body = hit.body;

        const collision::Shape* ground_shape = world.get_shape_at(hit.shape_index);
        const dynamics::PhysicsMaterial& ground_mat =
            ground_shape ? ground_shape->material_or_default() : dynamics::PhysicsMaterial::default_material();
        ws.surface_grip = ground_mat.dynamic_friction;
        ws.surface_rolling_resistance = ground_mat.rolling_resistance;

        const float length = std::clamp(hit.distance - wc.radius, wc.rest_length - wc.travel, wc.rest_length);
        ws.compression = wc.rest_length - length;
        ws.hub_offset = length;
        ws.compression_rate = (ws.compression - prev_compression) / h;

        const float k = wc.resolved_stiffness(corner_mass);
        // Damping from the hub's real velocity along the suspension axis, not from the
        // finite-differenced compression: the latter lags by a substep and rings.
        const glm::vec3 hub_world = sc.anchor_world + sc.axis * length;
        glm::vec3 contact_vel = body->velocity_at_point(hub_world);
        if (const dynamics::Body* gb = world.get_body(hit.body)) {
            if (gb->type != dynamics::BodyType::Static) contact_vel -= gb->velocity_at_point(hit.point);
        }
        // Positive while compressing: `axis` points along the chassis' down direction, and the
        // suspension compresses exactly when the hub travels that way relative to the ground.
        const float axis_speed = glm::dot(contact_vel, sc.axis);
        const float critical = 2.0f * std::sqrt(std::max(k * corner_mass, 0.0f));
        const float c = critical * (axis_speed > 0.0f ? wc.damping_ratio : wc.rebound_ratio);

        sc.spring_force = k * ws.compression + c * axis_speed;
        sc.forward = project_to_plane_(fwd * std::cos(ws.steer_angle) + right * std::sin(ws.steer_angle),
                                       ws.contact_normal, fwd);
        sc.right = safe_normalize_(glm::cross(ws.contact_normal, sc.forward), right);
    }
    st.airborne = st.wheels_on_ground == 0;
    if (was_airborne && !st.airborne) {
        // The velocity BEFORE this substep's suspension impulses: afterwards the springs have
        // already taken the hit out of it, and the landing would read as gentle however hard it was.
        st.landing_impact = std::max(st.landing_impact, glm::dot(velocity_before, down));
    }

    // --- Pass 2: anti-roll, then the suspension impulse ---------------------------------
    for (std::size_t i = 0; i < n; ++i) scratch[i].load = scratch[i].spring_force;
    if (cfg.anti_roll > 0.0f) {
        for (std::size_t i = 0; i < n; ++i) {
            const int group = cfg.wheels[i].anti_roll_group;
            if (group < 0) continue;
            for (std::size_t j = i + 1; j < n; ++j) {
                if (cfg.wheels[j].anti_roll_group != group) continue;
                // Only across two wheels that are both on the ground. A real bar does push down
                // on a lifted wheel, but a raycast wheel in the air carries no load to give
                // back, so modelling that here only drains the loaded side -- and with the
                // transfer unbounded it drains it past zero, rolls the chassis further, and
                // diverges. A six-wheeler cresting a ramp found this immediately.
                if (!st.wheels[i].grounded || !st.wheels[j].grounded) continue;
                // The bar transfers load from the compressed side to the extended one, which is
                // what resists roll without adding wheel rate.
                const float diff = st.wheels[i].compression - st.wheels[j].compression;
                // Never move more than half of the lighter wheel's own load: beyond that the
                // bar, not the springs, decides what the axle carries.
                const float cap = 0.5f * std::min(scratch[i].spring_force, scratch[j].spring_force);
                const float transfer = std::clamp(cfg.anti_roll * diff, -cap, cap);
                scratch[i].load -= transfer;
                scratch[j].load += transfer;
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        WheelState& ws = st.wheels[i];
        if (!ws.grounded) continue;
        // A suspension can push the chassis up, never pull it down.
        ws.suspension_force = std::max(scratch[i].load, 0.0f);
        scratch[i].load = ws.suspension_force;
        if (ws.suspension_force > 0.0f) {
            body->apply_impulse_at_position(ws.contact_normal * (ws.suspension_force * h), ws.contact_point,
                                            /*wake_body=*/false);
        }
    }

    // --- Pass 3: tyre forces ------------------------------------------------------------
    const float grounded_mass = body->mass / static_cast<float>(std::max(st.wheels_on_ground, 1));
    st.max_slip = 0.0f;
    st.any_wheel_slipping = false;
    for (std::size_t i = 0; i < n; ++i) {
        const WheelConfig& wc = cfg.wheels[i];
        WheelState& ws = st.wheels[i];
        WheelScratch& sc = scratch[i];
        const bool was_slipping = ws.slipping;
        if (!ws.grounded) {
            sc.traction_torque = 0.0f;
            continue;
        }

        glm::vec3 patch_vel = body->velocity_at_point(ws.contact_point);
        if (const dynamics::Body* gb = world.get_body(ws.contact_body)) {
            if (gb->type != dynamics::BodyType::Static) patch_vel -= gb->velocity_at_point(ws.contact_point);
        }
        const float v_long = glm::dot(patch_vel, sc.forward);
        const float v_lat = glm::dot(patch_vel, sc.right);

        const float rim = ws.spin * wc.radius;
        // A 1 m/s floor on the denominator: slip ratio is otherwise unbounded at a standstill,
        // where every real tyre model needs the same guard.
        ws.slip_ratio = std::clamp((rim - v_long) / std::max(std::abs(v_long), 1.0f), -4.0f, 4.0f);
        ws.slip_angle = std::atan2(v_lat, std::max(std::abs(v_long), 0.5f));

        sc.mu = available_friction(cfg.tire, ws.surface_grip, ws.suspension_force, nominal_load);
        const TireForces f = tire_forces(cfg.tire, ws.slip_ratio, ws.slip_angle, ws.suspension_force, sc.mu);
        sc.traction_torque = sc.mu * ws.suspension_force * wc.radius;

        // Clamp each axis so one substep can never reverse the velocity it is resisting -- the
        // standard explicit-integration guard, and what keeps a stopped vehicle from buzzing.
        //
        // The longitudinal bound is the impulse that would exactly null the slip, and it has to
        // account for BOTH ends of the contact: an impulse J slows the chassis by J/m and spins
        // the wheel down by J*r^2/I, so the slip it removes is J*(1/m + r^2/I). Leaving the
        // wheel's term out (bounding on the chassis alone) lets a heavy wheel's reaction torque
        // overshoot every substep -- the spin flips sign at 60 Hz, the tyre force averages to
        // nothing, and the vehicle sits at full throttle going nowhere. A dozer's 70 kg rollers
        // are where that first showed up; lighter wheels merely jitter.
        float imp_long = f.longitudinal * h;
        float imp_lat = f.lateral * h;
        const float slip_compliance = 1.0f / grounded_mass + (wc.radius * wc.radius) / wc.spin_inertia();
        const float max_long = std::abs(rim - v_long) / slip_compliance;
        const float max_lat = std::abs(v_lat) * grounded_mass;
        imp_long = std::clamp(imp_long, -max_long, max_long);
        imp_lat = std::clamp(imp_lat, -max_lat, max_lat);

        ws.force_long = imp_long / h;
        ws.force_lat = imp_lat / h;
        ws.slipping = f.sliding;
        st.max_slip = std::max(st.max_slip, std::abs(ws.slip_ratio));
        if (f.sliding) st.any_wheel_slipping = true;
        if (f.sliding && !was_slipping && i < 32) st.slip_started_mask |= 1u << i;

        body->apply_impulse_at_position(sc.forward * imp_long + sc.right * imp_lat, ws.contact_point,
                                        /*wake_body=*/false);
    }

    // --- Pass 4: engine speed and torque to the wheels ----------------------------------
    const float ratio = cfg.gearbox.ratio(st.gear);
    const float total_share = cfg.total_drive_share();
    /** How hard the driver is asking for torque, whichever way it reaches the ground. */
    const float drive_demand =
        differential_steer ? std::max(std::abs(left_cmd), std::abs(right_cmd)) : throttle;

    float driven_spin = 0.0f;
    if (total_share > 0.0f) {
        for (std::size_t i = 0; i < n; ++i) {
            const float share = std::max(cfg.wheels[i].drive_share, 0.0f);
            if (share <= 0.0f) continue;
            // In differential steering the two sides can turn opposite ways, so their signed
            // mean is ~0 while spinning on the spot; the engine still sees the work being done.
            const float s = cfg.steer_mode == SteerMode::Differential ? std::abs(st.wheels[i].spin)
                                                                      : st.wheels[i].spin;
            driven_spin += s * share;
        }
        driven_spin /= total_share;
    }

    const bool unpowered = total_share <= 0.0f || cfg.gearbox.forward_ratios.empty();
    float rpm_target = cfg.engine.idle_rpm;
    if (unpowered) {
        rpm_target = cfg.engine.idle_rpm;  // a trailer has no engine; keep the readout honest
    } else if (differential_steer) {
        // Hydrostatic: the engine answers the throttle, not the tracks (see the gear block).
        rpm_target = cfg.engine.idle_rpm + drive_demand * (cfg.engine.max_rpm - cfg.engine.idle_rpm);
    } else if (std::abs(ratio) > 1e-4f) {
        // Rigidly coupled to the driven wheels, a gear change included: a change cuts TORQUE
        // (`shift_timer`, below), it does not disconnect the engine. Letting the revs free-rev
        // through a change instead makes an automatic run away up its gearbox -- the next
        // shift decision reads those free revs, upshifts on them, and repeats, so the vehicle
        // ends up in top gear below walking pace.
        rpm_target = std::abs(driven_spin * ratio) * k_rad_s_to_rpm;
    } else {
        rpm_target = cfg.engine.idle_rpm + drive_demand * (cfg.engine.max_rpm - cfg.engine.idle_rpm);
    }
    // Floored at idle but deliberately NOT capped at the redline: engine_torque() falls away to
    // nothing just past `max_rpm`, and that roll-off is what makes a gear's top speed a real
    // limit. Clamping the revs instead leaves the engine pinned at full torque for ever, and the
    // vehicle accelerates past any gearing.
    rpm_target = std::max(rpm_target, cfg.engine.idle_rpm);
    const float response = 1.0f - std::exp(-std::max(cfg.engine.response, 0.1f) * h);
    st.engine_rpm += (rpm_target - st.engine_rpm) * response;

    // Drive torque and engine braking are kept apart, because they do not act the same way:
    // drive torque pushes the way the gear points, while engine braking always resists whatever
    // the wheel is actually doing. Folding the two into one signed number (drive - braking,
    // times the gear ratio) is wrong wherever the wheel's rotation disagrees with the gear --
    // most sharply under differential steering, where a side commanded backwards made the
    // braking term drive its rollers the wrong way and spun up a parked dozer's tracks.
    const float torque_curve = engine_torque(cfg.engine, st.engine_rpm);
    const float rev_fraction = st.engine_rpm / std::max(cfg.engine.max_rpm, 1.0f);
    const bool torque_flows = st.shift_timer <= 0.0f && std::abs(ratio) > 1e-4f;

    /** @brief Axle drive torque for a throttle demand of 0..1, Nm, signed by the gear. */
    const auto axle_drive_for = [&](float demand) -> float {
        if (!torque_flows) return 0.0f;
        return torque_curve * std::clamp(demand, 0.0f, 1.0f) * ratio * cfg.gearbox.efficiency;
    };
    /** @brief Magnitude of the axle braking a throttle demand of 0..1 leaves on, Nm. */
    const auto axle_engine_brake_for = [&](float demand) -> float {
        if (!torque_flows) return 0.0f;
        return cfg.engine.braking * (1.0f - std::clamp(demand, 0.0f, 1.0f)) * rev_fraction *
               std::abs(ratio) * cfg.gearbox.efficiency;
    };
    const float axle_torque = axle_drive_for(throttle);

    // The track speed the drive tops out at, rad/s: what the ratio would allow at the redline.
    // A hydrostatic drive makes near-flat torque up to it and nothing beyond, which is what
    // gives a tracked machine a definite top speed without a gearbox to limit it.
    const float max_track_spin = std::abs(ratio) > 1e-4f
                                     ? (cfg.engine.max_rpm / k_rad_s_to_rpm) / std::abs(ratio)
                                     : 0.0f;

    // Open differential: both wheels of an axle get the same torque, so the one with less grip
    // caps the pair -- lift a wheel and the axle gives up. Locked: each wheel takes its share
    // regardless, and the pair is dragged toward a common speed below.
    const bool diff_couples = cfg.drive.differential != DifferentialType::Open;
    const float lock_amount = cfg.drive.differential == DifferentialType::Locked
                                  ? 1.0f
                                  : std::clamp(cfg.drive.limited_slip, 0.0f, 1.0f);

    // --- Pass 5: wheel spin -------------------------------------------------------------
    for (std::size_t i = 0; i < n; ++i) {
        const WheelConfig& wc = cfg.wheels[i];
        WheelState& ws = st.wheels[i];
        const WheelScratch& sc = scratch[i];
        const float inertia = wc.spin_inertia();

        float wheel_torque = 0.0f;
        if (total_share > 0.0f && wc.drive_share > 0.0f) {
            const float share = wc.drive_share / total_share;
            float t = axle_torque * share;
            float demand = throttle;
            if (differential_steer) {
                // Each track is its own throttle, so a side commanded backwards must make real
                // drive torque -- not merely the engine braking a global zero throttle would
                // leave, which is nowhere near enough to turn a 12-tonne machine on the spot.
                demand = std::abs(sc.drive_command);
                const float falloff =
                    max_track_spin > 1e-3f
                        ? std::clamp(1.0f - std::abs(ws.spin) / max_track_spin, 0.0f, 1.0f)
                        : 1.0f;
                t = axle_drive_for(demand) * share * falloff *
                    (sc.drive_command >= 0.0f ? 1.0f : -1.0f);
            }
            if (!diff_couples && ws.grounded) {
                // Traction-limited, by the weaker wheel of the pair.
                float pair_limit = sc.traction_torque;
                for (std::size_t j = 0; j < n; ++j) {
                    if (j == i || cfg.wheels[j].anti_roll_group < 0) continue;
                    if (cfg.wheels[j].anti_roll_group != wc.anti_roll_group) continue;
                    if (cfg.wheels[j].drive_share <= 0.0f) continue;
                    pair_limit = std::min(pair_limit, scratch[j].traction_torque);
                }
                // 1.3x: an open diff still lets a little more through than the weakest wheel's
                // static limit, and clamping dead on it makes a stopped vehicle unable to start.
                t = std::clamp(t, -pair_limit * 1.3f - 1.0f, pair_limit * 1.3f + 1.0f);
            }
            wheel_torque += t;

            // Engine braking, resisting this wheel's own rotation and never reversing it.
            const float eb = axle_engine_brake_for(demand) * share;
            if (eb > 0.0f && std::abs(ws.spin) > 1e-3f) {
                wheel_torque -= std::copysign(std::min(eb, std::abs(ws.spin) * inertia / h), ws.spin);
            }
        }

        // Reaction to the tyre force, and rolling resistance.
        wheel_torque -= ws.force_long * wc.radius;
        if (ws.grounded && std::abs(ws.spin) > 1e-3f) {
            const float rr = cfg.tire.rolling_resistance * ws.surface_rolling_resistance *
                             ws.suspension_force * wc.radius;
            wheel_torque -= std::copysign(rr, ws.spin);
        }

        ws.spin += wheel_torque / inertia * h;

        // Brakes last, clamped so they can stop a wheel but never spin it backwards. The brake
        // torque has to be turned into a change in SPIN (t * h / I) before it is applied -- using
        // the torque itself as a velocity delta overshoots by a factor of I/h, which flips the
        // spin's sign every substep and runs away: a braked wheel ends up turning faster than any
        // throttle could drive it, and the vehicle never slows down at all.
        const float brake_t = cfg.brake_torque * brake * wc.brake_share +
                              cfg.handbrake_torque * handbrake * wc.handbrake_share;
        if (brake_t > 0.0f) {
            const float brake_delta = std::min(brake_t * h / inertia, std::abs(ws.spin));
            ws.spin -= std::copysign(brake_delta, ws.spin);
        }
        ws.spin_angle += ws.spin * h;
    }

    // Locked / limited-slip coupling: drag each driven pair toward their common speed.
    if (diff_couples && lock_amount > 0.0f) {
        const float blend = std::clamp(lock_amount * cfg.drive.coupling * h, 0.0f, 1.0f);
        for (std::size_t i = 0; i < n; ++i) {
            if (cfg.wheels[i].anti_roll_group < 0 || cfg.wheels[i].drive_share <= 0.0f) continue;
            for (std::size_t j = i + 1; j < n; ++j) {
                if (cfg.wheels[j].anti_roll_group != cfg.wheels[i].anti_roll_group) continue;
                if (cfg.wheels[j].drive_share <= 0.0f) continue;
                const float mean = 0.5f * (st.wheels[i].spin + st.wheels[j].spin);
                st.wheels[i].spin += (mean - st.wheels[i].spin) * blend;
                st.wheels[j].spin += (mean - st.wheels[j].spin) * blend;
            }
        }
    }

    // --- Aerodynamics and the air righting ----------------------------------------------
    // Both act at the centre of mass (apply_impulse_at_position there adds no torque) rather
    // than through apply_impulse(), which would wake a parked vehicle every substep.
    if (st.speed > 0.1f && cfg.drag > 0.0f) {
        body->apply_impulse_at_position(-body->linear_velocity * (cfg.drag * st.speed * h), body->position,
                                        /*wake_body=*/false);
    }
    if (st.speed > 0.1f && cfg.downforce > 0.0f && !st.airborne) {
        body->apply_impulse_at_position(down * (cfg.downforce * st.speed * st.speed * h), body->position,
                                        /*wake_body=*/false);
    }
    if (st.airborne && cfg.air_righting > 0.0f) {
        const glm::vec3 lean = glm::cross(up, world_up);  // magnitude = sin(lean angle)
        const glm::vec3 alpha = lean * cfg.air_righting - body->angular_velocity * cfg.air_damping;
        const glm::mat3 inertia_world = glm::inverse(body->inv_inertia_world);
        body->apply_angular_impulse(inertia_world * (alpha * h));
    }
}

} // namespace vehicle
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_VEHICLE_VEHICLE_SIM_H

/**
 * @file drivetrain.h
 * @brief Engine torque curve, gearbox and differential: how much torque reaches each driven
 *        wheel, and at what engine speed. Pure data and pure functions.
 *
 * The chain is rpm -> torque -> gear ratio -> differential -> wheel. Read backwards it is also
 * how the engine learns its own speed: the driven wheels' rotation times the current ratio IS
 * the engine rpm (a rigid clutch), which is why wheelspin revs the engine and why a stalled
 * wheel on a locked differential drags the revs down.
 */

#ifndef PHYSXCOOPA_VEHICLE_DRIVETRAIN_H
#define PHYSXCOOPA_VEHICLE_DRIVETRAIN_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace coopa {
namespace physx {
namespace vehicle {

/** @brief Radians per second to revolutions per minute. */
inline constexpr float k_rad_s_to_rpm = 60.0f / (2.0f * 3.14159265358979f);

/**
 * @struct EngineConfig
 * @brief A torque curve defined by three points -- idle, peak and redline -- plus the braking a
 *        closed throttle applies.
 */
struct EngineConfig {
    float idle_rpm = 800.0f;
    float max_rpm = 6500.0f;
    /** @brief Torque at `peak_rpm`, Nm. The single number that decides how a vehicle pulls:
     *         ~300 for a hot hatch, ~3000 for a haul truck. */
    float peak_torque = 320.0f;
    float peak_rpm = 4000.0f;
    /** @brief Torque at idle and at the redline, as fractions of `peak_torque`. A diesel's curve
     *         is flat (both high); a small petrol engine falls away at both ends. */
    float torque_at_idle = 0.55f;
    float torque_at_max = 0.70f;
    /** @brief Nm of engine braking at the redline with the throttle shut, scaling with rpm. */
    float braking = 40.0f;
    /** @brief How fast the engine's rotating mass follows the speed the gearbox demands, 1/s.
     *         Low values make the revs lazy -- a heavy flywheel. */
    float response = 12.0f;
};

/**
 * @brief Torque the engine makes at `rpm` at full throttle, Nm.
 *
 * Two linear segments through (idle, peak, redline). Past the redline it falls to zero over
 * 500 rpm, which is the limiter.
 */
inline float engine_torque(const EngineConfig& e, float rpm) {
    const float idle = std::max(e.idle_rpm, 1.0f);
    const float peak = std::max(e.peak_rpm, idle + 1.0f);
    const float top = std::max(e.max_rpm, peak + 1.0f);
    const float t_idle = e.peak_torque * e.torque_at_idle;
    const float t_top = e.peak_torque * e.torque_at_max;

    if (rpm <= idle) return t_idle;
    if (rpm <= peak) return t_idle + (e.peak_torque - t_idle) * (rpm - idle) / (peak - idle);
    if (rpm <= top) return e.peak_torque + (t_top - e.peak_torque) * (rpm - peak) / (top - peak);
    return std::max(t_top * (1.0f - (rpm - top) / 500.0f), 0.0f);
}

/**
 * @struct GearboxConfig
 * @brief Gear ratios and when to change them.
 */
struct GearboxConfig {
    /** @brief Forward gear ratios, first gear first. */
    std::vector<float> forward_ratios{3.2f, 2.1f, 1.5f, 1.1f, 0.85f};
    float reverse_ratio = 3.4f;
    float final_drive = 3.9f;
    /** @brief Fraction of torque that survives the drivetrain. */
    float efficiency = 0.9f;

    bool automatic = true;
    float shift_up_rpm = 5800.0f;
    float shift_down_rpm = 2200.0f;
    /** @brief Seconds of no torque during a change. */
    float shift_time = 0.2f;
    /**
     * @brief Seconds a gear must be held before another change is considered, on top of
     *        `shift_time`.
     *
     * Without a dwell an automatic can hunt: it upshifts, the change cuts torque, the revs fall
     * through the downshift threshold while no torque is being made, it downshifts, the torque
     * returns and the revs climb straight back through the upshift threshold. A heavy vehicle
     * with a long `shift_time` can spend nearly all of its time mid-change that way, which looks
     * exactly like an engine with no power.
     */
    float shift_cooldown = 0.6f;

    /** @brief Total ratio for a gear index: 0 is neutral, -1 reverse, 1..n forward. */
    float ratio(int gear) const {
        if (gear == 0) return 0.0f;
        if (gear < 0) return -reverse_ratio * final_drive;
        const std::size_t i = static_cast<std::size_t>(gear - 1);
        if (i >= forward_ratios.size()) return 0.0f;
        return forward_ratios[i] * final_drive;
    }

    int top_gear() const { return static_cast<int>(forward_ratios.size()); }
};

/** @brief How an axle's two wheels share torque. */
enum class DifferentialType {
    /** @brief Equal torque to both wheels, so the one with less grip sets the pair's total --
     *         lift a wheel and the axle gives up. The road-car default. */
    Open,
    /** @brief Partly couples the two, bounded by `DriveConfig::limited_slip`. */
    LimitedSlip,
    /** @brief Both wheels forced to the same speed, so one wheel with grip can pull the vehicle
     *         out. What makes an offroad truck an offroad truck. */
    Locked,
};

/**
 * @struct DriveConfig
 * @brief Which differential each axle runs, and how it couples the pair.
 *
 * Which wheels are driven at all is `WheelConfig::drive_share`, not a layout enum here -- one
 * source of truth, and it allows a 6x6 whose middle axle takes a smaller share.
 */
struct DriveConfig {
    DifferentialType differential = DifferentialType::Open;
    /** @brief For LimitedSlip: 0 behaves as Open, 1 as Locked. */
    float limited_slip = 0.5f;
    /** @brief How hard a Locked/LimitedSlip axle drags its two wheels to a common speed, 1/s. */
    float coupling = 25.0f;
};

/** @brief How steering reaches the ground. */
enum class SteerMode {
    /** @brief Steered wheels turn, inner wheel more than outer. */
    Ackermann,
    /** @brief No wheel turns; the two sides are driven at different speeds instead. A tracked
     *         vehicle, which can therefore spin on the spot. */
    Differential,
};

/**
 * @brief Picks the gear an automatic gearbox should be in.
 *
 * `rpm` here must be the engine speed the vehicle's GROUND SPEED implies in the current gear, not
 * the one its wheels are actually turning at. The two differ exactly when a wheel is slipping,
 * and that is when using the real figure goes wrong: a truck that spins its wheels pulling away
 * reports a redline it is nowhere near, upshifts on it, loses torque to the change, slows, drops
 * back down, and hunts between two gears without ever moving. Real transmissions shift on
 * output-shaft speed for the same reason. vehicle_sim.h computes it.
 *
 * @param box           The gearbox.
 * @param gear          Current gear (0 neutral, -1 reverse, 1..n forward).
 * @param rpm           Engine speed implied by the road speed in this gear.
 * @param forward_speed Signed speed along the vehicle's forward axis, m/s.
 * @param throttle      0..1, after the reverse swap (see vehicle_sim.h).
 * @return The gear to be in; equal to `gear` when no change is due.
 */
inline int auto_gear(const GearboxConfig& box, int gear, float rpm, float forward_speed, float throttle) {
    if (!box.automatic || gear <= 0) return gear;
    if (rpm >= box.shift_up_rpm && gear < box.top_gear()) return gear + 1;
    // Don't drop a gear just because the throttle is shut and the revs fell with it -- that is
    // engine braking, not a vehicle that needs a lower gear.
    if (rpm <= box.shift_down_rpm && gear > 1 && throttle > 0.05f) return gear - 1;
    if (std::abs(forward_speed) < 0.5f && gear > 1) return 1;
    return gear;
}

} // namespace vehicle
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_VEHICLE_DRIVETRAIN_H

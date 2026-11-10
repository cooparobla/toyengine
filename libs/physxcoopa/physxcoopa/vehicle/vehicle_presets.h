/**
 * @file vehicle_presets.h
 * @brief The shipped vehicle tunings, by name -- `sports_car`, `pickup_4x4`, `hauler_6x6`,
 *        `buggy`, `dozer` and `trailer`.
 *
 * Every one of them runs the same vehicle::simulate(); what makes a hauler crawl over a log and
 * a sports car spin its wheels in the same mud is entirely the numbers in here. A scene names a
 * preset and overrides the few keys it cares about, so a vehicle is a handful of lines of YAML
 * rather than fifty.
 *
 * The anchors assume the chassis' centre of mass is where the prefab puts it (low, near the
 * middle), since WheelConfig::anchor is measured from it -- see wheel.h. The masses each tuning
 * was balanced against are in its comment; a vehicle at a very different mass needs its spring
 * rates revisited, which `natural_frequency` does automatically (it derives stiffness from the
 * corner's real load) as long as the Rigidbody's mass is set to something sane.
 */

#ifndef PHYSXCOOPA_VEHICLE_VEHICLE_PRESETS_H
#define PHYSXCOOPA_VEHICLE_VEHICLE_PRESETS_H

#include <physxcoopa/vehicle/vehicle_sim.h>

#include <string>
#include <vector>

namespace coopa {
namespace physx {
namespace vehicle {

namespace detail {

/** @brief One wheel, spelled out positionally to keep the tables below readable. */
inline WheelConfig wheel_(const glm::vec3& anchor, WheelSide side, int group, float radius, float width,
                          float mass, float rest_length, float travel, float natural_frequency,
                          float damping_ratio, float rebound_ratio, bool steered, float drive_share,
                          float brake_share, float handbrake_share) {
    WheelConfig w;
    w.anchor = anchor;
    w.side = side;
    w.anti_roll_group = group;
    w.radius = radius;
    w.width = width;
    w.mass = mass;
    w.rest_length = rest_length;
    w.travel = travel;
    w.natural_frequency = natural_frequency;
    w.damping_ratio = damping_ratio;
    w.rebound_ratio = rebound_ratio;
    w.steered = steered;
    w.drive_share = drive_share;
    w.brake_share = brake_share;
    w.handbrake_share = handbrake_share;
    return w;
}

/** @brief A symmetric axle: the same wheel mirrored across the centreline. */
inline void axle_(std::vector<WheelConfig>& out, float half_track, float y, float z, int group, float radius,
                  float width, float mass, float rest_length, float travel, float natural_frequency,
                  float damping_ratio, float rebound_ratio, bool steered, float drive_share,
                  float brake_share, float handbrake_share) {
    out.push_back(wheel_({-half_track, y, z}, WheelSide::Left, group, radius, width, mass, rest_length, travel,
                         natural_frequency, damping_ratio, rebound_ratio, steered, drive_share, brake_share,
                         handbrake_share));
    out.push_back(wheel_({half_track, y, z}, WheelSide::Right, group, radius, width, mass, rest_length, travel,
                         natural_frequency, damping_ratio, rebound_ratio, steered, drive_share, brake_share,
                         handbrake_share));
}

} // namespace detail

/**
 * @brief Light, stiff, grippy, rear-wheel drive: ~1300 kg.
 *
 * Short travel and a hard anti-roll bar keep it flat, and a low tyre plateau means a slide is
 * punished rather than enjoyed -- it is fast on tarmac and useless on anything else. Its 0.13 m
 * of travel is what makes the course's kerbs and logs impassable for it.
 */
inline VehicleConfig sports_car_preset() {
    VehicleConfig c;
    detail::axle_(c.wheels, 0.78f, 1.25f, -0.22f, 0, 0.33f, 0.22f, 20.0f, 0.30f, 0.13f, 2.1f, 0.42f, 0.60f,
                  /*steered=*/true, /*drive=*/0.0f, /*brake=*/1.0f, /*handbrake=*/0.0f);
    detail::axle_(c.wheels, 0.78f, -1.30f, -0.22f, 1, 0.33f, 0.22f, 20.0f, 0.30f, 0.13f, 2.1f, 0.42f, 0.60f,
                  false, 1.0f, 0.70f, 1.0f);

    c.tire.grip = 1.35f;
    c.tire.peak_slip_ratio = 0.10f;
    c.tire.peak_slip_angle = 0.12f;
    c.tire.plateau = 0.68f;
    c.tire.rolling_resistance = 0.9f;
    c.tire.load_sensitivity = 0.25f;

    c.engine.idle_rpm = 900.0f;
    c.engine.max_rpm = 7000.0f;
    c.engine.peak_torque = 320.0f;
    c.engine.peak_rpm = 4800.0f;
    c.engine.torque_at_idle = 0.50f;
    c.engine.torque_at_max = 0.78f;
    c.engine.braking = 45.0f;
    c.engine.response = 14.0f;

    c.gearbox.forward_ratios = {3.1f, 2.1f, 1.55f, 1.2f, 0.95f, 0.85f};
    c.gearbox.reverse_ratio = 3.2f;
    c.gearbox.final_drive = 3.9f;
    c.gearbox.shift_up_rpm = 6600.0f;
    c.gearbox.shift_down_rpm = 2600.0f;
    c.gearbox.shift_time = 0.12f;

    c.drive.differential = DifferentialType::LimitedSlip;
    c.drive.limited_slip = 0.35f;

    c.max_steer_deg = 32.0f;
    c.min_steer_deg = 7.0f;
    c.steer_falloff_speed = 40.0f;
    c.steer_rate_deg = 300.0f;
    c.steer_return_rate_deg = 420.0f;

    c.brake_torque = 4200.0f;
    c.handbrake_torque = 5000.0f;
    c.anti_roll = 14000.0f;
    c.drag = 0.42f;
    c.downforce = 0.6f;
    c.air_righting = 2.0f;
    c.air_damping = 1.2f;
    return c;
}

/**
 * @brief The all-rounder: ~2100 kg, limited-slip all-wheel drive, 0.26 m of travel.
 *
 * Rear-biased torque so it still rotates on the throttle, enough travel and ground clearance for
 * the gravel and the rock ramps, and it will cross the shallow end of the mud -- but it bogs in
 * the deep end, which is what the hauler is for.
 */
inline VehicleConfig pickup_4x4_preset() {
    VehicleConfig c;
    detail::axle_(c.wheels, 0.85f, 1.45f, -0.32f, 0, 0.40f, 0.28f, 30.0f, 0.42f, 0.26f, 1.45f, 0.45f, 0.65f,
                  true, 0.5f, 1.0f, 0.0f);
    detail::axle_(c.wheels, 0.85f, -1.40f, -0.32f, 1, 0.40f, 0.28f, 30.0f, 0.42f, 0.26f, 1.45f, 0.45f, 0.65f,
                  false, 1.0f, 0.85f, 1.0f);

    c.tire.grip = 1.05f;
    c.tire.peak_slip_ratio = 0.14f;
    c.tire.peak_slip_angle = 0.16f;
    c.tire.plateau = 0.80f;
    c.tire.rolling_resistance = 1.0f;

    c.engine.idle_rpm = 800.0f;
    c.engine.max_rpm = 5200.0f;
    c.engine.peak_torque = 420.0f;
    c.engine.peak_rpm = 3200.0f;
    c.engine.torque_at_idle = 0.70f;
    c.engine.torque_at_max = 0.60f;
    c.engine.braking = 50.0f;
    c.engine.response = 11.0f;

    c.gearbox.forward_ratios = {4.0f, 2.4f, 1.6f, 1.15f, 0.88f};
    c.gearbox.reverse_ratio = 4.0f;
    c.gearbox.final_drive = 4.1f;
    c.gearbox.shift_up_rpm = 4700.0f;
    c.gearbox.shift_down_rpm = 1700.0f;
    c.gearbox.shift_time = 0.22f;

    c.drive.differential = DifferentialType::LimitedSlip;
    c.drive.limited_slip = 0.6f;

    c.max_steer_deg = 36.0f;
    c.min_steer_deg = 11.0f;
    c.steer_falloff_speed = 28.0f;
    c.steer_rate_deg = 240.0f;
    c.steer_return_rate_deg = 340.0f;

    c.brake_torque = 5200.0f;
    c.handbrake_torque = 7000.0f;
    c.anti_roll = 8000.0f;
    c.drag = 0.9f;
    c.air_righting = 2.5f;
    return c;
}

/**
 * @brief Six driven wheels, locked differentials, 0.40 m of travel: ~9000 kg.
 *
 * Geared for torque rather than speed (~30 m/s flat out) and locked so a single wheel with grip
 * drags the whole machine out of a hole. This is the one that crosses the deep mud and steps
 * over the 0.4 m ledge; everything about it is slow on purpose.
 */
inline VehicleConfig hauler_6x6_preset() {
    VehicleConfig c;
    detail::axle_(c.wheels, 1.05f, 2.60f, -0.50f, 0, 0.62f, 0.40f, 90.0f, 0.60f, 0.40f, 1.25f, 0.50f, 0.70f,
                  true, 0.7f, 1.0f, 0.0f);
    detail::axle_(c.wheels, 1.05f, -0.60f, -0.50f, 1, 0.62f, 0.40f, 90.0f, 0.60f, 0.40f, 1.25f, 0.50f, 0.70f,
                  false, 1.0f, 1.0f, 0.5f);
    detail::axle_(c.wheels, 1.05f, -2.00f, -0.50f, 2, 0.62f, 0.40f, 90.0f, 0.60f, 0.40f, 1.25f, 0.50f, 0.70f,
                  false, 1.0f, 1.0f, 1.0f);

    c.tire.grip = 0.95f;
    c.tire.peak_slip_ratio = 0.18f;
    c.tire.peak_slip_angle = 0.20f;
    c.tire.plateau = 0.88f;
    c.tire.rolling_resistance = 1.1f;
    c.tire.load_sensitivity = 0.15f;

    c.engine.idle_rpm = 650.0f;
    c.engine.max_rpm = 2600.0f;
    c.engine.peak_torque = 2600.0f;
    c.engine.peak_rpm = 1400.0f;
    c.engine.torque_at_idle = 0.85f;
    c.engine.torque_at_max = 0.70f;
    c.engine.braking = 120.0f;
    c.engine.response = 8.0f;

    c.gearbox.forward_ratios = {6.5f, 3.8f, 2.3f, 1.5f, 1.0f};
    c.gearbox.reverse_ratio = 6.0f;
    c.gearbox.final_drive = 5.2f;
    c.gearbox.shift_up_rpm = 2300.0f;
    c.gearbox.shift_down_rpm = 1100.0f;
    c.gearbox.shift_time = 0.45f;

    c.drive.differential = DifferentialType::Locked;
    c.drive.coupling = 30.0f;

    c.max_steer_deg = 38.0f;
    c.min_steer_deg = 16.0f;
    c.steer_falloff_speed = 14.0f;
    c.steer_rate_deg = 150.0f;
    c.steer_return_rate_deg = 200.0f;

    c.brake_torque = 22000.0f;
    c.handbrake_torque = 30000.0f;
    c.anti_roll = 25000.0f;
    c.drag = 3.2f;
    c.air_righting = 1.2f;
    return c;
}

/**
 * @brief Light, enormous travel, loose tyres: ~700 kg.
 *
 * 0.42 m of soft travel and a high tyre plateau mean it slides predictably and lands jumps
 * without bottoming out; strong air righting keeps it pointing the right way in the air. The
 * worst of the lineup at putting power down, the best at being thrown around.
 */
inline VehicleConfig buggy_preset() {
    VehicleConfig c;
    detail::axle_(c.wheels, 0.85f, 1.20f, -0.30f, 0, 0.38f, 0.30f, 16.0f, 0.50f, 0.42f, 1.50f, 0.35f, 0.55f,
                  true, 0.7f, 1.0f, 0.0f);
    detail::axle_(c.wheels, 0.85f, -1.25f, -0.30f, 1, 0.38f, 0.30f, 16.0f, 0.50f, 0.42f, 1.50f, 0.35f, 0.55f,
                  false, 1.0f, 0.8f, 1.0f);

    c.tire.grip = 0.95f;
    c.tire.peak_slip_ratio = 0.16f;
    c.tire.peak_slip_angle = 0.20f;
    c.tire.plateau = 0.85f;
    c.tire.rolling_resistance = 0.95f;

    c.engine.idle_rpm = 1000.0f;
    c.engine.max_rpm = 7500.0f;
    c.engine.peak_torque = 240.0f;
    c.engine.peak_rpm = 5200.0f;
    c.engine.torque_at_idle = 0.55f;
    c.engine.torque_at_max = 0.75f;
    c.engine.braking = 30.0f;
    c.engine.response = 16.0f;

    c.gearbox.forward_ratios = {3.4f, 2.2f, 1.55f, 1.15f};
    c.gearbox.reverse_ratio = 3.4f;
    c.gearbox.final_drive = 4.3f;
    c.gearbox.shift_up_rpm = 7000.0f;
    c.gearbox.shift_down_rpm = 2800.0f;
    c.gearbox.shift_time = 0.14f;

    c.drive.differential = DifferentialType::LimitedSlip;
    c.drive.limited_slip = 0.8f;

    c.max_steer_deg = 38.0f;
    c.min_steer_deg = 14.0f;
    c.steer_falloff_speed = 26.0f;
    c.steer_rate_deg = 320.0f;
    c.steer_return_rate_deg = 420.0f;

    c.brake_torque = 2600.0f;
    c.handbrake_torque = 4000.0f;
    c.anti_roll = 3000.0f;
    c.drag = 0.55f;
    c.air_righting = 4.0f;
    c.air_damping = 2.0f;
    return c;
}

/**
 * @brief Tracked: eight rollers, no steered wheels, differential steering. ~12000 kg.
 *
 * `SteerMode::Differential` drives the two sides against each other, so full steer with no
 * throttle spins it on the spot. The rollers share one group per side, which is what the locked
 * differential's coupling uses to hold each whole side to a single speed -- a track, not eight
 * independent wheels.
 *
 * `lateral_grip` is deliberately BELOW 1. A track digging in sounds like it should resist
 * sliding sideways hardest of all, but skid steering is precisely the act of dragging both
 * tracks sideways across the ground: a machine whose lateral grip beats its longitudinal drive
 * cannot turn at all, however much torque the two sides fight each other with. Rollers 1.7 m
 * fore and aft of the centre resist a yaw on a longer arm than the 1.15 m the drive pushes on,
 * so the lateral limit has to sit well under the longitudinal one -- under
 * 1.15/1.75 of it, or the rollers' restoring couple simply wins. A long, flat track plus a low
 * centre of mass is what keeps it on a side slope, not lateral friction.
 */
inline VehicleConfig dozer_preset() {
    VehicleConfig c;
    const float y[4] = {1.70f, 0.55f, -0.60f, -1.75f};
    for (int i = 0; i < 4; ++i) {
        c.wheels.push_back(detail::wheel_({-1.15f, y[i], -0.45f}, WheelSide::Left, 0, 0.45f, 0.50f, 70.0f,
                                          0.30f, 0.12f, 2.40f, 0.70f, 0.80f, false, 1.0f, 1.0f, 1.0f));
        c.wheels.push_back(detail::wheel_({1.15f, y[i], -0.45f}, WheelSide::Right, 1, 0.45f, 0.50f, 70.0f,
                                          0.30f, 0.12f, 2.40f, 0.70f, 0.80f, false, 1.0f, 1.0f, 1.0f));
    }

    c.steer_mode = SteerMode::Differential;

    c.tire.grip = 1.10f;
    c.tire.lateral_grip = 0.40f;
    c.tire.peak_slip_ratio = 0.30f;
    c.tire.peak_slip_angle = 0.50f;
    c.tire.plateau = 0.95f;
    c.tire.rolling_resistance = 1.30f;
    c.tire.load_sensitivity = 0.10f;

    c.engine.idle_rpm = 700.0f;
    c.engine.max_rpm = 2400.0f;
    c.engine.peak_torque = 3000.0f;
    c.engine.peak_rpm = 1300.0f;
    c.engine.torque_at_idle = 0.90f;
    c.engine.torque_at_max = 0.75f;
    c.engine.braking = 150.0f;
    c.engine.response = 7.0f;

    c.gearbox.forward_ratios = {5.0f, 2.6f, 1.6f};
    c.gearbox.reverse_ratio = 5.0f;
    c.gearbox.final_drive = 6.5f;
    c.gearbox.shift_up_rpm = 2100.0f;
    c.gearbox.shift_down_rpm = 1000.0f;
    c.gearbox.shift_time = 0.5f;

    c.drive.differential = DifferentialType::Locked;
    c.drive.coupling = 40.0f;

    // Steering does not reach a wheel angle at all here, so the lock figures are inert; they are
    // left at the default so a scene overriding `steer_mode` back to Ackermann still works.
    c.steer_rate_deg = 400.0f;
    c.steer_return_rate_deg = 500.0f;

    c.brake_torque = 40000.0f;
    c.handbrake_torque = 60000.0f;
    // Rollers on one track must not fight each other through an anti-roll bar.
    c.anti_roll = 0.0f;
    c.drag = 4.0f;
    c.air_righting = 0.8f;
    return c;
}

/**
 * @brief Unpowered two-axle trailer: ~1500 kg. Rolls, brakes and carries load; no engine.
 *
 * With no forward ratios the gearbox passes no torque whatever the throttle, so this is safe to
 * leave `player_input: false` and tow with a HingeJoint. Its wheels still suspend, grip and roll,
 * which is what makes a loaded trailer push a truck wide in a corner.
 */
inline VehicleConfig trailer_preset() {
    VehicleConfig c;
    detail::axle_(c.wheels, 0.95f, 0.40f, -0.30f, 0, 0.38f, 0.26f, 25.0f, 0.40f, 0.22f, 1.60f, 0.45f, 0.60f,
                  false, 0.0f, 0.6f, 0.0f);
    detail::axle_(c.wheels, 0.95f, -0.40f, -0.30f, 1, 0.38f, 0.26f, 25.0f, 0.40f, 0.22f, 1.60f, 0.45f, 0.60f,
                  false, 0.0f, 0.6f, 0.0f);

    c.tire.grip = 1.0f;
    c.tire.peak_slip_ratio = 0.14f;
    c.tire.peak_slip_angle = 0.16f;
    c.tire.plateau = 0.80f;

    c.gearbox.forward_ratios.clear();
    c.gearbox.automatic = false;

    c.brake_torque = 3000.0f;
    c.handbrake_torque = 4000.0f;
    c.anti_roll = 4000.0f;
    c.drag = 0.8f;
    c.air_righting = 0.0f;
    return c;
}

/** @brief Every preset name, in the order the editor and the demo scene list them. */
inline const std::vector<std::string>& preset_names() {
    static const std::vector<std::string> names{"sports_car", "pickup_4x4", "hauler_6x6",
                                                 "buggy",      "dozer",      "trailer"};
    return names;
}

/** @brief True if `name` is a known preset. */
inline bool has_preset(const std::string& name) {
    const std::vector<std::string>& names = preset_names();
    for (const std::string& n : names) {
        if (n == name) return true;
    }
    return false;
}

/**
 * @brief The named tuning, by value (the caller then applies its own overrides).
 *
 * An unknown name returns `pickup_4x4`, the most forgiving of the set -- a typo'd preset should
 * give a driveable vehicle rather than a four-wheeled brick with no suspension, in the same
 * spirit as physx_yaml.h's other parsers falling back rather than throwing.
 */
inline VehicleConfig preset(const std::string& name) {
    if (name == "sports_car") return sports_car_preset();
    if (name == "pickup_4x4") return pickup_4x4_preset();
    if (name == "hauler_6x6") return hauler_6x6_preset();
    if (name == "buggy") return buggy_preset();
    if (name == "dozer") return dozer_preset();
    if (name == "trailer") return trailer_preset();
    return pickup_4x4_preset();
}

} // namespace vehicle
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_VEHICLE_VEHICLE_PRESETS_H

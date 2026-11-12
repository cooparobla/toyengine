/**
 * @file vehicle.h
 * @brief VehicleComponent -- a driveable vehicle on a Rigidbody chassis, configured from a named
 *        preset in scene YAML and simulated by system/vehicle_system.h.
 *
 * This is the scene-facing half of vehicle/vehicle_sim.h, in the same relationship Cloth has to
 * the cloth solver: the component owns the tuning and the live state, the system owns the
 * stepping. It needs a `Rigidbody` and a collider on the same object -- the chassis is an
 * ordinary dynamic body, and the wheels are raycasts rather than bodies of their own.
 *
 * Per-frame input is PUSHED in, never read from a device here (the same contract as toyengine's
 * CharacterController): a host writes `controls` before the physics phase. `player_input` marks
 * the vehicles a host's driver should write to at all, and `active` which one of those is
 * currently being driven.
 *
 * Example YAML:
 * @code
 * - type: Vehicle
 *   preset: hauler_6x6        # sports_car | pickup_4x4 | hauler_6x6 | buggy | dozer | trailer
 *   max_steer_deg: 34         # any VehicleConfig key overrides the preset's value
 *   auto_recover_time: 4      # rights itself after 4 s on its roof (0, the default: never)
 *   wheels:                   # optional: names the child object drawn at each wheel, in the
 *     - object: wheel_fl      # preset's own wheel order (front axle first, left then right)
 *     - object: wheel_fr
 *     - object: wheel_ml
 *       drive_share: 0.8      # per-wheel overrides are allowed too
 *     - object: wheel_mr
 *     - object: wheel_rl
 *     - object: wheel_rr
 * @endcode
 */

#ifndef PHYSXCOOPA_COMPONENTS_VEHICLE_H
#define PHYSXCOOPA_COMPONENTS_VEHICLE_H

#include <physxcoopa/vehicle/vehicle_presets.h>
#include <physxcoopa/vehicle/vehicle_sim.h>

#include <coopa/event/signal.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class VehicleComponent
 * @brief See the file doc. Requires a Rigidbody and a collider on the same SceneObject.
 */
class VehicleComponent : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Vehicle"; }

    /** @brief Name of the tuning this vehicle started from (vehicle/vehicle_presets.h). Purely
     *         informational once `config` is resolved -- the parser applies the preset and then
     *         the scene's overrides, so changing this at runtime does nothing by itself. */
    std::string preset = "pickup_4x4";

    /**
     * @brief The resolved tuning: the preset with the scene's overrides applied.
     *
     * Wheel anchors in here are relative to the owning object's ORIGIN, which is how a scene
     * authors them. The system converts them to the centre-of-mass-relative frame the sim wants
     * (see vehicle/wheel.h) when it binds, so this stays the frame an author can reason about.
     */
    vehicle::VehicleConfig config = vehicle::pickup_4x4_preset();

    /** @brief Child object drawn at each wheel, parallel to `config.wheels`. An empty name (or a
     *         shorter list) simply leaves that wheel undrawn -- the simulation does not care. */
    std::vector<std::string> wheel_objects;

    /** @brief Whether a host's driver should push the player's input into this vehicle at all.
     *         False for anything steered by something else: a towed trailer, a parked prop. */
    bool player_input = true;
    /** @brief Whether this is the vehicle currently being driven. A host cycling between
     *         vehicles moves this flag; `controls` is zeroed on whatever it leaves. */
    bool active = true;

    /** @brief This frame's driver input, written by the host before the physics phase. */
    vehicle::VehicleControls controls;

    /** @brief Asks for the vehicle to be righted -- the same thing the host's recover key does.
     *         Smooth, not a teleport: it floats clear, turns back upright about the heading it
     *         had, and lands on its suspension (VehicleConfig::recover_time). */
    void recover() { controls.recover = true; }
    /** @brief Mid-recovery right now. */
    bool is_recovering() const { return state_.is_recovering(); }
    /** @brief Lying on its side or roof (more than 60 degrees off upright). */
    bool is_upside_down() const { return state_.upside_down; }

    // --- Signals ---
    /** @brief The new gear (0 neutral, -1 reverse, 1..n forward). */
    coopa::event::Signal<int> on_gear_change;
    /** @brief Touch-down after every wheel was off the ground, with the speed along the chassis'
     *         down axis at the moment of landing (m/s). */
    coopa::event::Signal<float> on_landed;
    /** @brief A wheel went past its grip limit, with its index -- a skid mark or a tyre squeal.
     *         Fires on the transition, not every substep a wheel stays sliding. */
    coopa::event::Signal<int> on_wheel_slip;

    // --- Live state, written by VehicleSystem every substep ---
    const vehicle::VehicleState& state() const { return state_; }
    vehicle::VehicleState& state_mut() { return state_; }

    float speed_kmh() const { return state_.speed * 3.6f; }
    float forward_speed() const { return state_.forward_speed; }
    float engine_rpm() const { return state_.engine_rpm; }
    int gear() const { return state_.gear; }
    int wheels_on_ground() const { return state_.wheels_on_ground; }
    bool is_airborne() const { return state_.airborne; }
    bool is_slipping() const { return state_.any_wheel_slipping; }

    /** @brief True once VehicleSystem has found this vehicle's chassis body. A vehicle whose
     *         object has no Rigidbody never binds and never simulates. */
    bool is_bound() const { return body_id_.is_valid(); }
    dynamics::BodyId body_id() const { return body_id_; }

    /** @brief The tuning actually handed to vehicle::simulate(): `config` with every wheel anchor
     *         moved into the body's centre-of-mass frame. */
    const vehicle::VehicleConfig& sim_config() const { return sim_config_; }

    /** @brief Called only by VehicleSystem's bind pass. */
    void set_binding(dynamics::BodyId id, const glm::vec3& local_center_of_mass) {
        body_id_ = id;
        sim_config_ = config;
        for (vehicle::WheelConfig& w : sim_config_.wheels) w.anchor -= local_center_of_mass;
        state_.wheels.assign(sim_config_.wheels.size(), vehicle::WheelState{});
        for (std::size_t i = 0; i < sim_config_.wheels.size(); ++i) {
            // Start each wheel at full extension rather than at zero, so the first frame's visual
            // is the authored pose and not a hub buried in the chassis.
            state_.wheels[i].hub_offset = sim_config_.wheels[i].rest_length;
        }
        state_.gear = sim_config_.gearbox.forward_ratios.empty() ? 0 : 1;
        state_.engine_rpm = sim_config_.engine.idle_rpm;
    }

    /** @brief Called only by VehicleSystem when the chassis body goes away. */
    void clear_binding() {
        body_id_ = dynamics::BodyId{};
        state_.valid = false;
    }

private:
    dynamics::BodyId body_id_;
    vehicle::VehicleConfig sim_config_;
    vehicle::VehicleState state_;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_VEHICLE_H

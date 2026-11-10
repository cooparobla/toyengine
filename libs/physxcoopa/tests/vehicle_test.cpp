/**
 * @file vehicle_test.cpp
 * @brief vehicle::simulate() on real colliders: ride height, surface grip, obstacles, steering
 *        (both modes), air behaviour and determinism -- plus the tyre curve on its own.
 *
 * These are the LIBRARY's contract. Component/system/YAML behaviour (binding, wheel visuals,
 * preset overrides from scene YAML) belongs to toyengine's own vehicle_test.cpp, the same split
 * character_motor_test.cpp has with CharacterController's tests.
 *
 * Every check is a behaviour a tuning is meant to produce, not a number the implementation
 * happens to emit, so the bounds are deliberately wide: the point of
 * `hauler_crosses_mud_a_sports_car_bogs_in` is the ORDERING, not either speed.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/dynamics/inertia.h>
#include <physxcoopa/vehicle/vehicle_presets.h>
#include <physxcoopa/world.h>

#include <cmath>
#include <string>
#include <vector>

COOPA_TEST_SUITE("vehicle");

using namespace coopa::physx;

namespace {

/**
 * @struct Spec
 * @brief Chassis mass, collider and spawn height, mirroring assets/objects/vehicles/<name>.yaml.
 *
 * `collider_center_z` matters as much as the size: it is the chassis' GROUND CLEARANCE, which is
 * what decides whether a kerb stops the vehicle (a wheel is a downward ray and will find ground
 * above its own hub, so only the chassis collider can refuse an obstacle). PhysicsSystem folds a
 * single collider's centre into the body's position, so these tests do the same -- see Rig::build.
 */
struct Spec {
    const char* preset;
    float mass;
    glm::vec3 half;
    float collider_center_z;
    /** @brief The prefab's Rigidbody `center_of_mass` override, relative to the collider's
     *         centre. The single number that decides whether the vehicle tips: it rolls once
     *         lateral acceleration passes half-track / centre-of-mass height. */
    float com_override_z;
    float spawn_z;
};

inline const std::vector<Spec>& specs() {
    static const std::vector<Spec> v{
        {"sports_car", 1300.0f, {0.88f, 2.05f, 0.632f}, -0.032f, -0.221f, 0.81f},
        {"pickup_4x4", 2100.0f, {0.95f, 2.40f, 0.855f}, 0.095f, -0.435f, 1.04f},
        {"hauler_6x6", 9000.0f, {1.22f, 3.40f, 1.141f}, 0.059f, -0.691f, 1.58f},
        {"buggy", 700.0f, {0.88f, 1.60f, 0.665f}, -0.065f, -0.415f, 1.09f},
        {"dozer", 12000.0f, {1.20f, 2.10f, 0.736f}, -0.136f, -0.435f, 1.17f},
        {"trailer", 1500.0f, {1.05f, 1.65f, 0.649f}, -0.049f, -0.349f, 1.00f},
    };
    return v;
}

inline const Spec& spec_for(const std::string& preset) {
    for (const Spec& s : specs()) {
        if (preset == s.preset) return s;
    }
    return specs()[1];
}

/** @brief A vehicle on a flat floor of one material, stepped by hand. */
struct Rig {
    PhysicsWorld world;
    dynamics::BodyId chassis;
    vehicle::VehicleConfig cfg;
    vehicle::VehicleState state;
    vehicle::VehicleControls in;
    dynamics::PhysicsMaterial ground;
    coopa::event::Connection connection;

    /** @param friction Ground friction: 0.95 tarmac, 0.60 gravel, 0.45 mud, 0.02 ice.
     *  @param rolling  Ground rolling resistance: 0.015 tarmac, 0.05 gravel, 0.15 mud. */
    void build(const std::string& preset, float friction = 0.95f, float rolling = 0.015f) {
        const Spec& s = spec_for(preset);
        cfg = vehicle::preset(preset);
        ground.dynamic_friction = friction;
        ground.static_friction = friction;
        ground.rolling_resistance = rolling;

        dynamics::Body floor;
        floor.type = dynamics::BodyType::Static;
        floor.position = {0.0f, 0.0f, -2.0f};
        collision::Shape floor_shape = collision::Shape::make_box(glm::vec3(900.0f, 900.0f, 2.0f));
        floor_shape.material = &ground;
        world.add_body(floor, floor_shape);

        // PhysicsSystem folds a single collider's `center` into the body's position and reports it
        // as RigidbodyComponent::local_center_of_mass(), which VehicleComponent::set_binding()
        // then subtracts from every wheel anchor. Reproduced here so a test vehicle has exactly
        // the geometry its prefab does -- otherwise the anchors would be measured from the wrong
        // origin and every ride height would be off.
        dynamics::Body body;
        body.mass = s.mass;
        body.inv_mass = 1.0f / s.mass;
        const float com = s.collider_center_z + s.com_override_z;
        body.position = {0.0f, 0.0f, s.spawn_z + com};
        // The collider stays where it was authored, so its local centre is the offset back from
        // the body's (lowered) origin to the box's own centre.
        collision::Shape shape = collision::Shape::make_box(s.half, {0.0f, 0.0f, -s.com_override_z});
        body.inv_inertia_local = dynamics::inertia_for_shape(shape, s.mass);
        dynamics::update_world_inertia(body);
        chassis = world.add_body(body, shape);
        for (vehicle::WheelConfig& w : cfg.wheels) w.anchor.z -= com;

        connection = world.on_substep.connect([this](PhysicsWorld& w, float h) {
            vehicle::simulate(w, chassis, cfg, in, state, h);
        });
    }

    /** @brief Adds a static box obstacle whose top face is at `top`. */
    void obstacle(const glm::vec3& center_xy_top, const glm::vec3& half) {
        dynamics::Body b;
        b.type = dynamics::BodyType::Static;
        b.position = {center_xy_top.x, center_xy_top.y, center_xy_top.z - half.z};
        collision::Shape shape = collision::Shape::make_box(half);
        shape.material = &ground;
        world.add_body(b, shape);
    }

    void step(float seconds) {
        const int steps = static_cast<int>(seconds * 60.0f);
        for (int i = 0; i < steps; ++i) world.step_fixed(1.0f / 60.0f);
    }
    const dynamics::Body& body() const { return *world.get_body(chassis); }
    glm::vec3 position() const { return body().position; }
    /** @brief Heading in degrees: 0 facing +Y, NEGATIVE when turned to the right. */
    float heading_deg() const {
        const glm::vec3 f = body().orientation * glm::vec3(0.0f, 1.0f, 0.0f);
        return glm::degrees(std::atan2(-f.x, f.y));
    }
    /** @brief How far the chassis leans off the world's up, degrees. */
    float lean_deg() const {
        const glm::vec3 up = body().orientation * glm::vec3(0.0f, 0.0f, 1.0f);
        return glm::degrees(std::acos(std::clamp(up.z, -1.0f, 1.0f)));
    }
};

/** @brief Settles the vehicle, then holds full throttle for `seconds` and returns its speed. */
float full_throttle_speed(const std::string& preset, float friction, float rolling, float seconds) {
    Rig r;
    r.build(preset, friction, rolling);
    r.step(1.5f);
    r.in.throttle = 1.0f;
    r.step(seconds);
    return r.state.forward_speed;
}

} // namespace

/**
 * @brief Every preset settles on its springs rather than on its bump stops or at full extension,
 *        and then stops moving.
 *
 * A corner carrying mass m on rate k settles at m*g/k. Landing outside 15-80% of the travel means
 * the preset's `natural_frequency` disagrees with the mass the prefab gives it: too stiff and the
 * wheel tops out over every bump, too soft and it rides on the stop with no travel left.
 */
COOPA_TEST(every_preset_settles_within_its_suspension_travel) {
    for (const Spec& s : specs()) {
        Rig r;
        r.build(s.preset);
        r.step(4.0f);
        const std::string tag = s.preset;

        EXPECT_EQ(r.state.wheels_on_ground, static_cast<int>(r.state.wheels.size()));
        for (std::size_t i = 0; i < r.state.wheels.size(); ++i) {
            const float travel = r.cfg.wheels[i].travel;
            const float fraction = r.state.wheels[i].compression / travel;
            coopa::test::expect(fraction > 0.15f && fraction < 0.80f,
                                tag + ": wheel " + std::to_string(i) + " settles mid-travel (at " +
                                    std::to_string(fraction) + " of it)");
        }
        coopa::test::expect(glm::length(r.body().linear_velocity) < 0.3f,
                            tag + ": at rest after settling");
        coopa::test::expect(r.lean_deg() < 2.0f, tag + ": sits level");
        coopa::test::expect(std::abs(r.position().z - (s.spawn_z + s.collider_center_z +
                                                       s.com_override_z)) < 0.25f,
                            tag + ": rides near the height its prefab spawns at");
    }
}

/**
 * @brief The engine idles when nothing is asked of it.
 *
 * Worth its own check because two separate bugs drove a parked vehicle's wheels: engine braking
 * that carried the gear's sign rather than opposing the wheel, and a tyre impulse clamp that
 * ignored wheel inertia and oscillated the spin at 60 Hz. Both showed up here first, as revs on
 * a vehicle nobody was driving.
 */
COOPA_TEST(a_parked_vehicle_idles_and_its_wheels_do_not_turn) {
    for (const Spec& s : specs()) {
        Rig r;
        r.build(s.preset);
        r.step(4.0f);
        const std::string tag = s.preset;
        EXPECT_NEAR(r.state.engine_rpm, r.cfg.engine.idle_rpm, 60.0f);
        for (std::size_t i = 0; i < r.state.wheels.size(); ++i) {
            coopa::test::expect(std::abs(r.state.wheels[i].spin) < 1.0f,
                                tag + ": wheel " + std::to_string(i) + " is not turning at rest");
        }
    }
}

/**
 * @brief Surface grip reaches the ground: the same vehicle, the same input, three surfaces.
 *
 * Measured over FOUR seconds, while it is still accelerating. Given long enough every vehicle
 * converges on the speed its gearing and drag allow whatever it is driving on -- grip decides how
 * fast it gets there, not where it ends up -- so a late sample compresses the very difference
 * this is here to show.
 */
COOPA_TEST(the_same_vehicle_accelerates_hardest_on_the_grippiest_surface) {
    for (const char* preset : {"sports_car", "pickup_4x4", "hauler_6x6"}) {
        const float tarmac = full_throttle_speed(preset, 0.95f, 0.015f, 4.0f);
        const float gravel = full_throttle_speed(preset, 0.60f, 0.050f, 4.0f);
        const float mud = full_throttle_speed(preset, 0.45f, 0.150f, 4.0f);
        const std::string tag = preset;
        coopa::test::expect(tarmac > gravel * 1.1f,
                            tag + ": tarmac (" + std::to_string(tarmac) + ") beats gravel (" +
                                std::to_string(gravel) + ")");
        coopa::test::expect(gravel > mud,
                            tag + ": gravel (" + std::to_string(gravel) + ") beats mud (" +
                                std::to_string(mud) + ")");
    }
}

/**
 * @brief The headline difference between the two ends of the lineup.
 *
 * In deep mud the hauler's long gearing and locked differentials keep it moving while the sports
 * car's power only spins its wheels -- and on tarmac the order reverses completely. One course,
 * two outcomes, from tuning alone.
 */
COOPA_TEST(hauler_crosses_mud_a_sports_car_bogs_in) {
    const float hauler_mud = full_throttle_speed("hauler_6x6", 0.45f, 0.150f, 12.0f);
    const float car_mud = full_throttle_speed("sports_car", 0.45f, 0.150f, 12.0f);
    const float hauler_tarmac = full_throttle_speed("hauler_6x6", 0.95f, 0.015f, 12.0f);
    const float car_tarmac = full_throttle_speed("sports_car", 0.95f, 0.015f, 12.0f);

    coopa::test::expect(hauler_mud > 0.8f, "the hauler keeps moving in mud (" +
                                               std::to_string(hauler_mud) + " m/s)");
    coopa::test::expect(car_mud < car_tarmac * 0.5f,
                        "the sports car loses over half its pace in mud (" + std::to_string(car_mud) +
                            " vs " + std::to_string(car_tarmac) + " m/s)");
    coopa::test::expect(car_tarmac > hauler_tarmac * 1.5f,
                        "on tarmac the car is far quicker (" + std::to_string(car_tarmac) + " vs " +
                            std::to_string(hauler_tarmac) + " m/s)");
}

/**
 * @brief Suspension travel and clearance decide what can be driven over.
 *
 * A 0.40 m ledge across the path: the hauler (0.40 m of travel, 0.62 m wheels) steps onto it, the
 * sports car (0.13 m, 0.33 m wheels) is stopped by it.
 */
COOPA_TEST(a_ledge_stops_the_sports_car_and_not_the_hauler) {
    auto reaches_ledge_top = [](const char* preset, float ledge_top) {
        Rig r;
        r.build(preset);
        // A long, wide ledge starting 6 m ahead, so there is room to get up to speed first.
        r.obstacle({0.0f, 16.0f, ledge_top}, {12.0f, 10.0f, ledge_top * 0.5f});
        r.step(1.5f);
        r.in.throttle = 1.0f;
        r.step(10.0f);
        return r.position().y > 8.0f && r.position().z > ledge_top * 0.5f;
    };
    coopa::test::expect(reaches_ledge_top("hauler_6x6", 0.40f), "the hauler climbs the 0.40 m ledge");
    coopa::test::expect(!reaches_ledge_top("sports_car", 0.40f), "the sports car does not");
}

/** @brief Positive steer turns right, in both steering modes. */
COOPA_TEST(steering_right_turns_right) {
    {
        Rig r;
        r.build("pickup_4x4");
        r.step(1.5f);
        r.in.throttle = 1.0f;
        for (int i = 0; i < 1800 && r.state.forward_speed < 12.0f; ++i) r.world.step_fixed(1.0f / 60.0f);
        const float before = r.heading_deg();
        r.in.throttle = 0.35f;
        r.in.steer = 1.0f;
        r.step(3.0f);
        // Heading is negative to the right (see Rig::heading_deg).
        coopa::test::expect(r.heading_deg() - before < -20.0f,
                            "Ackermann: turned right by " + std::to_string(before - r.heading_deg()) +
                                " degrees");
    }
    {
        Rig r;
        r.build("dozer");
        r.step(1.5f);
        const float before = r.heading_deg();
        r.in.steer = 1.0f;
        r.step(3.0f);
        coopa::test::expect(r.heading_deg() - before < -8.0f,
                            "differential: turned right by " +
                                std::to_string(before - r.heading_deg()) + " degrees");
    }
}

/**
 * @brief The dozer turns on the spot: differential steering with no drive counter-rotates its two
 *        tracks, so it yaws without going anywhere.
 */
COOPA_TEST(the_dozer_pivots_without_travelling) {
    Rig r;
    r.build("dozer");
    r.step(1.5f);
    const glm::vec3 start = r.position();
    const float before = r.heading_deg();
    r.in.steer = 1.0f;
    r.step(4.0f);

    const float turned = std::abs(r.heading_deg() - before);
    const float travelled = glm::length(glm::vec2(r.position() - start));
    coopa::test::expect(turned > 15.0f, "pivoted " + std::to_string(turned) + " degrees");
    coopa::test::expect(travelled < 2.0f, "barely moved (" + std::to_string(travelled) + " m)");
    // The two sides must be turning opposite ways -- that IS the mechanism.
    coopa::test::expect(r.state.wheels[0].spin * r.state.wheels[1].spin < 0.0f,
                        "the two tracks counter-rotate");
}

/** @brief The handbrake locks the rear wheels and breaks their grip, which is the slide. */
COOPA_TEST(the_handbrake_breaks_rear_traction) {
    Rig r;
    r.build("sports_car");
    r.step(1.5f);
    r.in.throttle = 1.0f;
    for (int i = 0; i < 1800 && r.state.forward_speed < 20.0f; ++i) r.world.step_fixed(1.0f / 60.0f);
    ASSERT_GT(r.state.forward_speed, 15.0f);

    r.in.throttle = 0.0f;
    r.in.handbrake = 1.0f;
    r.in.steer = 1.0f;
    r.step(1.0f);

    // Rear wheels are 2 and 3 (front axle first, left then right).
    const float rear_spin = std::abs(r.state.wheels[2].spin) + std::abs(r.state.wheels[3].spin);
    const float front_spin = std::abs(r.state.wheels[0].spin) + std::abs(r.state.wheels[1].spin);
    coopa::test::expect(rear_spin < front_spin * 0.5f,
                        "the rears are locked while the fronts still turn (" +
                            std::to_string(rear_spin) + " vs " + std::to_string(front_spin) + ")");
    coopa::test::expect(r.state.any_wheel_slipping, "a wheel is sliding");
    float worst = 0.0f;
    for (const vehicle::WheelState& w : r.state.wheels) worst = std::max(worst, std::abs(w.slip_angle));
    coopa::test::expect(glm::degrees(worst) > 10.0f,
                        "the car is sideways (" + std::to_string(glm::degrees(worst)) + " deg of slip)");
}

/** @brief Braking stops a vehicle, and reverse then backs it up -- the standstill pedal swap. */
COOPA_TEST(brake_stops_then_selects_reverse) {
    Rig r;
    r.build("pickup_4x4");
    r.step(1.5f);
    r.in.throttle = 1.0f;
    for (int i = 0; i < 1800 && r.state.forward_speed < 12.0f; ++i) r.world.step_fixed(1.0f / 60.0f);

    r.in.throttle = 0.0f;
    r.in.brake = 1.0f;
    r.step(5.0f);
    coopa::test::expect(r.state.forward_speed < 0.0f,
                        "held on the brake it stops and reverses (" +
                            std::to_string(r.state.forward_speed) + " m/s)");
    EXPECT_EQ(r.state.gear, -1);

    r.in.brake = 0.0f;
    r.in.throttle = 1.0f;
    r.step(4.0f);
    coopa::test::expect(r.state.gear > 0, "throttle from reverse selects a forward gear");
    coopa::test::expect(r.state.forward_speed > 0.5f, "and it drives forward again");
}

/** @brief A towed trailer has no engine: no gear ratio means no torque, whatever the throttle. */
COOPA_TEST(the_trailer_is_unpowered) {
    Rig r;
    r.build("trailer");
    r.step(1.5f);
    r.in.throttle = 1.0f;
    r.step(6.0f);
    coopa::test::expect(std::abs(r.state.forward_speed) < 0.5f,
                        "full throttle moves it nowhere (" + std::to_string(r.state.forward_speed) +
                            " m/s)");
    EXPECT_NEAR(r.state.engine_rpm, r.cfg.engine.idle_rpm, 1.0f);
}

/** @brief A jump lands on its wheels: `air_righting` levels the chassis while it is off the ground. */
COOPA_TEST(a_jumped_buggy_lands_on_its_wheels) {
    Rig r;
    r.build("buggy");
    r.step(1.0f);
    // Pitch it nose-up and throw it, the way a ramp would.
    dynamics::Body& b = *r.world.get_body(r.chassis);
    b.position.z += 4.0f;
    b.orientation = glm::angleAxis(glm::radians(55.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    dynamics::update_world_inertia(b);
    b.linear_velocity = {0.0f, 14.0f, 3.0f};
    b.angular_velocity = {1.2f, 0.0f, 0.0f};

    r.step(0.2f);
    coopa::test::expect(r.state.airborne, "it is in the air");
    r.step(5.0f);
    coopa::test::expect(!r.state.airborne, "it came back down");
    coopa::test::expect(r.lean_deg() < 25.0f,
                        "and landed roughly level (" + std::to_string(r.lean_deg()) + " deg of lean)");
}

/** @brief A landing is reported once, with the speed it arrived at -- the hook a host hangs a
 *         suspension thump off. */
COOPA_TEST(a_landing_reports_its_impact_speed) {
    Rig r;
    r.build("pickup_4x4");
    r.step(1.5f);
    dynamics::Body& b = *r.world.get_body(r.chassis);
    b.position.z += 3.0f;
    // Teleporting a body does not wake it, and a sleeping chassis is skipped entirely (see
    // simulate()). Waking it is what any caller moving a sleeping body has to do -- the same
    // reason RigidbodyComponent::set_velocity() wakes explicitly.
    b.wake();
    r.state.landing_impact = 0.0f;
    r.step(2.0f);
    coopa::test::expect(r.state.landing_impact > 3.0f,
                        "a 3 m drop lands at " + std::to_string(r.state.landing_impact) + " m/s");
}

/** @brief Nothing tunnels or sinks, whatever the driver does. */
COOPA_TEST(random_input_never_puts_the_chassis_through_the_floor) {
    for (const char* preset : {"sports_car", "hauler_6x6", "dozer", "buggy"}) {
        Rig r;
        r.build(preset, 0.7f, 0.05f);
        r.step(1.0f);
        // A cheap deterministic input sequence -- no <random>, so a failure is reproducible from
        // the preset name alone.
        uint32_t seed = 12345u;
        auto next = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>((seed >> 16) & 0xFFFF) / 65535.0f;
        };
        float lowest = r.position().z;
        for (int frame = 0; frame < 600; ++frame) {
            r.in.throttle = next();
            r.in.brake = next() > 0.8f ? 1.0f : 0.0f;
            r.in.steer = next() * 2.0f - 1.0f;
            r.in.handbrake = next() > 0.9f ? 1.0f : 0.0f;
            r.world.step_fixed(1.0f / 60.0f);
            lowest = std::min(lowest, r.position().z);
        }
        const std::string tag = preset;
        coopa::test::expect(lowest > -1.0f,
                            tag + ": never sank through the floor (lowest z " +
                                std::to_string(lowest) + ")");
        coopa::test::expect(std::isfinite(r.position().x) && std::isfinite(r.position().z),
                            tag + ": no NaN escaped the solver");
    }
}

/**
 * @brief Hard cornering rolls the body on its springs; it does not tip the vehicle over.
 *
 * A vehicle tips once its lateral acceleration passes `half-track / centre-of-mass height`, so
 * this is really a check on where the prefabs put their mass. With the mass left at the
 * collider's geometric centre the pickup flipped on a flat skidpad at 12 m/s and the hauler at
 * any speed at all -- the tyres could out-grip the chassis' own stability.
 */
COOPA_TEST(hard_cornering_leans_the_body_without_flipping_it) {
    for (const char* preset : {"sports_car", "pickup_4x4", "hauler_6x6", "buggy"}) {
        for (float entry : {8.0f, 14.0f, 20.0f}) {
            Rig r;
            r.build(preset);
            r.step(1.5f);
            r.in.throttle = 1.0f;
            for (int i = 0; i < 3000 && r.state.forward_speed < entry; ++i) {
                r.world.step_fixed(1.0f / 60.0f);
            }
            if (r.state.forward_speed < entry * 0.8f) continue;  // can't reach it; nothing to test

            r.in.throttle = 0.3f;
            r.in.steer = 1.0f;
            float worst = 0.0f;
            for (int i = 0; i < 300; ++i) {
                r.world.step_fixed(1.0f / 60.0f);
                worst = std::max(worst, r.lean_deg());
            }
            const std::string tag = std::string(preset) + " at " + std::to_string(int(entry)) + " m/s";
            coopa::test::expect(worst < 35.0f,
                                tag + ": stays on its wheels through a full-lock corner (leaned " +
                                    std::to_string(worst) + " deg)");
            coopa::test::expect(worst > 0.5f, tag + ": but the body does roll on its springs");
        }
    }
}

/**
 * @brief An automatic pulling away under full throttle works UP its gearbox, without hunting.
 *
 * The failure this guards against is specific and was severe: engine speed is derived from wheel
 * speed, a locked-differential truck spins all six wheels pulling away, the box upshifts on that
 * wheelspin, the change cuts torque for `shift_time`, the wheels slow, it downshifts, and it
 * repeats -- spending nearly all of its time mid-change. The hauler took 10 s to reach 10 m/s
 * doing that, against 4 s once the decision read road speed and a dwell was added.
 */
COOPA_TEST(an_automatic_works_up_its_gearbox_without_hunting) {
    Rig r;
    r.build("hauler_6x6");
    r.step(1.5f);
    r.in.throttle = 1.0f;

    int changes = 0;
    int previous = r.state.gear;
    float mid_change_frames = 0.0f;
    const int frames = 60 * 12;
    for (int i = 0; i < frames; ++i) {
        r.world.step_fixed(1.0f / 60.0f);
        if (r.state.gear != previous) {
            ++changes;
            previous = r.state.gear;
        }
        if (r.state.shift_timer > 0.0f) mid_change_frames += 1.0f;
    }

    coopa::test::expect(r.state.forward_speed > 14.0f,
                        "12 s of full throttle gets it moving properly (" +
                            std::to_string(r.state.forward_speed) + " m/s)");
    coopa::test::expect(r.state.gear >= 3, "and up into the higher gears (gear " +
                                               std::to_string(r.state.gear) + ")");
    // A clean run up a 5-speed box is 4 changes; hunting produced more than 20.
    coopa::test::expect(changes <= 8, "without hunting (" + std::to_string(changes) + " changes)");
    const float mid_change = mid_change_frames / static_cast<float>(frames);
    coopa::test::expect(mid_change < 0.3f,
                        "and with torque actually flowing (" +
                            std::to_string(int(mid_change * 100.0f)) + "% of the time mid-change)");
}

/**
 * @brief A flipped vehicle can be righted: it turns smoothly back upright about the heading it
 *        had and lands on its wheels.
 */
COOPA_TEST(a_flipped_vehicle_rights_itself_when_recovery_is_asked_for) {
    for (const char* preset : {"sports_car", "pickup_4x4", "hauler_6x6", "dozer"}) {
        for (float roll : {180.0f, 95.0f}) {
            Rig r;
            r.build(preset);
            // Drop it in upside down, pointing somewhere specific.
            dynamics::Body& b = *r.world.get_body(r.chassis);
            b.position.z += 1.0f;
            b.orientation = glm::angleAxis(glm::radians(40.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
                            glm::angleAxis(glm::radians(roll), glm::vec3(0.0f, 1.0f, 0.0f));
            dynamics::update_world_inertia(b);
            b.wake();
            r.step(3.0f);

            const std::string tag = std::string(preset) + " rolled " + std::to_string(int(roll));
            coopa::test::expect(r.lean_deg() > 60.0f, tag + ": starts on its side or roof");
            coopa::test::expect(r.state.upside_down, tag + ": and knows it");
            const float heading_before = r.heading_deg();

            r.in.recover = true;
            r.world.step_fixed(1.0f / 60.0f);
            r.in.recover = false;
            coopa::test::expect(r.state.is_recovering(), tag + ": the recovery starts");

            // Smooth, not a snap: nothing may spin faster than a couple of turns a second.
            float peak_spin = 0.0f;
            for (int i = 0; i < 60 * 4; ++i) {
                r.world.step_fixed(1.0f / 60.0f);
                peak_spin = std::max(peak_spin, glm::length(r.body().angular_velocity));
            }

            coopa::test::expect(r.lean_deg() < 5.0f,
                                tag + ": ends upright (" + std::to_string(r.lean_deg()) + " deg)");
            coopa::test::expect(r.state.wheels_on_ground == static_cast<int>(r.state.wheels.size()),
                                tag + ": on all of its wheels");
            coopa::test::expect(!r.state.is_recovering(), tag + ": and the recovery has finished");
            coopa::test::expect(peak_spin < 12.0f,
                                tag + ": turned over smoothly (peak " + std::to_string(peak_spin) +
                                    " rad/s)");
            float drift = r.heading_deg() - heading_before;
            while (drift > 180.0f) drift -= 360.0f;
            while (drift < -180.0f) drift += 360.0f;
            coopa::test::expect(std::abs(drift) < 20.0f,
                                tag + ": still facing the way it was (" + std::to_string(drift) +
                                    " deg of drift)");
        }
    }
}

/** @brief Recovery on an upright, parked vehicle does nothing worth noticing. */
COOPA_TEST(recovering_an_upright_vehicle_barely_disturbs_it) {
    Rig r;
    r.build("pickup_4x4");
    r.step(3.0f);
    const glm::vec3 before = r.position();
    r.in.recover = true;
    r.world.step_fixed(1.0f / 60.0f);
    r.in.recover = false;
    r.step(4.0f);
    coopa::test::expect(r.lean_deg() < 3.0f, "still level");
    coopa::test::expect(glm::length(glm::vec2(r.position() - before)) < 1.0f, "and still parked");
    coopa::test::expect(r.state.wheels_on_ground == 4, "back on its wheels");
}

/** @brief `auto_recover_time` rights a vehicle left on its roof, with no input at all. */
COOPA_TEST(auto_recovery_rights_a_vehicle_left_on_its_roof) {
    Rig r;
    r.build("pickup_4x4");
    r.cfg.auto_recover_time = 2.0f;
    dynamics::Body& b = *r.world.get_body(r.chassis);
    b.position.z += 1.0f;
    b.orientation = glm::angleAxis(glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    dynamics::update_world_inertia(b);
    b.wake();

    r.step(1.5f);
    coopa::test::expect(r.lean_deg() > 60.0f, "it is on its roof and the timer has not run out");
    r.step(6.0f);
    coopa::test::expect(r.lean_deg() < 5.0f,
                        "after auto_recover_time it righted itself unasked (" +
                            std::to_string(r.lean_deg()) + " deg)");
    coopa::test::expect(r.state.wheels_on_ground == 4, "on its wheels");
}

/** @brief Two identical runs produce an identical world, as the rest of the library guarantees. */
COOPA_TEST(simulation_is_deterministic) {
    auto run_hash = [] {
        Rig r;
        r.build("pickup_4x4");
        r.step(1.0f);
        r.in.throttle = 0.8f;
        r.in.steer = 0.4f;
        r.step(4.0f);
        return r.world.world_state_hash();
    };
    EXPECT_EQ(run_hash(), run_hash());
}

/** @brief The tyre curve on its own: it rises to its peak, falls past it, and holds a plateau. */
COOPA_TEST(the_slip_curve_peaks_then_plateaus) {
    const float peak = 0.12f;
    const float plateau = 0.7f;
    EXPECT_NEAR(vehicle::slip_curve(0.0f, peak, plateau), 0.0f, 1e-5f);
    EXPECT_NEAR(vehicle::slip_curve(peak, peak, plateau), 1.0f, 1e-5f);
    EXPECT_NEAR(vehicle::slip_curve(peak * 0.5f, peak, plateau), 0.5f, 1e-5f);
    // Symmetric in the sign of the slip -- the magnitude is what the force scales with.
    EXPECT_NEAR(vehicle::slip_curve(-peak, peak, plateau), 1.0f, 1e-5f);
    EXPECT_LT(vehicle::slip_curve(peak * 2.0f, peak, plateau), 1.0f);
    EXPECT_NEAR(vehicle::slip_curve(peak * 50.0f, peak, plateau), plateau, 1e-5f);
}

/**
 * @brief The friction circle: a wheel using all of its grip to accelerate has none left to corner
 *        with, and the total never exceeds what the contact can hold.
 */
COOPA_TEST(the_friction_circle_bounds_the_combined_force) {
    vehicle::TireConfig tire;
    const float load = 4000.0f;
    const float mu = 1.0f;
    const float limit = mu * load;

    // Cornering hard with no drive slip: lateral force only, within the limit.
    const vehicle::TireForces lateral = vehicle::tire_forces(tire, 0.0f, 0.5f, load, mu);
    EXPECT_NEAR(lateral.longitudinal, 0.0f, 1.0f);
    coopa::test::expect(std::abs(lateral.lateral) <= limit * 1.001f, "lateral alone is bounded");
    coopa::test::expect(lateral.lateral < 0.0f, "lateral force resists the slip");

    // Both axes demanded at once: the pair is clamped onto the circle.
    const vehicle::TireForces both = vehicle::tire_forces(tire, 0.6f, 0.5f, load, mu);
    const float magnitude = std::sqrt(both.longitudinal * both.longitudinal + both.lateral * both.lateral);
    coopa::test::expect(magnitude <= limit * 1.001f,
                        "combined force stays on the circle (" + std::to_string(magnitude) + " <= " +
                            std::to_string(limit) + ")");
    coopa::test::expect(std::abs(both.lateral) < std::abs(lateral.lateral),
                        "and accelerating costs cornering grip");
    coopa::test::expect(both.sliding, "which is reported as sliding");

    // No load, no force.
    const vehicle::TireForces airborne = vehicle::tire_forces(tire, 0.6f, 0.5f, 0.0f, mu);
    EXPECT_NEAR(airborne.longitudinal, 0.0f, 1e-6f);
    EXPECT_NEAR(airborne.lateral, 0.0f, 1e-6f);
}

/** @brief Load sensitivity: a wheel pressed harder than nominal gives up some of its coefficient. */
COOPA_TEST(friction_falls_as_load_rises_past_nominal) {
    vehicle::TireConfig tire;
    tire.load_sensitivity = 0.3f;
    const float nominal = 4000.0f;
    const float at_nominal = vehicle::available_friction(tire, 1.0f, nominal, nominal);
    const float overloaded = vehicle::available_friction(tire, 1.0f, nominal * 2.0f, nominal);
    EXPECT_NEAR(at_nominal, 1.0f, 1e-5f);
    EXPECT_NEAR(overloaded, 0.7f, 1e-5f);
    // Under nominal there is no bonus -- the curve only ever takes grip away.
    EXPECT_NEAR(vehicle::available_friction(tire, 1.0f, nominal * 0.5f, nominal), 1.0f, 1e-5f);
}

/** @brief The engine curve passes through its three authored points and dies past the limiter. */
COOPA_TEST(the_engine_curve_honours_its_points_and_its_limiter) {
    vehicle::EngineConfig e;
    e.idle_rpm = 800.0f;
    e.peak_rpm = 4000.0f;
    e.max_rpm = 6500.0f;
    e.peak_torque = 300.0f;
    e.torque_at_idle = 0.5f;
    e.torque_at_max = 0.7f;

    EXPECT_NEAR(vehicle::engine_torque(e, 800.0f), 150.0f, 0.5f);
    EXPECT_NEAR(vehicle::engine_torque(e, 4000.0f), 300.0f, 0.5f);
    EXPECT_NEAR(vehicle::engine_torque(e, 6500.0f), 210.0f, 0.5f);
    EXPECT_NEAR(vehicle::engine_torque(e, 400.0f), 150.0f, 0.5f);  // below idle holds
    // Past the redline the curve falls to nothing over 500 rpm, and that roll-off -- not a clamp
    // on the revs -- is what gives every gear a top speed.
    EXPECT_NEAR(vehicle::engine_torque(e, 7000.0f), 0.0f, 0.5f);
    EXPECT_NEAR(vehicle::engine_torque(e, 9000.0f), 0.0f, 1e-5f);
}

/** @brief Each shipped preset is internally coherent: drivable, braked, and sprung. */
COOPA_TEST(every_preset_is_self_consistent) {
    for (const std::string& name : vehicle::preset_names()) {
        const vehicle::VehicleConfig c = vehicle::preset(name);
        coopa::test::expect(c.wheels.size() >= 4, name + ": has at least four wheels");
        coopa::test::expect(c.brake_torque > 0.0f, name + ": has brakes");

        bool any_steered = false;
        for (const vehicle::WheelConfig& w : c.wheels) {
            coopa::test::expect(w.radius > 0.0f && w.travel > 0.0f && w.rest_length >= w.travel,
                                name + ": wheel geometry is sane");
            coopa::test::expect(w.resolved_stiffness(1000.0f) > 0.0f, name + ": wheels are sprung");
            coopa::test::expect(w.spin_inertia() > 0.0f, name + ": wheels have inertia");
            if (w.steered) any_steered = true;
        }
        // Either the wheels steer, or the two sides are driven against each other. A vehicle with
        // neither could not be turned at all -- except a trailer, which is steered by whatever
        // tows it and has no steering of its own.
        if (name != "trailer") {
            coopa::test::expect(any_steered || c.steer_mode == vehicle::SteerMode::Differential,
                                name + ": can be steered somehow");
        }

        // Gear ratios must fall monotonically -- a gearbox that went the other way would upshift
        // into more torque and never settle.
        for (std::size_t i = 1; i < c.gearbox.forward_ratios.size(); ++i) {
            coopa::test::expect(c.gearbox.forward_ratios[i] < c.gearbox.forward_ratios[i - 1],
                                name + ": gear " + std::to_string(i + 1) + " is taller than the one below");
        }
        if (name == "trailer") {
            coopa::test::expect(c.total_drive_share() <= 0.0f, "trailer: nothing drives it");
        } else {
            coopa::test::expect(c.total_drive_share() > 0.0f, name + ": something drives it");
            coopa::test::expect(!c.gearbox.forward_ratios.empty(), name + ": has gears");
            coopa::test::expect(c.engine.peak_torque > 0.0f, name + ": has an engine");
            coopa::test::expect(c.engine.peak_rpm > c.engine.idle_rpm &&
                                    c.engine.max_rpm > c.engine.peak_rpm,
                                name + ": its rev range is ordered");
        }
        if (name == "dozer") {
            coopa::test::expect(c.steer_mode == vehicle::SteerMode::Differential,
                                "dozer: steers by its tracks");
            // Skid steering IS dragging both tracks sideways; lateral grip above the longitudinal
            // limit would stop it turning at all. See vehicle/README.md.
            coopa::test::expect(c.tire.lateral_grip < 1.0f, "dozer: its tracks can be dragged sideways");
            coopa::test::expect(c.anti_roll == 0.0f, "dozer: no anti-roll bar across a track");
        }
    }
}

/** @brief An unknown preset name gives a driveable vehicle rather than throwing or returning a
 *         four-wheeled brick with no suspension. */
COOPA_TEST(an_unknown_preset_falls_back_to_a_driveable_one) {
    EXPECT_FALSE(vehicle::has_preset("spaceship"));
    EXPECT_TRUE(vehicle::has_preset("hauler_6x6"));
    const vehicle::VehicleConfig fallback = vehicle::preset("spaceship");
    EXPECT_EQ(fallback.wheels.size(), vehicle::pickup_4x4_preset().wheels.size());
    coopa::test::expect(fallback.total_drive_share() > 0.0f, "the fallback is driveable");
}

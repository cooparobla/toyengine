/**
 * @file vehicle_test.cpp
 * @brief The Vehicle COMPONENT and VehicleSystem: binding to a chassis Rigidbody, driving from
 *        pushed-in controls, the wheel visuals, the signals, and the YAML parser's preset +
 *        override rules -- against the real `vehicle_demo` scene and its real prefabs.
 *
 * The simulation itself (suspension, tyre curve, drivetrain, steering) is physxcoopa's contract
 * and is tested in libs/physxcoopa/tests/vehicle_test.cpp. What is here is everything that only
 * exists once a vehicle is a scene object: that the prefabs bind, that the wheel anchors survive
 * the centre-of-mass conversion, and that the component's keys reach the simulation.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/components/transform_component.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/components/vehicle.h>
#include <physxcoopa/physx_yaml.h>
#include <physxcoopa/util/physics_settings.h>
#include <physxcoopa/system/physics_system.h>
#include <physxcoopa/system/vehicle_system.h>
#include <toyengine/scene/register.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("vehicle");

using coopa::physx::components::RigidbodyComponent;
using coopa::physx::components::VehicleComponent;
using coopa::scene::Scene;
using coopa::scene::SceneObject;

namespace {

/** @brief Registers every parser these tests need. Static AssetManager: the parser lambdas capture
 *         it by reference and must outlive every later load in this process. */
void register_parsers() {
    static coopa::asset::AssetManager assets;
    static bool done = false;
    if (done) return;
    // Without a search root the colliders' `material: tarmac` references fail to load and every
    // surface silently falls back to PhysicsMaterial::default_material() -- which would quietly
    // flatten the whole point of the course, since grip and rolling resistance come from those
    // files.
    assets.add_search_root(std::string(ROOT_DIR) + "/assets");
    coopa::physx::register_physics_components(assets);
    toy::scene::register_scene_components();
    done = true;
}

/** @brief The real demo scene, started, with physics and the vehicle system installed. */
struct DemoScene {
    std::unique_ptr<Scene> scene;

    /** @brief The search root is restored on the way out: it is process-wide state, and a suite
     *         that left it pointing at this checkout's assets would change how later suites in the
     *         same binary resolve their own scratch projects. */
    ~DemoScene() { coopa::scene::SceneLoader::set_search_roots({}); }

    void load() {
        register_parsers();
        // The scene instances `prefab: objects/vehicles/<name>`, which SceneLoader resolves
        // against its search roots -- so they have to point at this checkout's assets.
        coopa::scene::SceneLoader::set_search_roots({std::string(ROOT_DIR) + "/assets"});
        const std::string path =
            std::string(ROOT_DIR) + "/assets/scenes/physics/vehicle_demo/scene.yaml";

        // The scene's own `settings.physics` has to be applied, as the Engine does -- the course
        // sets `sleep_time` so nothing on it falls asleep, and without that a settled prop is
        // immovable (physxcoopa does not wake a sleeping body on a new contact). Installing
        // physics with default settings here would test a course that behaves differently from
        // the one that ships.
        std::ifstream in(path);
        fkyaml::node root = fkyaml::node::deserialize(in);
        coopa::physx::util::PhysicsSettings physics;
        if (root.contains("scene") && root["scene"].contains("settings") &&
            root["scene"]["settings"].contains("physics")) {
            physics = coopa::physx::util::parse_physics_settings(root["scene"]["settings"]["physics"]);
        }

        scene = std::make_unique<Scene>(coopa::scene::SceneLoader::load(path));
        scene->start();
        coopa::physx::system::install_physics_system(*scene, physics);
        coopa::physx::system::install_vehicle_system(*scene);
    }

    void run(float seconds) {
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < static_cast<int>(seconds / dt); ++i) {
            scene->update(dt);
            scene->late_update(dt);
        }
    }

    VehicleComponent* vehicle(const std::string& name) {
        SceneObject* obj = scene->find_object(name);
        return obj ? obj->get_component<VehicleComponent>() : nullptr;
    }
    glm::vec3 position(const std::string& name) {
        SceneObject* obj = scene->find_object(name);
        return obj ? obj->get_transform()->transform().position() : glm::vec3(0.0f);
    }
    /** @brief How far the chassis leans off world up, degrees. */
    float lean_deg(const std::string& name) {
        SceneObject* obj = scene->find_object(name);
        if (!obj) return 180.0f;
        const glm::vec3 up = obj->get_transform()->transform().rotation_quat() * glm::vec3(0, 0, 1);
        return glm::degrees(std::acos(std::clamp(up.z, -1.0f, 1.0f)));
    }
};

const std::vector<std::string>& demo_vehicles() {
    static const std::vector<std::string> v{"sports_car", "pickup_4x4", "hauler_6x6",
                                             "buggy",      "dozer",      "tipper_trailer"};
    return v;
}

} // namespace

/**
 * @brief Every vehicle in the demo scene binds to its chassis, settles on its wheels, and does
 *        not fall through the floor it is parked on.
 *
 * Binding is a frame late by design: PhysicsSystem's reconcile pass (order 100) creates the body,
 * and VehicleSystem's bind pass runs at 96, so the first frame finds no body yet.
 */
COOPA_TEST(every_demo_vehicle_binds_and_settles_on_its_wheels) {
    DemoScene d;
    d.load();
    d.run(3.0f);

    for (const std::string& name : demo_vehicles()) {
        VehicleComponent* v = d.vehicle(name);
        if (!expect(v != nullptr, name + ": the prefab carries a Vehicle component")) continue;
        expect(v->is_bound(), name + ": bound to its chassis body");
        expect(v->state().valid, name + ": has been simulated");
        expect(v->wheels_on_ground() == static_cast<int>(v->state().wheels.size()),
               name + ": every wheel is on the ground");
        const glm::vec3 p = d.position(name);
        expect(p.z > 0.2f && p.z < 3.0f, name + ": sits on the depot (z " + std::to_string(p.z) + ")");
        expect(d.lean_deg(name) < 5.0f, name + ": sits level");
        expect(v->speed_kmh() < 2.0f, name + ": parked");
    }
}

/**
 * @brief Each prefab's `wheels:` list names children that exist, and as many as its preset has.
 *
 * A misnamed wheel object is silent -- the simulation does not care, only the visual does -- so
 * this is the only thing that would catch it.
 */
COOPA_TEST(every_prefab_names_wheel_children_that_exist) {
    DemoScene d;
    d.load();
    d.run(0.1f);

    for (const std::string& name : demo_vehicles()) {
        VehicleComponent* v = d.vehicle(name);
        if (!expect(v != nullptr, name + ": has a Vehicle")) continue;
        expect(v->wheel_objects.size() == v->config.wheels.size(),
               name + ": one wheel object per wheel (" + std::to_string(v->wheel_objects.size()) +
                   " of " + std::to_string(v->config.wheels.size()) + ")");
        for (std::size_t i = 0; i < v->wheel_objects.size(); ++i) {
            const std::string& child = v->wheel_objects[i];
            expect(!child.empty(), name + ": wheel " + std::to_string(i) + " names an object");
            expect(v->owner->find_child(child) != nullptr,
                   name + ": its wheel object '" + child + "' exists");
        }
    }
}

/**
 * @brief Driving works end to end: the throttle the host pushes in reaches the chassis, and the
 *        vehicle drives off in the direction it is facing.
 */
COOPA_TEST(pushed_in_throttle_drives_the_vehicle_forward) {
    DemoScene d;
    d.load();
    d.run(2.0f);

    VehicleComponent* v = d.vehicle("pickup_4x4");
    ASSERT_TRUE(v != nullptr);
    const glm::vec3 start = d.position("pickup_4x4");
    // Three seconds, not five: the depot's exit runs into the skidpad's slalom, and a vehicle
    // that drove into a cone would be measuring the crash rather than the acceleration.
    v->controls.throttle = 1.0f;
    d.run(3.0f);
    const glm::vec3 end = d.position("pickup_4x4");

    expect(end.y - start.y > 4.0f,
           "it drove forward along its own +Y (" + std::to_string(end.y - start.y) + " m)");
    expect(std::abs(end.x - start.x) < 2.5f, "and roughly straight");
    expect(v->speed_kmh() > 35.0f, "reaching " + std::to_string(v->speed_kmh()) + " km/h");
    expect(v->engine_rpm() > 1000.0f, "with the engine turning (" + std::to_string(v->engine_rpm()) + " rpm)");
    expect(d.lean_deg("pickup_4x4") < 12.0f, "still upright");
}

/**
 * @brief The wheel visuals track the simulation: each wheel child sits `hub_offset` below its
 *        authored anchor, and rolls as the vehicle moves.
 *
 * The anchor check is the one that matters: the component converts anchors into the body's
 * centre-of-mass frame for the simulation and keeps the authored, origin-relative ones for the
 * visuals. Confusing the two puts every wheel off by the collider's centre offset.
 */
COOPA_TEST(wheel_visuals_follow_the_suspension_and_roll) {
    DemoScene d;
    d.load();
    d.run(2.0f);

    VehicleComponent* v = d.vehicle("pickup_4x4");
    ASSERT_TRUE(v != nullptr);
    for (std::size_t i = 0; i < v->wheel_objects.size(); ++i) {
        SceneObject* wheel = v->owner->find_child(v->wheel_objects[i]);
        if (!expect(wheel != nullptr, "wheel " + std::to_string(i) + " resolves")) continue;
        const glm::vec3 local = wheel->get_transform()->transform().position();
        const glm::vec3 anchor = v->config.wheels[i].anchor;
        const float expected_z = anchor.z - v->state().wheels[i].hub_offset;
        expect_near(local.x, anchor.x, 1e-4f, "wheel " + std::to_string(i) + ": sits at its anchor in x");
        expect_near(local.y, anchor.y, 1e-4f, "wheel " + std::to_string(i) + ": and in y");
        expect_near(local.z, expected_z, 1e-4f,
                    "wheel " + std::to_string(i) + ": hangs hub_offset below the anchor");
    }

    const float before = v->state().wheels[0].spin_angle;
    v->controls.throttle = 1.0f;
    d.run(3.0f);
    expect(std::abs(v->state().wheels[0].spin_angle - before) > 3.0f,
           "the wheels have rolled (" +
               std::to_string(std::abs(v->state().wheels[0].spin_angle - before)) + " rad)");
    // And the visual followed the roll, rather than staying at the identity.
    SceneObject* wheel = v->owner->find_child(v->wheel_objects[0]);
    const glm::quat r = wheel->get_transform()->transform().rotation_quat();
    expect(std::abs(r.w) < 0.9999f, "its transform carries that rotation");
}

/**
 * @brief VehicleSystem clears the controls of a `player_input` vehicle that is not `active`.
 *
 * Without it, a vehicle the player stopped driving mid-corner would hold that steering input for
 * the rest of the scene and drive itself off into the distance.
 */
COOPA_TEST(an_inactive_vehicle_has_its_controls_cleared) {
    DemoScene d;
    d.load();
    d.run(2.0f);

    VehicleComponent* parked = d.vehicle("sports_car");
    ASSERT_TRUE(parked != nullptr);
    expect(!parked->active, "the sports car is not the active vehicle");

    const glm::vec3 start = d.position("sports_car");
    parked->controls.throttle = 1.0f;
    parked->controls.steer = 1.0f;
    d.run(3.0f);

    expect_near(parked->controls.throttle, 0.0f, 1e-6f, "its throttle was cleared");
    expect_near(parked->controls.steer, 0.0f, 1e-6f, "and its steering");
    expect(glm::length(d.position("sports_car") - start) < 1.0f, "so it stayed put");
}

/** @brief A towed trailer is excluded from the player's input entirely. */
COOPA_TEST(the_trailer_opts_out_of_player_input) {
    DemoScene d;
    d.load();
    d.run(1.0f);
    VehicleComponent* t = d.vehicle("tipper_trailer");
    ASSERT_TRUE(t != nullptr);
    expect(!t->player_input, "the trailer is not driveable");
    expect(t->config.total_drive_share() <= 0.0f, "and nothing drives its wheels");
}

/**
 * @brief The hitched trailer follows the hauler, at a roughly fixed distance, without jack-knifing.
 *
 * The hitch is an ordinary HingeJoint about the vertical axis -- there is no tow-hitch type -- so
 * what this really checks is that an unpowered second vehicle rolls along behind a driven one
 * rather than being dragged sideways or left behind.
 */
COOPA_TEST(the_trailer_follows_the_hauler_that_tows_it) {
    DemoScene d;
    d.load();
    d.run(2.0f);

    VehicleComponent* hauler = d.vehicle("hauler_6x6");
    VehicleComponent* trailer = d.vehicle("tipper_trailer");
    ASSERT_TRUE(hauler != nullptr && trailer != nullptr);

    const float rest_gap = glm::length(d.position("hauler_6x6") - d.position("tipper_trailer"));
    expect(rest_gap > 1.0f && rest_gap < 12.0f,
           "they start hitched together (" + std::to_string(rest_gap) + " m apart)");

    const glm::vec3 trailer_start = d.position("tipper_trailer");
    // Make the hauler the driven vehicle first -- exactly what pressing V does. Without this the
    // system zeroes its controls every frame, because only the ACTIVE vehicle takes input, and the
    // throttle set below would never reach the ground.
    if (VehicleComponent* pickup = d.vehicle("pickup_4x4")) pickup->active = false;
    hauler->active = true;
    hauler->controls.throttle = 1.0f;
    for (int i = 0; i < 360; ++i) {
        hauler->controls.throttle = 1.0f;  // re-asserted each frame, as a host's driver would
        d.run(1.0f / 60.0f);
    }

    const glm::vec3 hauler_end = d.position("hauler_6x6");
    const glm::vec3 trailer_end = d.position("tipper_trailer");
    const float travelled = glm::length(trailer_end - trailer_start);
    const float gap = glm::length(hauler_end - trailer_end);

    expect(travelled > 3.0f, "the trailer was towed along (" + std::to_string(travelled) + " m)");
    expect(std::abs(gap - rest_gap) < 2.0f,
           "staying about the same distance behind (" + std::to_string(gap) + " vs " +
               std::to_string(rest_gap) + " m)");
    expect(trailer->wheels_on_ground() >= 3, "on its wheels, not dragged over");
    expect(d.lean_deg("tipper_trailer") < 20.0f,
           "and upright (" + std::to_string(d.lean_deg("tipper_trailer")) + " deg of lean)");
}

/**
 * @brief The recovery a host's key triggers works through the component: a flipped vehicle in the
 *        real scene is righted, lands on its wheels, and the one-shot is consumed.
 */
COOPA_TEST(the_recover_control_rights_a_flipped_vehicle) {
    DemoScene d;
    d.load();
    d.run(2.0f);

    VehicleComponent* v = d.vehicle("pickup_4x4");
    ASSERT_TRUE(v != nullptr);
    auto* rb = v->owner->get_component<RigidbodyComponent>();
    ASSERT_TRUE(rb != nullptr);

    // Roll it onto its roof where it stands.
    coopa::util::Transform& t = v->owner->get_transform()->transform();
    t.set_position(t.position() + glm::vec3(0.0f, 0.0f, 1.2f));
    t.set_rotation_quat(glm::angleAxis(glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    rb->wake();
    d.run(2.5f);
    expect(d.lean_deg("pickup_4x4") > 60.0f,
           "it is on its roof (" + std::to_string(d.lean_deg("pickup_4x4")) + " deg)");
    expect(v->is_upside_down(), "and reports it");

    v->recover();
    d.run(0.1f);
    expect(v->is_recovering(), "the recovery started");
    expect(!v->controls.recover, "and the one-shot was consumed");

    d.run(4.0f);
    expect(d.lean_deg("pickup_4x4") < 6.0f,
           "it ended upright (" + std::to_string(d.lean_deg("pickup_4x4")) + " deg)");
    expect(v->wheels_on_ground() == 4, "on all four wheels");
    expect(!v->is_recovering(), "and the recovery finished");
}

/**
 * @brief Cornering hard in the real scene leans the body without tipping it.
 *
 * The prefabs' `center_of_mass` is what this proves -- it is the key the inspector and the YAML
 * now expose, and with it left at the collider's centre this vehicle flipped on flat tarmac.
 */
COOPA_TEST(cornering_hard_in_the_scene_does_not_flip_the_vehicle) {
    DemoScene d;
    d.load();
    d.run(2.0f);

    VehicleComponent* v = d.vehicle("pickup_4x4");
    ASSERT_TRUE(v != nullptr);
    auto* rb = v->owner->get_component<RigidbodyComponent>();
    ASSERT_TRUE(rb != nullptr);
    expect(std::abs(rb->local_center_of_mass().z) > 0.1f,
           "the prefab really does move its centre of mass down");

    v->controls.throttle = 1.0f;
    for (int i = 0; i < 600 && v->forward_speed() < 14.0f; ++i) {
        v->controls.throttle = 1.0f;
        d.run(1.0f / 60.0f);
    }
    expect(v->forward_speed() > 10.0f,
           "got up to speed (" + std::to_string(v->forward_speed()) + " m/s)");

    float worst = 0.0f;
    for (int i = 0; i < 240; ++i) {
        v->controls.throttle = 0.3f;
        v->controls.steer = 1.0f;
        d.run(1.0f / 60.0f);
        worst = std::max(worst, d.lean_deg("pickup_4x4"));
    }
    expect(worst < 35.0f, "a full-lock corner leans it but does not flip it (" +
                              std::to_string(worst) + " deg)");
}

/** @brief Gear changes reach gameplay as a signal -- the hook for a shift sound. */
COOPA_TEST(a_gear_change_fires_its_signal) {
    DemoScene d;
    d.load();
    d.run(2.0f);

    VehicleComponent* v = d.vehicle("pickup_4x4");
    ASSERT_TRUE(v != nullptr);
    int changes = 0;
    int last_gear = v->gear();
    int highest_gear = v->gear();
    auto conn = v->on_gear_change.connect([&](int gear) {
        ++changes;
        last_gear = gear;
        highest_gear = std::max(highest_gear, gear);
    });

    v->controls.throttle = 1.0f;
    d.run(8.0f);
    conn.disconnect();

    expect(changes > 0, "it changed gear at least once under full throttle");
    expect(last_gear == v->gear(), "and the signal reported the gear it ended in");
    // The gear it ends in says nothing: 8 s of full throttle takes it off the depot and into the
    // course's cones and ramps, and slowing down against those correctly drops it back down the
    // box. What the signal has to prove is that it worked UP it.
    expect(highest_gear > 1, "working up the gearbox (reached gear " + std::to_string(highest_gear) + ")");
}

/**
 * @brief The parser: `preset:` picks a tuning, every other key overrides one of its values, and
 *        `wheels:` is positional against the preset's own wheel order.
 */
COOPA_TEST(yaml_applies_a_preset_then_its_overrides) {
    namespace fs = std::filesystem;
    register_parsers();
    const fs::path root = coopa::test::scratch_dir() / "vehicle_yaml" / "assets";
    fs::remove_all(root.parent_path());
    fs::create_directories(root / "scenes" / "level");
    {
        std::ofstream o(root / "scenes" / "level" / "scene.yaml");
        o << "scene:\n"
             "  scene_name: Level\n"
             "  root_objects:\n"
             "    - name: truck\n"
             "      components:\n"
             "        - { type: Transform }\n"
             "        - { type: BoxCollider, size: { x: 2, y: 6, z: 2 } }\n"
             "        - { type: Rigidbody, mass: 9000 }\n"
             "        - type: Vehicle\n"
             "          preset: hauler_6x6\n"
             "          max_steer_deg: 21.5\n"
             "          anti_roll: 1234.0\n"
             "          steer_mode: differential\n"
             "          tire: { grip: 1.75, plateau: 0.5 }\n"
             "          engine: { peak_torque: 4321.0 }\n"
             "          gearbox: { forward_ratios: [5.0, 2.5, 1.25], automatic: false }\n"
             "          drive: { differential: open, limited_slip: 0.25 }\n"
             "          player_input: false\n"
             "          active: false\n"
             "          wheels:\n"
             "            - { object: w0, drive_share: 0.125 }\n"
             "            - { object: w1 }\n"
             "            - { object: w2, radius: 0.9, steered: true, side: right }\n";
    }
    coopa::scene::SceneLoader::set_search_roots({root.string()});
    Scene scene = coopa::scene::SceneLoader::load((root / "scenes" / "level" / "scene.yaml").string());
    coopa::scene::SceneLoader::set_search_roots({});

    ASSERT_FALSE(scene.root_objects().empty());
    auto* v = scene.root_objects()[0]->get_component<VehicleComponent>();
    ASSERT_TRUE(v != nullptr);

    expect(v->preset == "hauler_6x6", "the preset name is kept");
    expect(v->config.wheels.size() == 6u, "and its six wheels came with it");
    expect_near(v->config.max_steer_deg, 21.5f, 1e-5f, "a scalar override lands");
    expect_near(v->config.anti_roll, 1234.0f, 1e-5f, "so does another");
    expect(v->config.steer_mode == coopa::physx::vehicle::SteerMode::Differential, "and the steer mode");
    expect_near(v->config.tire.grip, 1.75f, 1e-5f, "a `tire` sub-key overrides");
    expect_near(v->config.tire.plateau, 0.5f, 1e-5f, "...and another in the same map");
    // Untouched keys must still be the preset's, not a default-constructed value.
    expect_near(v->config.tire.peak_slip_ratio,
                coopa::physx::vehicle::hauler_6x6_preset().tire.peak_slip_ratio, 1e-6f,
                "an untouched tyre key keeps the preset's value");
    expect_near(v->config.engine.peak_torque, 4321.0f, 1e-5f, "the engine's torque overrides");
    expect(v->config.gearbox.forward_ratios.size() == 3u, "a gear list replaces the preset's");
    expect(!v->config.gearbox.automatic, "and the automatic flag");
    expect(v->config.drive.differential == coopa::physx::vehicle::DifferentialType::Open,
           "the differential type parses by name");
    expect_near(v->config.drive.limited_slip, 0.25f, 1e-5f, "and its slip factor");
    expect(!v->player_input && !v->active, "the input flags parse");

    // `wheels:` is positional: three entries override the first three of the preset's six, name
    // their objects, and leave the other three alone.
    expect(v->wheel_objects.size() == 3u, "three wheel objects were named");
    expect(v->wheel_objects[0] == "w0" && v->wheel_objects[2] == "w2", "in order");
    expect_near(v->config.wheels[0].drive_share, 0.125f, 1e-5f, "a per-wheel override lands");
    expect_near(v->config.wheels[2].radius, 0.9f, 1e-5f, "including geometry");
    expect(v->config.wheels[2].steered, "and flags");
    expect(v->config.wheels[2].side == coopa::physx::vehicle::WheelSide::Right, "and the side");
    expect_near(v->config.wheels[1].radius, coopa::physx::vehicle::hauler_6x6_preset().wheels[1].radius,
                1e-6f, "a wheel with only an `object` keeps the preset's geometry");
    expect_near(v->config.wheels[4].radius, coopa::physx::vehicle::hauler_6x6_preset().wheels[4].radius,
                1e-6f, "and a wheel with no entry at all is untouched");
}

/** @brief A Vehicle with no Rigidbody never binds, and says so rather than crashing. */
COOPA_TEST(a_vehicle_without_a_rigidbody_never_binds) {
    register_parsers();
    Scene scene("no_body");
    auto obj = std::make_unique<SceneObject>("floating");
    obj->add_component<coopa::scene::TransformComponent>();
    auto* v = obj->add_component<VehicleComponent>();
    scene.add_root_object(std::move(obj));
    scene.start();
    coopa::physx::system::install_physics_system(scene);
    coopa::physx::system::install_vehicle_system(scene);
    for (int i = 0; i < 30; ++i) {
        scene.update(1.0f / 60.0f);
        scene.late_update(1.0f / 60.0f);
    }
    expect(!v->is_bound(), "it never bound");
    expect(!v->state().valid, "and was never simulated");
}

/**
 * @brief The component's anchors stay in the frame a scene authors them in, and the simulation's
 *        copy is shifted into the body's centre-of-mass frame.
 *
 * The demo's chassis colliders are deliberately offset (their size is set by ground clearance),
 * so the two frames differ by a real amount here -- which is exactly when confusing them hurts.
 */
COOPA_TEST(sim_anchors_are_centre_of_mass_relative_and_authored_ones_are_not) {
    DemoScene d;
    d.load();
    d.run(2.0f);

    VehicleComponent* v = d.vehicle("pickup_4x4");
    ASSERT_TRUE(v != nullptr);
    auto* rb = v->owner->get_component<RigidbodyComponent>();
    ASSERT_TRUE(rb != nullptr);

    const glm::vec3 offset = rb->local_center_of_mass();
    expect(std::abs(offset.z) > 1e-4f, "this chassis' centre of mass really is offset from its origin");
    for (std::size_t i = 0; i < v->config.wheels.size(); ++i) {
        const glm::vec3 authored = v->config.wheels[i].anchor;
        const glm::vec3 sim = v->sim_config().wheels[i].anchor;
        expect_near(glm::length((authored - offset) - sim), 0.0f, 1e-5f,
                    "wheel " + std::to_string(i) + ": the simulation's anchor is the authored one "
                    "minus the centre-of-mass offset");
    }
}

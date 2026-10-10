/**
 * @file camera_controller_test.cpp
 * @brief CameraController (toyengine/scene/camera_controller.h) driven through a real Scene with no
 *        device: orbit aim, pitch/zoom clamps, tracking, seeding, movement smoothing, wall
 *        collision and first-person mode.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <coopa/scene/scene.h>
#include <toyengine/scene/camera_controller.h>

#include "engine/support/checks.h"
#include "engine/support/character_rig.h"

COOPA_TEST_SUITE("camera_controller");

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;
using toy::scene::CameraController;
using namespace toy::test;

namespace {

/**
 * @brief Builds a one-object scene with a Transform + CameraController, NOT yet
 * started -- mirrors SceneLoader's real order (every component on an object is
 * configured from YAML before Scene::start() ever runs), so callers should set
 * any cc-> fields they care about (target, tracker, clamps, ...) before calling
 * scene->start() themselves. Getting this order right matters: start() seeds
 * distance/yaw/pitch and the initial smoothed_target_ from whatever `target`/
 * `tracker` already holds, so changing them afterwards would leave that seed
 * (and one frame of exponential lag before the real target catches up) stale.
 */
std::unique_ptr<Scene> make_orbit_scene(CameraController** out_cc, const glm::vec3& seed_pos) {
    auto scene = std::make_unique<Scene>("orbit_test");
    auto obj = std::make_unique<SceneObject>("camera");
    auto* tc = obj->add_component<TransformComponent>();
    tc->transform().set_position(seed_pos);
    *out_cc = obj->add_component<CameraController>();
    scene->add_root_object(std::move(obj));
    return scene;
}

/** @brief True if the camera's local -Z axis points at `target` within `epsilon_deg`. */
bool camera_aims_at(const TransformComponent& tc, const glm::vec3& target, float epsilon_deg = 0.5f) {
    glm::mat4 world = tc.get_world_matrix();
    glm::vec3 pos = glm::vec3(world[3]);
    glm::vec3 forward = -glm::normalize(glm::vec3(world[2]));
    glm::vec3 to_target = target - pos;
    if (glm::length(to_target) < 1e-5f) return true;
    to_target = glm::normalize(to_target);
    float cos_angle = glm::clamp(glm::dot(forward, to_target), -1.0f, 1.0f);
    return glm::degrees(std::acos(cos_angle)) <= epsilon_deg;
}

} // namespace

COOPA_TEST(orbit_keeps_aim_on_the_target_under_mouse_input) {
    CameraController* cc = nullptr;
    auto scene = make_orbit_scene(&cc, glm::vec3(0.0f, -10.0f, 6.0f));
    cc->target = glm::vec3(0.0f, 0.0f, 0.5f);
    scene->start();

    auto* tc = scene->root_objects()[0]->get_transform();
    // start() already seeded distance/yaw/pitch from the initial pose and aimed
    // at the target; update() with zero input should reproduce that exactly.
    scene->update(1.0f / 60.0f);
    expect(camera_aims_at(*tc, cc->target),
          "camera_controller: orbit with zero input still aims at the target");

    // Apply some mouse-driven yaw/pitch and confirm the aim survives it.
    cc->mouse_delta = glm::vec2(37.0f, -12.0f);
    scene->update(1.0f / 60.0f);
    expect(camera_aims_at(*tc, cc->target),
          "camera_controller: orbit after mouse yaw/pitch still aims at the target");
}

COOPA_TEST(pitch_and_zoom_saturate_at_their_limits) {
    {
        CameraController* cc = nullptr;
        auto scene = make_orbit_scene(&cc, glm::vec3(0.0f, -5.0f, 2.0f));
        cc->min_pitch_deg = 0.0f;
        cc->max_pitch_deg = 85.0f;
        scene->start();

        // A huge upward mouse motion should saturate at max_pitch_deg, not overshoot or flip.
        cc->mouse_delta = glm::vec2(0.0f, 100000.0f);
        scene->update(1.0f / 60.0f);
        expect(cc->pitch_deg == cc->max_pitch_deg,
              "camera_controller: pitch saturates at max_pitch_deg instead of overshooting");

        cc->mouse_delta = glm::vec2(0.0f, -100000.0f);
        scene->update(1.0f / 60.0f);
        expect(cc->pitch_deg == cc->min_pitch_deg,
              "camera_controller: pitch saturates at min_pitch_deg instead of overshooting");
    }
    {
        CameraController* cc = nullptr;
        auto scene = make_orbit_scene(&cc, glm::vec3(0.0f, -5.0f, 0.0f));
        cc->min_distance = 1.0f;
        cc->max_distance = 10.0f;
        scene->start(); // seed distance ~= 5

        cc->scroll_input = 1000.0f; // zoom in hard
        scene->update(1.0f / 60.0f);
        expect(cc->distance == cc->min_distance,
              "camera_controller: zoom-in clamps at min_distance");

        cc->scroll_input = -1000.0f; // zoom out hard
        scene->update(1.0f / 60.0f);
        expect(cc->distance == cc->max_distance,
              "camera_controller: zoom-out clamps at max_distance");
    }
}

COOPA_TEST(tracker_is_followed_and_an_unresolved_one_falls_back) {
    {
        auto scene = std::make_unique<Scene>("tracker_test");

        auto target_obj = std::make_unique<SceneObject>("target");
        auto* target_tc = target_obj->add_component<TransformComponent>();
        target_tc->transform().set_position(glm::vec3(0.0f, 0.0f, 0.0f));
        scene->add_root_object(std::move(target_obj));

        auto cam_obj = std::make_unique<SceneObject>("camera");
        auto* cam_tc = cam_obj->add_component<TransformComponent>();
        cam_tc->transform().set_position(glm::vec3(0.0f, -5.0f, 2.0f));
        auto* cc = cam_obj->add_component<CameraController>();
        cc->tracker = "target";
        cc->follow_smoothing = 0.0f; // snap instantly
        scene->add_root_object(std::move(cam_obj));

        scene->start();

        // Move the tracked object and confirm the camera re-aims at its new position in one frame.
        scene->root_objects()[0]->get_transform()->transform().set_position(glm::vec3(3.0f, 1.0f, 0.5f));
        scene->update(1.0f / 60.0f);

        expect(camera_aims_at(*cam_tc, glm::vec3(3.0f, 1.0f, 0.5f)),
              "camera_controller: follow_smoothing=0 snaps onto the tracked object in one frame");
    }
    {
        CameraController* cc = nullptr;
        auto scene = make_orbit_scene(&cc, glm::vec3(0.0f, -5.0f, 2.0f));
        cc->tracker = "does_not_exist";
        cc->target = glm::vec3(1.0f, 2.0f, 0.0f);
        scene->start();

        // Should not crash, and should fall back to the static target.
        scene->update(1.0f / 60.0f);
        auto* tc = scene->root_objects()[0]->get_transform();
        expect(camera_aims_at(*tc, cc->target),
              "camera_controller: an unresolved tracker name falls back to the static target");
    }
}

COOPA_TEST(start_seeds_from_the_transform_without_a_teleport) {
    // A camera placed by hand in YAML (no explicit distance/yaw/pitch) should not
    // jump on frame one -- start() must derive those from the seed transform.
    glm::vec3 seed_pos(0.0f, -10.0f, 6.0f);
    CameraController* cc = nullptr;
    auto scene = make_orbit_scene(&cc, seed_pos);
    cc->target = glm::vec3(0.0f, 0.0f, 0.5f);
    scene->start();

    auto* tc = scene->root_objects()[0]->get_transform();
    glm::vec3 pos_before = glm::vec3(tc->get_world_matrix()[3]);

    scene->update(0.0f); // zero dt, zero input -- must reproduce the seed pose exactly
    glm::vec3 pos_after = glm::vec3(tc->get_world_matrix()[3]);

    expect(glm::length(pos_after - pos_before) < 1e-3f,
          "camera_controller: seeding from the initial transform doesn't teleport on frame one");
}

COOPA_TEST(movement_smoothing_zero_is_instant_and_max_drags_then_converges) {
    {
        // Default movement_smoothing (0) must reproduce the pre-drag behavior exactly:
        // a mouse input is fully applied to the actual camera pose the same frame.
        CameraController* cc = nullptr;
        auto scene = make_orbit_scene(&cc, glm::vec3(0.0f, -5.0f, 0.0f)); // yaw=0, pitch=0, distance=5
        scene->start();
        auto* tc = scene->root_objects()[0]->get_transform();

        cc->mouse_delta = glm::vec2(600.0f, 0.0f); // 600px * 0.15 deg/px = 90 degrees of yaw
        scene->update(1.0f / 60.0f);

        glm::vec3 pos = glm::vec3(tc->get_world_matrix()[3]);
        expect(glm::length(pos - glm::vec3(5.0f, 0.0f, 0.0f)) < 0.01f,
              "camera_controller: movement_smoothing=0 applies mouse input to the pose in the same frame");
    }
    {
        // At movement_smoothing=1 (max drag), the same single-frame yaw input should
        // barely move the camera at first, then converge close to the fully-applied
        // pose once given several seconds of simulated time with no further input.
        CameraController* cc = nullptr;
        auto scene = make_orbit_scene(&cc, glm::vec3(0.0f, -5.0f, 0.0f)); // yaw=0, pitch=0, distance=5
        cc->movement_smoothing = 1.0f;
        scene->start();
        auto* tc = scene->root_objects()[0]->get_transform();

        cc->mouse_delta = glm::vec2(600.0f, 0.0f); // -> raw yaw_deg jumps to 90 immediately
        scene->update(1.0f / 60.0f);
        expect_near(cc->yaw_deg, 90.0f, 0.01f,
                    "camera_controller: raw yaw_deg accumulates the full mouse input in one frame regardless of drag");

        glm::vec3 pos_one_frame = glm::vec3(tc->get_world_matrix()[3]);
        expect(glm::length(pos_one_frame - glm::vec3(0.0f, -5.0f, 0.0f)) < 0.2f,
              "camera_controller: movement_smoothing=1 keeps the camera pose nearly unchanged one frame after a large input");

        cc->mouse_delta = glm::vec2(0.0f); // mouse released; let the drag catch up
        for (int i = 0; i < 900; ++i) scene->update(1.0f / 60.0f); // ~15 simulated seconds
        glm::vec3 pos_converged = glm::vec3(tc->get_world_matrix()[3]);
        expect(glm::length(pos_converged - glm::vec3(5.0f, 0.0f, 0.0f)) < 0.3f,
              "camera_controller: movement_smoothing=1 still converges to the input-driven pose given enough time");
    }
}

/**
 * @brief A tracking orbit camera collides: a wall between the tracked object and the camera
 *        pulls it in to the wall; with collide off it stays at its distance.
 */
COOPA_TEST(orbit_pulls_in_to_walls_only_when_collide_is_on) {
    for (bool collide : {true, false}) {
        CharacterRig rig;
        character_ground(rig);
        rig.box("wall", glm::vec3(0.0f, -3.0f, 2.0f), glm::vec3(6.0f, 0.4f, 4.0f));
        rig.spawn(glm::vec3(0.0f));
        auto cam_obj = std::make_unique<SceneObject>("camera");
        cam_obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, -8.0f, 1.0f));
        auto* cam = cam_obj->add_component<toy::scene::CameraController>();
        cam->tracker = "player";
        cam->target_offset = glm::vec3(0.0f, 0.0f, 1.0f);
        cam->distance = 8.0f;
        cam->yaw_deg = 0.0f;
        cam->pitch_deg = 5.0f;
        cam->collide = collide;
        cam->collision_radius = 0.2f;
        SceneObject* cam_raw = cam_obj.get();
        rig.scene.add_root_object(std::move(cam_obj));
        rig.start();
        rig.step(60);
        const float d = glm::length(cam_raw->get_transform()->transform().position() - glm::vec3(0.0f, 0.0f, 1.0f));
        if (collide) {
            // The wall's near face is at y = -2.8; the sphere stops radius short of it.
            expect(d < 2.9f && d > 2.4f, "camera collision: pulled in to the wall (distance " + std::to_string(d) + ")");
        } else {
            expect_near(d, 8.0f, 0.01f, "camera collision: collide off keeps the distance");
        }
    }
}

/** @brief FirstPerson: the camera sits at eye height on the tracked character, and mouse yaw
 *         turns the character; W then walks the way the camera faces. */
COOPA_TEST(first_person_mouse_yaw_turns_the_character) {
    CharacterRig rig;
    character_ground(rig);
    rig.spawn(glm::vec3(0.0f));
    auto cam_obj = std::make_unique<SceneObject>("camera");
    cam_obj->add_component<TransformComponent>();
    auto* cam = cam_obj->add_component<toy::scene::CameraController>();
    cam->mode = toy::scene::CameraControlMode::FirstPerson;
    cam->tracker = "player";
    cam->eye_height = 1.6f;
    cam->mouse_sensitivity = 1.0f;
    SceneObject* cam_raw = cam_obj.get();
    rig.scene.add_root_object(std::move(cam_obj));
    rig.start();
    rig.step(10);
    cam->mouse_delta = glm::vec2(90.0f, 0.0f); // +90 degrees of yaw in one frame
    rig.step(1);
    cam->mouse_delta = glm::vec2(0.0f);
    rig.step(2);
    expect_near(rig.cc->yaw_deg(), 90.0f, 0.5f, "first person: mouse yaw turned the character");
    const glm::vec3 eye = cam_raw->get_transform()->transform().position();
    expect_near(eye.z, rig.feet().z + 1.6f, 0.01f, "first person: camera at eye height");
    rig.cc->move_basis_yaw_deg = rig.cc->yaw_deg(); // what the engine driver derives from the camera
    rig.cc->face_movement = false;
    rig.step(60, glm::vec2(0.0f, 1.0f));
    // Yaw 90 faces -X (forward(yaw) = (-sin, cos)).
    expect(rig.feet().x < -1.0f && std::abs(rig.feet().y) < 0.1f, "first person: W walks where the camera faces");
}

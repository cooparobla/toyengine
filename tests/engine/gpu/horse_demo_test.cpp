/**
 * @file horse_demo_test.cpp
 * @brief horse_demo end to end through the engine's real input routing: F next to the horse gets
 *        on (the camera follows the horse), W + Shift gallops and D turns it, letting go stops
 *        it, F gets off again -- the rider in the saddle and every pose finite throughout -- and
 *        the hill horse stands with its body pitched by its four-legged FootIK.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/foot_ik.h>
#include <toyengine/scene/mount.h>
#include <toyengine/scene/rider.h>

#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("horse_demo");

using namespace coopa::test;
using namespace toy::test;
using coopa::input::Key;
using coopa::input::KeyAction;

namespace {

bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

/** @brief Every joint under `root` has a finite world position. */
bool all_finite(coopa::scene::SceneObject* root) {
    bool ok = true;
    root->for_each_recursive([&](const coopa::scene::SceneObject& o) {
        auto* tc = const_cast<coopa::scene::SceneObject&>(o).get_transform();
        if (tc && !finite(glm::vec3(tc->get_world_matrix()[3]))) ok = false;
    });
    return ok;
}

} // namespace

COOPA_TEST(ride_the_horse_through_the_engines_input) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    toy::core::Engine engine(make_test_config("assets/scenes/gameplay/horse_demo/scene.yaml", 320, 180, 160, 90));
    tick_frames(engine, 30);
    coopa::scene::Scene& scene = engine.scene();
    auto* player = scene.find_object("player");
    auto* horse = scene.find_object("horse");
    auto* rider = player ? player->get_component<toy::scene::Rider>() : nullptr;
    auto* mount = horse ? horse->get_component<toy::scene::Mount>() : nullptr;
    auto* cam = scene.find_first_component<toy::scene::CameraController>();
    auto* horse_cc = horse ? horse->get_component<toy::scene::CharacterController>() : nullptr;
    expect(rider && mount && cam && horse_cc, "scene: the player's Rider, the horse's Mount and the camera");
    if (!rider || !mount || !cam || !horse_cc) return;

    auto tap = [&](Key k) {
        engine.queue_input([k](coopa::input::Input& in) { in.push_key(k, 0, KeyAction::Press, coopa::input::Mods::None); });
        tick_frames(engine, 1);
        engine.queue_input([k](coopa::input::Input& in) { in.push_key(k, 0, KeyAction::Release, coopa::input::Mods::None); });
    };
    auto hold = [&](const std::vector<Key>& keys, KeyAction a) {
        for (Key k : keys) engine.queue_input([k, a](coopa::input::Input& in) { in.push_key(k, 0, a, coopa::input::Mods::None); });
    };
    float worst_seat = 0.0f;
    bool finite_ok = true;
    auto ride_frames = [&](int n) {
        for (int i = 0; i < n; ++i) {
            tick_frames(engine, 1);
            if (rider->state() == toy::scene::Rider::State::Riding) {
                auto* saddle = mount->socket(mount->saddle);
                const glm::mat4 sm = saddle->get_transform()->get_world_matrix();
                const glm::vec3 seat = glm::vec3(sm[3]) + glm::normalize(glm::quat_cast(glm::mat3(sm))) * rider->seat_offset;
                worst_seat = std::max(worst_seat, glm::length(glm::vec3(player->get_transform()->get_world_matrix()[3]) - seat));
            }
            finite_ok = finite_ok && all_finite(player) && all_finite(horse);
        }
    };

    // On foot, WASD moves the player only: the unridden horse stands where it is.
    {
        const glm::vec3 horse_at = horse->get_transform()->transform().position();
        const glm::vec3 player_at = player->get_transform()->transform().position();
        hold({Key::W, Key::A}, KeyAction::Press);
        tick_frames(engine, 60);
        hold({Key::W, Key::A}, KeyAction::Release);
        tick_frames(engine, 30);
        expect(glm::length(player->get_transform()->transform().position() - player_at) > 1.0f, "on foot: WASD walks the player");
        expect(glm::length(horse->get_transform()->transform().position() - horse_at) < 0.02f,
               "on foot: the unridden horse ignores WASD");
    }

    // Out to the open field west of the stations, so the gallop and the turn have room.
    horse_cc->teleport(glm::vec3(-30.0f, -20.0f, 0.0f), 0.0f);
    tick_frames(engine, 5);
    auto* player_cc = player->get_component<toy::scene::CharacterController>();
    const glm::vec3 mp = glm::vec3(mount->socket(mount->mount_point_l)->get_transform()->get_world_matrix()[3]);
    player_cc->teleport(glm::vec3(mp.x - 0.2f, mp.y, 0.0f), 0.0f);
    tick_frames(engine, 20);
    tap(Key::F);
    ride_frames(70);
    expect(rider->state() == toy::scene::Rider::State::Riding, "F: the player got on the horse");
    expect(cam->tracker == "horse", "F: the camera follows the horse (tracker " + cam->tracker + ")");

    const glm::vec3 start = horse->get_transform()->transform().position();
    hold({Key::W, Key::LeftShift}, KeyAction::Press);
    ride_frames(150);
    const glm::vec3 v = horse_cc->velocity();
    expect(glm::length(glm::vec2(v.x, v.y)) > 0.8f * mount->gallop_speed, "W+Shift: galloping (" + std::to_string(glm::length(glm::vec2(v.x, v.y))) + " m/s)");
    expect(mount->gait() == mount->gallop_state, "W+Shift: the gallop clip plays (" + mount->gait() + ")");
    expect(glm::length(horse->get_transform()->transform().position() - start) > 8.0f, "W+Shift: the horse travelled");
    hold({Key::W}, KeyAction::Release);
    hold({Key::D}, KeyAction::Press);
    const float yaw0 = horse_cc->yaw_deg();
    ride_frames(60);
    expect(std::abs(horse_cc->yaw_deg() - yaw0) > 20.0f, "D: the horse turned");
    hold({Key::D, Key::LeftShift}, KeyAction::Release);
    ride_frames(200);
    const glm::vec3 v2 = horse_cc->velocity();
    expect(glm::length(glm::vec2(v2.x, v2.y)) < 0.1f, "let go: the horse stopped");

    tap(Key::F);
    ride_frames(80);
    expect(rider->state() == toy::scene::Rider::State::OnFoot, "F again: the player got off");
    expect(cam->tracker == "player", "F again: the camera follows the player again");
    ride_frames(20);
    expect(player_cc && !player_cc->is_suspended() && player_cc->is_grounded(), "F again: on foot, on the ground");
    expect(worst_seat < 1e-3f, "riding: the rider never left the saddle (" + std::to_string(worst_seat) + " m)");
    expect(finite_ok, "riding: every joint of the player and horse stayed finite");

    auto* hill = scene.find_object("horse_hill");
    auto* hill_ik = hill ? hill->get_component<toy::scene::FootIK>() : nullptr;
    expect(hill_ik && std::abs(hill_ik->body_pitch_deg()) > 2.0f,
           "hill: the horse on the slope stands pitched (" + std::to_string(hill_ik ? hill_ik->body_pitch_deg() : 0.0f) + " deg)");
}

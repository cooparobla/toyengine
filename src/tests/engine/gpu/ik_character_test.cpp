/**
 * @file ik_character_test.cpp
 * @brief ik_character_demo end to end: the fully IK mannequin (objects/characters/mannequin_ik)
 *        as the player and as the foot-IK station's idler -- every limb, the spine and the head
 *        solved, FootIK lifting the rig's own foot targets -- posed like character_demo's
 *        keyframed mannequins in the same places.
 */

#include <coopa/testing/test.h>

#include <string>

#include <glm/glm.hpp>
#include <toyengine/rig/rig_component.h>
#include <toyengine/rig/rig_pose_math.h>
#include <toyengine/core/engine.h>
#include <toyengine/scene/character_anim_driver.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/foot_ik.h>
#include <vector>

#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("ik_character");

using namespace coopa::test;
using namespace toy::test;

/**
 * @brief The `foot_ik_demo` station's idlers -- character_demo's objects/mannequin (keyframed)
 *        and ik_character_demo's objects/mannequin_ik (every bone solved by rig-space IK, its
 *        clips baked from the keyframed one's) -- settle into the same pose standing on the
 *        same 0.2 m block: FootIK finds the IK rig's own leg TwoBoneIKs (drives_targets()) and
 *        both rigs' named bones land within a few cm of each other.
 */
COOPA_TEST(ik_mannequins_stand_like_the_keyframed_ones) {
    // Fixed, not wall-clock, dt: "solved" does noticeably more per-frame work (ChainIK +
    // four TwoBoneIKs + LookAtIK on top of what "keyed" runs), so without this the two engines'
    // real elapsed time per tick differs and the idle clip's cyclic motion drifts out of phase
    // over 90 frames -- amplified at the head/hands by the rest-basis-dominated lever arm above
    // the chest, far past this test's few-cm tolerance.
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    toy::core::Engine keyed(make_test_config("assets/scenes/gameplay/character_demo/scene.yaml", 320, 180, 160, 90));
    toy::core::Engine solved(make_test_config("assets/scenes/gameplay/ik_character_demo/scene.yaml", 320, 180, 160, 90));
    tick_frames(keyed, 90);
    tick_frames(solved, 90);

    auto* keyed_obj = keyed.scene().find_object("foot_ik_demo");
    auto* solved_obj = solved.scene().find_object("foot_ik_demo");
    auto* keyed_rig = keyed_obj ? keyed_obj->get_component<toy::rig::Rig>() : nullptr;
    auto* solved_rig = solved_obj ? solved_obj->get_component<toy::rig::Rig>() : nullptr;
    expect(keyed_rig && solved_rig, "ik mannequins: both foot_ik_demo idlers have a Rig");
    if (!keyed_rig || !solved_rig) return;

    auto* foot_ik = solved_obj->get_component<toy::scene::FootIK>();
    expect(foot_ik && foot_ik->drives_targets(), "ik mannequins: the IK rig's FootIK drives its own leg targets");

    for (const char* bone : {"pelvis", "chest", "head", "foot_l", "foot_r", "hand_l", "hand_r"}) {
        const int ki = keyed_rig->find_bone(bone);
        const int si = solved_rig->find_bone(bone);
        expect(ki >= 0 && si >= 0, std::string("ik mannequins: both rigs have bone '") + bone + "'");
        if (ki < 0 || si < 0) continue;
        const glm::vec3 kp = toy::rig::bone_world_position(*keyed_rig, ki);
        const glm::vec3 sp = toy::rig::bone_world_position(*solved_rig, si);
        expect(glm::length(kp - sp) < 0.1f, std::string("ik mannequins: '") + bone + "' lands near the keyframed pose (" +
                                                 std::to_string(glm::length(kp - sp)) + " m apart)");
    }
}

/**
 * @brief The fall-state trap, in both character benches: run at the steep ramp, jump at it a few
 *        times, then turn away. Jumping into the steep face drops the player into the V at its
 *        foot; it must slide out and land -- grounded, out of the jump state, free to walk off.
 */
COOPA_TEST(jumping_at_the_steep_ramp_never_strands_the_player_in_the_air) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    using coopa::input::Key;
    for (const char* scene : {"assets/scenes/gameplay/ik_character_demo/scene.yaml", "assets/scenes/gameplay/character_demo/scene.yaml"}) {
        toy::core::Engine engine(make_test_config(scene, 320, 180, 160, 90));
        tick_frames(engine, 5);
        auto* player = engine.scene().find_object("player");
        auto* cc = player ? player->get_component<toy::scene::CharacterController>() : nullptr;
        auto* drv = player ? player->get_component<toy::scene::CharacterAnimDriver>() : nullptr;
        expect(cc && drv, std::string(scene) + ": the player has its controller and anim driver");
        if (!cc || !drv) continue;
        cc->teleport(glm::vec3(2.0f, 2.2f, 0.0f), 0.0f);   // facing the steep ramp's foot (y = 4)
        tick_frames(engine, 20);
        auto press = [&](const std::vector<Key>& keys, coopa::input::KeyAction a) {
            for (Key k : keys) engine.queue_input([k, a](coopa::input::Input& in) { in.push_key(k, 0, a, coopa::input::Mods::None); });
        };
        press({Key::W, Key::LeftShift}, coopa::input::KeyAction::Press);
        tick_frames(engine, 50);
        for (int j = 0; j < 3; ++j) {
            press({Key::Space}, coopa::input::KeyAction::Press);
            tick_frames(engine, 1);
            press({Key::Space}, coopa::input::KeyAction::Release);
            tick_frames(engine, 40);
        }
        press({Key::W}, coopa::input::KeyAction::Release);
        press({Key::S}, coopa::input::KeyAction::Press);
        tick_frames(engine, 40);
        press({Key::S, Key::LeftShift}, coopa::input::KeyAction::Release);
        tick_frames(engine, 60);
        const glm::vec3 p = player->get_transform()->transform().position();
        expect(cc->is_grounded() && drv->current() != "jump",
               std::string(scene) + ": after jumping at the steep ramp and turning away, the player is grounded (state " +
                   drv->current() + ", pos " + std::to_string(p.y) + ", " + std::to_string(p.z) + ")");
        expect(p.y < 3.0f, std::string(scene) + ": ...and walked away from it (y " + std::to_string(p.y) + ")");
    }
}

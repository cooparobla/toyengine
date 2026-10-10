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
#include <coopa/animation/ik_system.h>
#include <toyengine/core/engine.h>
#include <toyengine/scene/character_anim_driver.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/foot_ik.h>
#include <vector>

#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("ik_character");

using namespace coopa::test;
using namespace toy::test;

namespace {

glm::vec3 joint(coopa::scene::Scene& scene, const std::string& rig, const std::string& name) {
    coopa::scene::SceneObject* root = scene.find_object(rig);
    coopa::scene::SceneObject* o = root ? root->find_descendant(name) : nullptr;
    return o ? glm::vec3(o->get_transform()->get_world_matrix()[3]) : glm::vec3(-99.0f);
}

} // namespace

/** @brief The fully IK player and foot-IK idler stand like the keyframed ones: the IK solvers are
 *         all gathered, FootIK drives the rigs' own foot targets, the idler's right foot stands on
 *         its 0.2 m block, and the joints sit where character_demo's keyframed mannequins put them. */
COOPA_TEST(ik_mannequins_stand_like_the_keyframed_ones) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    struct Pose { glm::vec3 j[6]; };
    const char* joints[6] = {"foot_l", "foot_r", "hand_l", "hand_r", "head", "shin_r"};
    auto capture = [&](const char* scene_path, bool ik, Pose& player, Pose& idler) {
        toy::core::Engine engine(make_test_config(scene_path, 320, 180, 160, 90));
        tick_frames(engine, 60);
        coopa::scene::Scene& scene = engine.scene();
        for (int i = 0; i < 6; ++i) {
            player.j[i] = joint(scene, "player", joints[i]) - joint(scene, "player", "pelvis");
            idler.j[i] = joint(scene, "foot_ik_demo", joints[i]);
        }
        if (!ik) return;
        auto* sys = dynamic_cast<coopa::anim::IkSystem*>(scene.find_system("IK"));
        expect(sys && sys->chain_count() == 2 && sys->two_bone_count() == 8 && sys->look_at_count() == 2,
               "ik scene: both IK mannequins' solvers gathered (2 spines, 8 limbs, 2 heads)");
        for (const char* rig : {"player", "foot_ik_demo"}) {
            auto* root = scene.find_object(rig);
            auto* foot_ik = root ? root->get_component<toy::scene::FootIK>() : nullptr;
            expect(foot_ik && foot_ik->drives_targets(), std::string("ik scene: ") + rig + "'s FootIK drives its own foot targets");
        }
    };
    Pose fk_player, fk_idler, ik_player, ik_idler;
    capture("assets/scenes/gameplay/character_demo/scene.yaml", false, fk_player, fk_idler);
    capture("assets/scenes/gameplay/ik_character_demo/scene.yaml", true, ik_player, ik_idler);

    // The idler faces -Y over a 0.2 m block under its right foot: FootIK raised that foot onto it.
    expect_near(ik_idler.j[1].z - ik_idler.j[0].z, fk_idler.j[1].z - fk_idler.j[0].z, 0.01f,
                "ik scene: the idler's right foot is raised onto the block as far as the keyframed idler's");
    expect(ik_idler.j[1].z - ik_idler.j[0].z > 0.15f, "ik scene: ...which is onto the 0.2 m block");
    for (int i = 0; i < 6; ++i) {
        // Not the player's knee: its capsule hovers ~2 cm, FootIK lowers the pelvis that far, and
        // the keyframed player's own FootIK leg solve eases the reach (2% soft limit) -- bending
        // that knee a few cm past the clip, where the IK rig's leg solve follows the clip exactly.
        if (std::string(joints[i]) == "shin_r") continue;
        expect(glm::length(ik_player.j[i] - fk_player.j[i]) < 0.03f,
               std::string("ik scene: the player's ") + joints[i] + " sits where the keyframed player's does (" +
                   std::to_string(glm::length(ik_player.j[i] - fk_player.j[i])) + " m off)");
    }
    for (int i = 0; i < 6; ++i) {
        expect(glm::length(ik_idler.j[i] - fk_idler.j[i]) < 0.03f,
               std::string("ik scene: the idler's ") + joints[i] + " sits where the keyframed idler's does (" +
                   std::to_string(glm::length(ik_idler.j[i] - fk_idler.j[i])) + " m off)");
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

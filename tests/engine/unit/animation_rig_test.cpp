/**
 * @file animation_rig_test.cpp
 * @brief Rigs as object hierarchies: a clip file's quaternion and position tracks drive bones by
 *        path (nlerp, children follow), and root motion hands a walk's travel to a
 *        CharacterController (which still collides) instead of the pelvis bone.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <fkYAML/node.hpp>
#include <coopa/animation/animation_system.h>
#include <coopa/animation/animation_yaml.h>
#include <coopa/scene/scene.h>

#include "engine/support/checks.h"
#include "engine/support/character_rig.h"

COOPA_TEST_SUITE("animation_rig");

using namespace toy::test;

/** @brief A clip file's rotation_quat track drives a bone of an object hierarchy (nlerp), and
 *         a position track a deeper one, both addressed by paths under the Animator's object. */
COOPA_TEST(clip_tracks_drive_a_bone_hierarchy_by_path) {
    auto scene = std::make_unique<Scene>("rig_cpu");
    auto root = std::make_unique<SceneObject>("Rig");
    root->add_component<TransformComponent>();
    auto* animator = root->add_component<coopa::anim::Animator>();
    auto arm = std::make_unique<SceneObject>("Arm");
    arm->add_component<TransformComponent>();
    auto hand = std::make_unique<SceneObject>("Hand");
    hand->add_component<TransformComponent>()->transform().set_position(glm::vec3(1, 0, 0));
    SceneObject* hand_raw = hand.get();
    SceneObject* arm_raw = arm->add_child(std::move(hand)) ? arm.get() : nullptr;
    arm_raw->get_transform();
    hand_raw->get_transform()->set_parent_transform(&arm_raw->get_transform()->transform());
    root->add_child(std::move(arm));
    arm_raw->get_transform()->set_parent_transform(&root->get_transform()->transform());
    SceneObject* root_raw = root.get();
    scene->add_root_object(std::move(root));

    const float s = std::sqrt(0.5f);
    const fkyaml::node clip_yaml = fkyaml::node::deserialize(std::string(
        "clip:\n"
        "  name: wave\n"
        "  wrap: loop\n"
        "  length: 1.0\n"
        "  tracks:\n"
        "    - object: Arm\n"
        "      property: rotation_quat\n"
        "      keys:\n"
        "        - {time: 0.0, value: [0, 0, 0, 1]}\n"
        "        - {time: 1.0, value: [0, 0, ") + std::to_string(s) + ", " + std::to_string(s) + "]}\n"
        "    - object: Arm/Hand\n"
        "      property: position\n"
        "      keys:\n"
        "        - {time: 0.0, value: [1, 0, 0]}\n"
        "        - {time: 1.0, value: [2, 0, 0]}\n");
    animator->add_state("wave", std::make_shared<coopa::anim::AnimationClip>(coopa::anim::parse_clip(clip_yaml)));
    scene->start();
    animator->play("wave");
    animator->sample_at(0.5f);
    const glm::quat q = arm_raw->get_transform()->transform().rotation_quat();
    const float angle = glm::degrees(2.0f * std::atan2(std::abs(q.z), q.w));
    expect_near(angle, 45.0f, 0.5f, "rig: a rotation_quat track halfway through 0 -> 90 deg is 45 deg (nlerp)");
    expect_near(glm::length(q), 1.0f, 1e-5f, "rig: ...and stays a unit quaternion");
    expect_near(hand_raw->get_transform()->transform().position().x, 1.5f, 1e-5f, "rig: a nested path (Arm/Hand) is animated too");
    const glm::vec3 hand_world(hand_raw->get_transform()->get_world_matrix()[3]);
    expect(std::abs(hand_world.x - 1.5f * std::cos(glm::radians(45.0f))) < 1e-3f &&
               std::abs(hand_world.y - 1.5f * std::sin(glm::radians(45.0f))) < 1e-3f,
           "rig: the child follows its animated parent (it is an object hierarchy)");
    (void)root_raw;
}

/** @brief A root-motion walk drives a CharacterController: the Animator hands the clip's pelvis
 *         travel to the controller (use_root_motion), which moves by it (colliding), while the
 *         pelvis bone stays over the feet. Without use_root_motion the Animator moves the Transform. */
COOPA_TEST(root_motion_moves_the_character_controller) {
    CharacterRig rig;
    character_ground(rig);
    rig.spawn(glm::vec3(0.0f));
    SceneObject* pelvis = nullptr;
    {
        auto obj = std::make_unique<SceneObject>("pelvis");
        auto* tc = obj->add_component<TransformComponent>();
        tc->transform().set_position(glm::vec3(0.0f, 0.0f, 0.95f));
        tc->set_parent_transform(&rig.player->get_transform()->transform());
        pelvis = rig.player->add_child(std::move(obj));
    }
    auto* animator = rig.player->add_component<coopa::anim::Animator>();
    animator->apply_root_motion = true;
    const fkyaml::node clip_yaml = fkyaml::node::deserialize(std::string(
        "clip:\n"
        "  name: walk\n"
        "  wrap: loop\n"
        "  length: 1.0\n"
        "  root_motion: {object: pelvis, translation: xy}\n"
        "  tracks:\n"
        "    - object: pelvis\n"
        "      property: position.y\n"
        "      keys:\n"
        "        - {time: 0.0, value: 0}\n"
        "        - {time: 1.0, value: 1.5}\n"));
    animator->add_state("walk", std::make_shared<coopa::anim::AnimationClip>(coopa::anim::parse_clip(clip_yaml)));
    animator->auto_play = "walk";
    rig.cc->use_root_motion = true;
    rig.box("wall", glm::vec3(0.0f, 4.0f, 1.0f), glm::vec3(4.0f, 0.4f, 2.0f));   // face at y 3.8
    rig.start();
    coopa::anim::install_animation_system(rig.scene);
    // 2 s of walking at 1.5 m/s, less two frames: the controller runs before animation (one frame
    // of lag), and the freshly spawned controller is airborne until its first ground probe --
    // root motion only moves a grounded character.
    rig.step(120);
    expect_near(rig.feet().y, 1.5f * (2.0f - 2.0f / 60.0f), 0.02f, "root motion: the controller walked the clip's travel");
    expect(rig.cc->is_grounded(), "root motion: ...on the ground");
    expect_near(pelvis->get_transform()->transform().position().y, 0.0f, 1e-5f, "root motion: the pelvis bone is held over the feet");

    // On into the wall: the controller still collides.
    rig.step(180);
    expect(rig.feet().y < 3.8f - rig.cc->radius + 0.05f, "root motion: a wall stops the root-motion walk (y " + std::to_string(rig.feet().y) + ")");

    // Root motion off on the controller: it declines, and the delta is still extracted.
    rig.cc->use_root_motion = false;
    rig.step(1);
    expect(animator->root_motion_delta().translation.y > 0.0f, "root motion: still extracted with the controller declining");
}

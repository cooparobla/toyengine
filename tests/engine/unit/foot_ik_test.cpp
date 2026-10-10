/**
 * @file foot_ik_test.cpp
 * @brief FootIK on a mannequin-proportioned leg rig: a foot over a step is raised onto it, a
 *        dip drops the pelvis without accumulating, and weight 0 returns the animated pose.
 */

#include <coopa/testing/test.h>

#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <coopa/animation/ik_system.h>
#include <physxcoopa/components/capsule_collider.h>
#include <toyengine/scene/foot_ik.h>

#include "engine/support/checks.h"
#include "engine/support/character_rig.h"

COOPA_TEST_SUITE("foot_ik");

using namespace toy::test;

namespace {

/** @brief A mannequin-proportioned pair of legs under a pelvis (bones as in the mannequin rig:
 *         pelvis 0.95 m up, thigh 0.43, shin 0.41 -- ankles 0.06 m above the feet origin). */
SceneObject* build_leg_rig(CharacterRig& rig, const glm::vec3& at) {
    auto root = std::make_unique<SceneObject>("legs");
    root->add_component<TransformComponent>()->transform().set_position(at);
    SceneObject* r = rig.scene.add_root_object(std::move(root));
    auto bone = [](SceneObject* parent, const std::string& name, const glm::vec3& pos) {
        auto obj = std::make_unique<SceneObject>(name);
        auto* tc = obj->add_component<TransformComponent>();
        tc->transform().set_position(pos);
        tc->set_parent_transform(&parent->get_transform()->transform());
        return parent->add_child(std::move(obj));
    };
    SceneObject* pelvis = bone(r, "pelvis", glm::vec3(0.0f, 0.0f, 0.95f));
    for (const char* side : {"l", "r"}) {
        const float sx = std::string(side) == "l" ? -0.1f : 0.1f;
        SceneObject* thigh = bone(pelvis, std::string("thigh_") + side, glm::vec3(sx, 0.0f, -0.05f));
        // A slight animated-looking bend, so the knee has a direction even before IK.
        thigh->get_transform()->transform().set_rotation(glm::vec3(10.0f, 0.0f, 0.0f));
        SceneObject* shin = bone(thigh, std::string("shin_") + side, glm::vec3(0.0f, 0.0f, -0.43f));
        shin->get_transform()->transform().set_rotation(glm::vec3(-25.0f, 0.0f, 0.0f));
        bone(shin, std::string("foot_") + side, glm::vec3(0.0f, 0.0f, -0.41f));
    }
    // The character's own capsule body, which the foot rays start inside and must ignore.
    auto* rb = r->add_component<coopa::physx::components::RigidbodyComponent>();
    rb->is_kinematic = true;
    rb->use_gravity = false;
    auto* cap = r->add_component<coopa::physx::components::CapsuleCollider>();
    cap->set_radius(0.3f);
    cap->set_height(1.8f);
    cap->set_direction(2);
    cap->set_center(glm::vec3(0.0f, 0.0f, 0.9f));
    r->add_component<toy::scene::FootIK>()->blend_speed = 0.0f;
    return r;
}

} // namespace

/** @brief FootIK on a step: a foot over a 0.15 m block is raised onto it and the other stays on
 *         the ground; over a 0.2 m dip the pelvis drops so the low foot reaches down, the other
 *         knee bends; weight 0 returns the animated (input) pose. */
COOPA_TEST(feet_meet_steps_and_dips_and_weight_zero_restores_the_pose) {
    auto world_z = [](SceneObject* root, const std::string& path) {
        SceneObject* o = coopa::anim::resolve_ik_path(root, nullptr, path);
        return o ? glm::vec3(o->get_transform()->get_world_matrix()[3]) : glm::vec3(-99.0f);
    };
    const std::string foot_l = "pelvis/thigh_l/shin_l/foot_l", foot_r = "pelvis/thigh_r/shin_r/foot_r";
    {
        CharacterRig rig;
        character_ground(rig);
        rig.box("step", glm::vec3(0.4f, 0.0f, 0.075f), glm::vec3(0.5f, 0.8f, 0.15f));   // x 0.15 .. 0.65
        SceneObject* legs = build_leg_rig(rig, glm::vec3(0.1f, 0.0f, 0.0f));   // right foot at x 0.2 (on it), left at 0.0
        rig.scene.start();
        rig.physics = coopa::physx::system::install_physics_system(rig.scene);
        coopa::anim::install_ik_system(rig.scene);
        const float rest_z = world_z(legs, foot_l).z;
        for (int i = 0; i < 3; ++i) { rig.scene.update(1.0f / 60.0f); rig.scene.late_update(1.0f / 60.0f); }
        auto* ik = legs->get_component<toy::scene::FootIK>();
        expect_near(ik->foot_offset(1), 0.15f, 1e-3f, "foot ik: the right ray found the step top (not the own capsule)");
        expect_near(ik->foot_offset(0), 0.0f, 1e-3f, "foot ik: the left ray found the ground");
        expect_near(ik->pelvis_offset(), 0.0f, 1e-5f, "foot ik: nothing below the base plane, so the pelvis stays");
        expect_near(world_z(legs, foot_r).z, rest_z + 0.15f, 0.01f, "foot ik: the right foot is raised onto the step");
        expect_near(world_z(legs, foot_l).z, rest_z, 0.01f, "foot ik: the left foot stays on the ground");
        const glm::vec3 knee_r = world_z(legs, "pelvis/thigh_r/shin_r");
        expect(knee_r.y > 0.03f, "foot ik: the raised leg's knee bends forward (y " + std::to_string(knee_r.y) + ")");

        // Weight 0: back to the input pose exactly (no IK left baked into the bones).
        ik->weight = 0.0f;
        rig.scene.update(1.0f / 60.0f);
        rig.scene.late_update(1.0f / 60.0f);
        expect_near(world_z(legs, foot_r).z, rest_z, 1e-4f, "foot ik: weight 0 restores the animated pose");
    }
    {
        CharacterRig rig;
        // Ground at z = 0 on the right (x > 0), a 0.2 m dip on the left.
        rig.box("ground_r", glm::vec3(15.0f, 0.0f, -0.5f), glm::vec3(30.0f, 30.0f, 1.0f));
        rig.box("ground_l", glm::vec3(-15.0f, 0.0f, -0.7f), glm::vec3(30.0f, 30.0f, 1.0f));
        SceneObject* legs = build_leg_rig(rig, glm::vec3(0.0f));
        rig.scene.start();
        rig.physics = coopa::physx::system::install_physics_system(rig.scene);
        coopa::anim::install_ik_system(rig.scene);
        const float rest_z = world_z(legs, foot_r).z;
        for (int i = 0; i < 3; ++i) { rig.scene.update(1.0f / 60.0f); rig.scene.late_update(1.0f / 60.0f); }
        auto* ik = legs->get_component<toy::scene::FootIK>();
        expect_near(ik->pelvis_offset(), -0.2f, 1e-3f, "foot ik: the pelvis drops by the lower foot's offset");
        expect_near(world_z(legs, "pelvis").z, 0.75f, 1e-3f, "foot ik: ...which moves the pelvis bone down");
        expect_near(world_z(legs, foot_l).z, rest_z - 0.2f, 0.01f, "foot ik: the left foot reaches down into the dip");
        expect_near(world_z(legs, foot_r).z, rest_z, 0.01f, "foot ik: the right foot stays on its ground (knee bent)");
        // Several frames on: stable, and the pelvis offset is not compounded on the bone.
        for (int i = 0; i < 10; ++i) { rig.scene.update(1.0f / 60.0f); rig.scene.late_update(1.0f / 60.0f); }
        expect_near(world_z(legs, "pelvis").z, 0.75f, 1e-3f, "foot ik: the pelvis drop does not accumulate");
    }
}

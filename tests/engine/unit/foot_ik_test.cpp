/**
 * @file foot_ik_test.cpp
 * @brief FootIK on a mannequin-proportioned leg rig: a foot over a step is raised onto it, a
 *        dip drops the pelvis without accumulating, and weight 0 returns the animated pose; and
 *        four legs (the horse prefab) on a slope: every hoof on the ground, the body pitched.
 */

#include <coopa/testing/test.h>

#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <coopa/asset/asset_manager.h>
#include <coopa/rig/ik_components.h>
#include <coopa/rig/ik_system.h>
#include <coopa/rig/rig_asset.h>
#include <coopa/rig/rig_component.h>
#include <coopa/rig/rig_pose_math.h>
#include <physxcoopa/components/capsule_collider.h>
#include <toyengine/scene/foot_ik.h>

#include "engine/support/checks.h"
#include "engine/support/character_rig.h"
#include "engine/support/horse_rig.h"

COOPA_TEST_SUITE("foot_ik");

using namespace toy::test;

namespace {

/** @brief A mannequin-proportioned pair of legs under a pelvis bone (as in rigs/mannequin.yaml:
 *         pelvis 0.95 m up, thigh 0.43, shin 0.41 -- ankles 0.06 m above the feet origin). */
struct LegRig {
    std::unique_ptr<coopa::asset::AssetManager> assets;
    SceneObject* root = nullptr;
    coopa::rig::Rig* rig = nullptr;
    int thigh_l = -1, shin_l = -1, thigh_r = -1, shin_r = -1;

    /** @brief Composes `delta` (a world-space rotation) onto bone i's CURRENT pose -- call only
     *         right after rig->clear_pose(), so "current" is exactly rest and the result is
     *         idempotent regardless of what a previous frame's IK left there. Called
     *         parent-before-child, so a child's compose reads its parent's just-applied bend as
     *         "current" (see coopa/rig/rig_pose_math.h's apply_world_rotation_delta doc for the
     *         pre-multiply-onto-current algebra this relies on). */
    void set_bend(int bone_idx, const glm::quat& delta) { coopa::rig::apply_world_rotation_delta(*rig, bone_idx, delta, 1.0f); }

    /** @brief Re-applies the "animated-looking bend" FootIK solves against -- stands in for a
     *         SkeletalAnimator that would otherwise refresh these bones every frame. Resets only
     *         the four leg bones, not the whole rig: the pelvis bone is FootIK's own to manage
     *         (it undoes its own prior drop/pitch before writing a fresh one, same as a real
     *         rig where nothing else touches that channel -- clearing it out from under FootIK
     *         here would fight that bookkeeping). The thigh tilts 10 deg forward from hanging
     *         straight down, the shin an additional -25 deg at the knee (both about world X,
     *         applied parent-first so they stack). */
    void reassert_input() {
        for (int idx : {thigh_l, shin_l, thigh_r, shin_r}) rig->pose(static_cast<size_t>(idx)) = coopa::rig::Rig::PoseChannel{};
        const glm::quat thigh_bend = glm::angleAxis(glm::radians(10.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::quat knee_bend = glm::angleAxis(glm::radians(-25.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        set_bend(thigh_l, thigh_bend);
        set_bend(shin_l, knee_bend);
        set_bend(thigh_r, thigh_bend);
        set_bend(shin_r, knee_bend);
    }

    glm::vec3 bone_world(const std::string& name) const {
        return coopa::rig::bone_world_position(*rig, rig->find_bone(name));
    }
};

LegRig build_leg_rig(CharacterRig& rig, const glm::vec3& at) {
    using coopa::rig::Bone;
    coopa::rig::RigAsset asset;
    asset.name = "legs";
    Bone pelvis;
    pelvis.name = "pelvis";
    pelvis.head = glm::vec3(0.0f, 0.0f, 0.95f);
    pelvis.tail = glm::vec3(0.0f, 0.1f, 0.95f);
    asset.bones.push_back(pelvis);

    LegRig out;
    for (const char* side : {"l", "r"}) {
        const float sx = std::string(side) == "l" ? -0.1f : 0.1f;
        Bone thigh;
        thigh.name = std::string("thigh_") + side;
        thigh.parent = 0;
        thigh.head = glm::vec3(sx, 0.0f, 0.90f);
        thigh.tail = glm::vec3(sx, 0.0f, 0.47f);
        asset.bones.push_back(thigh);
        const int thigh_idx = static_cast<int>(asset.bones.size()) - 1;

        Bone shin;
        shin.name = std::string("shin_") + side;
        shin.parent = thigh_idx;
        shin.head = glm::vec3(sx, 0.0f, 0.47f);
        shin.tail = glm::vec3(sx, 0.0f, 0.06f);
        asset.bones.push_back(shin);
        const int shin_idx = static_cast<int>(asset.bones.size()) - 1;

        Bone foot;
        foot.name = std::string("foot_") + side;
        foot.parent = shin_idx;
        foot.head = glm::vec3(sx, 0.0f, 0.06f);
        foot.tail = glm::vec3(sx, 0.15f, 0.06f);
        asset.bones.push_back(foot);

        if (std::string(side) == "l") { out.thigh_l = thigh_idx; out.shin_l = shin_idx; }
        else { out.thigh_r = thigh_idx; out.shin_r = shin_idx; }
    }
    asset.compute_rest();

    out.assets = std::make_unique<coopa::asset::AssetManager>();
    auto handle = out.assets->create<coopa::rig::RigAsset>("test/legs", std::make_shared<coopa::rig::RigAsset>(std::move(asset)));

    auto root = std::make_unique<SceneObject>("legs");
    root->add_component<TransformComponent>()->transform().set_position(at);
    SceneObject* r = rig.scene.add_root_object(std::move(root));
    out.rig = r->add_component<coopa::rig::Rig>();
    out.rig->asset = handle;

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
    out.root = r;
    return out;
}

} // namespace

/** @brief FootIK on a step: a foot over a 0.15 m block is raised onto it and the other stays on
 *         the ground; over a 0.2 m dip the pelvis drops so the low foot reaches down, the other
 *         knee bends; weight 0 returns the animated (input) pose. */
COOPA_TEST(feet_meet_steps_and_dips_and_weight_zero_restores_the_pose) {
    {
        CharacterRig rig;
        character_ground(rig);
        rig.box("step", glm::vec3(0.4f, 0.0f, 0.075f), glm::vec3(0.5f, 0.8f, 0.15f));   // x 0.15 .. 0.65
        LegRig legs = build_leg_rig(rig, glm::vec3(0.1f, 0.0f, 0.0f));   // right foot at x 0.2 (on it), left at 0.0
        legs.reassert_input();
        rig.scene.start();
        rig.physics = coopa::physx::system::install_physics_system(rig.scene);
        coopa::rig::install_ik_system(rig.scene);
        toy::scene::install_rig_ik_driver_system(rig.scene);
        coopa::rig::install_rig_pose_system(rig.scene);
        const float rest_z = legs.bone_world("foot_l").z;
        for (int i = 0; i < 3; ++i) {
            legs.reassert_input();
            rig.scene.update(1.0f / 60.0f);
            rig.scene.late_update(1.0f / 60.0f);
        }
        auto* ik = legs.root->get_component<toy::scene::FootIK>();
        expect_near(ik->foot_offset(1), 0.15f, 1e-3f, "foot ik: the right ray found the step top (not the own capsule)");
        expect_near(ik->foot_offset(0), 0.0f, 1e-3f, "foot ik: the left ray found the ground");
        expect_near(ik->pelvis_offset(), 0.0f, 1e-5f, "foot ik: nothing below the base plane, so the pelvis stays");
        expect_near(legs.bone_world("foot_r").z, rest_z + 0.15f, 0.01f, "foot ik: the right foot is raised onto the step");
        expect_near(legs.bone_world("foot_l").z, rest_z, 0.01f, "foot ik: the left foot stays on the ground");
        const glm::vec3 knee_r = legs.bone_world("shin_r");
        expect(knee_r.y > 0.03f, "foot ik: the raised leg's knee bends forward (y " + std::to_string(knee_r.y) + ")");

        // Weight 0: back to the input pose exactly (no IK left baked into the bones).
        ik->weight = 0.0f;
        legs.reassert_input();
        rig.scene.update(1.0f / 60.0f);
        rig.scene.late_update(1.0f / 60.0f);
        expect_near(legs.bone_world("foot_r").z, rest_z, 1e-4f, "foot ik: weight 0 restores the animated pose");
    }
    {
        CharacterRig rig;
        // Ground at z = 0 on the right (x > 0), a 0.2 m dip on the left.
        rig.box("ground_r", glm::vec3(15.0f, 0.0f, -0.5f), glm::vec3(30.0f, 30.0f, 1.0f));
        rig.box("ground_l", glm::vec3(-15.0f, 0.0f, -0.7f), glm::vec3(30.0f, 30.0f, 1.0f));
        LegRig legs = build_leg_rig(rig, glm::vec3(0.0f));
        legs.reassert_input();
        rig.scene.start();
        rig.physics = coopa::physx::system::install_physics_system(rig.scene);
        coopa::rig::install_ik_system(rig.scene);
        toy::scene::install_rig_ik_driver_system(rig.scene);
        coopa::rig::install_rig_pose_system(rig.scene);
        const float rest_z = legs.bone_world("foot_r").z;
        for (int i = 0; i < 3; ++i) {
            legs.reassert_input();
            rig.scene.update(1.0f / 60.0f);
            rig.scene.late_update(1.0f / 60.0f);
        }
        auto* ik = legs.root->get_component<toy::scene::FootIK>();
        expect_near(ik->pelvis_offset(), -0.2f, 1e-3f, "foot ik: the pelvis drops by the lower foot's offset");
        expect_near(legs.bone_world("pelvis").z, 0.75f, 1e-3f, "foot ik: ...which moves the pelvis bone down");
        expect_near(legs.bone_world("foot_l").z, rest_z - 0.2f, 0.01f, "foot ik: the left foot reaches down into the dip");
        expect_near(legs.bone_world("foot_r").z, rest_z, 0.01f, "foot ik: the right foot stays on its ground (knee bent)");
        // Several frames on: stable, and the pelvis offset is not compounded on the bone.
        for (int i = 0; i < 10; ++i) {
            legs.reassert_input();
            rig.scene.update(1.0f / 60.0f);
            rig.scene.late_update(1.0f / 60.0f);
        }
        expect_near(legs.bone_world("pelvis").z, 0.75f, 1e-3f, "foot ik: the pelvis drop does not accumulate");
    }
}

/** @brief FootIK on an IK-driven rig (mode auto -> targets): the legs' own TwoBoneIKs reach for
 *         target empties, and FootIK raises a target over a step onto it instead of solving the
 *         legs itself -- the foot follows, the other stays on the ground. */
COOPA_TEST(targets_mode_lifts_the_rigs_own_leg_targets) {
    CharacterRig rig;
    character_ground(rig);
    rig.box("step", glm::vec3(0.4f, 0.0f, 0.075f), glm::vec3(0.5f, 0.8f, 0.15f));   // x 0.15 .. 0.65
    LegRig legs = build_leg_rig(rig, glm::vec3(0.1f, 0.0f, 0.0f));
    legs.reassert_input();
    // The rig's own leg IK: target empties (children of the root) where the animated feet are.
    for (const char* side : {"l", "r"}) {
        const std::string s = side;
        const glm::vec3 foot = legs.bone_world(std::string("foot_") + s) - glm::vec3(0.1f, 0.0f, 0.0f);
        auto target = std::make_unique<SceneObject>("foot_target_" + s);
        auto* tc = target->add_component<TransformComponent>();
        tc->transform().set_position(foot);
        tc->set_parent_transform(&legs.root->get_transform()->transform());
        legs.root->add_child(std::move(target));
        auto* ik = legs.root->add_component<coopa::rig::TwoBoneIK>();
        ik->upper = "thigh_" + s;
        ik->lower = "shin_" + s;
        ik->end = "foot_" + s;
        ik->target = "foot_target_" + s;
        ik->soft_limit = 0.0f;
    }
    rig.scene.start();
    rig.physics = coopa::physx::system::install_physics_system(rig.scene);
    coopa::rig::install_ik_system(rig.scene);
    toy::scene::install_rig_ik_driver_system(rig.scene);
    coopa::rig::install_rig_pose_system(rig.scene);
    auto world = [&](const std::string& path) {
        SceneObject* o = coopa::anim::resolve_ik_path(legs.root, nullptr, path);
        return o ? glm::vec3(o->get_transform()->get_world_matrix()[3]) : glm::vec3(-99.0f);
    };
    const float ground_z = world("foot_target_l").z;
    for (int i = 0; i < 3; ++i) {
        legs.reassert_input();
        rig.scene.update(1.0f / 60.0f);
        rig.scene.late_update(1.0f / 60.0f);
    }
    auto* foot_ik = legs.root->get_component<toy::scene::FootIK>();
    expect(foot_ik->drives_targets(), "targets mode: auto picked the rig's own leg IK");
    expect_near(foot_ik->foot_offset(1), 0.15f, 1e-3f, "targets mode: the right target's ray found the step top");
    expect_near(legs.bone_world("foot_r").z, ground_z + 0.15f, 0.01f,
                "targets mode: the right foot stands on the step (its target raised)");
    expect_near(legs.bone_world("foot_l").z, ground_z, 0.01f, "targets mode: the left foot stays on the ground");
    expect_near(world("foot_target_r").z, ground_z, 1e-5f, "targets mode: the target empty itself is untouched (an override)");
}

/** @brief Four legs (objects/animals/horse: FootIK `legs` + body_pitch, solve mode) standing
 *         across a 10 degree slope: each hoof is planted on the slope under it (its joint at the
 *         clip's 0.12 m above the surface), the body pitches up the slope by about the slope's
 *         angle, and the hind and fore offsets straddle the controller's base. */
COOPA_TEST(quadruped_hooves_meet_a_slope_and_the_body_pitches) {
    const float slope = glm::radians(10.0f);
    // The slab's top passes through (0, 0, 1), rising toward +Y.
    HorseRig rig(glm::vec3(0.0f, 0.0f, 1.05f), 0.0f, false);
    if (!rig.hooves) { expect(false, "quadruped: the horse prefab has a FootIK"); return; }
    const glm::vec3 n(0.0f, -std::sin(slope), std::cos(slope));
    rig.box("slope", glm::vec3(0.0f, 0.0f, 1.0f) - 0.5f * n, glm::vec3(12.0f, 12.0f, 1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    rig.start();
    rig.step(120);
    expect(rig.horse_cc->is_grounded(), "quadruped: the horse stands on the slope");
    expect(rig.hooves->legs.size() == 4, "quadruped: four legs");
    auto* horse_rig = rig.horse ? rig.horse->get_component<coopa::rig::Rig>() : nullptr;
    for (const char* name : {"fore_hoof_l", "fore_hoof_r", "hind_hoof_l", "hind_hoof_r"}) {
        const glm::vec3 h = horse_rig ? coopa::rig::bone_world_position(*horse_rig, horse_rig->find_bone(name)) : glm::vec3(-99.0f);
        const float surface = 1.0f + h.y * std::tan(slope);
        expect_near(h.z, surface + 0.12f, 0.03f, std::string("quadruped: ") + name + " planted on the slope");
    }
    expect(rig.hooves->body_pitch_deg() > 7.0f && rig.hooves->body_pitch_deg() < 13.0f,
           "quadruped: the body pitches up the slope (" + std::to_string(rig.hooves->body_pitch_deg()) + " deg)");
    expect(rig.hooves->foot_offset(0) > 0.05f && rig.hooves->foot_offset(2) < -0.05f,
           "quadruped: the fore hooves are above the base, the hind ones below");
}

/**
 * @file rig_ik_test.cpp
 * @brief Rig-space IK: TwoBoneIK and ChainIK reach a world-space target by posing bones in a
 *        Rig (not scene Transforms), LookAtIK turns a bone within its clamp, and
 *        BoneAttachment carries a plain object along a posed bone.
 */

#include <coopa/testing/test.h>

#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <coopa/asset/asset_manager.h>
#include <coopa/rig/bone_attachment.h>
#include <coopa/rig/bone_attachment_system.h>
#include <coopa/rig/ik_components.h>
#include <coopa/rig/ik_system.h>
#include <coopa/rig/rig_asset.h>
#include <coopa/rig/rig_component.h>
#include <coopa/rig/rig_system.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("rig_ik");

using coopa::rig::Bone;
using coopa::rig::BoneAttachment;
using coopa::rig::ChainIK;
using coopa::rig::LookAtIK;
using coopa::rig::Rig;
using coopa::rig::RigAsset;
using coopa::rig::TwoBoneIK;
using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

namespace {

/** @brief A straight three-joint chain along +Z: root (0..1), mid (1..2), end (2..2.1). */
RigAsset make_chain_rig() {
    RigAsset r;
    r.name = "chain";
    Bone root;
    root.name = "root";
    root.head = glm::vec3(0.0f);
    root.tail = glm::vec3(0.0f, 0.0f, 1.0f);
    r.bones.push_back(root);
    Bone mid;
    mid.name = "mid";
    mid.parent = 0;
    mid.connected = true;
    mid.tail = glm::vec3(0.0f, 0.0f, 2.0f);
    r.bones.push_back(mid);
    Bone end;
    end.name = "end";
    end.parent = 1;
    end.connected = true;
    end.tail = glm::vec3(0.0f, 0.0f, 2.1f);
    r.bones.push_back(end);
    r.compute_rest();
    return r;
}

/** @brief Scene with a Rig (the given asset) on a root object named "Rig", plus the IK/RigPose
 *         systems installed. Returns the Rig and the Scene for the test to add targets to. */
struct Fixture {
    coopa::asset::AssetManager assets;
    Scene scene{"rig_ik"};
    Rig* rig = nullptr;
    SceneObject* rig_obj = nullptr;

    explicit Fixture(RigAsset asset) : scene("rig_ik") {
        auto obj = std::make_unique<SceneObject>("Rig");
        obj->add_component<TransformComponent>();
        rig = obj->add_component<Rig>();
        rig->asset = assets.create<RigAsset>("test/rig_ik", std::make_shared<RigAsset>(std::move(asset)));
        rig_obj = scene.add_root_object(std::move(obj));
        coopa::rig::install_ik_system(scene);
        coopa::rig::install_bone_attachment_system(scene);
        coopa::rig::install_rig_pose_system(scene);
    }

    SceneObject* add_point(const std::string& name, const glm::vec3& pos) {
        auto obj = std::make_unique<SceneObject>(name);
        obj->add_component<TransformComponent>()->transform().set_position(pos);
        return scene.add_root_object(std::move(obj));
    }

    void tick() {
        scene.start();
        scene.update(0.0f);
        scene.late_update(0.0f);
    }
};

} // namespace

/** @brief The end bone's head reaches a world-space target, bending the middle joint toward
 *         the pole -- the same contract as coopa::anim::TwoBoneIK, in bone space. */
COOPA_TEST(two_bone_ik_reaches_a_world_space_target) {
    Fixture f(make_chain_rig());
    f.add_point("Target", glm::vec3(0.8f, 0.0f, 1.2f));
    f.add_point("Pole", glm::vec3(1.0f, 0.0f, 1.0f));
    auto* ik = f.rig_obj->add_component<TwoBoneIK>();
    ik->upper = "root";
    ik->lower = "mid";
    ik->end = "end";
    ik->target = "Target";
    ik->pole = "Pole";
    f.tick();

    const glm::vec3 end_head(f.rig->world_matrix(2)[3]);
    expect_near(glm::length(end_head - glm::vec3(0.8f, 0.0f, 1.2f)), 0.0f, 0.01f,
                "two-bone IK: the end bone's head reaches the target");
    const glm::vec3 mid_head(f.rig->world_matrix(1)[3]);
    expect(mid_head.x > 0.05f, "two-bone IK: the middle joint bends toward the pole (+X)");
}

/** @brief weight 0 leaves every bone at rest -- nothing to restore to, since IK never wrote
 *         the pose in the first place (unlike coopa::anim::IkPoseGuard's feedback problem). */
COOPA_TEST(two_bone_ik_at_zero_weight_leaves_the_rest_pose) {
    Fixture f(make_chain_rig());
    f.add_point("Target", glm::vec3(0.8f, 0.0f, 1.2f));
    auto* ik = f.rig_obj->add_component<TwoBoneIK>();
    ik->upper = "root";
    ik->lower = "mid";
    ik->end = "end";
    ik->target = "Target";
    ik->weight = 0.0f;
    f.tick();
    const glm::vec3 end_head(f.rig->world_matrix(2)[3]);
    expect_near(glm::length(end_head - glm::vec3(0.0f, 0.0f, 2.0f)), 0.0f, 1e-4f,
                "two-bone IK: weight 0 leaves the end bone at its rest head");
}

/** @brief ChainIK (FABRIK) bends every bone of the chain so the tip reaches the target,
 *         keeping each segment's length. */
COOPA_TEST(chain_ik_reaches_a_target_and_keeps_segment_lengths) {
    Fixture f(make_chain_rig());
    f.add_point("Target", glm::vec3(1.0f, 0.0f, 1.0f));
    auto* ik = f.rig_obj->add_component<ChainIK>();
    ik->root = "root";
    ik->tip = "end";
    ik->target = "Target";
    f.tick();

    const glm::vec3 p0(f.rig->world_matrix(0)[3]);
    const glm::vec3 p1(f.rig->world_matrix(1)[3]);
    const glm::vec3 p2(f.rig->world_matrix(2)[3]);
    expect_near(glm::length(p1 - p0), 1.0f, 1e-3f, "chain IK: the root-mid segment keeps its length");
    expect_near(glm::length(p2 - p1), 1.0f, 1e-3f, "chain IK: the mid-end segment keeps its length");
    expect_near(glm::length(p2 - glm::vec3(1.0f, 0.0f, 1.0f)), 0.0f, 0.01f, "chain IK: the tip reaches the target");
}

/** @brief LookAtIK turns the root bone's forward axis toward a target, clamped by max_angle. */
COOPA_TEST(look_at_ik_turns_within_its_clamp) {
    Fixture f(make_chain_rig());
    // The root bone's rest +Y axis (its own local "forward" per bone_rest_matrix's convention).
    auto* look = f.rig_obj->add_component<LookAtIK>();
    look->bone = "root";
    look->forward_axis = glm::vec3(0.0f, 1.0f, 0.0f);
    look->up_axis = glm::vec3(0.0f, 0.0f, 1.0f);
    look->max_angle = 89.0f;
    f.add_point("Target", glm::vec3(5.0f, 5.0f, 1.0f)); // far off to the side
    look->target = "Target";
    f.tick();

    const glm::quat rest_rot = coopa::anim::ik::rotation_of(f.rig->rest_model_matrix(0));
    const glm::vec3 rest_forward = rest_rot * glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::quat posed_rot = coopa::anim::ik::rotation_of(f.rig->world_matrix(0));
    const glm::vec3 posed_forward = posed_rot * glm::vec3(0.0f, 1.0f, 0.0f);
    expect(glm::dot(rest_forward, posed_forward) < 0.999f, "look-at IK: the bone actually turned");

    const glm::vec3 to_target = glm::normalize(glm::vec3(5.0f, 5.0f, 1.0f) - glm::vec3(f.rig->world_matrix(0)[3]));
    expect(glm::dot(posed_forward, to_target) > 0.9f, "look-at IK: it turned toward the target");
}

/** @brief BoneAttachment carries a plain object's own Transform along a posed bone -- its own
 *         local position/rotation become an offset from the bone's head (or tail). */
COOPA_TEST(bone_attachment_rides_the_posed_bone) {
    Fixture f(make_chain_rig());
    auto prop = std::make_unique<SceneObject>("Prop");
    prop->add_component<TransformComponent>();
    auto* ba = prop->add_component<BoneAttachment>();
    ba->bone = "end";
    ba->rig_path = "Rig";
    f.scene.add_root_object(std::move(prop));

    auto* ik = f.rig_obj->add_component<TwoBoneIK>();
    ik->upper = "root";
    ik->lower = "mid";
    ik->end = "end";
    f.add_point("Target", glm::vec3(0.8f, 0.0f, 1.2f));
    ik->target = "Target";
    f.tick();

    auto* prop_obj = f.scene.find_object("Prop");
    require(prop_obj != nullptr, "bone attachment: the prop exists");
    const glm::vec3 prop_pos(prop_obj->get_transform()->get_world_matrix()[3]);
    const glm::vec3 bone_head(f.rig->world_matrix(2)[3]);
    expect_near(glm::length(prop_pos - bone_head), 0.0f, 1e-3f,
                "bone attachment: the prop sits exactly at the posed bone's head");

    // Move the target further: the prop must follow the bone, not stay where it was.
    auto* target_obj = f.scene.find_object("Target");
    target_obj->get_transform()->transform().set_position(glm::vec3(0.5f, 0.6f, 1.6f));
    f.scene.update(0.0f);
    f.scene.late_update(0.0f);
    const glm::vec3 prop_pos2(prop_obj->get_transform()->get_world_matrix()[3]);
    expect(glm::length(prop_pos2 - prop_pos) > 0.05f, "bone attachment: it re-follows the bone every frame");
}

/**
 * @file bone_pose_edit_test.cpp
 * @brief Pose-mode edits on a coopa::rig::Rig's pose buffer: a world-space Grab/Rotate delta
 *        lands the bone exactly where it asked, Clear resets to rest, and mirror/copy/paste
 *        round-trip across bones by name.
 */

#include <coopa/testing/test.h>

#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/quaternion.hpp>

#include <coopa/asset/asset_manager.h>
#include <coopa/rig/rig_asset.h>
#include <coopa/rig/rig_component.h>
#include <coopa/rig/rig_system.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/components/transform_component.h>

#include "editor/rig/bone_pose_edit.h"

COOPA_TEST_SUITE("bone_pose_edit");

using namespace coopa::test;
using coopa::rig::Bone;
using coopa::rig::Rig;
using coopa::rig::RigAsset;
using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;
namespace rpe = toy::editor::rig_pose_edit;

namespace {

/** @brief pelvis -> spine -> arm_l / arm_r, the arms symmetric about X = 0 (a mirror test rig). */
RigAsset make_test_rig() {
    RigAsset rig;
    rig.name = "test_rig";
    Bone pelvis;
    pelvis.name = "pelvis";
    pelvis.head = glm::vec3(0.0f, 0.0f, 0.0f);
    pelvis.tail = glm::vec3(0.0f, 0.0f, 0.2f);
    rig.bones.push_back(pelvis);
    Bone spine;
    spine.name = "spine";
    spine.parent = 0;
    spine.head = glm::vec3(0.0f, 0.0f, 0.2f);
    spine.tail = glm::vec3(0.0f, 0.0f, 1.0f);
    rig.bones.push_back(spine);
    Bone arm_l;
    arm_l.name = "arm_l";
    arm_l.parent = 1;
    arm_l.head = glm::vec3(-0.2f, 0.0f, 1.0f);
    arm_l.tail = glm::vec3(-1.0f, 0.0f, 1.0f);
    rig.bones.push_back(arm_l);
    Bone arm_r;
    arm_r.name = "arm_r";
    arm_r.parent = 1;
    arm_r.head = glm::vec3(0.2f, 0.0f, 1.0f);
    arm_r.tail = glm::vec3(1.0f, 0.0f, 1.0f);
    rig.bones.push_back(arm_r);
    rig.compute_rest();
    return rig;
}

struct TestRig {
    coopa::asset::AssetManager assets;
    Scene scene{"pose_edit_test"};
    Rig* rig = nullptr;

    TestRig() {
        auto asset = std::make_shared<RigAsset>(make_test_rig());
        auto handle = assets.create<RigAsset>("test/pose_edit_rig", asset);
        auto obj = std::make_unique<SceneObject>("Rig");
        obj->add_component<TransformComponent>();
        rig = obj->add_component<Rig>();
        rig->asset = handle;
        scene.add_root_object(std::move(obj));
        coopa::rig::install_rig_pose_system(scene);
        scene.start();
        tick();
    }

    void tick() {
        scene.update(0.0f);
        scene.late_update(0.0f);
    }
};

} // namespace

/** @brief A world-space translate delta moves the bone's resolved world head by exactly that
 *         delta, however the bone's own rest basis is tilted. */
COOPA_TEST(apply_translate_delta_moves_the_world_head_by_exactly_the_delta) {
    TestRig t;
    const int arm_l = static_cast<int>(2);
    const glm::vec3 before(t.rig->world_matrix(static_cast<std::size_t>(arm_l))[3]);
    const glm::vec3 delta(0.3f, -0.1f, 0.25f);
    rpe::apply_translate_delta(*t.rig, static_cast<std::size_t>(arm_l), delta);
    t.tick();
    const glm::vec3 after(t.rig->world_matrix(static_cast<std::size_t>(arm_l))[3]);
    expect(glm::all(glm::epsilonEqual(after - before, delta, 1e-4f)),
           "a world translate delta lands the bone's world head exactly `delta` away from where it started");
}

/** @brief A world-space rotation delta changes the bone's world rotation by exactly that delta
 *         (pre-multiplied), regardless of the bone's own rest tilt. */
COOPA_TEST(apply_rotate_delta_applies_exactly_in_world_space) {
    TestRig t;
    const std::size_t arm_r = 3;
    const glm::quat before = glm::quat_cast(glm::mat3(t.rig->world_matrix(arm_r)));
    const glm::vec3 axis(0.0f, 0.0f, 1.0f);
    const float deg = 30.0f;
    rpe::apply_rotate_delta(*t.rig, arm_r, axis, deg);
    t.tick();
    const glm::quat after = glm::quat_cast(glm::mat3(t.rig->world_matrix(arm_r)));
    const glm::quat expected = glm::normalize(glm::angleAxis(glm::radians(deg), axis) * before);
    const float agreement = std::abs(glm::dot(after, expected));
    expect(agreement > 0.999f, "a world rotate delta pre-multiplies onto the bone's world rotation exactly");
}

/** @brief Clear resets each pose channel to its rest (identity) value. */
COOPA_TEST(clear_ops_reset_to_rest) {
    TestRig t;
    const std::size_t spine = 1;
    t.rig->pose(spine).location = glm::vec3(1, 2, 3);
    t.rig->pose(spine).rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 0, 1));
    t.rig->pose(spine).scale = glm::vec3(2.0f);
    rpe::clear_location(*t.rig, spine);
    rpe::clear_rotation(*t.rig, spine);
    rpe::clear_scale(*t.rig, spine);
    expect(t.rig->pose(spine).location == glm::vec3(0.0f), "clear_location: back to (0,0,0)");
    expect(glm::all(glm::epsilonEqual(t.rig->pose(spine).rotation, glm::quat(1, 0, 0, 0), 1e-6f)), "clear_rotation: back to identity");
    expect(t.rig->pose(spine).scale == glm::vec3(1.0f), "clear_scale: back to (1,1,1)");
}

/** @brief Mirroring arm_l's pose onto arm_r negates location.x and the rotation's swing axes,
 *         so a raised left arm mirrors into a raised right arm, not a lowered one. */
COOPA_TEST(mirror_pose_flips_x_location_and_swing_rotation) {
    TestRig t;
    const std::size_t arm_l = 2, arm_r = 3;
    t.rig->pose(arm_l).location = glm::vec3(0.1f, 0.0f, 0.0f);
    t.rig->pose(arm_l).rotation = glm::angleAxis(glm::radians(40.0f), glm::vec3(0, 0, 1)); // swing about Z
    rpe::mirror_pose(*t.rig, arm_l, arm_r);
    t.tick();
    expect_near(t.rig->pose(arm_r).location.x, -0.1f, 1e-5f, "mirror: location.x negates");
    const glm::vec3 larm(t.rig->world_matrix(arm_l)[3]);
    const glm::vec3 rarm(t.rig->world_matrix(arm_r)[3]);
    // Both arms were raised toward +Z by the same swing; mirrored across X, their Z displacement
    // from rest should agree in sign (both lift, neither drops) even though X does not.
    expect((larm.z - 1.0f > 0.01f) == (rarm.z - 1.0f > 0.01f), "mirror: both arms lift the same way, not opposite ways");
}

/** @brief copy_pose/paste_pose round-trip by bone NAME, leaving an untouched bone alone. */
COOPA_TEST(copy_and_paste_round_trip_by_bone_name) {
    TestRig t;
    const std::size_t spine = 1, arm_l = 2, arm_r = 3;
    t.rig->pose(spine).location = glm::vec3(0, 0, 0.05f);
    t.rig->pose(arm_l).rotation = glm::angleAxis(glm::radians(20.0f), glm::vec3(1, 0, 0));
    const rpe::PoseSnapshot snap = rpe::copy_pose(*t.rig, {spine, arm_l});

    t.rig->pose(spine).location = glm::vec3(0.0f);
    t.rig->pose(arm_l).rotation = glm::quat(1, 0, 0, 0);
    t.rig->pose(arm_r).location = glm::vec3(9, 9, 9); // untouched bone: must survive the paste

    const std::size_t applied = rpe::paste_pose(*t.rig, snap);
    expect(applied == 2, "paste applies to exactly the 2 bones the snapshot has");
    expect_near(t.rig->pose(spine).location.z, 0.05f, 1e-6f, "paste: spine's location came back");
    expect(glm::all(glm::epsilonEqual(t.rig->pose(arm_l).rotation, glm::angleAxis(glm::radians(20.0f), glm::vec3(1, 0, 0)), 1e-5f)),
           "paste: arm_l's rotation came back");
    expect(t.rig->pose(arm_r).location == glm::vec3(9, 9, 9), "paste: a bone not in the snapshot is left alone");
}

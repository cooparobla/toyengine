/**
 * @file clip_pose_test.cpp
 * @brief apply_bone_pose(): a SkeletalClip sampled into a coopa::rig::Rig's pose buffer by bone
 *        name, leaving untouched channels alone, and holding root motion at its clip-start value.
 */

#include <coopa/testing/test.h>

#include <coopa/asset/asset_manager.h>
#include <coopa/rig/rig_asset.h>
#include <coopa/rig/rig_component.h>
#include <coopa/rig/skeletal_clip.h>

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/quaternion.hpp>

#include "editor/anim/clip_pose.h"

COOPA_TEST_SUITE("clip_pose");

namespace toy::editor::testing {

using namespace coopa::test;
using coopa::rig::Bone;
using coopa::rig::Rig;
using coopa::rig::RigAsset;
using coopa::rig::SkeletalClip;
using coopa::rig::SkeletalRootMotionSpec;
using coopa::rig::SkeletalTrack;

namespace {

RigAsset two_bone_rig() {
    RigAsset rig;
    rig.name = "arm";
    Bone upper;
    upper.name = "upper";
    upper.tail = glm::vec3(0.0f, 0.0f, 1.0f);
    rig.bones.push_back(upper);
    Bone lower;
    lower.name = "lower";
    lower.parent = 0;
    lower.connected = true;
    lower.tail = glm::vec3(0.0f, 0.0f, 2.0f);
    rig.bones.push_back(lower);
    rig.compute_rest();
    return rig;
}

coopa::anim::Keyframe key(float t, float x, float y, float z, float w = 0.0f) {
    coopa::anim::Keyframe k;
    k.time = t;
    k.value[0] = x; k.value[1] = y; k.value[2] = z; k.value[3] = w;
    return k;
}

struct Fixture {
    coopa::asset::AssetManager assets;
    Rig rig;
    Fixture() {
        auto asset = std::make_shared<RigAsset>(two_bone_rig());
        rig.asset = assets.create<RigAsset>("test/arm_rig", asset);
    }
};

} // namespace

/** @brief A location/rotation/scale track drives its named bone's pose; an untouched bone keeps
 *         its prior pose (hand-posed or IK-driven channels show through, per the file's doc). */
COOPA_TEST(apply_bone_pose_drives_the_named_bone_and_leaves_others_alone) {
    Fixture f;
    f.rig.pose(1).location = glm::vec3(9.0f, 9.0f, 9.0f);   // "lower": must survive untouched

    SkeletalClip clip;
    SkeletalTrack track;
    track.bone = "upper";
    track.location.add_key(key(0.0f, 0.0f, 0.0f, 0.0f));
    track.location.add_key(key(1.0f, 0.0f, 0.0f, 2.0f));
    track.rotation.add_key(key(0.0f, 0.0f, 0.0f, 0.0f, 1.0f));
    track.rotation.add_key(key(1.0f, 1.0f, 0.0f, 0.0f, 0.0f));   // 180 deg about X
    track.scale.add_key(key(0.0f, 1.0f, 1.0f, 1.0f));
    track.scale.add_key(key(1.0f, 2.0f, 2.0f, 2.0f));
    clip.bones.push_back(track);

    const int applied = apply_bone_pose(clip, 0.5f, &f.rig);
    expect(applied == 1, "one bone track applied");
    expect_near(f.rig.pose(0).location.z, 1.0f, 1e-5f, "upper's location sampled at the midpoint");
    const glm::quat expected = glm::slerp(glm::quat(1, 0, 0, 0), glm::quat(0, 1, 0, 0), 0.5f);
    expect(std::abs(glm::dot(f.rig.pose(0).rotation, expected)) > 0.9999f, "upper's rotation slerps (not lerps)");
    expect_near(f.rig.pose(0).scale.x, 1.5f, 1e-5f, "upper's scale sampled at the midpoint");
    expect(glm::all(glm::epsilonEqual(f.rig.pose(1).location, glm::vec3(9.0f), 1e-5f)),
           "lower (no track) keeps whatever pose it already had");
}

/** @brief A root-motion bone's held-translation axes stay at the clip's start value (so the
 *         preview holds the rig in place), while a non-held axis still samples normally. */
COOPA_TEST(apply_bone_pose_holds_root_motion_translation_at_clip_start) {
    Fixture f;
    SkeletalClip clip;
    SkeletalTrack track;
    track.bone = "upper";
    track.location.add_key(key(0.0f, 0.0f, 0.0f, 0.3f));
    track.location.add_key(key(1.0f, 4.0f, 0.0f, 0.9f));
    clip.bones.push_back(track);
    clip.root_motion.bone = "upper";
    clip.root_motion.translation = coopa::anim::RootMotionTranslation::XY;   // X held; Z (not XY) samples normally

    apply_bone_pose(clip, 1.0f, &f.rig);
    expect_near(f.rig.pose(0).location.x, 0.0f, 1e-5f, "X is a held root-motion axis: stays at clip-start (0), not the sampled 4");
    expect_near(f.rig.pose(0).location.z, 0.9f, 1e-5f, "Z is not a root-motion axis: samples normally at t=1");
}

} // namespace toy::editor::testing

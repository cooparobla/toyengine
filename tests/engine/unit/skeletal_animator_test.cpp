/**
 * @file skeletal_animator_test.cpp
 * @brief SkeletalAnimator plays SkeletalClips onto a sibling Rig's pose: bone tracks by name,
 *        slerp crossfade blending, event firing, and root motion extraction.
 */

#include <coopa/testing/test.h>

#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/quaternion.hpp>

#include <coopa/asset/asset_manager.h>
#include <coopa/rig/rig_asset.h>
#include <coopa/rig/rig_component.h>
#include <coopa/rig/rig_system.h>
#include <coopa/rig/skeletal_animation_system.h>
#include <coopa/rig/skeletal_animator.h>
#include <coopa/scene/scene.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("skeletal_animator");

using coopa::rig::Bone;
using coopa::rig::Rig;
using coopa::rig::RigAsset;
using coopa::rig::SkeletalAnimator;
using coopa::rig::SkeletalAnimatorState;
using coopa::rig::SkeletalClip;
using coopa::rig::SkeletalTrack;
using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

namespace {

RigAsset make_arm_rig() {
    RigAsset rig;
    rig.name = "arm";
    Bone upper;
    upper.name = "upper";
    upper.head = glm::vec3(0.0f, 0.0f, 0.0f);
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

/** @brief A clip rotating "upper" from identity to `target` over 1 second, by quaternion keys. */
std::shared_ptr<SkeletalClip> make_rotation_clip(const std::string& name, const glm::quat& target) {
    auto clip = std::make_shared<SkeletalClip>();
    clip->name = name;
    clip->set_explicit_length(1.0f);
    SkeletalTrack track;
    track.bone = "upper";
    coopa::anim::Keyframe k0;
    k0.time = 0.0f;
    k0.value[0] = 0.0f; k0.value[1] = 0.0f; k0.value[2] = 0.0f; k0.value[3] = 1.0f;
    coopa::anim::Keyframe k1;
    k1.time = 1.0f;
    k1.value[0] = target.x; k1.value[1] = target.y; k1.value[2] = target.z; k1.value[3] = target.w;
    track.rotation.add_key(k0);
    track.rotation.add_key(k1);
    track.rotation.sort_keys();
    clip->bones.push_back(track);
    return clip;
}

struct Fixture {
    coopa::asset::AssetManager assets;
    Scene scene{"skel"};
    Rig* rig = nullptr;
    SkeletalAnimator* animator = nullptr;
    SceneObject* object = nullptr;

    Fixture() : scene("skel") {
        auto obj = std::make_unique<SceneObject>("Rig");
        obj->add_component<TransformComponent>();
        rig = obj->add_component<Rig>();
        auto asset = std::make_shared<RigAsset>(make_arm_rig());
        rig->asset = assets.create<RigAsset>("test/arm_rig", asset);
        animator = obj->add_component<SkeletalAnimator>();
        object = scene.add_root_object(std::move(obj));
        coopa::rig::install_skeletal_animation_system(scene);
        coopa::rig::install_rig_pose_system(scene);
        scene.start();
    }

    void tick(float dt) {
        scene.update(dt);
        scene.late_update(dt);
    }
};

} // namespace

/** @brief A rotation_quat-equivalent bone track drives the named bone's pose, nothing else. */
COOPA_TEST(clip_tracks_drive_a_bone_by_name) {
    Fixture f;
    const glm::quat target = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    f.animator->add_state("swing", make_rotation_clip("swing", target));
    f.animator->play("swing");
    f.animator->sample_at(0.5f); // mid-clip, so the default Loop wrap's time-0-equals-time-length aliasing never enters into it
    f.tick(0.0f);
    const glm::quat pose_rot = f.rig->pose(0).rotation;
    const glm::quat expected = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), target, 0.5f);
    expect(std::abs(glm::dot(pose_rot, expected)) > 0.9999f, "skeletal clip: the named bone's pose rotation matches a slerp sample mid-clip");
    expect_near(f.rig->pose(1).rotation.w, 1.0f, 1e-5f, "skeletal clip: an untouched bone (lower) stays at its identity pose");
}

/** @brief Crossfading two rotation clips slerps, taking the short arc regardless of how the
 *         keys were authored -- unlike coopa::anim's componentwise-lerp rotation_quat property. */
COOPA_TEST(crossfade_blends_by_slerp_between_two_clips) {
    Fixture f;
    const glm::quat a = glm::angleAxis(glm::radians(80.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::quat b = glm::angleAxis(glm::radians(-80.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    SkeletalAnimatorState* a_state = f.animator->add_state("a", make_rotation_clip("a", a));
    a_state->has_wrap_override = true;
    a_state->wrap_override = coopa::anim::WrapMode::Once; // holds its end key once reached, instead of looping back to 0
    f.animator->add_state("b", make_rotation_clip("b", b));
    f.animator->play("a");
    f.animator->sample_at(1.0f); // settle "a" at its end key
    f.tick(0.0f);
    f.animator->crossfade("b", 1.0f); // "b" starts at its own time 0 (identity) the instant the fade begins
    f.tick(0.5f);                     // halfway through the 1 s crossfade

    const glm::quat pose_rot = f.rig->pose(0).rotation;
    // At the crossfade's halfway point, "a" is still held at its end key (a) and "b" has
    // advanced 0.5 s into its own 1 s clip (toward b, at t=0.5). The two layers blend by
    // slerp(prev, cur, weight) -- not nlerp -- so the result must itself be a valid slerp
    // between the two layers' own instantaneous rotations, at the reported crossfade weight.
    const glm::quat b_half = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), b, 0.5f);
    const glm::quat expected = glm::slerp(a, b_half, 0.5f);
    expect(std::abs(glm::dot(pose_rot, expected)) > 0.999f, "crossfade: the blended pose is a true slerp of the two layers");
}

/** @brief A clip event fires once as the playhead crosses it, exactly like coopa::anim::Animator. */
COOPA_TEST(events_fire_during_advance) {
    Fixture f;
    auto clip = make_rotation_clip("swing", glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 0, 1)));
    clip->events.push_back({0.5f, "mid", "", 0.0f});
    f.animator->add_state("swing", clip);
    f.animator->auto_play = "swing";
    int fired = 0;
    auto conn = f.animator->on_event.connect([&](const coopa::anim::AnimationEvent& ev) {
        if (ev.name == "mid") ++fired;
    });
    f.scene.start(); // re-run start() to bind auto_play now that the state exists
    f.tick(0.6f);
    expect(fired == 1, "events: the event at t=0.5 fires exactly once crossing it in one frame");
    f.tick(1.0f); // raw time 0.6 -> 1.6: crosses the loop boundary at 1.0, then local 0.5 again at 1.5
    expect(fired == 2, "events: a second crossing (via looping) fires it again");
    (void)conn;
}

/** @brief Root motion: the pelvis bone's location track is extracted as travel and (optionally)
 *         applied to the owner's Transform, mirroring coopa::anim::Animator's root motion. */
COOPA_TEST(root_motion_extracts_bone_travel) {
    Fixture f;
    auto clip = std::make_shared<SkeletalClip>();
    clip->name = "walk";
    clip->set_explicit_length(1.0f);
    clip->root_motion.bone = "upper";
    clip->root_motion.translation = coopa::anim::RootMotionTranslation::XY;
    SkeletalTrack track;
    track.bone = "upper";
    coopa::anim::Keyframe k0, k1;
    k0.time = 0.0f; k0.value[0] = 0.0f; k0.value[1] = 0.0f; k0.value[2] = 0.0f;
    k1.time = 1.0f; k1.value[0] = 2.0f; k1.value[1] = 0.0f; k1.value[2] = 0.0f;
    track.location.add_key(k0);
    track.location.add_key(k1);
    track.location.sort_keys();
    clip->bones.push_back(track);
    f.animator->add_state("walk", clip);
    f.animator->auto_play = "walk";
    f.scene.start();

    f.tick(0.5f); // half the clip: 1 m of the 2 m root travel
    expect_near(f.animator->root_motion_delta().translation.x, 1.0f, 1e-3f, "root motion: half the clip's travel is extracted this frame");
    expect_near(f.rig->pose(0).location.x, 0.0f, 1e-3f, "root motion: the root bone itself is held at its clip-start value, not carrying the travel");
    expect_near(f.object->get_transform()->transform().position().x, 0.0f, 1e-3f, "root motion: apply_root_motion is off by default -- the owner does not move");

    f.animator->apply_root_motion = true;
    f.tick(0.4f);
    expect(f.object->get_transform()->transform().position().x > 0.5f, "root motion: with apply_root_motion on, the owner's Transform moves by the extracted delta");
}

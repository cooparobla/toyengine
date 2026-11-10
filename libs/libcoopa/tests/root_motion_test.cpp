/**
 * @file root_motion_test.cpp
 * @brief Animator root motion: per-frame deltas integrate exactly across loop boundaries (and
 *        one big multi-loop step), extracted channels are held on the bone while the rest still
 *        animate, crossfading to a clip without root motion ramps the delta down by weight, and
 *        yaw goes to an IRootMotionReceiver when it accepts (else to the Transform).
 */
#include <coopa/testing/test.h>

#include "support/animation.h"

#include <coopa/animation/animation_clip.h>
#include <coopa/animation/animation_system.h>
#include <coopa/animation/animator.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

COOPA_TEST_SUITE("root_motion");

using namespace coopa::anim;
using namespace coopa::scene;
using libcoopa_test::add_bone;
using libcoopa_test::event_clip;
using libcoopa_test::linear_track;
using libcoopa_test::step;

COOPA_TEST(translation_integrates_exactly_across_loops_and_fades_out_by_weight) {
    Scene scene("RootMotion");
    auto obj = std::make_unique<SceneObject>("Rig");
    obj->add_component<TransformComponent>();
    auto* animator = obj->add_component<Animator>();
    SceneObject* rig  = scene.add_root_object(std::move(obj));
    SceneObject* hips = add_bone(rig, "hips", {0.0f, 0.0f, 1.0f});

    // Walk: hips travel 0 -> 2 along +Y per 1 s cycle and bob in Z (z stays on the bone: xy mode).
    auto walk = std::make_shared<AnimationClip>();
    walk->wrap = WrapMode::Loop;
    walk->set_explicit_length(1.0f);
    walk->root_motion.object = "hips";
    walk->root_motion.translation = RootMotionTranslation::XY;
    walk->tracks.push_back(linear_track("position.y", 1.0f, 0.0f, 2.0f, "hips"));
    AnimationTrack tz;
    tz.object_path = "hips";
    tz.property = "position.z";
    tz.curve.add_key(Keyframe{0.0f, {1.0f, 0.0f, 0.0f, 1.0f}, Interpolation::Linear});
    tz.curve.add_key(Keyframe{0.5f, {1.2f, 0.0f, 0.0f, 1.0f}, Interpolation::Linear});
    tz.curve.add_key(Keyframe{1.0f, {1.0f, 0.0f, 0.0f, 1.0f}, Interpolation::Linear});
    tz.curve.sort_keys();
    walk->tracks.push_back(tz);
    animator->add_state("walk", walk);
    animator->add_state("idle", event_clip(1.0f, WrapMode::Loop, {})); // no root motion
    animator->auto_play = "walk";
    animator->apply_root_motion = true;

    install_animation_system(scene);
    scene.start();
    step(scene, 0.375f, 2); // t = 0.75
    EXPECT_NEAR(animator->root_motion_delta().translation.y, 0.75f, 1e-4f);
    step(scene, 0.375f, 1); // 0.75 -> 1.125 crosses the loop: (2 - 1.5) + (0.25 - 0) = 0.75
    EXPECT_NEAR(animator->root_motion_delta().translation.y, 0.75f, 1e-4f);
    step(scene, 0.375f, 1); // t = 1.5: 1.5 cycles = 3 m
    EXPECT_VEC_NEAR(rig->get_transform()->transform().position(), glm::vec3(0.0f, 3.0f, 0.0f), 1e-4f);
    const glm::vec3 hips_pos = hips->get_transform()->transform().position();
    EXPECT_NEAR(hips_pos.y, 0.0f, 1e-5f); // extracted channel held at the clip start
    EXPECT_NEAR(hips_pos.z, 1.2f, 1e-4f); // z not extracted (xy): still animated

    // A big step over two boundaries still integrates exactly: 2.5 s = 5 m.
    step(scene, 2.5f, 1);
    EXPECT_NEAR(rig->get_transform()->transform().position().y, 8.0f, 1e-3f);

    // Crossfading out to a clip without root motion ramps the delta down by weight.
    animator->crossfade("idle", 0.5f);
    float last = 1e9f;
    for (int i = 0; i < 4; ++i) {
        step(scene, 0.125f, 1);
        const float w = std::min(1.0f, 0.125f * (i + 1) / 0.5f);
        const float d = animator->root_motion_delta().translation.y;
        EXPECT_NEAR(d, (1.0f - w) * 0.25f, 1e-4f); // walk moves 0.25 per 0.125 s
        EXPECT_LE(d, last);
        last = d;
    }
    EXPECT_NEAR(animator->root_motion_delta().translation.y, 0.0f, 1e-6f);
}

COOPA_TEST(yaw_goes_to_an_accepting_receiver_otherwise_to_the_transform) {
    /** @brief Consumes root motion when enabled, like toyengine's CharacterController. */
    class Receiver : public Component, public IRootMotionReceiver {
    public:
        std::string type_name() const override { return "Receiver"; }
        bool accept = true;
        glm::vec3 total{0.0f};
        float yaw = 0.0f;
        bool consume_root_motion(const glm::vec3& d, float y) override {
            if (!accept) return false;
            total += d;
            yaw += y;
            return true;
        }
    };

    Scene scene("RootMotionYaw");
    auto obj = std::make_unique<SceneObject>("Rig");
    obj->add_component<TransformComponent>();
    auto* animator = obj->add_component<Animator>();
    auto* receiver = obj->add_component<Receiver>();
    SceneObject* rig  = scene.add_root_object(std::move(obj));
    SceneObject* hips = add_bone(rig, "hips", {0.0f, 0.0f, 1.0f});

    // A turn: hips yaw 0 -> 90 degrees over 1 s, no travel.
    auto turn = std::make_shared<AnimationClip>();
    turn->wrap = WrapMode::Once;
    turn->set_explicit_length(1.0f);
    turn->root_motion.object = "hips";
    turn->root_motion.translation = RootMotionTranslation::None;
    turn->root_motion.yaw = true;
    AnimationTrack tr;
    tr.object_path = "hips";
    tr.property = "rotation_quat";
    const glm::quat q0(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::quat q1 = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    tr.curve.add_key(Keyframe{0.0f, {q0.x, q0.y, q0.z, q0.w}, Interpolation::Linear});
    tr.curve.add_key(Keyframe{1.0f, {q1.x, q1.y, q1.z, q1.w}, Interpolation::Linear});
    tr.curve.sort_keys();
    turn->tracks.push_back(tr);
    animator->add_state("turn", turn);
    animator->auto_play = "turn";
    animator->apply_root_motion = true;

    install_animation_system(scene);
    scene.start();
    step(scene, 0.25f, 2);
    // Receiver took it; the Transform didn't move.
    EXPECT_NEAR(receiver->yaw, 45.0f, 0.5f);
    EXPECT_NEAR(rig->get_transform()->transform().rotation_degrees().z, 0.0f, 1e-5f);
    // The bone's yaw is held at the clip start.
    EXPECT_NEAR(std::abs(hips->get_transform()->transform().rotation_quat().w), 1.0f, 1e-4f);

    // A receiver that declines leaves the Animator to turn the Transform itself.
    receiver->accept = false;
    step(scene, 0.25f, 4); // to the end (clamped): the remaining 45 degrees
    EXPECT_NEAR(rig->get_transform()->transform().rotation_degrees().z, 45.0f, 0.5f);
    EXPECT_NEAR(receiver->yaw, 45.0f, 0.5f);
}

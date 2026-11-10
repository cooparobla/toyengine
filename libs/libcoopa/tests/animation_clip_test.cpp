/**
 * @file animation_clip_test.cpp
 * @brief Animation data, independent of playback: AnimationCurve sampling (linear, clamping,
 *        step, ease-in-out), and clip YAML parsing through AnimationClipLoader / parse_clip()
 *        (tracks, events sorted by time, root-motion settings, malformed files failing cleanly).
 */
#include <coopa/testing/test.h>

#include "support/scratch_file.h"

#include <coopa/animation/animation_clip.h>
#include <coopa/animation/animation_clip_loader.h>
#include <coopa/animation/animation_curve.h>
#include <coopa/animation/animation_yaml.h>
#include <coopa/animation/keyframe.h>
#include <coopa/asset/asset_manager.h>

#include <memory>
#include <string>

COOPA_TEST_SUITE("animation_clip");

using namespace coopa::anim;

COOPA_TEST(curve_sampling_interpolates_clamps_and_honours_easing) {
    AnimationCurve curve;
    curve.add_key(Keyframe{0.0f, {0.0f, 0.0f, 0.0f, 1.0f}, Interpolation::Linear});
    curve.add_key(Keyframe{1.0f, {10.0f, 0.0f, 0.0f, 1.0f}, Interpolation::Linear});
    curve.sort_keys();

    float out[1];
    curve.sample(0.0f, 1, out);
    EXPECT_NEAR(out[0], 0.0f, 1e-5f);
    curve.sample(1.0f, 1, out);
    EXPECT_NEAR(out[0], 10.0f, 1e-5f);
    curve.sample(0.5f, 1, out);
    EXPECT_NEAR(out[0], 5.0f, 1e-5f);
    curve.sample(-1.0f, 1, out); // clamps before the first key
    EXPECT_NEAR(out[0], 0.0f, 1e-5f);
    curve.sample(5.0f, 1, out); // clamps after the last key
    EXPECT_NEAR(out[0], 10.0f, 1e-5f);

    AnimationCurve empty;
    empty.sample(0.5f, 1, out);
    EXPECT_NEAR(out[0], 0.0f, 1e-5f);

    AnimationCurve step;
    step.add_key(Keyframe{0.0f, {1.0f, 0.0f, 0.0f, 1.0f}, Interpolation::Step});
    step.add_key(Keyframe{1.0f, {2.0f, 0.0f, 0.0f, 1.0f}, Interpolation::Linear});
    step.sort_keys();
    step.sample(0.5f, 1, out); // Step holds the leaving key's value for the whole segment
    EXPECT_NEAR(out[0], 1.0f, 1e-5f);

    AnimationCurve eio;
    eio.add_key(Keyframe{0.0f, {0.0f, 0.0f, 0.0f, 1.0f}, Interpolation::EaseInOut});
    eio.add_key(Keyframe{1.0f, {1.0f, 0.0f, 0.0f, 1.0f}, Interpolation::Linear});
    eio.sort_keys();
    eio.sample(0.5f, 1, out); // EaseInOut is symmetric: exactly 0.5 at the midpoint
    EXPECT_NEAR(out[0], 0.5f, 1e-5f);
}

COOPA_TEST(clip_loader_reads_tracks_and_fails_cleanly_on_a_non_clip) {
    coopa::asset::AssetManager assets;
    assets.register_loader<AnimationClip>(std::make_unique<AnimationClipLoader>());

    const std::string good_path = libcoopa_test::write_scratch_file("good.yaml",
        "clip:\n"
        "  name: test_clip\n"
        "  wrap: once\n"
        "  tracks:\n"
        "    - object: \"\"\n"
        "      component: Transform\n"
        "      property: position\n"
        "      keys:\n"
        "        - { time: 0.0, value: { x: 0, y: 0, z: 0 } }\n"
        "        - { time: 1.0, value: { x: 10, y: 0, z: 0 } }\n");
    auto good = assets.load<AnimationClip>(good_path);
    ASSERT_TRUE(good.is_loaded());
    EXPECT_EQ(good->name, std::string("test_clip"));
    ASSERT_EQ(good->tracks.size(), 1u);
    EXPECT_NEAR(good->tracks[0].curve.keys()[1].time, 1.0f, 1e-5f);
    EXPECT_TRUE(good->tracks[0].curve.keys()[1].easing == Interpolation::Linear);

    auto bad = assets.load<AnimationClip>(libcoopa_test::write_scratch_file("bad.yaml", "not_a_clip: true\n"));
    EXPECT_TRUE(bad.is_failed());
    EXPECT_FALSE(bad.error().empty());
}

COOPA_TEST(clip_yaml_parses_events_sorted_by_time_and_root_motion) {
    fkyaml::node root = fkyaml::node::deserialize(std::string(
        "clip:\n"
        "  name: walk\n"
        "  length: 1.0\n"
        "  root_motion: {object: hips, translation: xy, rotation: yaw}\n"
        "  events:\n"
        "    - {time: 0.5, name: footstep, string: right, float: 0.7}\n"
        "    - {time: 0.0, name: footstep, string: left}\n"
        "  tracks: []\n"));
    AnimationClip clip = parse_clip(root);
    ASSERT_EQ(clip.events.size(), 2u);
    EXPECT_EQ(clip.events[0].string_value, std::string("left")); // sorted by time
    EXPECT_EQ(clip.events[1].name, std::string("footstep"));
    EXPECT_NEAR(clip.events[1].float_value, 0.7f, 1e-6f);
    EXPECT_EQ(clip.root_motion.object, std::string("hips"));
    EXPECT_TRUE(clip.root_motion.translation == RootMotionTranslation::XY);
    EXPECT_TRUE(clip.root_motion.yaw);
    EXPECT_TRUE(clip.root_motion.enabled());
}

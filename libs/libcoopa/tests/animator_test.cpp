/**
 * @file animator_test.cpp
 * @brief Animator + AnimationSystem playback on a live Scene: property registry lookup,
 *        channel masks and binding dedup, wrap modes, crossfades (keyframed and procedural),
 *        binding by name/path and lazily, rotation without shortest-path wrap, parallel ==
 *        serial evaluation, the procedural orbit's closed form, runtime-spawned/destroyed
 *        animators, and the scene-YAML "Animator" component end to end.
 *
 * Events, root motion and IK have their own suites.
 */
#include <coopa/testing/test.h>

#include "support/animation.h"
#include "support/scratch_file.h"

#include <coopa/animation/animated_property.h>
#include <coopa/animation/animation_clip.h>
#include <coopa/animation/animation_clip_loader.h>
#include <coopa/animation/animation_system.h>
#include <coopa/animation/animation_yaml.h>
#include <coopa/animation/animator.h>
#include <coopa/animation/procedural_track.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/job/engine.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

COOPA_TEST_SUITE("animator");

using namespace coopa::anim;
using namespace coopa::scene;
using libcoopa_test::add_animated_object;
using libcoopa_test::linear_clip;
using libcoopa_test::linear_track;
using libcoopa_test::position_of;
using libcoopa_test::step;

namespace {

/** @brief A minimal test-only component exposing one vec3 and one float property to animate. */
class TestProbeComponent : public Component {
public:
    std::string type_name() const override { return "TestProbe"; }
    glm::vec3 pos{0.0f};
    float alpha = 0.0f;

    const glm::vec3& position() const { return pos; }
    void set_position(const glm::vec3& p) { pos = p; }
    float get_alpha() const { return alpha; }
    void set_alpha(float a) { alpha = a; }
};

/** @brief Registers TestProbe's properties once per process (the registry is a global singleton). */
void register_probe_properties() {
    static const bool registered = [] {
        auto& reg = AnimatedPropertyRegistry::instance();
        reg.register_vec<TestProbeComponent, glm::vec3>(
            "TestProbe", "pos", &TestProbeComponent::position, &TestProbeComponent::set_position);
        reg.register_float<TestProbeComponent>(
            "TestProbe", "alpha", &TestProbeComponent::get_alpha, &TestProbeComponent::set_alpha);
        return true;
    }();
    (void)registered;
}

} // namespace

COOPA_TEST(property_registry_resolves_names_and_channel_suffixes) {
    register_probe_properties();
    auto& reg = AnimatedPropertyRegistry::instance();
    uint8_t mask = 0;
    const auto* pos = reg.find("TestProbe", "pos", &mask);
    ASSERT_TRUE(pos != nullptr);
    EXPECT_EQ(static_cast<int>(mask), 0x0F);
    EXPECT_EQ(static_cast<int>(pos->component_count), 3);

    const auto* pos_y = reg.find("TestProbe", "pos.y", &mask);
    EXPECT_TRUE(pos_y == pos); // same underlying property, channel-suffixed
    EXPECT_EQ(static_cast<int>(mask), 0x02);

    EXPECT_TRUE(reg.find("TestProbe", "nonexistent") == nullptr);
    EXPECT_TRUE(reg.find("NoSuchComponent", "pos") == nullptr);

    auto names = reg.properties_for("TestProbe");
    EXPECT_TRUE(std::find(names.begin(), names.end(), "pos") != names.end());
    EXPECT_TRUE(std::find(names.begin(), names.end(), "alpha") != names.end());
}

COOPA_TEST(channel_suffixed_track_leaves_other_channels_untouched) {
    Scene scene("ChannelMask");
    auto* animator = add_animated_object(scene, "Obj");
    scene.find_object("Obj")->get_transform()->transform().set_position({1.0f, 2.0f, 3.0f});
    animator->add_state("s", linear_clip(WrapMode::Loop, 2.0f, "position.y", 99.0f, 99.0f)); // one channel
    animator->auto_play = "s";

    install_animation_system(scene);
    scene.start();
    step(scene, 0.5f);

    EXPECT_VEC_NEAR(position_of(scene, "Obj"), glm::vec3(1.0f, 99.0f, 3.0f), 1e-5f); // only y driven
}

COOPA_TEST(per_channel_tracks_dedup_into_one_binding_without_clobbering) {
    Scene scene("Dedup");
    auto* animator = add_animated_object(scene, "Obj");
    auto clip = linear_clip(WrapMode::Loop, 2.0f, "position.x", 5.0f, 5.0f);
    clip->tracks.push_back(linear_track("position.z", 2.0f, 7.0f, 7.0f));
    animator->add_state("s", clip);
    animator->auto_play = "s";

    install_animation_system(scene);
    scene.start();
    // Two tracks on the same property must share ONE binding; separate bindings each writing
    // the whole vec3 clobbered one another's channel.
    EXPECT_EQ(animator->binding_count(), 1u);

    step(scene, 0.5f);
    const glm::vec3 pos = position_of(scene, "Obj");
    EXPECT_NEAR(pos.x, 5.0f, 1e-5f);
    EXPECT_NEAR(pos.z, 7.0f, 1e-5f);
}

COOPA_TEST(wrap_modes_once_loop_and_pingpong_sample_the_right_time) {
    // Once: clamps at length, fires on_state_finished exactly once.
    {
        Scene scene("WrapOnce");
        auto* animator = add_animated_object(scene, "Obj");
        animator->add_state("once", linear_clip(WrapMode::Once, 1.0f, "position", 0.0f, 10.0f));
        animator->auto_play = "once";
        install_animation_system(scene);
        scene.start();

        int finished_count = 0;
        animator->on_state_finished.connect([&](const std::string&) { ++finished_count; });

        step(scene, 1.5f); // past length=1.0
        EXPECT_EQ(finished_count, 1);
        EXPECT_NEAR(position_of(scene, "Obj").x, 10.0f, 1e-4f); // holds the final pose

        step(scene, 0.2f); // still finished; must not re-fire
        EXPECT_EQ(finished_count, 1);
    }
    // Loop: wraps back to 0.
    {
        Scene scene("WrapLoop");
        auto* animator = add_animated_object(scene, "Obj");
        animator->add_state("loop", linear_clip(WrapMode::Loop, 1.0f, "position", 0.0f, 10.0f));
        animator->auto_play = "loop";
        install_animation_system(scene);
        scene.start();
        step(scene, 1.25f); // wraps to 0.25 -> x == 2.5
        EXPECT_NEAR(position_of(scene, "Obj").x, 2.5f, 1e-3f);
    }
    // PingPong: reflects back and forth between 0 and length.
    {
        Scene scene("WrapPingPong");
        auto* animator = add_animated_object(scene, "Obj");
        animator->add_state("pp", linear_clip(WrapMode::PingPong, 1.0f, "position", 0.0f, 10.0f));
        animator->auto_play = "pp";
        install_animation_system(scene);
        scene.start();
        step(scene, 1.25f); // period=2.0; raw t=1.25 -> reflected sample_time=0.75 -> x==7.5
        EXPECT_NEAR(position_of(scene, "Obj").x, 7.5f, 1e-3f);
    }
}

COOPA_TEST(crossfade_blends_by_weight_for_keyframed_and_procedural_states) {
    // From-state A holds x=0, target B holds x=10: halfway through a 1 s fade x==5, after it x==10.
    // Run with A keyframed and with A procedural -- both kinds must share the blend accumulator.
    for (bool procedural_a : {false, true}) {
        Scene scene(procedural_a ? "CrossfadeProcedural" : "CrossfadeKeyframed");
        auto* animator = add_animated_object(scene, "Obj");

        std::shared_ptr<AnimationClip> clip_a;
        if (procedural_a) {
            clip_a = std::make_shared<AnimationClip>();
            clip_a->wrap = WrapMode::Loop;
            clip_a->set_explicit_length(10.0f);
            AnimationTrack ta;
            ta.property = "position";
            ta.kind = TrackKind::Procedural;
            ta.procedural.type = "constant";
            ta.procedural.params.vectors["value"] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            clip_a->tracks.push_back(ta);
        } else {
            clip_a = linear_clip(WrapMode::Loop, 10.0f, "position", 0.0f, 0.0f);
        }
        animator->add_state("A", clip_a);
        animator->add_state("B", linear_clip(WrapMode::Loop, 10.0f, "position", 10.0f, 10.0f));
        animator->auto_play = "A";

        install_animation_system(scene);
        scene.start();
        animator->crossfade("B", 1.0f);

        step(scene, 0.5f); // 50% faded
        coopa::test::expect_near(position_of(scene, "Obj").x, 5.0f, 1e-3f,
                                 procedural_a ? "procedural A: half-way blend" : "keyframed A: half-way blend");
        step(scene, 0.6f); // past the full fade -> B only
        coopa::test::expect_near(position_of(scene, "Obj").x, 10.0f, 1e-3f,
                                 procedural_a ? "procedural A: fade completes" : "keyframed A: fade completes");
    }
}

COOPA_TEST(tracks_bind_by_descendant_path_or_scene_name_and_drop_unresolvable) {
    Scene scene("BindByNameAndPath");
    auto root = std::make_unique<SceneObject>("Root");
    root->add_component<TransformComponent>();
    auto* animator = root->add_component<Animator>();

    auto child = std::make_unique<SceneObject>("Child");
    child->add_component<TransformComponent>();
    auto grandchild = std::make_unique<SceneObject>("Grandchild");
    grandchild->add_component<TransformComponent>();
    SceneObject* grandchild_raw = child->add_child(std::move(grandchild));
    root->add_child(std::move(child));

    auto sibling = std::make_unique<SceneObject>("Sibling");
    sibling->add_component<TransformComponent>();
    scene.add_root_object(std::move(root));
    scene.add_root_object(std::move(sibling));

    auto clip = std::make_shared<AnimationClip>();
    clip->wrap = WrapMode::Once;
    clip->set_explicit_length(1.0f);
    // "Child/Grandchild" -- segment-walked descendant path.
    clip->tracks.push_back(linear_track("position", 1.0f, 1.0f, 1.0f, "Child/Grandchild"));
    // "Sibling" -- not a descendant, resolved via Scene::find_object.
    clip->tracks.push_back(linear_track("position", 1.0f, 2.0f, 2.0f, "Sibling"));
    // Unresolvable target -- must be dropped gracefully, not throw.
    clip->tracks.push_back(linear_track("position", 1.0f, 3.0f, 3.0f, "DoesNotExist"));

    animator->add_state("s", clip);
    animator->auto_play = "s";

    install_animation_system(scene);
    scene.start();
    step(scene, 0.1f);

    EXPECT_NEAR(grandchild_raw->get_transform()->transform().position().x, 1.0f, 1e-4f);
    EXPECT_NEAR(position_of(scene, "Sibling").x, 2.0f, 1e-4f);
    // Only 2 of the 3 tracks resolved to a binding; the unresolvable one was dropped, not thrown.
    EXPECT_EQ(animator->binding_count(), 2u);
}

COOPA_TEST(animator_on_initially_inactive_object_binds_lazily) {
    // SceneObject::start() returns early for inactive objects, so an Animator
    // on an object inactive at Scene::start() time never receives its own
    // start() -- advance_()'s "if (!bound_) rebind()" is the fallback that
    // must catch this once the object is reactivated.
    Scene scene("LazyBindInactive");
    auto obj = std::make_unique<SceneObject>("Obj", /*active=*/false);
    obj->add_component<TransformComponent>();
    auto* animator = obj->add_component<Animator>();
    scene.add_root_object(std::move(obj));
    animator->add_state("s", linear_clip(WrapMode::Once, 1.0f, "position", 0.0f, 10.0f));
    animator->auto_play = "s";

    install_animation_system(scene);
    scene.start(); // Obj is inactive -> Animator::start() never runs.
    EXPECT_EQ(animator->binding_count(), 0u);

    scene.find_object("Obj")->set_active(true);
    // Since start() never ran, auto_play never triggered play() either; the
    // system's cached animator list is still fresh from before the object was
    // activated, so this exercises the lazy rebind path specifically.
    animator->play("s");
    step(scene, 0.5f);

    EXPECT_GT(animator->binding_count(), 0u);
    EXPECT_NEAR(position_of(scene, "Obj").x, 5.0f, 1e-3f);
}

COOPA_TEST(rotation_tracks_interpolate_past_360_without_wrapping) {
    Scene scene("RotationNoWrap");
    auto* animator = add_animated_object(scene, "Obj");
    auto clip = std::make_shared<AnimationClip>();
    clip->wrap = WrapMode::Once;
    clip->set_explicit_length(1.0f);
    AnimationTrack track;
    track.property = "rotation";
    track.curve.add_key(Keyframe{0.0f, {0.0f, 0.0f, 0.0f, 1.0f}, Interpolation::Linear});
    track.curve.add_key(Keyframe{1.0f, {0.0f, 0.0f, 720.0f, 1.0f}, Interpolation::Linear}); // two full spins
    track.curve.sort_keys();
    clip->tracks.push_back(track);
    animator->add_state("s", clip);
    animator->auto_play = "s";

    install_animation_system(scene);
    scene.start();
    step(scene, 0.5f);

    // At the midpoint, z must be 360 (halfway through two full spins), NOT 0
    // (which a shortest-path wrap would incorrectly produce).
    EXPECT_NEAR(scene.find_object("Obj")->get_transform()->transform().rotation_degrees().z, 360.0f, 1e-2f);
}

COOPA_TEST(parallel_evaluation_matches_serial) {
    constexpr int kAnimatorCount = 64;
    auto build_scene = [](Scene& scene) {
        for (int i = 0; i < kAnimatorCount; ++i) {
            auto* animator = add_animated_object(scene, "Obj" + std::to_string(i));
            animator->add_state("s", linear_clip(WrapMode::Loop, 2.0f, "position",
                                                 static_cast<float>(i), static_cast<float>(i) * 2.0f));
            animator->auto_play = "s";
        }
    };

    Scene serial_scene("Serial");
    build_scene(serial_scene);
    install_animation_system(serial_scene)->set_parallel_threshold(SIZE_MAX); // force serial
    serial_scene.start();
    step(serial_scene, 0.37f);

    coopa::job::JobEngine engine(4);
    Scene parallel_scene("Parallel");
    build_scene(parallel_scene);
    parallel_scene.set_job_engine(&engine);
    AnimationSystem* parallel_sys = install_animation_system(parallel_scene);
    parallel_sys->set_parallel_threshold(0); // force parallel
    parallel_sys->set_chunk_size(3);
    parallel_scene.start();
    step(parallel_scene, 0.37f);

    int mismatches = 0;
    for (int i = 0; i < kAnimatorCount; ++i) {
        const std::string name = "Obj" + std::to_string(i);
        mismatches += std::abs(position_of(serial_scene, name).x - position_of(parallel_scene, name).x) >= 1e-6f;
    }
    EXPECT_EQ(mismatches, 0);
    engine.shutdown();
}

COOPA_TEST(procedural_orbit_matches_its_closed_form) {
    Scene scene("ProceduralOrbit");
    auto* animator = add_animated_object(scene, "Obj");
    auto clip = std::make_shared<AnimationClip>();
    clip->wrap = WrapMode::Loop;
    clip->set_explicit_length(6.283185307f);
    AnimationTrack track;
    track.property = "position";
    track.kind = TrackKind::Procedural;
    track.procedural.type = "orbit";
    track.procedural.params.vectors["center"] = glm::vec4(-1.5f, 0.0f, 0.0f, 1.0f);
    track.procedural.params.scalars["radius"] = 2.7f;
    track.procedural.params.scalars["speed"] = 1.0f;
    track.procedural.params.scalars["height"] = 1.0f;
    track.procedural.params.scalars["initial_angle"] = 0.0f;
    clip->tracks.push_back(track);
    animator->add_state("orbit", clip);
    animator->auto_play = "orbit";

    install_animation_system(scene);
    scene.start();

    float accumulated = 0.0f;
    for (float t : {0.0f, 0.5f, 1.0f, 1.5707963f, 3.14159265f, 6.0f}) {
        step(scene, t - accumulated);
        accumulated = t;
        // The closed-form orbit the procedural evaluator must reproduce:
        // centre + radius * {cos, sin}(speed * t + phase), with a fixed z.
        const glm::vec3 expected(-1.5f + 2.7f * std::cos(t), 2.7f * std::sin(t), 1.0f);
        EXPECT_VEC_NEAR(position_of(scene, "Obj"), expected, 1e-4f);
    }
}

// An Animator built into a running scene after the AnimationSystem's first gather (a runtime
// spawn) animates without refresh(), and a destroyed one is dropped without being touched.
COOPA_TEST(runtime_spawned_animators_join_and_destroyed_ones_are_dropped) {
    auto make_rig = [](const std::string& name) {
        auto obj = std::make_unique<SceneObject>(name);
        obj->add_component<TransformComponent>();
        auto* animator = obj->add_component<Animator>();
        animator->add_state("move", linear_clip(WrapMode::Loop, 1.0f, "position", 0.0f, 10.0f));
        animator->auto_play = "move";
        return obj;
    };

    Scene scene("Spawned");
    scene.add_root_object(make_rig("First"));
    AnimationSystem* sys = install_animation_system(scene);
    scene.start();
    step(scene, 0.25f);
    EXPECT_EQ(sys->animator_count(), 1u);

    // Spawned the way SceneLoader::spawn() does it: parented, adopted, started.
    SceneObject* spawned = scene.add_root_object(make_rig("Spawned"));
    scene.adopt(*spawned);
    spawned->start();
    step(scene, 0.5f);
    EXPECT_EQ(sys->animator_count(), 2u);
    EXPECT_NEAR(position_of(scene, "Spawned").x, 5.0f, 1e-4f);

    // Destroyed: dropped on the next frame (a stale pointer here would be a use-after-free).
    ASSERT_TRUE(scene.remove_root_object(scene.find_object("First")));
    step(scene, 0.25f);
    EXPECT_EQ(sys->animator_count(), 1u);
    EXPECT_NEAR(position_of(scene, "Spawned").x, 7.5f, 1e-4f);
}

COOPA_TEST(scene_yaml_animator_loads_its_clip_and_plays) {
    coopa::asset::AssetManager assets;
    libcoopa_test::write_scratch_file("clip.yaml",
        "clip:\n"
        "  name: yaml_clip\n"
        "  wrap: once\n"
        "  tracks:\n"
        "    - object: \"\"\n"
        "      component: Transform\n"
        "      property: position\n"
        "      keys:\n"
        "        - { time: 0.0, value: { x: 0, y: 0, z: 0 } }\n"
        "        - { time: 1.0, value: { x: 10, y: 0, z: 0 } }\n");
    const std::string scene_path = libcoopa_test::write_scratch_file("scene.yaml",
        "scene:\n"
        "  scene_name: YamlAnimatorScene\n"
        "  root_objects:\n"
        "    - name: Obj\n"
        "      components:\n"
        "        - type: Animator\n"
        "          auto_play: move\n"
        "          states:\n"
        "            - name: move\n"
        "              clip: clip.yaml\n");

    assets.add_search_root(coopa::test::scratch_dir().string());
    register_animation_components(assets);
    // The registered "Animator" parser's closure captures `assets` by reference -- clear the
    // registry before `assets` is destroyed (declared after it, so it runs first, including when
    // a fatal check ends the test early), mirroring every other AssetManager-backed parser
    // registration in this codebase.
    struct ClearParsersOnExit {
        ~ClearParsersOnExit() { SceneLoader::clear_component_parsers(); }
    } clear_parsers_on_exit;

    {
        // Scoped so `scene` (whose Animator holds an AssetHandle<AnimationClip>
        // bound to `assets`) is destroyed BEFORE assets.shutdown() runs below
        // -- shutdown() destroys every AssetSlot (see its doc), and that
        // handle's destructor dereferences the slot unconditionally.
        Scene scene = SceneLoader::load(scene_path);
        install_animation_system(scene);

        auto* obj = scene.find_object("Obj");
        ASSERT_TRUE(obj != nullptr);
        auto* animator = obj->get_component<Animator>();
        ASSERT_TRUE(animator != nullptr);

        // The clip loads fast in practice, but poll briefly rather than assuming a fixed
        // number of frames completes it against the asset IO thread.
        for (int i = 0; i < 200 && animator->binding_count() == 0; ++i) {
            assets.update(0.016f);
            step(scene, 0.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(animator->current_state(), std::string("move"));
        ASSERT_GT(animator->binding_count(), 0u);

        step(scene, 0.5f);
        EXPECT_NEAR(obj->get_transform()->transform().position().x, 5.0f, 1e-2f);
    }
}

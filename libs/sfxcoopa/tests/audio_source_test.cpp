/**
 * @file audio_source_test.cpp
 * @brief components/audio_source.h + sfx_yaml.h: AudioSource deserialization (map and tag
 *        forms), play_on_start, bus routing and its Master fallback, missing/unsupported clips
 *        degrading without throwing, voice lifetime tied to the owning object, on_finished,
 *        and clip resolution through SfxResources search roots with the sidecar volume applied.
 *        Spatial behaviour and AudioListener live in audio_listener_test.cpp.
 */
#include <coopa/testing/test.h>

#include "support/scene_audio.h"

#include <cmath>

COOPA_TEST_SUITE("audio_source");

using namespace sfx_test;

COOPA_TEST(parses_yaml_and_plays_on_start) {
    AudioEngine engine(AudioFormat{48000, 2});
    engine.create_bus("SFX", engine.master());
    ResourcesScope scope(engine);

    const std::string clip = fixture_copy(k_whoosh);
    coopa::scene::Scene scene = load_scene(source_scene_yaml(clip,
        "          bus: SFX\n"
        "          volume: 0.5\n"
        "          pitch: 1\n"
        "          loop: true\n"
        "          play_on_start: true\n"
        "          priority: 3\n"
        "          min_distance: 2.0\n"
        "          max_distance: 30.0\n"
        "          curve: Linear\n"));

    auto* src = scene.find_first_component<AudioSource>();
    ASSERT_TRUE(src != nullptr);
    EXPECT_TRUE(src->clip_path == clip);
    EXPECT_TRUE(src->bus == "SFX");
    EXPECT_NEAR(src->volume, 0.5f, 1e-6f);
    EXPECT_NEAR(src->pitch, 1.0f, 1e-6f);
    EXPECT_TRUE(src->loop);
    EXPECT_TRUE(src->play_on_start);
    EXPECT_FALSE(src->spatialize);
    EXPECT_EQ(src->priority, 3);
    EXPECT_NEAR(src->spatial.attenuation.min_distance, 2.0f, 1e-6f);
    EXPECT_NEAR(src->spatial.attenuation.max_distance, 30.0f, 1e-6f);
    EXPECT_TRUE(src->spatial.attenuation.mode == coopa::sfx::spatial::RolloffMode::Linear);
    EXPECT_TRUE(src->type_name() == "AudioSource");

    // The first frame (run by load_scene) fired play_on_start.
    ASSERT_TRUE(src->is_playing());
    EXPECT_GT(render_rms(engine, 4096), 1e-3f);

    src->stop();
    EXPECT_FALSE(src->is_playing());
}

COOPA_TEST(tag_form_parses_with_default_bus) {
    AudioEngine engine(AudioFormat{48000, 2});
    ResourcesScope scope(engine);
    const std::string clip = fixture_copy(k_whoosh);
    coopa::scene::Scene scene = load_scene(
        "scene:\n"
        "  root_objects:\n"
        "    - name: Speaker\n"
        "      components:\n"
        "        - !AudioSource { clip: \"" + clip + "\", volume: 0.25 }\n");
    auto* src = scene.find_first_component<AudioSource>();
    ASSERT_TRUE(src != nullptr);
    EXPECT_NEAR(src->volume, 0.25f, 1e-6f);
    EXPECT_TRUE(src->bus == "SFX");  // default
}

COOPA_TEST(routes_through_its_bus) {
    AudioEngine engine(AudioFormat{48000, 2});
    auto& sfx_bus = engine.create_bus("SFX", engine.master());
    sfx_bus.set_volume(0.0f);
    ResourcesScope scope(engine);

    coopa::scene::Scene scene = load_scene(source_scene_yaml(fixture_copy(k_whoosh),
        "          play_on_start: true\n"));
    auto* src = scene.find_first_component<AudioSource>();
    ASSERT_TRUE(src->is_playing());
    EXPECT_LT(render_rms(engine, 4096), 1e-7f);  // its bus is at zero: silent

    sfx_bus.set_volume(1.0f);
    EXPECT_GT(render_rms(engine, 4096), 1e-3f);
}

COOPA_TEST(unknown_bus_falls_back_to_master) {
    AudioEngine engine(AudioFormat{48000, 2});  // no SFX bus at all
    ResourcesScope scope(engine);
    coopa::scene::Scene scene = load_scene(source_scene_yaml(fixture_copy(k_whoosh),
        "          bus: NoSuchBus\n"
        "          play_on_start: true\n"));
    EXPECT_GT(render_rms(engine, 4096), 1e-3f);
}

COOPA_TEST(missing_clip_stays_silent_without_throwing) {
    AudioEngine engine(AudioFormat{48000, 2});
    ResourcesScope scope(engine);
    coopa::scene::Scene scene = load_scene(source_scene_yaml("does/not/exist.wav", "          play_on_start: true\n"));
    auto* src = scene.find_first_component<AudioSource>();
    EXPECT_FALSE(src->is_playing());
    EXPECT_FALSE(src->play());
    scene.update(0.016f);
    EXPECT_LT(render_rms(engine, 1024), 1e-7f);
}

COOPA_TEST(stereo_clip_with_spatialize_falls_back_to_2d) {
    // AudioClip::load throws for a spatialized stereo clip; the component must degrade to 2D
    // playback instead of failing the scene.
    AudioEngine engine(AudioFormat{48000, 2});
    ResourcesScope scope(engine);
    coopa::scene::Scene scene = load_scene(source_scene_yaml(fixture_copy(k_whoosh),
        "          spatialize: true\n"
        "          play_on_start: true\n", 3.0f));
    auto* src = scene.find_first_component<AudioSource>();
    EXPECT_TRUE(src->is_playing());
    EXPECT_GT(render_rms(engine, 4096), 1e-3f);
}

COOPA_TEST(destroying_the_object_stops_its_voice) {
    AudioEngine engine(AudioFormat{48000, 2});
    ResourcesScope scope(engine);
    coopa::scene::Scene scene = load_scene(source_scene_yaml(fixture_copy(k_whoosh),
        "          loop: true\n"
        "          play_on_start: true\n"));
    auto* src = scene.find_first_component<AudioSource>();
    const coopa::sfx::core::VoiceHandle h = src->voice();
    ASSERT_TRUE(engine.is_voice_active(h));
    EXPECT_GT(render_rms(engine, 2048), 1e-3f);

    ASSERT_TRUE(scene.remove_root_object(scene.find_object("Speaker")));
    render_rms(engine, 2048);  // stop fade (~5 ms) completes inside this block
    EXPECT_LT(render_rms(engine, 2048), 1e-7f);
    engine.update(0.016f);
    EXPECT_FALSE(engine.is_voice_active(h));
}

COOPA_TEST(on_finished_fires_once) {
    AudioEngine engine(AudioFormat{48000, 2});
    ResourcesScope scope(engine);
    coopa::scene::Scene scene = load_scene(source_scene_yaml(fixture_copy(k_whoosh),
        "          play_on_start: true\n"));
    auto* src = scene.find_first_component<AudioSource>();
    int finished = 0;
    auto conn = src->on_finished.connect([&]() { ++finished; });

    // 31903 frames @44.1k ~= 34724 @48k, plus the end fade.
    for (int i = 0; i < 9; ++i) render_rms(engine, 4096);
    engine.update(0.016f);
    scene.update(0.016f);
    EXPECT_EQ(finished, 1);
    EXPECT_FALSE(src->is_playing());
    engine.update(0.016f);
    scene.update(0.016f);
    EXPECT_EQ(finished, 1);
    (void)conn;
}

COOPA_TEST(clip_resolves_through_search_roots_and_applies_sidecar_volume) {
    const std::string mono = mono_whoosh_copy("half_volume.wav", 0.5f);
    const std::string root = std::filesystem::path(mono).parent_path().string();

    AudioEngine engine(AudioFormat{48000, 2});
    ResourcesScope scope(engine);
    SfxResources::instance().add_search_root(root);
    EXPECT_TRUE(SfxResources::instance().resolve("half_volume.wav") == mono);
    EXPECT_TRUE(SfxResources::instance().resolve(mono) == mono);  // absolute: as-is

    // Scene file lives somewhere the clip isn't, so only the search root can find it.
    coopa::scene::Scene scene = load_scene(
        source_scene_yaml("half_volume.wav", "          bus: Master\n          play_on_start: true\n"),
        (coopa::test::scratch_dir("elsewhere") / "scene.yaml").string());
    std::vector<float> got(2048 * 2, 0.0f);
    engine.render_offline(got.data(), 2048);

    // Reference: same clip via the engine directly at gain 0.5 (AudioEngine::play ignores sidecar volume).
    AudioEngine reference(AudioFormat{48000, 2});
    coopa::sfx::core::PlayParams params;
    params.gain = 0.5f;
    reference.play(mono, params);
    std::vector<float> want(2048 * 2, 0.0f);
    reference.render_offline(want.data(), 2048);

    float peak = 0.0f;
    for (size_t i = 0; i < got.size(); ++i) {
        ASSERT_NEAR(got[i], want[i], 1e-6f);
        peak = std::max(peak, std::fabs(want[i]));
    }
    EXPECT_GT(peak, 1e-3f);
}

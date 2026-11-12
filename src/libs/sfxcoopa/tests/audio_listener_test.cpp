/**
 * @file audio_listener_test.cpp
 * @brief components/audio_listener.h and spatialized AudioSources: left/right handedness of a
 *        spatialized source against the default listener and against a moved AudioListener,
 *        the listener's once-per-frame push, and listener_from_matrix() agreeing with
 *        Listener::set_from_transform().
 */
#include <coopa/testing/test.h>

#include "support/scene_audio.h"

#include <sfxcoopa/spatial/listener.h>

#include <coopa/util/transform.h>

COOPA_TEST_SUITE("audio_listener");

using namespace sfx_test;

COOPA_TEST(spatialized_source_to_the_right_is_louder_right) {
    AudioEngine engine(AudioFormat{48000, 2});
    engine.create_bus("SFX", engine.master());
    ResourcesScope scope(engine);
    const std::string clip = mono_whoosh_copy();

    coopa::scene::Scene scene = load_scene(source_scene_yaml(clip,
        "          spatialize: true\n"
        "          play_on_start: true\n"
        "          loop: true\n"
        "          min_distance: 1.0\n"
        "          max_distance: 50.0\n", 3.0f));
    scene.update(0.016f);
    engine.update(0.016f);

    float l = 0.0f, r = 0.0f;
    render_rms(engine, 4096, l, r);
    render_rms(engine, 4096, l, r);  // second block: past the initial ramp/delay fill
    EXPECT_GT(r, 1e-4f);
    EXPECT_GT(r, l * 1.5f);
}

COOPA_TEST(moved_listener_flips_which_ear_hears_the_source) {
    AudioEngine engine(AudioFormat{48000, 2});
    ResourcesScope scope(engine);
    const std::string clip = mono_whoosh_copy();

    // Source at x=+3, listener at x=+6 (facing -Z): the source is now on the listener's LEFT.
    coopa::scene::Scene scene = load_scene(
        "scene:\n"
        "  root_objects:\n"
        "    - name: Ears\n"
        "      components:\n"
        "        - type: Transform\n"
        "          position: { x: 6.0, y: 0.0, z: 0.0 }\n"
        "        - type: AudioListener\n"
        "    - name: Speaker\n"
        "      components:\n"
        "        - type: Transform\n"
        "          position: { x: 3.0, y: 0.0, z: 0.0 }\n"
        "        - type: AudioSource\n"
        "          clip: \"" + clip + "\"\n"
        "          bus: Master\n"
        "          spatialize: true\n"
        "          loop: true\n"
        "          play_on_start: true\n");
    EXPECT_EQ(SfxResources::instance().listener_count(), 1);
    // load_scene() already ran the first frame, whose push this consumes.
    EXPECT_TRUE(SfxResources::instance().consume_listener_pushed());
    EXPECT_FALSE(SfxResources::instance().consume_listener_pushed());
    scene.update(0.016f);
    EXPECT_TRUE(SfxResources::instance().consume_listener_pushed());
    EXPECT_FALSE(SfxResources::instance().consume_listener_pushed());  // cleared by the previous call
    engine.update(0.016f);

    float l = 0.0f, r = 0.0f;
    render_rms(engine, 4096, l, r);
    render_rms(engine, 4096, l, r);
    EXPECT_GT(l, r * 1.5f);
}

COOPA_TEST(listener_from_matrix_matches_set_from_transform) {
    coopa::util::Transform t;
    t.set_position(glm::vec3(1.0f, 2.0f, 3.0f));
    t.set_rotation(glm::vec3(0.0f, 90.0f, 0.0f));
    coopa::sfx::spatial::Listener expected;
    expected.set_from_transform(t);
    coopa::sfx::spatial::Listener got = coopa::sfx::components::listener_from_matrix(t.get_world_matrix());
    EXPECT_VEC_NEAR(got.position, expected.position, 1e-5f);
    EXPECT_VEC_NEAR(got.forward, expected.forward, 1e-5f);
    EXPECT_VEC_NEAR(got.up, expected.up, 1e-5f);
}

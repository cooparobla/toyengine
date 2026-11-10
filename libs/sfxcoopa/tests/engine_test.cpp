/**
 * @file engine_test.cpp
 * @brief core/engine.h: AudioEngine end to end through render_offline() -- a played clip
 *        reproduces its samples, on_voice_finished fires exactly once, a clip outlives the
 *        caller's reference while its voice fades, and two plays issued before any render
 *        get distinct voices.
 */
#include <coopa/testing/test.h>

#include "support/clip_fixtures.h"

#include <sfxcoopa/core/engine.h>
#include <sfxcoopa/data/audio_format.h>

#include <algorithm>
#include <vector>

COOPA_TEST_SUITE("engine");

using namespace coopa::sfx::core;
using coopa::sfx::data::AudioFormat;

COOPA_TEST(render_offline_reproduces_the_clip_samples) {
    AudioEngine engine(AudioFormat{44100, 2});
    const std::string path = sfx_test::fixture_copy(sfx_test::k_whoosh);
    auto clip = engine.load_clip(path);
    engine.play(path);

    const uint64_t frames = clip->frame_count();
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    uint64_t rendered = 0;
    while (rendered < frames) {
        uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(4096, frames - rendered));
        engine.render_offline(out.data() + static_cast<size_t>(rendered) * 2, chunk);
        rendered += chunk;
    }

    for (uint64_t i = 0; i < frames * 2; ++i) {
        ASSERT_NEAR(out[static_cast<size_t>(i)], clip->data()[i], 1e-5f);
    }
}

COOPA_TEST(on_voice_finished_fires_exactly_once) {
    AudioEngine engine(AudioFormat{44100, 2});
    const std::string path = sfx_test::fixture_copy(sfx_test::k_whoosh);
    auto clip = engine.load_clip(path);
    engine.play(path);

    const uint64_t total_needed = clip->frame_count() + 300;  // + fade margin
    std::vector<float> out(4096 * 2, 0.0f);
    uint64_t rendered = 0;
    while (rendered < total_needed) {
        uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(4096, total_needed - rendered));
        engine.render_offline(out.data(), chunk);
        rendered += chunk;
    }

    int finished_count = 0;
    auto conn = engine.on_voice_finished.connect([&](VoiceHandle) { ++finished_count; });
    engine.update(0.016f);
    EXPECT_EQ(finished_count, 1);
    engine.update(0.016f);  // draining again must not re-fire for the same, already-handled event
    EXPECT_EQ(finished_count, 1);
    (void)conn;
}

COOPA_TEST(clip_outlives_a_dropped_local_ref_while_its_voice_fades) {
    // Lifetime regression: the engine's cache + the per-voice ref must keep the clip's samples
    // alive through the stop fade and the reclaim, or the audio thread reads freed memory.
    AudioEngine engine(AudioFormat{44100, 2});
    const std::string path = sfx_test::fixture_copy(sfx_test::k_whoosh);
    auto clip = engine.load_clip(path);
    VoiceHandle h = engine.play(path);
    clip.reset();  // drop our own local reference
    engine.stop(h);

    std::vector<float> out(64 * 2, 0.0f);
    for (int i = 0; i < 10; ++i) engine.render_offline(out.data(), 64);  // must not crash mid-fade
    engine.update(0.016f);                                              // reclaims + releases the per-voice ref
    for (int i = 0; i < 10; ++i) engine.render_offline(out.data(), 64);  // voice now Free: must still not crash
    EXPECT_FALSE(engine.is_voice_active(h));
}

COOPA_TEST(two_plays_before_a_render_use_distinct_voices_and_sum) {
    // Regression: play(); update(dt) (no render in between); play() must not reuse the first
    // voice's slot.
    AudioEngine engine(AudioFormat{48000, 2});
    const std::string path = sfx_test::fixture_copy(sfx_test::k_whoosh);
    auto clip = engine.load_clip(path);
    auto h1 = engine.play(path);
    engine.update(0.016f);
    auto h2 = engine.play(path);
    EXPECT_NE(h1.index, h2.index);
    std::vector<float> out(128, 0.0f);
    engine.render_offline(out.data(), 64);
    for (int i = 0; i < 128; ++i) {
        EXPECT_NEAR(out[static_cast<size_t>(i)], clip->data()[static_cast<size_t>(i)] * 2.0f, 1e-4f);
    }
}

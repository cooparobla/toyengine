/**
 * @file voice_test.cpp
 * @brief mixer/voice.h: VoicePool stealing and generation-checked handles (a stale handle must
 *        never alias a reused slot), sample-exact loop wrapping, click-free stop fades, zipper-
 *        free spatial motion, and doppler pitch applying only while spatialized.
 */
#include <coopa/testing/test.h>

#include <sfxcoopa/data/audio_format.h>
#include <sfxcoopa/mixer/voice.h>
#include <sfxcoopa/spatial/listener.h>
#include <sfxcoopa/spatial/panner.h>

#include <algorithm>
#include <cmath>
#include <vector>

COOPA_TEST_SUITE("voice");

using namespace coopa::sfx::mixer;

COOPA_TEST(full_pool_steals_the_lowest_priority_oldest_voice) {
    VoicePool pool(2, 48000);
    VoiceHandle h1 = pool.allocate(0, 0);   // priority 0, started_at 0 (oldest)
    VoiceHandle h2 = pool.allocate(0, 1);   // priority 0, started_at 1
    ASSERT_TRUE(pool.is_valid(h1));
    ASSERT_TRUE(pool.is_valid(h2));

    VoiceHandle h3 = pool.allocate(0, 2);  // pool full: must steal, not throw
    ASSERT_TRUE(h3.is_valid());
    EXPECT_TRUE(pool.is_valid(h3));
    EXPECT_EQ(h3.index, h1.index);     // tie-broken by oldest started_at: h1's slot was stolen
    EXPECT_FALSE(pool.is_valid(h1));   // h1 is now stale
    EXPECT_TRUE(pool.is_valid(h2));    // h2 untouched
}

COOPA_TEST(released_handle_never_aliases_the_reallocated_slot) {
    VoicePool pool(1, 48000);
    VoiceHandle h1 = pool.allocate(0, 0);
    Voice* v = pool.get(h1);
    ASSERT_TRUE(v != nullptr);

    std::vector<float> tiny_clip(4, 1.0f);
    SourceRef src{tiny_clip.data(), tiny_clip.size(), 1};
    v->play(src, 0, 1.0f, 1.0f, false, 0, 0);
    v->stop();
    std::vector<float> scratch(1000, 0.0f);
    v->render_add(scratch.data(), 1000, 1);
    ASSERT_TRUE(v->state() == VoiceState::Free);

    pool.reclaim_finished();
    EXPECT_FALSE(pool.is_valid(h1));  // released: stale immediately, even before reallocation

    VoiceHandle h2 = pool.allocate(1, 100);
    EXPECT_EQ(h2.index, h1.index);          // same physical slot, reused
    EXPECT_NE(h2.generation, h1.generation);
    EXPECT_FALSE(pool.is_valid(h1));  // still a no-op: stale handle never aliases the new voice
    EXPECT_TRUE(pool.is_valid(h2));
}

COOPA_TEST(loop_wraps_at_the_exact_frame) {
    std::vector<float> data = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    SourceRef src{data.data(), data.size(), 1};
    Voice voice(48000);
    voice.play(src, 0, 1.0f, 1.0f, /*loop=*/true, /*loop_start_frame=*/2, /*loop_end_frame=*/8);

    std::vector<float> out(10, 0.0f);
    for (int i = 0; i < 10; ++i) {
        voice.render_add(&out[static_cast<size_t>(i)], 1, 1);
    }
    // Frames 0..7 play through once; frame 7's advance hits loop_end (8)
    // exactly and wraps to loop_start (2) with zero drift.
    float expected[10] = {0, 1, 2, 3, 4, 5, 6, 7, 2, 3};
    for (int i = 0; i < 10; ++i) EXPECT_NEAR(out[static_cast<size_t>(i)], expected[i], 1e-5f);
}

COOPA_TEST(stop_fades_out_without_a_click) {
    std::vector<float> data(2000, 1.0f);  // constant amplitude: any cut shows as one huge delta
    SourceRef src{data.data(), data.size(), 1};
    Voice voice(48000);
    voice.play(src, 0, 1.0f, 1.0f, false, 0, 0);
    voice.stop();

    std::vector<float> scratch(500, 0.0f);
    voice.render_add(scratch.data(), 500, 1);

    const uint32_t fade_frames = std::max<uint32_t>(1, 48000 * 5 / 1000);
    const float max_expected_delta = (1.0f / static_cast<float>(fade_frames)) * 1.5f;  // slack for the ramp-up start
    float max_delta = 0.0f;
    for (size_t i = 1; i < scratch.size(); ++i) {
        max_delta = std::max(max_delta, std::fabs(scratch[i] - scratch[i - 1]));
    }
    EXPECT_LE(max_delta, max_expected_delta);
    EXPECT_NEAR(scratch[0], 1.0f, 0.05f);            // starts near full gain
    EXPECT_NEAR(scratch[fade_frames], 0.0f, 1e-4f);  // fully faded by the documented ~5ms window
}

COOPA_TEST(orbiting_source_has_no_zipper_noise) {
    using namespace coopa::sfx::spatial;
    const uint32_t sample_rate = 48000;
    const uint32_t n = sample_rate * 2;  // 2 seconds
    std::vector<float> sine(n);
    for (uint32_t i = 0; i < n; ++i) {
        sine[i] = std::sin(2.0f * 3.14159265358979323846f * 1000.0f * static_cast<float>(i) / sample_rate);
    }
    SourceRef src{sine.data(), n, 1};
    Voice voice(sample_rate);
    voice.play(src, 0, 1.0f, 1.0f, false, 0, 0);
    voice.set_spatialize(true);  // play() resets spatialization

    BinauralPanner reference_panner(sample_rate);
    Listener listener;
    SpatialParams params;
    params.attenuation.min_distance = 100.0f;
    params.attenuation.max_distance = 1000.0f;  // keep distance gain ~constant; isolate ITD/ILD motion
    const float radius = 5.0f;

    std::vector<float> stereo(static_cast<size_t>(n) * 2);
    uint32_t rendered = 0;
    const uint32_t block = coopa::sfx::data::k_control_block_frames;
    while (rendered < n) {
        const float t = static_cast<float>(rendered) / sample_rate;
        const float angle = t * 3.14159265358979323846f;  // full 360 deg orbit over 2s
        params.position = glm::vec3(radius * std::sin(angle), 0.0f, -radius * std::cos(angle));
        voice.set_spatial_target(reference_panner.compute(params, listener, sample_rate));

        uint32_t n_this = std::min(block, n - rendered);
        voice.render_add(stereo.data() + static_cast<size_t>(rendered) * 2, n_this, 2);
        rendered += n_this;
    }

    // A pure 1kHz tone's own per-sample slope is ~0.131; a real zipper bug
    // (an unramped instantaneous gain/delay jump) would spike far above
    // that. This bound is generous enough to tolerate slow envelope
    // modulation from the orbit while still catching a genuine pop.
    float max_delta = 0.0f;
    for (size_t i = 2; i < stereo.size(); ++i) {
        max_delta = std::max(max_delta, std::fabs(stereo[i] - stereo[i - 2]));  // per-channel: stride 2
    }
    EXPECT_LT(max_delta, 0.5f);
}

COOPA_TEST(doppler_pitch_applies_only_while_spatialized) {
    using namespace coopa::sfx::spatial;
    const uint32_t sample_rate = 48000;
    const uint32_t n = 4800;
    std::vector<float> ones(n, 1.0f);
    SourceRef src{ones.data(), n, 1};
    const uint32_t block = coopa::sfx::data::k_control_block_frames;

    // Renders until the voice frees itself and returns how many frames that took.
    auto frames_to_finish = [&](bool spatialize, float pitch_scale) {
        Voice voice(sample_rate);
        voice.play(src, 0, 1.0f, 1.0f, false, 0, 0);
        voice.set_spatialize(spatialize);  // play() resets spatialization
        EarParams target;
        target.pitch_scale = pitch_scale;
        voice.set_spatial_target(target);
        std::vector<float> scratch(static_cast<size_t>(block) * 2);
        uint32_t rendered = 0;
        while (voice.state() != VoiceState::Free && rendered < n * 4) {
            std::fill(scratch.begin(), scratch.end(), 0.0f);
            voice.render_add(scratch.data(), block, 2);
            rendered += block;
        }
        return rendered;
    };

    const uint32_t base = frames_to_finish(true, 1.0f);
    const uint32_t fast = frames_to_finish(true, 2.0f);
    const uint32_t unpanned = frames_to_finish(false, 2.0f);
    EXPECT_LT(fast, base * 3 / 4);   // approaching source: clip plays out faster
    EXPECT_EQ(unpanned, base);       // doppler only applies while the panner is active
}

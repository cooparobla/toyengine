/**
 * @file audio_clip_test.cpp
 * @brief data/audio_clip.h: fully-decoded clips -- bit-exact at the engine's own rate, length
 *        after the dsp/resampler.h path to 48 kHz, the force_mono (L+R)/2 downmix, and the
 *        "spatialized clips must be mono" contract.
 */
#include <coopa/testing/test.h>

#include "support/clip_fixtures.h"

#include <sfxcoopa/data/audio_clip.h>
#include <sfxcoopa/data/audio_format.h>
#include <sfxcoopa/data/import_settings.h>

#include <cstring>
#include <stdexcept>
#include <string>

COOPA_TEST_SUITE("audio_clip");

using namespace coopa::sfx::data;

COOPA_TEST(clip_is_bit_exact_at_matching_engine_rate) {
    const std::string path = sfx_test::fixture_copy(sfx_test::k_whoosh);
    AudioClip clip = AudioClip::load(path, AudioFormat{44100, 2});
    ASSERT_EQ(clip.frame_count(), sfx_test::k_whoosh_frames);
    ASSERT_EQ(clip.format().channels, 2u);

    std::vector<uint8_t> raw = sfx_test::read_wav_data_chunk_raw(path);
    ASSERT_EQ(raw.size(), clip.frame_count() * clip.format().channels * sizeof(float));
    EXPECT_EQ(std::memcmp(raw.data(), clip.data(), raw.size()), 0);
}

COOPA_TEST(clip_resamples_to_48khz_within_one_frame) {
    AudioClip clip = AudioClip::load(sfx_test::fixture_copy(sfx_test::k_whoosh), AudioFormat{48000, 2});
    double expected = double(sfx_test::k_whoosh_frames) * 48000.0 / 44100.0;
    EXPECT_NEAR(static_cast<float>(clip.frame_count()), static_cast<float>(expected), 1.0f);
}

COOPA_TEST(force_mono_averages_the_two_channels) {
    const std::string path = sfx_test::fixture_copy(sfx_test::k_whoosh);
    ClipImportSettings stereo_settings;
    ClipImportSettings mono_settings;
    mono_settings.force_mono = true;

    AudioClip stereo_clip = AudioClip::load(path, AudioFormat{44100, 2}, stereo_settings);
    AudioClip mono_clip = AudioClip::load(path, AudioFormat{44100, 2}, mono_settings);

    ASSERT_EQ(mono_clip.format().channels, 1u);
    EXPECT_EQ(mono_clip.frame_count(), stereo_clip.frame_count());  // same frame count, half the floats
    // (L+R)*0.5 downmix: check a handful of frames directly.
    for (uint64_t i = 0; i < 10; ++i) {
        float l = stereo_clip.data()[i * 2];
        float r = stereo_clip.data()[i * 2 + 1];
        EXPECT_NEAR(mono_clip.data()[i], (l + r) * 0.5f, 1e-6f);
    }
}

COOPA_TEST(spatialize_requires_force_mono_for_a_stereo_source) {
    const std::string path = sfx_test::fixture_copy(sfx_test::k_whoosh);
    ClipImportSettings settings;
    settings.spatialize = true;  // stereo source, force_mono left false: forbidden
    bool threw = false;
    try {
        AudioClip::load(path, AudioFormat{44100, 2}, settings);
    } catch (const std::runtime_error& e) {
        threw = true;
        // The message must point the user at the fix.
        EXPECT_TRUE(std::string(e.what()).find("force_mono") != std::string::npos);
    }
    EXPECT_TRUE(threw);

    settings.force_mono = true;
    AudioClip clip = AudioClip::load(path, AudioFormat{44100, 2}, settings);  // must not throw
    EXPECT_EQ(clip.format().channels, 1u);
}

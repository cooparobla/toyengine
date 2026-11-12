/**
 * @file decoder_test.cpp
 * @brief data/decoder.h: bit-exact wav decode at the source rate (with the reported source
 *        format and length), length after miniaudio's in-decoder resample to 48 kHz, and an
 *        mp3 decode that matches the wav's waveform.
 */
#include <coopa/testing/test.h>

#include "support/clip_fixtures.h"

#include <sfxcoopa/data/audio_format.h>
#include <sfxcoopa/data/decoder.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

COOPA_TEST_SUITE("decoder");

using namespace coopa::sfx::data;

namespace {
float normalized_cross_correlation_peak(const std::vector<float>& a, const std::vector<float>& b, int max_lag) {
    double best = 0.0;
    double norm_a = 0.0, norm_b = 0.0;
    for (float v : a) norm_a += static_cast<double>(v) * v;
    for (float v : b) norm_b += static_cast<double>(v) * v;
    double denom = std::sqrt(norm_a * norm_b);
    if (denom < 1e-9) return 0.0f;
    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        double score = 0.0;
        for (size_t i = 0; i < a.size(); ++i) {
            long j = static_cast<long>(i) + lag;
            if (j < 0 || j >= static_cast<long>(b.size())) continue;
            score += static_cast<double>(a[i]) * static_cast<double>(b[static_cast<size_t>(j)]);
        }
        double normalized = score / denom;
        if (normalized > best) best = normalized;
    }
    return static_cast<float>(best);
}
}  // namespace

COOPA_TEST(wav_decodes_bit_exact_at_its_source_rate) {
    const std::string path = sfx_test::fixture_copy(sfx_test::k_whoosh);
    Decoder dec(path, AudioFormat{44100, 2});  // matches the file's own rate: fast-path territory
    EXPECT_EQ(dec.total_frames(), sfx_test::k_whoosh_frames);
    EXPECT_EQ(dec.format().sample_rate, 44100u);
    EXPECT_EQ(dec.format().channels, 2u);
    EXPECT_EQ(dec.source_format().sample_rate, 44100u);
    EXPECT_EQ(dec.source_format().channels, 2u);
    EXPECT_TRUE(dec.source_format().sample_format == SampleFormat::F32);

    const uint32_t prefix_frames = 256;
    std::vector<float> decoded(static_cast<size_t>(prefix_frames) * 2);
    ASSERT_EQ(dec.read_frames(decoded.data(), prefix_frames), static_cast<uint64_t>(prefix_frames));

    std::vector<uint8_t> raw = sfx_test::read_wav_data_chunk_raw(path);
    ASSERT_GE(raw.size(), decoded.size() * sizeof(float));
    EXPECT_EQ(std::memcmp(raw.data(), decoded.data(), decoded.size() * sizeof(float)), 0);
}

COOPA_TEST(resampling_to_48khz_scales_the_frame_count) {
    Decoder dec(sfx_test::fixture_copy(sfx_test::k_whoosh), AudioFormat{48000, 2});
    EXPECT_EQ(dec.format().sample_rate, 48000u);
    // miniaudio's own resampler runs inside the decoder here (distinct from
    // our dsp/resampler.h, used at the AudioClip layer); expect it to land
    // within a sample or two of the exact ratio.
    double expected = double(sfx_test::k_whoosh_frames) * 48000.0 / 44100.0;
    EXPECT_NEAR(static_cast<float>(dec.total_frames()), static_cast<float>(expected), 2.0f);
}

COOPA_TEST(mp3_decode_matches_the_wav_waveform) {
    Decoder wav_dec(sfx_test::fixture_copy(sfx_test::k_whoosh), AudioFormat{44100, 2});
    Decoder mp3_dec(sfx_test::fixture_copy(sfx_test::k_whoosh_mp3), AudioFormat{44100, 2});

    uint64_t wav_frames = wav_dec.total_frames();
    uint64_t mp3_frames = mp3_dec.total_frames();
    // mp3 is lossy: encoders add ~1152 frames of decoder delay plus end
    // padding, so exact frame counts can't match -- allow a couple of
    // MPEG granules of slack.
    EXPECT_LT(std::llabs(static_cast<long long>(mp3_frames) - static_cast<long long>(wav_frames)), 2 * 1152 * 2);

    const uint32_t n = 8000;
    std::vector<float> wav_stereo(static_cast<size_t>(n) * 2), mp3_stereo(static_cast<size_t>(n) * 2);
    wav_dec.read_frames(wav_stereo.data(), n);
    mp3_dec.read_frames(mp3_stereo.data(), n);

    std::vector<float> wav_mono(n), mp3_mono(n);
    for (uint32_t i = 0; i < n; ++i) {
        wav_mono[i] = (wav_stereo[static_cast<size_t>(i) * 2] + wav_stereo[static_cast<size_t>(i) * 2 + 1]) * 0.5f;
        mp3_mono[i] = (mp3_stereo[static_cast<size_t>(i) * 2] + mp3_stereo[static_cast<size_t>(i) * 2 + 1]) * 0.5f;
    }

    // Waveforms should correlate strongly once aligned for the encoder's
    // decoder-delay offset -- correlate the whole waveform shape, not
    // individual samples (mp3 is lossy).
    EXPECT_GT(normalized_cross_correlation_peak(wav_mono, mp3_mono, 2000), 0.5f);
}

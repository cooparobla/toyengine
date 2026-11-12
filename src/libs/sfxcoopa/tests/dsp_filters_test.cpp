/**
 * @file dsp_filters_test.cpp
 * @brief dsp/one_pole.h, dsp/biquad.h, dsp/delay_line.h against analytic answers: unity DC
 *        gain, low-pass passband/stopband, and linear interpolation at fractional delays.
 *        The resampler has its own suite.
 */
#include <coopa/testing/test.h>

#include <sfxcoopa/dsp/biquad.h>
#include <sfxcoopa/dsp/delay_line.h>
#include <sfxcoopa/dsp/one_pole.h>

#include <cmath>
#include <cstdint>

COOPA_TEST_SUITE("dsp_filters");

namespace {
float rms_of_sine_through(coopa::sfx::dsp::Biquad filter, float freq_hz, uint32_t rate, int n) {
    const float w = 2.0f * 3.14159265358979323846f * freq_hz / static_cast<float>(rate);
    double sum_sq = 0.0;
    int measured = 0;
    for (int i = 0; i < n; ++i) {
        float x = std::sin(w * static_cast<float>(i));
        float y = filter.process(x);
        if (i >= n / 2) {  // skip transient, measure steady state
            sum_sq += static_cast<double>(y) * y;
            ++measured;
        }
    }
    return static_cast<float>(std::sqrt(sum_sq / measured));
}
}  // namespace

COOPA_TEST(one_pole_lowpass_has_unity_dc_gain) {
    coopa::sfx::dsp::OnePoleLowpass lp;
    lp.set_cutoff(1000.0f, 44100);
    float y = 0.0f;
    for (int i = 0; i < 20000; ++i) y = lp.process(1.0f);
    EXPECT_NEAR(y, 1.0f, 1e-4f);
}

COOPA_TEST(biquad_lowpass_passes_low_and_cuts_high_frequencies) {
    using namespace coopa::sfx::dsp;
    const uint32_t rate = 44100;
    const int n = 4000;
    float rms_100hz = rms_of_sine_through(Biquad::lowpass(1000.0f, rate), 100.0f, rate, n);
    float rms_10khz = rms_of_sine_through(Biquad::lowpass(1000.0f, rate), 10000.0f, rate, n);
    // A sine's RMS is amplitude/sqrt(2) ~= 0.707 when unattenuated.
    EXPECT_GT(rms_100hz, 0.6f);   // passband: close to unattenuated
    EXPECT_LT(rms_10khz, 0.2f);   // well past cutoff: strongly attenuated
}

COOPA_TEST(delay_line_interpolates_fractional_delays) {
    using namespace coopa::sfx::dsp;
    DelayLine dl(16);
    dl.set_delay_samples(2.5f);
    dl.push(1.0f);  // impulse: this is "delay 2" once two more samples are pushed
    dl.push(0.0f);
    dl.push(0.0f);
    // At delay 2.5, we're exactly between "delay 2" (the impulse, 1.0) and
    // "delay 3" (before the impulse, 0.0) -- energy split 50/50.
    EXPECT_NEAR(dl.read(), 0.5f, 1e-6f);

    dl.set_delay_samples(2.0f);
    EXPECT_NEAR(dl.read(), 1.0f, 1e-6f);  // exact integer tap: no interpolation blur

    dl.set_delay_samples(3.0f);
    EXPECT_NEAR(dl.read(), 0.0f, 1e-6f);
}

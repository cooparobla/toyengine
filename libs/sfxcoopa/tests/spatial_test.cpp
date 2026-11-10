/**
 * @file spatial_test.cpp
 * @brief spatial/: AttenuationCurve endpoints, the Listener's default basis (the -Z-forward,
 *        +Y-up, +X-right convention every spatial check relies on), and BinauralPanner's
 *        ITD/ILD, distance gain and doppler -- both its computed EarParams and the delay its
 *        process() actually applies.
 *
 * Not covered: front/back ITD equality (the cone of confusion) -- that is a limitation of the
 * spherical-head model, not a contract, and asserting it would only block an improvement.
 */
#include <coopa/testing/test.h>

#include <sfxcoopa/spatial/attenuation.h>
#include <sfxcoopa/spatial/listener.h>
#include <sfxcoopa/spatial/panner.h>

#include <glm/glm.hpp>

#include <vector>

COOPA_TEST_SUITE("spatial");

using namespace coopa::sfx::spatial;

namespace {
int cross_correlation_peak_lag(const std::vector<float>& a, const std::vector<float>& b, int max_lag) {
    int best_lag = 0;
    double best_score = -1.0;
    for (int lag = 0; lag <= max_lag; ++lag) {
        double score = 0.0;
        for (size_t n = static_cast<size_t>(lag); n < a.size(); ++n) {
            score += static_cast<double>(a[n]) * static_cast<double>(b[n - static_cast<size_t>(lag)]);
        }
        if (score > best_score) {
            best_score = score;
            best_lag = lag;
        }
    }
    return best_lag;
}
}  // namespace

COOPA_TEST(attenuation_curve_hits_its_endpoints_in_every_mode) {
    AttenuationCurve curve;
    curve.min_distance = 1.0f;
    curve.max_distance = 100.0f;
    for (RolloffMode mode : {RolloffMode::Inverse, RolloffMode::Linear, RolloffMode::Logarithmic}) {
        curve.mode = mode;
        EXPECT_EQ(curve.evaluate(1.0f), 1.0f);
        EXPECT_EQ(curve.evaluate(0.5f), 1.0f);  // at/inside min_distance
        EXPECT_EQ(curve.evaluate(100.0f), 0.0f);
        EXPECT_EQ(curve.evaluate(200.0f), 0.0f);  // at/beyond max_distance
        float mid = curve.evaluate(50.0f);
        EXPECT_TRUE(mid > 0.0f && mid < 1.0f);
    }
}

COOPA_TEST(listener_default_basis_is_identity) {
    Listener listener;  // forward (0,0,-1), up (0,1,0)
    glm::mat3 basis = listener.basis();
    // Default orientation's basis should be identity: right=(1,0,0),
    // up=(0,1,0), -forward=(0,0,1).
    glm::vec3 local = glm::transpose(basis) * glm::vec3(0.0f, 0.0f, -5.0f);
    EXPECT_VEC_NEAR(local, glm::vec3(0.0f, 0.0f, -5.0f), 1e-5f);
}

COOPA_TEST(panner_source_dead_ahead_is_symmetric) {
    BinauralPanner panner(48000);
    Listener listener;
    SpatialParams p;
    p.position = glm::vec3(0.0f, 0.0f, -5.0f);  // straight ahead
    EarParams ear = panner.compute(p, listener, 48000);
    EXPECT_NEAR(ear.gain_l, ear.gain_r, 1e-5f);
    EXPECT_NEAR(ear.delay_l, 0.0f, 1e-4f);
    EXPECT_NEAR(ear.delay_r, 0.0f, 1e-4f);
}

COOPA_TEST(panner_source_hard_right_is_louder_right_and_delays_left_ear) {
    BinauralPanner panner(48000);
    Listener listener;
    SpatialParams p;
    p.position = glm::vec3(5.0f, 0.0f, 0.0f);  // hard right
    EarParams ear = panner.compute(p, listener, 48000);
    EXPECT_GT(ear.gain_r, ear.gain_l);       // right ear (ipsilateral) louder
    EXPECT_GT(ear.delay_l, ear.delay_r);     // left ear (contralateral) delayed
    EXPECT_NEAR(ear.delay_l, 31.5f, 2.0f);   // Woodworth max ITD ~=32 samples @48kHz
    EXPECT_NEAR(ear.delay_r, 0.0f, 1e-4f);
}

COOPA_TEST(panner_applies_distance_attenuation) {
    // Isolate distance attenuation from head-shadow ILD (which legitimately
    // reduces gain somewhat even dead-ahead in this model, since neither
    // ear directly faces a source at azimuth 0) by disabling ITD/shadow --
    // this test is specifically about the distance curve wired into
    // compute(), unit-tested standalone in AttenuationCurve above.
    BinauralPanner panner(48000);
    Listener listener;
    SpatialParams near_p, far_p;
    near_p.enable_itd = near_p.enable_head_shadow = false;
    far_p.enable_itd = far_p.enable_head_shadow = false;
    near_p.position = glm::vec3(0.0f, 0.0f, -near_p.attenuation.min_distance);
    far_p.position = glm::vec3(0.0f, 0.0f, -far_p.attenuation.max_distance);
    EarParams ear_near = panner.compute(near_p, listener, 48000);
    EarParams ear_far = panner.compute(far_p, listener, 48000);
    EXPECT_NEAR(ear_near.gain_l, ear_near.gain_r, 1e-5f);
    EXPECT_GT(ear_near.gain_l, 0.9f);           // at min_distance: unattenuated
    EXPECT_NEAR(ear_far.gain_l, 0.0f, 1e-5f);   // at max_distance: silent
}

COOPA_TEST(panner_doppler_raises_pitch_for_approaching_source) {
    BinauralPanner panner(48000);
    Listener listener;  // stationary
    SpatialParams p;
    p.position = glm::vec3(0.0f, 0.0f, -10.0f);
    p.velocity = glm::vec3(0.0f, 0.0f, 10.0f);  // moving toward listener (at +z, listener at origin)
    EarParams ear = panner.compute(p, listener, 48000);
    EXPECT_GT(ear.pitch_scale, 1.0f);
}

COOPA_TEST(panner_process_delays_the_contralateral_ear) {
    BinauralPanner panner(48000);
    Listener listener;
    SpatialParams p;
    p.position = glm::vec3(5.0f, 0.0f, 0.0f);  // hard right
    panner.set_target(panner.compute(p, listener, 48000));

    const int n = 100;
    std::vector<float> mono_in(n, 0.0f);
    mono_in[0] = 1.0f;  // impulse
    std::vector<float> stereo(static_cast<size_t>(n) * 2);
    panner.process(mono_in.data(), stereo.data(), n);

    std::vector<float> left(n), right(n);
    for (int i = 0; i < n; ++i) {
        left[static_cast<size_t>(i)] = stereo[static_cast<size_t>(i) * 2];
        right[static_cast<size_t>(i)] = stereo[static_cast<size_t>(i) * 2 + 1];
    }
    // Left (contralateral) lags right (ipsilateral) by ~32 samples: the lag
    // that best aligns left[n] with right[n-lag] should land near there.
    EXPECT_NEAR(static_cast<float>(cross_correlation_peak_lag(left, right, 64)), 31.5f, 2.0f);
}

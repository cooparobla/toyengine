/**
 * @file water_waves_test.cpp
 * @brief The water surface's CPU mirror (toyengine/water/water_waves.h, water_surface_query.h):
 *        the inverse height query lands on the GPU's forward Gerstner crest, the precomputed WaveSet
 *        is the reference sum and fades with distance like the shader, and the triangle query
 *        interpolates inside and reports no water outside.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/water/water_surface_query.h>
#include <toyengine/water/water_waves.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("water_waves");

/** @brief Gerstner's horizontal displacement means "height above XY" needs the inverse; the
 *         CPU query must land on the same crest the forward sum (what the GPU draws) puts there. */
COOPA_TEST(height_query_inverts_the_forward_gerstner_sum) {
    toy::water::WaveParams w;
    w.amplitude = 0.4f;
    w.wavelength = 7.0f;
    w.direction = 0.6f;
    w.steepness = 0.8f;
    float worst = 0.0f;
    for (int i = 0; i < 64; ++i) {
        glm::vec2 p0(std::sin(i * 1.7f) * 20.0f, std::cos(i * 2.3f) * 20.0f);
        const float t = 0.37f * static_cast<float>(i);
        toy::water::WaveSample fwd = toy::water::evaluate(w, p0, t);
        glm::vec2 target = p0 + glm::vec2(fwd.displacement);
        float h = toy::water::height_at(w, target, t, [](const glm::vec2&) { return 1.0f; }, nullptr, 4);
        worst = std::max(worst, std::fabs(h - fwd.displacement.z));
    }
    expect(worst < 0.02f * w.amplitude * 4.0f,
           "water waves: inverted height matches the forward Gerstner sum (worst " + std::to_string(worst) + ")");

    toy::water::WaveParams calm;
    calm.amplitude = 0.0f;
    expect_near(toy::water::evaluate(calm, glm::vec2(3.0f), 1.0f).displacement.z, 0.0f, 1e-7f,
                "water waves: zero amplitude is flat");
    expect(toy::water::depth_attenuation(0.0f, 8.0f) < toy::water::depth_attenuation(10.0f, 8.0f),
           "water waves: shallow water calms the waves");
}

/** @brief The precomputed WaveSet is the same wave sum as the reference evaluate(), and the
 *         distance fade removes the short waves first and everything far enough away. */
COOPA_TEST(waveset_matches_the_reference_and_fades_with_distance) {
    toy::water::WaveParams w;
    w.amplitude = 0.4f;
    w.wavelength = 7.0f;
    w.direction = 0.6f;
    w.steepness = 0.8f;
    const toy::water::WaveSet ws = toy::water::WaveSet::from(w);
    expect(ws.matches(w), "waveset: built from these params");
    float worst = 0.0f;
    for (int i = 0; i < 64; ++i) {
        const glm::vec2 p(std::sin(i * 1.3f) * 30.0f, std::cos(i * 0.7f) * 30.0f);
        const float t = 0.21f * static_cast<float>(i);
        const float atten = 0.25f + 0.75f * static_cast<float>(i % 5) / 4.0f;
        const auto a = toy::water::evaluate(w, p, t, atten);
        const auto b = toy::water::evaluate(ws, p, t, atten, 0.0f);
        worst = std::max({worst, glm::length(a.displacement - b.displacement), glm::length(a.normal - b.normal),
                          std::fabs(a.crest - b.crest)});
    }
    expect(worst < 1e-5f, "waveset: matches the reference sum (worst " + std::to_string(worst) + ")");

    const float shortest = w.wavelength * toy::water::k_wave_length_ratio[toy::water::k_wave_count - 1];
    const float fs = toy::water::k_wave_fade_start;
    expect_near(toy::water::wave_distance_fade(shortest, fs * shortest * 0.9f), 1.0f, 1e-6f, "wave fade: none up close");
    expect_near(toy::water::wave_distance_fade(shortest, fs * shortest * 2.0f), 0.0f, 1e-6f, "wave fade: gone at 2x start");
    expect(toy::water::wave_distance_fade(shortest, fs * shortest * 1.5f) <
               toy::water::wave_distance_fade(w.wavelength, fs * shortest * 1.5f),
           "wave fade: short waves fade before long ones");
    const auto far = toy::water::evaluate(ws, glm::vec2(3.0f), 1.0f, 1.0f, 2.0f * fs * w.wavelength + 1.0f);
    expect(glm::length(far.displacement) < 1e-7f, "wave fade: nothing moves beyond every wave's fade");

    // The CPU query applies the same fade given a focus.
    toy::water::WaterSurfaceQuery q;
    std::vector<toy::water::WaterVertex> v(4);
    v[0].position = {-50.0f, -50.0f, 0.0f};
    v[1].position = {50.0f, -50.0f, 0.0f};
    v[2].position = {50.0f, 50.0f, 0.0f};
    v[3].position = {-50.0f, 50.0f, 0.0f};
    for (auto& x : v) x.depth = 100.0f;
    q.build(v, {0, 1, 2, 0, 2, 3});
    toy::water::WaveQueryOptions far_opt;
    far_opt.has_focus = true;
    far_opt.focus = glm::vec3(0.0f, 0.0f, 2.0f * fs * w.wavelength + 10.0f);
    toy::water::WaterSample s_near, s_far;
    expect(q.sample(glm::vec2(1.0f), ws, 0.5f, s_near) && q.sample(glm::vec2(1.0f), ws, 0.5f, s_far, far_opt),
           "water query: samples with and without a focus");
    expect(std::fabs(s_near.surface_height) > 1e-3f && std::fabs(s_far.surface_height) < 1e-6f,
           "water query: far from the focus the waves have faded, as on the GPU");
}

/** @brief The XY triangle grid: inside interpolates, outside reports no water. */
COOPA_TEST(surface_query_interpolates_inside_and_rejects_outside) {
    toy::water::WaterSurfaceQuery q;
    std::vector<toy::water::WaterVertex> v(4);
    v[0].position = {0.0f, 0.0f, 1.0f};
    v[1].position = {10.0f, 0.0f, 2.0f};
    v[2].position = {10.0f, 10.0f, 2.0f};
    v[3].position = {0.0f, 10.0f, 1.0f};
    for (auto& x : v) x.depth = 3.0f;
    v[1].flow = v[2].flow = glm::vec3(2.0f, 0.0f, 0.0f);
    q.build(v, {0, 1, 2, 0, 2, 3});
    toy::water::WaterBaseSample b;
    expect(q.sample_base(glm::vec2(5.0f, 5.0f), b), "water query: centre is on the surface");
    expect_near(b.height, 1.5f, 1e-4f, "water query: height interpolates across the quad");
    expect_near(b.flow.x, 1.0f, 1e-4f, "water query: flow interpolates across the quad");
    expect(!q.sample_base(glm::vec2(-1.0f, 5.0f), b), "water query: outside the mesh is not water");
}

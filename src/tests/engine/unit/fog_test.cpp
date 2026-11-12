/**
 * @file fog_test.cpp
 * @brief The global height fog's transmittance (toyengine/render/fog_math.h, the CPU mirror of
 *        gfx/fog.glsl) against Beer-Lambert and a numeric march.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <toyengine/render/fog_math.h>
#include <toyengine/render/toy_render_config.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("fog");

/**
 * @brief The global height fog's transmittance (fog_math.h, the CPU mirror of gfx/fog.glsl):
 *        uniform fog is Beer-Lambert, the height integral matches a numeric march, density is
 *        finite below the base, and an underwater eye skips the in-water part of the ray.
 */
COOPA_TEST(height_fog_transmittance_matches_a_numeric_integral) {
    using toy::render::fog_height_tau;
    using toy::render::fog_transmittance;
    toy::render::ToyRenderConfig c;
    c.fog_enabled = true;
    c.fog_mode = 1;
    c.fog_density = 0.02f;
    c.fog_height_falloff = 0.0f;
    c.fog_max_opacity = 1.0f;

    // Uniform: T = exp(-sigma * d).
    const float T_flat = fog_transmittance(c, {0, 0, 2}, {50, 0, 2});
    expect(std::abs(T_flat - std::exp(-0.02f * 50.0f)) < 1e-5f, "fog: uniform fog is exp(-density * distance)");

    // Height fog: the closed form matches a fine numeric march, for rays crossing the base.
    const float base = 1.0f, falloff = 8.0f, sigma = 0.05f;
    const glm::vec3 pairs[][2] = {{{0, 0, -5}, {40, 3, 20}}, {{0, 0, 30}, {-20, 10, 0.5f}},
                                  {{0, 0, 4}, {60, 0, 4.0005f}}, {{0, 0, -3}, {10, 0, -6}}};
    for (const auto& pr : pairs) {
        const glm::vec3 A = pr[0], B = pr[1];
        double march = 0.0;
        const int n = 20000;
        for (int i = 0; i < n; ++i) {
            const glm::vec3 p = glm::mix(A, B, (i + 0.5f) / n);
            const double d = p.z < base ? sigma : sigma * std::exp(-(p.z - base) / falloff);
            march += d * glm::length(B - A) / n;
        }
        const float tau = fog_height_tau(A, B, sigma, base, falloff);
        expect(std::abs(tau - march) < 1e-3 * std::max(1.0, march), "fog: height integral matches a numeric march");
    }

    // Far below the base the density is constant, not exponentially exploding.
    const float deep = fog_height_tau({0, 0, -200}, {10, 0, -200}, sigma, base, falloff);
    expect(std::isfinite(deep) && std::abs(deep - sigma * 10.0f) < 1e-4f, "fog: density is constant below the base");

    // Thinner upward: looking up fogs less than looking along the ground.
    c.fog_height_falloff = falloff; c.fog_height_base = base; c.fog_density = sigma;
    expect(fog_transmittance(c, {0, 0, 1}, {0, 0, 101}) > fog_transmittance(c, {0, 0, 1}, {100, 0, 1}),
           "fog: height fog thins with altitude");

    // Start distance and max opacity.
    c.fog_start_distance = 10.0f;
    expect(fog_transmittance(c, {0, 0, 1}, {9, 0, 1}) == 1.0f, "fog: nothing nearer than the start distance");
    c.fog_start_distance = 0.0f;
    c.fog_max_opacity = 0.6f;
    expect(fog_transmittance(c, {0, 0, 1}, {5000, 0, 1}) >= 0.4f - 1e-6f, "fog: max opacity floors transmittance");
    c.fog_max_opacity = 1.0f;

    // Under water: a ray that never leaves the water carries no air fog; one that does is fogged
    // only from where it crosses the surface.
    const float level = 0.0f;
    expect(fog_transmittance(c, {0, 0, -3}, {40, 0, -5}, true, level) == 1.0f,
           "fog: an underwater ray that stays under water gets no air fog");
    const glm::vec3 eye{0, 0, -2}, target{0, 30, 10};
    const glm::vec3 exit = eye + (target - eye) * ((level - eye.z) / (target.z - eye.z));
    const float T_uw = fog_transmittance(c, eye, target, true, level);
    expect(std::abs(T_uw - std::exp(-fog_height_tau(exit, target, sigma, base, falloff))) < 1e-5f,
           "fog: an underwater ray is fogged only above the surface");
}

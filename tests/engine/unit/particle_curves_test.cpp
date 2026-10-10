/**
 * @file particle_curves_test.cpp
 * @brief Particle value curves (toyengine/particles/particle_math.h): baked curve and gradient
 *        tables match exact evaluation, and the curl turbulence is divergence-free at unit RMS.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/particles/particle_math.h>
#include <toyengine/particles/particle_shape.h>
#include <toyengine/particles/particle_system.h>
#include <toyengine/particles/particle_system_runner.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("particle_curves");

/** @brief Curves and gradients bake to tables that match the exact piecewise-linear evaluation. */
COOPA_TEST(curves_gradients_and_turbulence_match_exact_evaluation) {
    using namespace toy::particles;
    FloatCurve c({{0.0f, 0.0f}, {0.25f, 1.0f}, {1.0f, 0.5f}});
    expect_near(c.evaluate(0.0f), 0.0f, 1e-5f, "curve: starts at its first key");
    expect_near(c.evaluate(0.25f), 1.0f, 0.02f, "curve: hits its middle key");
    expect_near(c.evaluate(1.0f), 0.5f, 1e-5f, "curve: ends at its last key");
    for (float t = 0.0f; t <= 1.0f; t += 0.037f) {
        if (std::abs(c.evaluate(t) - c.evaluate_exact(t)) > 0.02f) {
            expect(false, "curve: table lookup matches exact evaluation at t=" + std::to_string(t));
            break;
        }
    }
    expect_near(c.evaluate(-3.0f), 0.0f, 1e-5f, "curve: clamps below 0");
    expect_near(FloatCurve().evaluate(0.7f), 1.0f, 1e-6f, "curve: an empty curve is 1 everywhere");
    expect_near(FloatCurve::linear(0.0f, 2.0f).integral(1.0f), 1.0f, 0.01f, "curve: integral of a 0->2 ramp is 1");

    Gradient g({{0.0f, glm::vec4(1, 0, 0, 1)}, {1.0f, glm::vec4(0, 0, 1, 0)}});
    const glm::vec4 mid = g.evaluate(0.5f);
    expect_near(mid.r, 0.5f, 0.02f, "gradient: red halfway down");
    expect_near(mid.b, 0.5f, 0.02f, "gradient: blue halfway up");
    expect_near(mid.a, 0.5f, 0.02f, "gradient: alpha interpolates too");
    expect(Gradient().evaluate(0.3f) == glm::vec4(1.0f), "gradient: an empty gradient is opaque white");

    // Turbulence: divergence-free (the whole point of the curl) and unit RMS.
    TurbulenceField f;
    f.configure(7, 0.8f, 2);
    double sum2 = 0.0, max_div = 0.0;
    Rng rng(3);
    const float h = 1e-3f;
    for (int i = 0; i < 400; ++i) {
        const glm::vec3 p(rng.range(-5, 5), rng.range(-5, 5), rng.range(-5, 5));
        const glm::vec3 v = f.sample(p, 1.3f);
        sum2 += glm::dot(v, v);
        const float div = (f.sample(p + glm::vec3(h, 0, 0), 1.3f).x - f.sample(p - glm::vec3(h, 0, 0), 1.3f).x +
                           f.sample(p + glm::vec3(0, h, 0), 1.3f).y - f.sample(p - glm::vec3(0, h, 0), 1.3f).y +
                           f.sample(p + glm::vec3(0, 0, h), 1.3f).z - f.sample(p - glm::vec3(0, 0, h), 1.3f).z) / (2.0f * h);
        max_div = std::max(max_div, static_cast<double>(std::abs(div)));
    }
    expect_near(static_cast<float>(std::sqrt(sum2 / 400.0)), 1.0f, 0.35f, "turbulence: about unit RMS magnitude");
    expect(max_div < 0.05, "turbulence: divergence-free (max |div| " + std::to_string(max_div) + ")");
}

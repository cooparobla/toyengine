/**
 * @file terrain_noise_test.cpp
 * @brief heightmap_fbm() (toyengine/world/terrain_noise.h): deterministic per seed, varying with
 *        it, and bounded -- what the sculpt brush's Noise tool depends on.
 */

#include <coopa/testing/test.h>

#include <toyengine/world/terrain_noise.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("terrain_noise");

using toy::world::heightmap_fbm;

COOPA_TEST(same_seed_same_value) {
    const float a = heightmap_fbm(3.25f, -1.5f, 7, 4, 0.0f);
    const float b = heightmap_fbm(3.25f, -1.5f, 7, 4, 0.0f);
    expect_near(a, b, 1e-9f, "same inputs: identical output");
}

COOPA_TEST(different_seed_different_value) {
    const float a = heightmap_fbm(3.25f, -1.5f, 7, 4, 0.0f);
    const float b = heightmap_fbm(3.25f, -1.5f, 7, 8, 0.0f);
    expect(a != b, "different seeds: different output");
}

COOPA_TEST(stays_roughly_bounded) {
    for (int i = 0; i < 64; ++i) {
        const float v = heightmap_fbm(static_cast<float>(i) * 0.37f, static_cast<float>(i) * -0.19f, 42, 5, 0.4f);
        expect(v >= -1.5f && v <= 1.5f, "fbm stays roughly in [-1, 1] with some headroom for ridges");
    }
}

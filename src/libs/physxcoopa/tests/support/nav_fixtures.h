#pragma once

/**
 * @file nav_fixtures.h
 * @brief Shared navigation worlds: static boxes, the 10-step staircase world, the 240 m rocky
 *        open world (big enough for hierarchical flow fields), and a one-call bake.
 */

#include <physxcoopa/world.h>
#include <physxcoopa/nav/nav_baker.h>
#include <physxcoopa/nav/nav_source.h>
#include <coopa/job/engine.h>

#include <cmath>
#include <memory>
#include <random>
#include <vector>

namespace physxtest::navtest {

using namespace coopa::physx;

inline dynamics::BodyId static_box(PhysicsWorld& world, const glm::vec3& center, const glm::vec3& half) {
    dynamics::Body b;
    b.type = dynamics::BodyType::Static;
    b.position = center;
    return world.add_body(b, collision::Shape::make_box(half));
}

/** @brief 20 x 20 m ground (top at z = 0), a 10-step staircase rising +X from x = -3 to a
 *         platform (top z = 2) spanning x in [2, 8], y in [-4, 4]. */
inline void stairs_world(PhysicsWorld& world) {
    static_box(world, {0.0f, 0.0f, -0.5f}, {10.0f, 10.0f, 0.5f});
    static_box(world, {5.0f, 0.0f, 1.0f}, {3.0f, 4.0f, 1.0f});
    for (int i = 0; i < 10; ++i) {
        float top = 0.2f * static_cast<float>(i + 1);
        static_box(world, {-2.75f + 0.5f * static_cast<float>(i), 0.0f, 0.5f * top}, {0.25f, 1.0f, 0.5f * top});
    }
}

inline std::unique_ptr<nav::NavBaker> bake(const PhysicsWorld& world, coopa::job::JobEngine* jobs = nullptr,
                                           nav::NavBuildSettings settings = {}) {
    auto baker = std::make_unique<nav::NavBaker>(settings);
    std::vector<nav::SourceShape> sources;
    nav::gather_sources(world, {}, sources);
    baker->set_sources(sources);
    baker->build_all(jobs);
    return baker;
}

/** @brief 240 x 240 m of open ground with scattered rocks -- big enough that Auto goes
 *         hierarchical (30 x 30 tiles at 0.5 m cells). */
inline std::unique_ptr<nav::NavBaker> open_world(PhysicsWorld& world, uint32_t seed, bool long_wall = false) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-115.0f, 115.0f), sz(0.6f, 3.0f);
    static_box(world, {0.0f, 0.0f, -0.5f}, {120.0f, 120.0f, 0.5f});
    for (int i = 0; i < 300; ++i) {
        glm::vec3 c(u(rng), u(rng), 1.0f);
        if (glm::length(glm::vec2(c)) < 8.0f || (long_wall && std::fabs(c.y + 40.0f) < 6.0f)) continue;
        static_box(world, c, {sz(rng), sz(rng), 1.0f});
    }
    if (long_wall) {
        // y = -40, from x = -120 to 120 with a single 6 m gap at x = 90.
        static_box(world, {-16.5f, -40.0f, 1.5f}, {103.5f, 0.5f, 1.5f});
        static_box(world, {106.5f, -40.0f, 1.5f}, {13.5f, 0.5f, 1.5f});
    }
    nav::NavBuildSettings settings;
    settings.cell_size = 0.5f;
    return bake(world, nullptr, settings);
}

} // namespace physxtest::navtest

/**
 * @file particle_emission_test.cpp
 * @brief Particle emission: rate, bursts, lifetime, duration and the pool cap give exact counts;
 *        the mesh emitter samples by area; scatters place `count` instances on the surface,
 *        aligned to its normals, once.
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
#include "engine/support/particle_fixtures.h"

COOPA_TEST_SUITE("particle_emission");

using namespace toy::test;

/** @brief Rate, bursts, lifetime and a non-looping duration produce the expected counts. */
COOPA_TEST(rate_bursts_lifetime_and_cap_give_exact_counts) {
    PScene ps;
    auto* rate = add_system(ps, "rate", glm::vec3(0), [](ParticleSystem& s) {
        s.settings.rate = 30.0f;
        s.settings.start_lifetime = 100.0f;
    });
    auto* burst = add_system(ps, "burst", glm::vec3(0), [](ParticleSystem& s) {
        s.settings.rate = 0.0f;
        s.settings.start_lifetime = 100.0f;
        s.settings.bursts.push_back({0.5f, toy::particles::Range(7.0f), 3, 0.2f, 1.0f});   // 0.5, 0.7, 0.9 s
    });
    auto* oneshot = add_system(ps, "oneshot", glm::vec3(0), [](ParticleSystem& s) {
        s.settings.rate = 40.0f;
        s.settings.looping = false;
        s.settings.duration = 0.5f;
        s.settings.start_lifetime = 0.25f;
    });
    auto* capped = add_system(ps, "capped", glm::vec3(0), [](ParticleSystem& s) {
        s.settings.rate = 1000.0f;
        s.settings.max_particles = 50;
        s.settings.start_lifetime = 100.0f;
    });
    start(ps);
    run(ps, 1.0f);
    expect(std::abs(static_cast<int>(rate->particle_count()) - 30) <= 1,
           "emission: 30/s for 1 s makes 30 particles (" + std::to_string(rate->particle_count()) + ")");
    expect(burst->particle_count() == 21u, "emission: a 3-cycle burst of 7 fires 21 (" + std::to_string(burst->particle_count()) + ")");
    expect(capped->particle_count() == 50u, "emission: max_particles caps the pool");
    run(ps, 0.5f);
    expect(oneshot->particle_count() == 0u && !oneshot->is_alive(),
           "emission: a non-looping system stops after its duration and its particles die");
    expect(rate->is_alive(), "emission: a looping system keeps going");
}

/** @brief The mesh emitter samples by AREA, puts points on the surface, and carries its normal. */
COOPA_TEST(mesh_surface_samples_by_area) {
    using namespace toy::particles;
    const MeshSurface s = two_triangle_surface();
    expect(s.triangle_count() == 2u && s.vertex_count() == 6u, "mesh surface: two triangles, six vertices");
    expect_near(s.area(), 5.0f, 1e-4f, "mesh surface: total area");
    Rng rng(42);
    int big = 0;
    const int n = 20000;
    bool on_surface = true, normals_up = true;
    for (int i = 0; i < n; ++i) {
        const EmitSample e = s.sample(MeshEmitFrom::Faces, rng.next01(), rng);
        if (e.position.x >= 2.0f) ++big;
        on_surface &= std::abs(e.position.z) < 1e-5f;
        normals_up &= e.normal.z > 0.999f;
        // Inside the triangle it was drawn from.
        if (e.position.x < 2.0f) on_surface &= e.position.x + e.position.y <= 1.0001f;
    }
    expect_near(static_cast<float>(big) / n, 0.9f, 0.015f, "mesh surface: 90% of samples land on the triangle with 90% of the area");
    expect(on_surface, "mesh surface: every sample lies on its triangle");
    expect(normals_up, "mesh surface: samples carry the surface normal");

    // Even distribution: stratified u covers both triangles in exact proportion.
    int big_even = 0;
    for (int i = 0; i < 100; ++i) {
        const EmitSample e = s.sample(MeshEmitFrom::Faces, (i + rng.next01()) / 100.0f, rng);
        if (e.position.x >= 2.0f) ++big_even;
    }
    expect(big_even == 90, "mesh surface: an even distribution of 100 puts exactly 90 on the big triangle (" +
                               std::to_string(big_even) + ")");
    const EmitSample v = s.sample(MeshEmitFrom::Vertices, 0.0f, rng);
    expect(v.position == glm::vec3(0.0f), "mesh surface: vertex emission lands on a vertex");
    const EmitSample ed = s.sample(MeshEmitFrom::Edges, 0.5f, rng);
    expect(std::abs(ed.position.z) < 1e-6f && std::abs(glm::length(ed.tangent) - 1.0f) < 1e-4f,
           "mesh surface: edge emission lies on an edge, its tangent along it");
}

/** @brief A scatter places `count` instances on a tilted mesh, each oriented to its normal, once. */
COOPA_TEST(scatter_places_count_instances_aligned_to_normals) {
    using namespace toy::particles;
    // A 45-degree slope: z = x over [0,2]x[0,2].
    MeshSurface slope;
    const glm::vec3 n = glm::normalize(glm::vec3(-1, 0, 1));
    slope.build({{0, 0, 0}, {2, 0, 2}, {2, 2, 2}, {0, 0, 0}, {2, 2, 2}, {0, 2, 0}}, std::vector<glm::vec3>(6, n), {}, {});
    PScene ps;
    auto* sys = add_system(ps, "scatter", glm::vec3(10, 0, 0), [&](ParticleSystem& s) {
        s.settings.mode = ParticleMode::Scatter;
        s.settings.shape.type = EmitShape::Mesh;
        s.settings.shape.distribution = MeshDistribution::Even;
        s.settings.count = 64;
        s.settings.align_to_normal = true;
        s.settings.random_spin = true;
        s.settings.look.mode = toy::render::ParticleRenderMode::Aligned;
        s.set_shape_surface(slope);
    });
    start(ps);
    run(ps, 0.5f);
    expect(sys->particle_count() == 64u, "scatter: exactly `count` instances");
    const auto pos0 = sys->pool().pos;
    bool on_plane = true, aligned = true;
    for (size_t i = 0; i < sys->pool().size(); ++i) {
        const glm::vec3 p = sys->pool().pos[i];
        on_plane &= std::abs(p.z - p.x) < 1e-4f;
        aligned &= glm::dot(sys->pool().orient[i] * glm::vec3(0, 0, 1), n) > 0.9999f;
    }
    expect(on_plane, "scatter: every instance sits on the slope");
    expect(aligned, "scatter: every instance's +Z is the surface normal");
    run(ps, 2.0f);
    expect(sys->pool().pos == pos0, "scatter: instances never move or respawn");

    // Render prep follows the object: world position = object transform * local.
    sys->prepare_render(ParticleView{glm::vec3(0, -10, 5)});
    const auto& inst = sys->instances();
    expect(inst.size() == 64u, "scatter: one instance per particle");
    bool in_world = true;
    for (const auto& q : inst) in_world &= q.pos_size.x >= 10.0f - 1e-4f && q.pos_size.x <= 12.0f + 1e-4f;
    expect(in_world, "scatter: instances are drawn where the emitter object is");
}

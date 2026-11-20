/**
 * @file cloth_mesh_builder_test.cpp
 * @brief The arbitrary-mesh cloth builder: edge/bend topology, deterministic greedy-colouring
 *        batches, area-weighted masses, painted-channel pinning, and tether seeding from the
 *        pinned particles. Solve-stage behaviour for the skin channels is cloth_solver_test.cpp.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/cloth/cloth_mesh_builder.h>

#include <cmath>
#include <cstring>

COOPA_TEST_SUITE("cloth_mesh_builder");

using namespace coopa::physx;

namespace {

/**
 * @brief A res x res welded patch in the XY plane as plain positions + triangles -- the same
 *        layout make_grid_cloth() produces, but fed through the MESH path so every count below
 *        has a closed form to assert against.
 */
cloth::MeshClothDesc make_patch(uint32_t res, float size) {
    cloth::MeshClothDesc desc;
    const float spacing = size / static_cast<float>(res - 1);
    for (uint32_t y = 0; y < res; ++y) {
        for (uint32_t x = 0; x < res; ++x) {
            desc.positions.push_back(glm::vec3(static_cast<float>(x) * spacing,
                                               static_cast<float>(y) * spacing, 0.0f));
        }
    }
    for (uint32_t y = 0; y + 1 < res; ++y) {
        for (uint32_t x = 0; x + 1 < res; ++x) {
            const uint32_t i00 = y * res + x, i10 = y * res + x + 1;
            const uint32_t i01 = (y + 1) * res + x, i11 = (y + 1) * res + x + 1;
            desc.triangles.insert(desc.triangles.end(), {i00, i10, i11});
            desc.triangles.insert(desc.triangles.end(), {i00, i11, i01});
        }
    }
    return desc;
}

bool batches_disjoint(const std::vector<cloth::ClothConstraint>& list,
                      const std::vector<cloth::ConstraintBatch>& batches,
                      std::size_t particle_count) {
    for (const cloth::ConstraintBatch& b : batches) {
        std::vector<bool> seen(particle_count, false);
        for (uint32_t i = b.begin; i < b.end; ++i) {
            if (seen[list[i].a] || seen[list[i].b]) return false;
            seen[list[i].a] = true;
            seen[list[i].b] = true;
        }
    }
    return true;
}

std::size_t batch_coverage(const std::vector<cloth::ConstraintBatch>& batches) {
    std::size_t covered = 0;
    for (const cloth::ConstraintBatch& b : batches) covered += b.end - b.begin;
    return covered;
}

} // namespace

/**
 * @brief Topology counts on a patch with closed-form answers, plus the disjointness and full
 *        coverage every ConstraintBatch consumer depends on -- the same structural assertions
 *        grid_builder_topology_and_disjoint_batches makes for the grid path, because a gap or an
 *        overlap here fails silently as nondeterminism, never as a crash.
 */
COOPA_TEST(mesh_builder_topology_and_disjoint_batches) {
    const uint32_t res = 5;
    cloth::MeshClothDesc desc = make_patch(res, 4.0f);
    cloth::Cloth c = cloth::make_mesh_cloth(desc);

    ASSERT_TRUE(c.valid);
    ASSERT_TRUE(c.particles.size() == res * res);
    ASSERT_TRUE(c.columns == 0 && c.rows == 0);
    ASSERT_TRUE(c.triangles.size() == desc.triangles.size());

    // Unique edges: horizontal + vertical + one diagonal per quad.
    const std::size_t quads = (res - 1) * (res - 1);
    const std::size_t expected_stretch = 2u * res * (res - 1) + quads;
    ASSERT_TRUE(c.stretch.size() == expected_stretch);

    // Interior edges bend: every diagonal (both its triangles are in its own quad), plus the
    // horizontal/vertical edges not on the patch boundary.
    const std::size_t expected_bend = quads + 2u * (res - 1) * (res - 2);
    ASSERT_TRUE(c.bend.size() == expected_bend);

    const float spacing = 4.0f / static_cast<float>(res - 1);
    bool rest_lengths_ok = true;
    for (const cloth::ClothConstraint& k : c.stretch) {
        const float expect = glm::length(desc.positions[k.a] - desc.positions[k.b]);
        if (std::abs(k.rest_length - expect) > 1e-5f) rest_lengths_ok = false;
        if (k.rest_length < spacing - 1e-5f) rest_lengths_ok = false;
    }
    ASSERT_TRUE(rest_lengths_ok);

    ASSERT_TRUE(batches_disjoint(c.stretch, c.stretch_batches, c.particles.size()));
    ASSERT_TRUE(batches_disjoint(c.bend, c.bend_batches, c.particles.size()));
    ASSERT_TRUE(batch_coverage(c.stretch_batches) == c.stretch.size());
    ASSERT_TRUE(batch_coverage(c.bend_batches) == c.bend.size());

    // No channels painted: nothing pinned, no skin arrays adopted, no tethers.
    ASSERT_TRUE(c.skin.empty());
    ASSERT_TRUE(c.tethers.empty());
    bool all_free = true;
    for (const cloth::ClothParticle& p : c.particles) all_free = all_free && p.inv_mass > 0.0f;
    ASSERT_TRUE(all_free);

    // self_distance derived below the shortest rest edge, so self-collision cannot fight the
    // structural constraints (there is no grid 1-ring exemption on the mesh path).
    ASSERT_TRUE(c.params.self_distance > 0.0f);
    ASSERT_TRUE(c.params.self_distance < spacing);
}

/** @brief Area masses: the patch's whole mass is area * mass_per_area, and an interior vertex
 *         (6 adjacent triangles) carries twice the mass of a corner one (1 or 2). */
COOPA_TEST(mesh_builder_distributes_mass_by_area) {
    const uint32_t res = 5;
    cloth::MeshClothDesc desc = make_patch(res, 4.0f);
    desc.mass_per_area = 0.25f;
    cloth::Cloth c = cloth::make_mesh_cloth(desc);

    float total = 0.0f;
    for (const cloth::ClothParticle& p : c.particles) {
        ASSERT_TRUE(p.inv_mass > 0.0f);
        total += 1.0f / p.inv_mass;
    }
    ASSERT_NEAR(total, 4.0f * 4.0f * 0.25f, 1e-3f);

    // Corner (0,0) touches 1 triangle in this winding; interior (2,2) touches 6.
    const float corner_mass = 1.0f / c.particles[0].inv_mass;
    const float interior_mass = 1.0f / c.particles[2 * res + 2].inv_mass;
    ASSERT_TRUE(interior_mass > corner_mass * 2.0f);
}

/**
 * @brief Painted max_distance <= eps pins (inv_mass == 0), the skin arrays are adopted, the
 *        first-frame targets are seeded with the rest pose, and tethers run geodesically from
 *        the pinned row. A wrongly sized channel array is discarded, never misindexed.
 */
COOPA_TEST(mesh_builder_pins_painted_particles_and_seeds_tethers) {
    const uint32_t res = 5;
    cloth::MeshClothDesc desc = make_patch(res, 4.0f);
    const std::size_t n = desc.positions.size();

    // Top row (y == res-1) fully skinned, the rest free at 0.5 m.
    desc.channels.max_distance.assign(n, 0.5f);
    for (uint32_t x = 0; x < res; ++x) desc.channels.max_distance[(res - 1) * res + x] = 0.0f;
    desc.channels.anim_drive.assign(n, 1.0f);
    desc.channels.backstop_radius.assign(n - 1, 0.1f); // wrong size, must be discarded

    cloth::Cloth c = cloth::make_mesh_cloth(desc);

    ASSERT_TRUE(!c.skin.empty());
    ASSERT_TRUE(c.skin.max_distance.size() == n);
    ASSERT_TRUE(c.skin.anim_drive.size() == n);
    ASSERT_TRUE(c.skin.backstop_radius.empty());
    ASSERT_TRUE(c.skin_targets.position.size() == n);
    ASSERT_TRUE(c.skin_targets.prev_position.size() == n);

    uint32_t pinned = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (c.skin.max_distance[i] <= 1e-6f) {
            ASSERT_TRUE(c.particles[i].inv_mass == 0.0f);
            ++pinned;
        } else {
            ASSERT_TRUE(c.particles[i].inv_mass > 0.0f);
        }
    }
    ASSERT_TRUE(pinned == res);

    // One tether per free particle, anchored on a pinned particle, capped near the geodesic
    // (straight down a column: rows-below * spacing, times tether_scale).
    ASSERT_TRUE(c.tethers.size() == n - pinned);
    const float spacing = 4.0f / static_cast<float>(res - 1);
    bool tethers_ok = true;
    for (const cloth::ClothTether& t : c.tethers) {
        if (c.particles[t.anchor].inv_mass != 0.0f) tethers_ok = false;
        const uint32_t rows_below = (res - 1) - (t.particle / res);
        const float geodesic = static_cast<float>(rows_below) * spacing;
        if (t.max_length < geodesic - 1e-4f) tethers_ok = false;              // never shorter
        if (t.max_length > geodesic * c.params.tether_scale + 1e-4f) tethers_ok = false;
    }
    ASSERT_TRUE(tethers_ok);
}

/** @brief Building the same mesh twice yields byte-identical constraints and batches -- the
 *         deterministic-colouring contract the serial/parallel bit-identity rests on. */
COOPA_TEST(mesh_builder_is_deterministic) {
    cloth::MeshClothDesc desc = make_patch(7, 4.0f);
    desc.channels.max_distance.assign(desc.positions.size(), 0.3f);
    cloth::Cloth a = cloth::make_mesh_cloth(desc);
    cloth::Cloth b = cloth::make_mesh_cloth(desc);

    ASSERT_TRUE(a.stretch.size() == b.stretch.size());
    ASSERT_TRUE(a.bend.size() == b.bend.size());
    ASSERT_TRUE(a.stretch_batches.size() == b.stretch_batches.size());
    ASSERT_TRUE(a.bend_batches.size() == b.bend_batches.size());
    ASSERT_TRUE(std::memcmp(a.stretch.data(), b.stretch.data(),
                            a.stretch.size() * sizeof(cloth::ClothConstraint)) == 0);
    ASSERT_TRUE(std::memcmp(a.bend.data(), b.bend.data(),
                            a.bend.size() * sizeof(cloth::ClothConstraint)) == 0);
    ASSERT_TRUE(std::memcmp(a.stretch_batches.data(), b.stretch_batches.data(),
                            a.stretch_batches.size() * sizeof(cloth::ConstraintBatch)) == 0);
}

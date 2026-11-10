/**
 * @file hair_solver_test.cpp
 * @brief The XPBD strand solver: inextensibility, the two shape constraints that make a strand
 *        more than a one-wide sheet, collision, determinism and sleep.
 *
 * The claims worth testing are the ones a picture cannot settle. A groom of thousands of strands
 * looks plausible while being wrong in any of these ways, and each failure reads on screen as
 * something else: a stretching strand looks like rubber, a lost global shape looks like the
 * groom was never combed, a lost local shape looks like the curl parameter does nothing.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <physxcoopa/hair/strand_builder.h>
#include <physxcoopa/hair/strand_solver.h>
#include <physxcoopa/world.h>

#include "support/cloth_fixtures.h"

COOPA_TEST_SUITE("hair_solver");

namespace {

using namespace coopa::physx;
using coopa::test::expect;
using coopa::test::expect_near;

const glm::vec3 k_gravity{0.0f, 0.0f, -9.81f};

/**
 * @brief One straight strand of `count` particles running +X from `root`, `spacing` apart.
 *
 * SIDEWAYS, not up. A strand standing exactly along gravity is a pencil balanced on its tip: an
 * unstable equilibrium with no lateral force anywhere in it, so it never tips over and a test
 * built on it measures nothing. Hair leaves a scalp in every direction, and this is the direction
 * that actually swings.
 */
hair::StrandDesc straight_strand(const glm::vec3& root, int count = 8, float spacing = 0.05f) {
    hair::StrandDesc d;
    for (int k = 0; k < count; ++k) {
        d.points.push_back(root + glm::vec3(spacing * static_cast<float>(k), 0.0f, 0.0f));
    }
    return d;
}

/**
 * @brief A strand bent into a right angle in the horizontal plane: out along +X, then along +Y.
 *
 * The corner is what a curl test measures, and it is put in the plane PERPENDICULAR to gravity on
 * purpose -- so what survives the fall is what the local shape constraint held, not what gravity
 * happened to leave alone.
 */
hair::StrandDesc bent_strand(const glm::vec3& root, float spacing = 0.05f) {
    hair::StrandDesc d;
    for (int k = 0; k < 4; ++k) {
        d.points.push_back(root + glm::vec3(spacing * static_cast<float>(k), 0.0f, 0.0f));
    }
    for (int k = 1; k <= 4; ++k) {
        d.points.push_back(root + glm::vec3(spacing * 3.0f, spacing * static_cast<float>(k), 0.0f));
    }
    return d;
}

float strand_length(const hair::StrandSet& set, std::size_t index) {
    const hair::Strand& s = set.strands[index];
    float total = 0.0f;
    for (uint32_t k = 1; k < s.count; ++k) {
        total += glm::length(set.particles[s.first + k].position - set.particles[s.first + k - 1].position);
    }
    return total;
}

/// 4 substeps and 4 iterations: XPBD's residual stretch is an iteration-count property, and 4 is
/// what brings a hanging strand under half a percent (1 iteration leaves 1.4%).
void run(hair::StrandSet& set, int frames, const std::vector<cloth::ClothCollider>& colliders = {},
         float dt = 1.0f / 60.0f) {
    hair::StrandSolverScratch scratch;
    for (int i = 0; i < frames; ++i) {
        hair::solve_strands(set, colliders, k_gravity, 4, 4, dt, scratch);
    }
}

} // namespace

/** @brief A root is driven, never solved: it holds its position whatever gravity does. */
COOPA_TEST(roots_are_pinned_and_the_rest_of_the_strand_is_not) {
    hair::StrandParams params;
    hair::StrandSet set = hair::make_strands({straight_strand(glm::vec3(0.0f))}, params);
    expect(set.particles.size() == 8u, "hair: one strand of eight points is eight particles");
    expect(set.particles[0].inv_mass == 0.0f, "hair: the root has zero inverse mass -- it is pinned");
    expect(set.particles[1].inv_mass > 0.0f, "hair: and nothing else is");
    expect(set.stretch.size() == 7u, "hair: with one distance constraint per segment");

    const glm::vec3 root = set.particles[0].position;
    run(set, 60);
    expect_near(glm::length(set.particles[0].position - root), 0.0f, 1e-5f,
                "hair: the root has not moved a millimetre under a second of gravity");
    // Only a few millimetres: the DEFAULT params hold a groom fairly stiff, which is the point
    // of the global shape constraint. That it moved at all is what this proves.
    expect(set.particles[7].position.z < root.z - 0.005f,
           "hair: while the tip has fallen -- the strand is simulated, not frozen");
}

/** @brief Hair is inextensible: a strand hanging under gravity keeps its length. */
COOPA_TEST(a_hanging_strand_does_not_stretch) {
    hair::StrandParams params;
    params.global_shape = 0.0f;   // nothing holding it up but the distance constraints
    params.local_shape = 0.0f;
    hair::StrandSet set = hair::make_strands({straight_strand(glm::vec3(0.0f), 10, 0.05f)}, params);
    const float rest = strand_length(set, 0);
    expect_near(rest, 0.45f, 1e-4f, "hair: nine segments of 5 cm is 45 cm of hair");

    run(set, 180);
    const float hanging = strand_length(set, 0);
    expect(std::fabs(hanging - rest) < 0.01f * rest,
           "hair: three seconds of gravity changes its length by under one percent -- at zero "
           "compliance XPBD is inextensible, which a velocity-level spring could not be");
    expect(set.particles[9].position.z < -0.3f, "hair: and it really did hang downward");
}

/** @brief The global shape constraint is what makes a groom keep its comb. */
COOPA_TEST(the_global_shape_constraint_holds_the_groom_up) {
    const auto settled_tip_height = [](float global_shape) {
        hair::StrandParams params;
        params.global_shape = global_shape;
        params.local_shape = 0.0f;
        hair::StrandSet set = hair::make_strands({straight_strand(glm::vec3(0.0f), 8, 0.05f)}, params);
        run(set, 180);
        return set.particles[7].position.z;
    };

    const float limp = settled_tip_height(0.0f);
    const float stiff = settled_tip_height(0.6f);
    expect(limp < -0.1f, "hair: with no shape constraint the strand swings down and hangs");
    expect(stiff > limp + 0.1f,
           "hair: with one it stands back up toward where it was combed -- this is the stiffness "
           "knob, and without it every groom collapses the moment gravity touches it");
}

/** @brief Its falloff frees the tips, so hair swings from the ends rather than as a board. */
COOPA_TEST(the_global_constraint_falls_off_toward_the_tip) {
    hair::StrandParams params;
    params.global_shape = 0.5f;
    params.global_shape_falloff = 2.0f;
    params.local_shape = 0.0f;
    hair::StrandSet set = hair::make_strands({straight_strand(glm::vec3(0.0f), 9, 0.05f)}, params);
    std::vector<glm::vec3> start;
    for (const auto& p : set.particles) start.push_back(p.position);

    run(set, 180);
    const float near_root = glm::length(set.particles[2].position - start[2]);
    const float at_tip    = glm::length(set.particles[8].position - start[8]);
    expect(at_tip > near_root * 2.0f,
           "hair: the tip moves much further than the root end -- a uniform constraint would "
           "make the whole strand swing as one rigid board");
}

/** @brief The local shape constraint holds a bend; a distance-based bend could not. */
COOPA_TEST(the_local_shape_constraint_holds_a_curl) {
    const auto settled_corner_y = [](float local_shape) {
        hair::StrandParams params;
        params.global_shape = 0.0f;   // isolate the local constraint completely
        params.local_shape = local_shape;
        hair::StrandSet set = hair::make_strands({bent_strand(glm::vec3(0.0f))}, params);
        run(set, 240);
        // How far the tip still reaches along +Y -- the turn the strand was grown with.
        return std::fabs(set.particles[set.particles.size() - 1].position.y);
    };

    const float straightened = settled_corner_y(0.0f);
    const float held = settled_corner_y(0.9f);
    expect(held > straightened + 0.02f,
           "hair: the bend survives gravity when the local constraint holds it, and unwinds when "
           "it does not -- a distance constraint between i and i+2 cannot tell the difference, "
           "because a helix has the same such distance at every angle");
}

/** @brief A moving root carries the groom with it instead of leaving it behind. */
COOPA_TEST(moving_the_root_carries_the_strand) {
    hair::StrandParams params;
    params.global_shape = 0.4f;
    hair::StrandSet set = hair::make_strands({straight_strand(glm::vec3(0.0f))}, params);

    hair::StrandSolverScratch scratch;
    for (int i = 0; i < 120; ++i) {
        const float x = 0.01f * static_cast<float>(i);
        hair::set_strand_roots(set, {glm::vec3(x, 0.0f, 0.0f)});
        hair::solve_strands(set, {}, k_gravity, 4, 1, 1.0f / 60.0f, scratch);
    }
    expect_near(set.particles[0].position.x, 1.19f, 1e-3f, "hair: the root went where it was told");
    expect(set.particles[7].position.x > 0.6f,
           "hair: and the tip followed rather than being left at the origin");
    expect(std::fabs(strand_length(set, 0) - 0.35f) < 0.01f,
           "hair: without the strand stretching to get there");
    // The real inextensibility claim under motion: however the root is dragged, no part of the
    // strand can end up further from it than the strand is long.
    expect(glm::length(set.particles[7].position - set.particles[0].position) <= 0.35f + 0.005f,
           "hair: and no part of it is ever further from the root than the hair is long");
}

/** @brief A strand never ends up inside a collider it falls onto. */
COOPA_TEST(a_strand_stays_out_of_a_collider) {
    collision::Shape sphere = collision::Shape::make_sphere(0.3f);
    cloth::ClothCollider collider = physxtest::still_collider(sphere);

    hair::StrandParams params;
    params.global_shape = 0.0f;   // let it fall freely onto the sphere
    params.local_shape = 0.0f;
    params.thickness = 0.02f;
    // Rooted directly above the sphere so the strand drapes over it.
    hair::StrandSet set = hair::make_strands({straight_strand(glm::vec3(0.0f, 0.0f, 0.6f), 12, 0.05f)}, params);

    run(set, 300, {collider});

    float deepest = 1e9f;
    for (const cloth::ClothParticle& p : set.particles) {
        deepest = std::min(deepest, glm::length(p.position) - 0.3f);
    }
    expect(deepest > -0.005f,
           "hair: no particle is inside the sphere -- collisions run LAST of the position "
           "stages, so nothing can push one back in after them");
}

/** @brief Two runs from the same state produce bit-identical results. */
COOPA_TEST(the_solve_is_deterministic) {
    const auto simulate = []() {
        hair::StrandParams params;
        std::vector<hair::StrandDesc> descs;
        for (int i = 0; i < 6; ++i) {
            descs.push_back(straight_strand(glm::vec3(0.05f * static_cast<float>(i), 0.0f, 0.0f)));
        }
        hair::StrandSet set = hair::make_strands(descs, params);
        run(set, 90);
        std::vector<glm::vec3> out;
        for (const auto& p : set.particles) out.push_back(p.position);
        return out;
    };

    const std::vector<glm::vec3> a = simulate();
    const std::vector<glm::vec3> b = simulate();
    expect(a.size() == b.size() && !a.empty(), "hair: both runs produced the same groom");
    bool identical = true;
    for (std::size_t i = 0; i < a.size() && identical; ++i) identical = a[i] == b[i];
    expect(identical,
           "hair: bit-identical, which world_state_hash() requires -- the constraint batches are "
           "particle-disjoint so visiting order cannot change the result");
}

/** @brief Constraint batches really are particle-disjoint, which is what licenses that. */
COOPA_TEST(constraint_batches_share_no_particle) {
    std::vector<hair::StrandDesc> descs;
    for (int i = 0; i < 4; ++i) {
        descs.push_back(straight_strand(glm::vec3(0.1f * static_cast<float>(i), 0.0f, 0.0f), 9));
    }
    const hair::StrandSet set = hair::make_strands(descs, {});
    expect(set.stretch_batches.size() == 2u, "hair: a chain splits into exactly two parities");

    uint32_t covered = 0;
    for (const cloth::ConstraintBatch& batch : set.stretch_batches) {
        std::vector<uint32_t> seen;
        for (uint32_t ci = batch.begin; ci < batch.end; ++ci) {
            seen.push_back(set.stretch[ci].a);
            seen.push_back(set.stretch[ci].b);
        }
        std::sort(seen.begin(), seen.end());
        expect(std::adjacent_find(seen.begin(), seen.end()) == seen.end(),
               "hair: no particle appears twice within a batch");
        covered += batch.end - batch.begin;
    }
    expect(covered == set.stretch.size(), "hair: and every constraint is in exactly one batch");
}

/** @brief A groom that has stopped moving stops being solved. */
COOPA_TEST(a_settled_groom_goes_to_sleep_in_the_world) {
    PhysicsWorld world;
    hair::StrandParams params;
    params.sleep_time = 0.2f;
    params.sleep_threshold = 0.05f;
    const hair::StrandSetId id = world.add_strands(hair::make_strands({straight_strand(glm::vec3(0.0f))}, params));
    expect(world.is_valid(id), "hair: the world took the groom");
    expect(world.strand_set_count() == 1u, "hair: and counts it");

    for (int i = 0; i < 400; ++i) world.step(1.0f / 60.0f);
    const hair::StrandSet* set = world.get_strands(id);
    expect(set != nullptr && !set->awake,
           "hair: a groom that stopped moving stops being solved, as a resting cloth does");

    world.remove_strands(id);
    expect(!world.is_valid(id), "hair: and the handle goes stale when it is destroyed");
    expect(world.get_strands(id) == nullptr, "hair: with no way to reach the recycled slot");
}

/** @brief An empty or degenerate set is refused rather than half-created. */
COOPA_TEST(degenerate_input_builds_nothing) {
    expect(hair::make_strands({}, {}).empty(), "hair: no descriptors, no groom");

    hair::StrandDesc single;
    single.points.push_back(glm::vec3(0.0f));
    expect(hair::make_strands({single}, {}).empty(),
           "hair: one point is a position, not a strand");

    PhysicsWorld world;
    expect(!world.add_strands(hair::StrandSet{}).is_valid(),
           "hair: and the world refuses an empty set rather than giving out a handle to nothing");
}

/**
 * @file strand.h
 * @brief Data types for the XPBD hair solver: strands, their rest pose, tunables and the set
 *        itself.
 *
 * A strand is a chain of particles pinned at the root, which is a shape the cloth solver already
 * has most of the machinery for -- so this module REUSES `cloth::ClothParticle`,
 * `cloth::ClothConstraint`, `cloth::ConstraintBatch` and, for collision,
 * `cloth::ClothCollider` and `cloth::project_particle()` verbatim. Only two things are genuinely
 * new, and they are the reason a strand is not simply a one-wide sheet:
 *
 *   1. **The global shape constraint** pulls each particle toward its rest position carried by the
 *      strand's CURRENT root frame. It is what makes a groom return to the shape it was combed
 *      into rather than hanging limp the first time gravity touches it, and it is the cheap
 *      stiffness knob: strongest at the root, fading toward the tip, so tips stay free.
 *   2. **The local shape constraint** restores each segment's rest direction relative to the
 *      segment before it. It is what holds a curl. Cloth's distance-based bend constraint cannot:
 *      a distance between particles i and i+2 preserves STRAIGHTNESS, and a helix has a constant
 *      such distance at every angle, so a curl would unwind freely under it.
 *
 * Both are from AMD's TressFX, which is the reference realtime implementation.
 *
 * Like cloth, this is **one-way coupled**: a strand is pushed by the rigid world and never pushes
 * back. A 0.1 g hair destabilising the head it hangs off would be absurd, and it is also what
 * lets the whole set be solved once per frame, after the rigid substeps, against final poses.
 *
 * @see strand_builder.h for turning polylines into a set.
 * @see strand_solver.h   for the substep loop.
 * @see ../cloth/cloth.h  for the particle/constraint types this shares.
 */

#ifndef PHYSXCOOPA_HAIR_STRAND_H
#define PHYSXCOOPA_HAIR_STRAND_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include <physxcoopa/cloth/cloth.h>
#include <physxcoopa/dynamics/body.h>
#include <physxcoopa/geometry/aabb.h>

namespace coopa {
namespace physx {
namespace hair {

/**
 * @struct StrandSetId
 * @brief Generational handle to a strand set owned by PhysicsWorld.
 *
 * Same index+generation scheme as `cloth::ClothId` and `dynamics::BodyId`: a stale handle whose
 * slot has been recycled compares unequal and fails `PhysicsWorld::is_valid()`, rather than
 * silently addressing whatever groom now lives there.
 */
struct StrandSetId {
    static constexpr uint32_t k_invalid_index = 0xFFFFFFFFu;

    uint32_t index = k_invalid_index;
    uint32_t generation = 0;

    bool is_valid() const { return index != k_invalid_index; }
    bool operator==(const StrandSetId& o) const { return index == o.index && generation == o.generation; }
    bool operator!=(const StrandSetId& o) const { return !(*this == o); }
};

/**
 * @struct Strand
 * @brief One hair: a run of particles in the set's flat array, and where its root is now.
 *
 * The root is written from outside each frame (the scalp's transform, or a bone's), exactly as a
 * `cloth::ClothAnchor`'s world target is -- and `prev_root_*` is lerped FROM across the substeps
 * for the same reason: a head that turns quickly would otherwise yank every root in one whole-
 * frame jump at the top of the first substep and then hold still, which the distance constraints
 * turn into a visible whip-crack down the strand.
 */
struct Strand {
    uint32_t first = 0;  ///< Index of this strand's ROOT particle in `StrandSet::particles`.
    uint32_t count = 0;  ///< Particles in the chain, root included.

    glm::vec3 root_position{0.0f};
    glm::quat root_orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 prev_root_position{0.0f};
    glm::quat prev_root_orientation{1.0f, 0.0f, 0.0f, 0.0f};
};

/**
 * @struct StrandParams
 * @brief Per-set tunables. The ones a strand shares with cloth keep cloth's names and defaults.
 */
struct StrandParams {
    /** @brief Stretch compliance in m/N -- inverse stiffness. 0 is fully inextensible, which is
     *         right for hair and is stable in XPBD at exactly zero. */
    float stretch_compliance = 0.0f;

    /** @brief How hard a particle is pulled back to its combed position in the root's frame,
     *         0..1 per iteration. This is the main stiffness control: 0 is limp hair that forgets
     *         its groom the moment it moves, 1 is a wig that barely reacts. */
    float global_shape = 0.22f;

    /** @brief How fast `global_shape` fades from root to tip. 1 is linear; higher frees the tips
     *         sooner, which is what makes hair swing from the ends rather than as a board. */
    float global_shape_falloff = 1.6f;

    /** @brief How hard each segment's rest ANGLE to the segment before it is restored, 0..1.
     *         What holds a curl; 0 lets hair hang straight whatever it was grown as. */
    float local_shape = 0.35f;

    /** @brief Velocity damping, applied implicitly as v *= 1/(1 + damping*h) -- the same stable
     *         form cloth uses, rather than v *= (1 - damping*h), which sign-flips past damping*h = 1. */
    float damping = 0.08f;

    /** @brief Collision thickness in metres: particles are held this far off a collider. Hair is
     *         a curve with no volume of its own, so without an offset the strand's surface would
     *         sit exactly on the skin and z-fight it. */
    float thickness = 0.01f;

    /** @brief Coulomb friction at strand-collider contacts, applied positionally. 1 grips, so
     *         hair resting on a shoulder stays on it rather than sliding off. */
    float friction = 0.4f;

    float gravity_scale = 1.0f;
    /** @brief Hard speed clamp -- a safety net against one bad frame (a teleporting head)
     *         launching the groom. */
    float max_velocity = 50.0f;
    glm::vec3 external_acceleration{0.0f};

    /** @brief Wind velocity in m/s. Written each frame by the engine's WindSystem, exactly as a
     *         cloth's is, so hair and a cape in the same gust agree. */
    glm::vec3 wind{0.0f};
    float wind_turbulence = 0.1f;
    /** @brief Drag against the air, which is what makes hair trail behind a moving head. */
    float air_drag = 0.6f;

    uint32_t substeps = 0;    ///< 0: take PhysicsWorld's.
    uint32_t iterations = 0;  ///< 0: take PhysicsWorld's.

    /** @brief Speed below which a set is considered settled, m/s. */
    float sleep_threshold = 0.02f;
    /** @brief Seconds below that before it stops being solved at all. */
    float sleep_time = 0.6f;

    /** @brief Collision layer, matched against each shape's through the world's LayerMatrix --
     *         so a scene's `ignore_layer_collisions` governs hair too. */
    uint32_t layer = 0;
};

/**
 * @struct StrandSet
 * @brief A whole groom's simulated guides: particles, constraints and the rest pose.
 *
 * Flat arrays with per-strand ranges, not a vector per strand: a groom is many short chains, and
 * one allocation keeps the solve's memory access linear. The same choice `cloth::Cloth` makes.
 */
struct StrandSet {
    std::vector<cloth::ClothParticle>  particles;
    std::vector<cloth::ClothConstraint> stretch;
    /** @brief Pairwise particle-DISJOINT ranges of `stretch`, so a batch can be solved in any
     *         order on any thread with a bit-identical result -- which
     *         `PhysicsWorld::world_state_hash()` requires. Strands are independent and a chain
     *         splits by index parity, so there are exactly two. */
    std::vector<cloth::ConstraintBatch> stretch_batches;

    /** @brief Rest position of each particle in ITS STRAND'S ROOT FRAME. Parallel to `particles`.
     *         The global shape constraint carries these by the root's live orientation. */
    std::vector<glm::vec3> rest_local;
    /** @brief Rest offset from the previous particle, in the rest pose. Parallel to `particles`;
     *         entry `first` (a root) is zero. The local shape constraint restores the ANGLE
     *         between consecutive offsets, which is what a curl is. */
    std::vector<glm::vec3> rest_offset;

    std::vector<Strand> strands;

    /** @brief When non-empty, the ONLY bodies this set collides against -- the broadphase is not
     *         queried at all. Written by the engine each frame from resolved object names; empty
     *         (the default) collides with every nearby shape, layer-filtered, exactly as cloth
     *         does. */
    std::vector<dynamics::BodyId> collide_bodies;

    StrandParams   params;
    geometry::AABB bounds;
    bool  valid = false;    ///< Slot liveness, set by PhysicsWorld.
    bool  enabled = true;
    bool  awake = true;
    float sleep_timer = 0.0f;
    float time = 0.0f;      ///< Accumulated sim time; drives deterministic wind turbulence.

    bool empty() const { return particles.empty() || strands.empty(); }
};

/// @brief World AABB over every particle. Inverted (empty) for an empty set, which
///        `AABB::overlaps()` then rejects against everything.
inline geometry::AABB compute_bounds(const StrandSet& set) {
    geometry::AABB out;
    for (const cloth::ClothParticle& p : set.particles) {
        out.min = glm::min(out.min, p.position);
        out.max = glm::max(out.max, p.position);
    }
    return out;
}

/// @brief Largest particle speed, m/s. Drives the sleep decision and the broadphase query margin.
inline float max_particle_speed(const StrandSet& set) {
    float max2 = 0.0f;
    for (const cloth::ClothParticle& p : set.particles) {
        max2 = glm::max(max2, glm::dot(p.velocity, p.velocity));
    }
    return std::sqrt(max2);
}

} // namespace hair
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_HAIR_STRAND_H

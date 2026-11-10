/**
 * @file cloth.h
 * @brief Data types for the XPBD cloth solver: particles, distance constraints, tethers,
 *        anchors, tunables, and the Cloth aggregate itself.
 *
 * Cloth is deliberately NOT expressed as rigid bodies. A dynamics::Body carries an orientation,
 * two inertia tensors, sleep state, prev_ and last_written_ poses and a broadphase proxy -- over
 * 200 bytes and one AABB-tree node each. A 25x25 sheet would therefore cost 625 bodies, 625
 * tree proxies and 625 entries in every IslandUnionFind rebuild, for particles that have no
 * rotation, no inertia and no pairwise contacts with each other. Instead a Cloth owns a flat
 * ClothParticle array and the whole sheet shares ONE broadphase query per substep, mirroring how
 * collision::Shape was split out of dynamics::Body rather than folded into it.
 *
 * The solver is small-substep XPBD (Macklin et al. 2019, "Small Steps in Physics Simulation"),
 * not the velocity-level sequential impulses dynamics/solver.h uses for rigid contacts. Three
 * reasons, in order of weight:
 *
 *   1. Stiffness. Cloth wants to be nearly inextensible. A velocity-level spring reaching that
 *      stiffness is exactly the stiff-ODE case semi-implicit Euler handles worst -- it needs
 *      either a tiny timestep or iteration counts far past the 16 velocity_iterations the rigid
 *      solver budgets. XPBD projects positions directly and is unconditionally stable at any
 *      stiffness, including infinite (compliance == 0).
 *   2. Resolution independence. XPBD's compliance is a physical material parameter (inverse
 *      stiffness, m/N), so a 15x15 and a 45x45 sheet authored with the same compliance hang the
 *      same way. A raw spring constant does not survive a resolution change.
 *   3. It is what the reference implementation does. Unity's Cloth is NvCloth, which is PBD.
 *
 * The cost is that cloth does not push rigid bodies back (one-way coupling) -- also exactly what
 * Unity does, and what keeps a 1 g sheet from destabilising a 50 kg body it is draped over.
 *
 * @see cloth_builder.h  for constructing a grid sheet and its constraint batches.
 * @see cloth_solver.h   for the substep loop itself.
 * @see cloth_collision.h for particle-vs-collision::Shape projection.
 */

#ifndef PHYSXCOOPA_CLOTH_CLOTH_H
#define PHYSXCOOPA_CLOTH_CLOTH_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include <physxcoopa/geometry/aabb.h>
#include <physxcoopa/dynamics/body.h>

namespace coopa {
namespace physx {
namespace cloth {

/**
 * @struct ClothId
 * @brief Generational handle to a cloth owned by PhysicsWorld.
 *
 * Same index+generation scheme as dynamics::BodyId and dynamics::JointId: a stale handle whose
 * slot has since been recycled compares unequal and fails PhysicsWorld::is_valid(), instead of
 * silently addressing whatever cloth now lives in that slot.
 */
struct ClothId {
    static constexpr uint32_t k_invalid_index = 0xFFFFFFFFu;

    uint32_t index = k_invalid_index; /**< Slot index into PhysicsWorld's cloth array. */
    uint32_t generation = 0;          /**< Slot reuse counter at the time this handle was issued. */

    /** @brief True if this handle names a slot at all (says nothing about that slot's liveness). */
    bool is_valid() const { return index != k_invalid_index; }

    bool operator==(const ClothId& o) const { return index == o.index && generation == o.generation; }
    bool operator!=(const ClothId& o) const { return !(*this == o); }
};

/**
 * @struct ClothParticle
 * @brief One point mass in a cloth sheet.
 *
 * `prev_position` is the position at the START of the current substep, not the previous frame:
 * XPBD derives velocity from it as (position - prev_position) / h after the constraint
 * projections, which is what makes a positional correction show up as a real velocity change
 * rather than being silently discarded next substep.
 */
struct ClothParticle {
    glm::vec3 position{0.0f};      /**< Current world-space position. */
    glm::vec3 prev_position{0.0f}; /**< World position at the start of this substep. */
    glm::vec3 velocity{0.0f};      /**< World-space velocity, carried across substeps. */
    float inv_mass = 1.0f;         /**< 1/mass; exactly 0.0 means pinned (anchored or fixed). */
};

/**
 * @struct ClothConstraint
 * @brief One XPBD distance constraint C = |pa - pb| - rest_length, used for both stretch and bend.
 *
 * `lambda` is the constraint's Lagrange multiplier accumulator. It lives on the struct rather
 * than in a warm-start cache keyed by feature id (the scheme dynamics/solver.h uses for
 * contacts) for the same reason dynamics::HingeJoint keeps its own impulse accumulators: a
 * cloth constraint is a persistent object with a stable identity, never rediscovered by a
 * narrowphase, so there is nothing to key a lookup on and nothing to rebuild.
 *
 * Note that `lambda` is reset to zero at the top of every substep, not carried across them --
 * that is the XPBD contract (the multiplier is per-timestep, and alpha is scaled by 1/h^2).
 */
struct ClothConstraint {
    uint32_t a = 0;           /**< First particle index. */
    uint32_t b = 0;           /**< Second particle index. */
    float rest_length = 0.0f; /**< Target separation, captured from the rest pose. */
    float lambda = 0.0f;      /**< Lagrange multiplier accumulator for the current substep. */
};

/**
 * @struct ClothTether
 * @brief A one-sided maximum-distance constraint from a particle to an anchored particle.
 *
 * Also called a "long-range attachment" (Kim et al. 2012). Distance constraints propagate
 * stretch information only one edge per iteration, so at 4 substeps x 1 iteration a 25-particle-
 * wide sheet would need ~25 substeps for a tug at the pinned edge to reach the far corner --
 * which reads on screen as visible rubber-band stretching whenever the anchor body moves
 * quickly. A tether short-circuits that: it is a single constraint spanning the whole geodesic
 * path, so the far corner can never exceed its unstretched distance from the anchor no matter
 * how few iterations ran. One-sided, so it never pulls the sheet taut when it is slack.
 */
struct ClothTether {
    uint32_t particle = 0;    /**< The tethered particle. */
    uint32_t anchor = 0;      /**< Index of the anchored particle it is measured against. */
    float max_length = 0.0f;  /**< Geodesic rest distance to `anchor`, times params.tether_scale. */
};

/**
 * @struct ClothAnchor
 * @brief Pins a particle to a rigid body, so the sheet follows that body's motion.
 *
 * Anchored particles have inv_mass == 0, so every constraint treats them as infinitely massive
 * and they are never moved by the solve -- their position is written outright from the body's
 * pose at the top of each substep. This is one-way by construction: the body drives the cloth
 * and never feels it, which is what makes pinning a 1 g sheet to a 50 kg body stable.
 *
 * If `body` goes stale (the body was destroyed), the anchor degrades to a static pin at
 * `world_position` rather than teleporting the sheet to the origin.
 */
struct ClothAnchor {
    uint32_t particle = 0;              /**< Particle index held by this anchor. */
    dynamics::BodyId body;              /**< Body to follow; invalid/stale means a static pin. */
    glm::vec3 local_position{0.0f};     /**< Pin point in `body`'s local frame. */

    /** @brief This frame's resolved world target, written by PhysicsWorld before the solve.
     *
     *  Resolving the body pose OUTSIDE the solver is what keeps cloth_solver.h free of any
     *  PhysicsWorld dependency -- and it is resolved once per FRAME rather than once per cloth
     *  substep because the rigid state is frozen for the whole of it anyway (cloth runs after the
     *  frame's rigid substeps; see PhysicsWorld::step_cloths_). Doubles as the static-pin position
     *  when `body` is invalid. */
    glm::vec3 world_position{0.0f};

    /** @brief The previous frame's target, which the solver lerps FROM across its cloth substeps.
     *         Without it a fast-moving anchor body would yank its particles in one whole-frame
     *         jump at the top of the first cloth substep and then hold still for the rest, which
     *         the distance constraints turn into a visible whip-crack down the sheet. Lerping
     *         spreads that motion evenly, at no cost. */
    glm::vec3 prev_world_position{0.0f};
};

/**
 * @struct ClothParams
 * @brief Per-cloth tunables. Field names and defaults track Unity's Cloth component where an
 *        equivalent exists, so a value copied from a Unity inspector behaves recognisably here.
 */
struct ClothParams {
    /** @brief Stretch compliance in m/N -- inverse stiffness. 0 is fully inextensible and is the
     *         right default: XPBD is stable at zero compliance, unlike a spring at infinite k.
     *         Raise toward 1e-4 for a stretchy knit. */
    float stretch_compliance = 0.0f;

    /** @brief Bend compliance in m/N. Non-zero by default because a perfectly rigid bend makes a
     *         sheet behave like sheet metal -- it will not crease or fold over a sphere. 5e-5
     *         reads as a mid-weight fabric; 1e-3 is silk, 1e-6 is leather. */
    float bend_compliance = 5e-5f;

    /** @brief Velocity damping, applied implicitly as v *= 1/(1 + damping*h) -- the same form
     *         dynamics::integrate_forces() uses for Body::linear_drag, and for the same reason:
     *         the explicit form v *= (1 - damping*h) goes unstable (sign-flips the velocity) once
     *         damping*h exceeds 1, which a user typing a large number into a scene file will hit. */
    float damping = 0.05f;

    /** @brief Collision thickness in metres: particles are held this far off a collider's surface.
     *         The sheet is a zero-thickness triangle mesh, so without an offset the RENDERED
     *         surface sits exactly on the collider and z-fights it. Unity calls this the same. */
    float thickness = 0.03f;

    /** @brief Coulomb friction coefficient at cloth-collider contacts, applied positionally (see
     *         cloth_solver.h). 0 slides freely off a sphere; 1 grips. */
    float friction = 0.5f;

    /** @brief Multiplier on the world gravity vector; 0 makes a weightless banner. */
    float gravity_scale = 1.0f;

    /** @brief Hard speed clamp, mirroring PhysicsConfig::max_linear_velocity. A safety net
     *         against a single bad frame (a teleporting anchor body) launching the sheet. */
    float max_velocity = 100.0f;

    /** @brief Constant acceleration added to gravity. Unity's "External Acceleration". */
    glm::vec3 external_acceleration{0.0f};

    /** @brief Wind velocity in m/s. Wind is a VELOCITY, not an acceleration: the aerodynamic
     *         force below depends on the cloth's motion RELATIVE to the air, so a sheet already
     *         moving with the wind feels nothing from it -- which is what makes a banner settle
     *         into a steady flutter instead of accelerating forever. */
    glm::vec3 wind{0.0f};

    /** @brief Peak magnitude of a smooth deterministic gust added to `wind`. Unity's "Random
     *         Acceleration". Deterministic (driven by the solver's own time accumulator, not
     *         rand()) so the determinism harness still passes. */
    float wind_turbulence = 0.0f;

    /** @brief Aerodynamic drag coefficient -- the component of the per-triangle force along the
     *         face normal. This is what makes a sheet parachute rather than knife downward. */
    float air_drag = 0.02f;

    /** @brief Aerodynamic lift coefficient -- the tangential component. What makes a falling
     *         sheet slip sideways and flutter instead of dropping straight. */
    float air_lift = 0.02f;

    /** @brief Enables particle-vs-particle separation so the sheet cannot pass through itself.
     *         Off by default: it is the single most expensive stage (a spatial hash rebuild plus
     *         a 27-cell scan per particle per substep) and a sheet draped over a convex body
     *         rarely needs it. Turn on for anything that folds onto itself. */
    bool self_collision = false;

    /** @brief Minimum particle separation enforced by self-collision. 0 means "derive it at build
     *         time" -- cloth_builder.h sets it to 0.6x the grid's rest spacing, the largest value
     *         that cannot fight the structural constraints it shares particles with. */
    float self_distance = 0.0f;

    /** @brief Tether slack factor. 1.0 is a perfectly inextensible tether, which looks rigid;
     *         1.02 allows 2% stretch so the sheet still reads as fabric while its far corners
     *         stay put. Raise to disable tethers in practice, or clear Cloth::tethers outright. */
    float tether_scale = 1.02f;

    /** @brief Cloth substeps per physics substep; 0 inherits PhysicsConfig::cloth_substeps. */
    uint32_t substeps = 0;

    /** @brief Constraint iterations per cloth substep; 0 inherits PhysicsConfig::cloth_iterations.
     *         1 is right for small-substep XPBD -- the paper's central result is that spending a
     *         fixed budget on more substeps converges better than on more iterations. */
    uint32_t iterations = 0;

    /** @brief Max particle speed (m/s) that still counts as "settled" for sleeping. */
    float sleep_threshold = 0.05f;

    /** @brief Seconds below `sleep_threshold` before the cloth stops simulating. */
    float sleep_time = 0.5f;

    /** @brief Collision layer index, tested against each shape's layer through the world's
     *         LayerMatrix -- the same matrix rigid pairs use, so one scene-level setting governs
     *         both. */
    uint32_t layer = 0;
};

/**
 * @struct ConstraintBatch
 * @brief A half-open [begin, end) range of constraints whose particle sets are pairwise disjoint.
 *
 * This is the whole parallelism story for cloth. Gauss-Seidel over an unordered constraint list
 * is order-dependent, so parallelising it would break the determinism rule PhysicsWorld is built
 * on (see world_state_hash()'s doc). But if no two constraints in a batch share a particle, no
 * two can race and the result does not depend on the order they are visited in -- so a batch can
 * be handed to JobEngine::parallel_for and still produce a bit-identical hash. cloth_builder.h
 * emits four such batches for a grid's structural edges by parity, and the solve walks batches in
 * a fixed order, parallelising only WITHIN each one.
 */
struct ConstraintBatch {
    uint32_t begin = 0; /**< First constraint index in the batch. */
    uint32_t end = 0;   /**< One past the last constraint index. */
};

/**
 * @struct Cloth
 * @brief A complete cloth sheet: particles, constraints, anchors, render topology and tunables.
 *
 * Owned by PhysicsWorld in a generational slot array (see PhysicsWorld::add_cloth) and stepped by
 * PhysicsWorld::step() once per FRAME, after that frame's rigid substeps -- so collisions are
 * resolved against the bodies' final poses for the frame, which are also the poses that frame
 * draws. See PhysicsWorld::step_cloths_() for why the frame clock rather than the substep grid.
 */
struct Cloth {
    std::vector<ClothParticle> particles;  /**< Point masses, row-major for a grid sheet. */
    std::vector<ClothConstraint> stretch;  /**< Structural (and optionally shear) edges. */
    std::vector<ClothConstraint> bend;     /**< Two-apart edges resisting curvature. */
    std::vector<ConstraintBatch> stretch_batches; /**< Disjoint ranges over `stretch`. */
    std::vector<ConstraintBatch> bend_batches;    /**< Disjoint ranges over `bend`. */
    std::vector<ClothTether> tethers;      /**< Long-range max-distance constraints. */
    std::vector<ClothAnchor> anchors;      /**< Particles pinned to rigid bodies. */

    /** @brief Triangle index list (3 indices per triangle) over `particles`.
     *
     *  Serves two consumers at once: the solver's aerodynamic pass needs face normals and areas,
     *  and a renderer needs exactly this list as its index buffer. Keeping one copy here rather
     *  than one per consumer is what lets toyengine's ClothRenderer build its GPU mesh with a
     *  single memcpy and then only ever rewrite positions/normals. */
    std::vector<uint32_t> triangles;

    /** @brief Grid dimensions when this cloth came from make_grid_cloth(), else 0.
     *
     *  Not used by the solver at all -- carried so a renderer can derive UVs and row-aligned
     *  tangents without re-deriving the layout, and so tests can index particles as (x,y). */
    uint32_t columns = 0;
    uint32_t rows = 0;

    ClothParams params;          /**< Tunables; see ClothParams. */
    geometry::AABB bounds;       /**< World AABB over `particles`, refreshed after each step. */
    bool valid = false;          /**< Slot liveness, set by PhysicsWorld. */
    bool enabled = true;         /**< Author-facing on/off; a disabled cloth is skipped entirely. */
    bool awake = true;           /**< False once settled; see ClothParams::sleep_time. */
    float sleep_timer = 0.0f;    /**< Seconds spent below the sleep threshold. */
    float time = 0.0f;           /**< Accumulated sim time, drives deterministic wind turbulence. */
};

/** @brief Recomputes the world AABB over a cloth's particles. Returns an inverted (empty) AABB
 *         for a cloth with no particles, which AABB::overlaps() then rejects against everything. */
inline geometry::AABB compute_bounds(const Cloth& c) {
    geometry::AABB out;
    for (const ClothParticle& p : c.particles) {
        out.min = glm::min(out.min, p.position);
        out.max = glm::max(out.max, p.position);
    }
    return out;
}

/** @brief Largest particle speed in the cloth, in m/s. Drives both the sleep decision and the
 *         broadphase query margin (a fast sheet must query a correspondingly wider box). */
inline float max_particle_speed(const Cloth& c) {
    float max2 = 0.0f;
    for (const ClothParticle& p : c.particles) {
        max2 = glm::max(max2, glm::dot(p.velocity, p.velocity));
    }
    return std::sqrt(max2);
}

} // namespace cloth
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_CLOTH_CLOTH_H

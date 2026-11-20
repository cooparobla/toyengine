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
 *   2. Resolution independence. XPBD's compliance is a material parameter rather than a spring
 *      constant, so a 15x15 and a 45x45 sheet authored with the same compliance hang the same
 *      way -- but only once the constraint's own inverse mass is divided back out of it, which
 *      is why every compliance here is per unit mass (see ClothParams::bend_compliance). A raw
 *      spring constant does not survive a resolution change, and neither does a raw m/N
 *      compliance, since halving the spacing quarters the particle mass.
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
 * @struct ClothSkinChannels
 * @brief Per-particle painted channels for cloth driven by a skinned mesh ("character cloth").
 *
 * All arrays are either empty or sized to Cloth::particles. An all-empty struct (the grid-cloth
 * case) disables every skin stage in the solver outright -- see Cloth::skin. Field semantics
 * track Unreal's cloth paint channels, so values copied from an Unreal asset behave recognisably:
 *
 *   - `max_distance` is the radius (metres) of a sphere around the particle's skinned target
 *     position that the simulated particle may move within. A value at or below epsilon means
 *     the particle is fully skin-driven: the builder gives it inv_mass == 0 and the solver
 *     writes its position from the target each substep, exactly like an anchored particle.
 *   - `backstop_distance`/`backstop_radius` place a sphere of radius `backstop_radius` at
 *     `target - normal * (backstop_distance + backstop_radius)` that the particle is pushed out
 *     of -- a cheap stand-in for the body under the clothing, keeping fabric off the skin
 *     without a collider for every limb. A zero radius disables the backstop for that particle.
 *   - `anim_drive` in [0, 1] scales ClothParams::anim_drive_compliance: 0 disables the drive
 *     spring, 1 pulls at full authored stiffness toward the skinned pose.
 */
struct ClothSkinChannels {
    std::vector<float> max_distance;      /**< Allowed travel from the skinned target, metres. */
    std::vector<float> backstop_distance; /**< Backstop sphere surface offset behind the skin, metres. */
    std::vector<float> backstop_radius;   /**< Backstop sphere radius, metres; 0 disables. */
    std::vector<float> anim_drive;        /**< Painted gain on ClothParams::anim_drive_compliance, 0..1. */

    /** @brief True when no channel data is present; every skin solve stage is skipped. */
    bool empty() const { return max_distance.empty(); }
};

/**
 * @struct ClothSkinTargets
 * @brief This frame's skinned pose targets, written by the caller before each solve.
 *
 * The caller (toyengine's cloth system) skins the cloth's rest positions by the rig's current
 * pose and stores the result here, exactly as ClothAnchor::world_position is resolved outside
 * the solver -- which is what keeps cloth_solver.h free of any rig or scene dependency. The
 * solver lerps from the `prev_*` arrays across its substeps for the same no-whip-crack reason
 * anchors do, and advances them to the current values at the end of each solve.
 *
 * `position`/`prev_position` must be sized to Cloth::particles whenever Cloth::skin is
 * non-empty. The normal arrays are only read when a particle has a positive backstop radius and
 * may be left empty when no backstop is painted.
 */
struct ClothSkinTargets {
    std::vector<glm::vec3> position;      /**< Skinned target per particle, world space. */
    std::vector<glm::vec3> prev_position; /**< Last solve's targets; the substep lerp's start. */
    std::vector<glm::vec3> normal;        /**< Skinned surface normal per particle, world space, unit. */
    std::vector<glm::vec3> prev_normal;   /**< Last solve's normals. */
};

/**
 * @struct ClothParams
 * @brief Per-cloth tunables. Field names and defaults track Unity's Cloth component where an
 *        equivalent exists, so a value copied from a Unity inspector behaves recognisably here.
 */
struct ClothParams {
    /** @brief Stretch compliance -- inverse stiffness, per unit mass (see bend_compliance for the
     *         units and the scale). 0 is fully inextensible and is the right default: XPBD is
     *         stable at zero compliance, unlike a spring at infinite k. Raise toward 1e-4 for a
     *         stretchy knit. */
    float stretch_compliance = 0.0f;

    /**
     * @brief How readily the weave SCISSORS -- warp and weft rotating toward each other without
     *        either one changing length. The "trellis effect", and the main way real cloth
     *        conforms to a curved surface.
     *
     * Soft by default, and it has to be: wrapping a sphere needs 36% less circumference at the
     * equator than a flat disc has, a flat sheet cannot be laid on a sphere isometrically at all
     * (Gauss), and the material has to lose that circumference SOMEWHERE. A woven fabric loses it
     * by shearing and by folding, both of which leave every thread the length it was. Hold shear
     * rigid along with stretch and the sheet has nowhere to put the excess: it settles as a taut
     * cone standing off the collider, which on the ball demo was 0.6 m of air under a sheet that
     * should have been lying on it.
     *
     * Read on bend_compliance's per-unit-mass scale. Applies to a grid cloth's DIAGONAL
     * constraints (make_grid_cloth's `shear`); a mesh cloth has no separate shear degree of
     * freedom, its triangle edges being the whole in-plane story.
     */
    float shear_compliance = 5e-3f;

    /**
     * @brief In-plane compliance while a THREAD is shorter than its rest length -- how far the
     *        weave may be squashed along its own fibres. 0 (the default) means it may not.
     *
     * Cloth carries almost no compressive load and the temptation is to make this free, so the
     * fabric can "buckle". It cannot: a distance constraint that shortens does not buckle, it
     * SHRINKS, and the length is gone. Left free, a sheet conforms to a ball by shrink-wrapping
     * onto it -- measured at 12% mean thread compression on the ball demo, with the hem riding up
     * above the equator and the ball showing underneath. The excess circumference belongs in
     * `shear_compliance` and in folds, both of which preserve thread length. Raise this only for a
     * material that genuinely crushes (foam rubber, a quilted pad), and expect it to creep.
     */
    float compression_compliance = 0.0f;

    /**
     * @brief Bend compliance: how readily the sheet creases. Non-zero by default because a
     *        perfectly rigid bend makes a sheet behave like sheet metal -- it will not crease, and
     *        an inextensible sheet that cannot crease cannot wrap a sphere at all.
     *
     * **Per unit mass**, which is what makes the scale below mean anything. A raw compliance in
     * m/N is measured against the constraint's inverse mass, and a cloth particle is not a
     * kilogram: a 25x25 sheet weighing 1 kg puts 1.6 g on each, so w = 1/m is ~1250 and any
     * compliance small enough to read as "stiff" in m/N makes the constraint rigid to four
     * decimal places. Folding the inverse mass in here (project_distance()) removes it from the
     * answer entirely: the fraction of a constraint violation left standing after one iteration
     * is `alpha / (1 + alpha)` with `alpha = compliance / h^2`, for ANY particle mass and ANY
     * resolution -- so the same number describes the same fabric on a 15x15 banner and a 45x45
     * skirt, which is the resolution independence this file's header claims.
     *
     * The unit is therefore s^2 rather than m/N -- a compliance divided by a mass. Think of its
     * square root as the time the constraint takes to relax.
     *
     * The ladder below is measured, not estimated: it is how much of the ball demo's upper
     * hemisphere a 4 m sheet ends up lying against, everything else held at this file's
     * defaults. Bending is what a sheet has to give up to fold, so it is the single parameter
     * that decides whether cloth drapes at all, and the useful band is narrow -- above 5e-3 the
     * sheet has no bending resistance left to remove.
     *
     *   | compliance | lies on the ball | reads as         |
     *   |------------|------------------|------------------|
     *   | 5e-5       | 26%              | stiff card       |
     *   | 5e-4       | 52%              | leather          |
     *   | 5e-3       | 86%              | mid-weight cloth |
     *   | 5e-2       | 88%              | silk             |
     */
    float bend_compliance = 5e-3f;

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

    /** @brief Collides the triangle SURFACE, not only the particles, via ClothSurfaceSample.
     *
     *  On by default: without it a draped sheet shows the collider through its own triangles
     *  wherever a chord sags further than `thickness` (see ClothSurfaceSample), which is the
     *  single most visible cloth artefact there is. Costs one extra collider query per unique
     *  edge and per triangle -- roughly 3x the collision stage, still far cheaper than the
     *  subdivision that would otherwise be needed. Turn off for a sheet that never meets a
     *  convex collider, or to measure what it is buying. */
    bool collide_surface = true;

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

    /** @brief Anim-drive compliance -- the inverse stiffness of the per-particle spring pulling a
     *         skin-driven cloth back toward its skinned pose, before the painted
     *         ClothSkinChannels::anim_drive gain divides it. Per unit mass and read on exactly
     *         the scale bend_compliance documents, for the same reason: against a 2 g garment
     *         particle a raw m/N compliance this small is a hard pin, and a skirt welded to its
     *         skinned pose is a rigid bell that ignores the legs swinging inside it. Smaller is
     *         stiffer; 0 would pin outright, so the painted gain (not this) is the per-vertex
     *         on/off. Only read when Cloth::skin carries an anim_drive channel. */
    float anim_drive_compliance = 1e-3f;

    /** @brief Uniform multiplier on the painted ClothSkinChannels::max_distance values -- a
     *         whole-garment "looseness" dial that avoids repainting. Only read when Cloth::skin
     *         is present. */
    float max_distance_scale = 1.0f;

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
 * @struct ClothSurfaceSample
 * @brief A point ON the cloth's surface, between particles, that collides in its own right.
 *
 * Collision projects PARTICLES, which are points -- so a sheet whose every particle sits exactly
 * at the standoff still draws triangles whose interiors chord across a convex collider and dip
 * below its surface. The ball bleeding through a draped sheet is that chord, not a solver bug:
 * for a chord of length `c` on a sphere of radius `R` the midpoint sags `R - sqrt(R^2 - c^2/4)`,
 * which a 25x25 sheet stretched over a 1 m ball puts at ~8 mm -- most of a 30 mm thickness, and
 * all of it once the sheet stretches.
 *
 * A surface sample is the fix NvCloth calls a "virtual particle": a barycentric point on a
 * triangle that is projected out of colliders like a particle, with its correction distributed
 * back to the three real particles by weight. The cloth then holds its SURFACE off a collider
 * rather than just its vertices, at a fraction of the cost of subdividing the sheet.
 */
struct ClothSurfaceSample {
    uint32_t a = 0, b = 0, c = 0;             /**< The triangle's particles. */
    float wa = 0.0f, wb = 0.0f, wc = 0.0f;    /**< Barycentric weights; sum to 1. */
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
    std::vector<ClothConstraint> stretch;  /**< Structural edges: the warp and weft threads. */
    /** @brief Diagonal edges, held at ClothParams::shear_compliance rather than the threads'.
     *         Separate because they are a different material property: a weave scissors long
     *         before a thread stretches, and conflating the two leaves a sheet unable to drape.
     *         Empty for a mesh cloth, which has no shear degree of freedom of its own. */
    std::vector<ClothConstraint> shear;
    std::vector<ClothConstraint> bend;     /**< Two-apart edges resisting curvature. */
    std::vector<ConstraintBatch> stretch_batches; /**< Disjoint ranges over `stretch`. */
    std::vector<ConstraintBatch> shear_batches;   /**< Disjoint ranges over `shear`. */
    std::vector<ConstraintBatch> bend_batches;    /**< Disjoint ranges over `bend`. */
    std::vector<ClothTether> tethers;      /**< Long-range max-distance constraints. */
    std::vector<ClothAnchor> anchors;      /**< Particles pinned to rigid bodies. */

    /** @brief Points on the surface between particles that collide too; see ClothSurfaceSample.
     *         Empty leaves collision particle-only. */
    std::vector<ClothSurfaceSample> surface_samples;
    /** @brief Disjoint ranges over `surface_samples`, same contract as ConstraintBatch. */
    std::vector<ConstraintBatch> surface_batches;

    /** @brief Painted skin channels; empty (the grid case) disables every skin solve stage. */
    ClothSkinChannels skin;

    /** @brief Per-frame skinned pose targets; caller-written before each solve when `skin` is
     *         present. See ClothSkinTargets. */
    ClothSkinTargets skin_targets;

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

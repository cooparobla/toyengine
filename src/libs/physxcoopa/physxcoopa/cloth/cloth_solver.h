/**
 * @file cloth_solver.h
 * @brief The XPBD cloth substep loop: integrate, anchor, project constraints, collide, re-derive
 *        velocities.
 *
 * Stage order per cloth substep, and why it is this order:
 *
 *   1.  aerodynamics   per-triangle drag/lift accumulated into a per-particle acceleration
 *   2.  integrate      v += a*hs; v *= 1/(1+damping*hs); prev = p; p += v*hs
 *   3.  anchors        pinned particles written outright from their body's pose
 *   3b. skin pins      skin-driven particles (max_distance <= eps) written from their targets
 *   4.  constraints    stretch, shear, bend, anim-drive, batch by batch, `iterations` times
 *   5.  tethers        one-sided max-distance clamp to the nearest anchor
 *   5b. skin limits    max-distance sphere clamp and backstop push around the skinned targets
 *   6.  self-collision optional particle-vs-particle separation
 *   7.  collisions     advance each collider one substep, then project out of every candidate
 *   7b. surface        the same, for points BETWEEN particles (ClothSurfaceSample)
 *   8.  finalize       v = (p - prev)/hs, then Coulomb friction at recorded contacts, both
 *                      measured RELATIVE to the surface each contact is on
 *
 * The skin stages (3b, the anim-drive pass in 4, 5b) run only when Cloth::skin carries channel
 * data -- a grid cloth takes exactly the pre-existing path. They sit BEFORE self-collision and
 * rigid collision for the same reason everything else does: collision must win every tie, and a
 * max-distance clamp that ran after collision could push a particle back inside a body.
 *
 * Collision against rigid shapes runs LAST of the position stages, not first: a constraint, a
 * tether or a self-collision push can all shove a particle back into a body it was just lifted out
 * of, so whichever runs last is the one that actually holds. Cloth must not interpenetrate visibly,
 * and a fraction of a millimetre of constraint violation is invisible -- so collision wins every
 * tie. This is the same reasoning that puts position correction after the velocity iterations in
 * dynamics/solver.h.
 *
 * Velocities are re-derived from positions at the end (stage 8) rather than tracked through the
 * projections. That is the defining property of position-based dynamics: every correction --
 * constraint, tether, collision, self-collision -- automatically shows up in the velocity, with
 * no per-stage impulse bookkeeping and no way for the two to disagree.
 *
 * **That is also why a moving collider has to arrive gradually.** A correction read back as
 * velocity is only meaningful if the correction itself was. Hold a collider at its end-of-step
 * pose for every substep and a body travelling 4 m/s arrives 6.7 cm inside the sheet on substep
 * one; removing that in a 1/240 s substep reports 16 m/s of cloth, and the sheet is thrown off
 * the body it was lying on. Stage 7 therefore advances each collider across the substeps from
 * ClothCollider::sweep, so the intrusion per substep is what the collider actually travels in
 * one -- and the velocity that comes back out is the collider's own speed, which is the right
 * answer. The same reasoning already governs stage 3's anchor lerp.
 *
 * This file allocates nothing. All working memory lives in a caller-owned ClothSolverScratch that
 * is resized once and reused, matching PhysicsWorld's own scratch-vector discipline.
 */

#ifndef PHYSXCOOPA_CLOTH_CLOTH_SOLVER_H
#define PHYSXCOOPA_CLOTH_CLOTH_SOLVER_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <physxcoopa/cloth/cloth.h>
#include <physxcoopa/cloth/cloth_collision.h>
#include <physxcoopa/util/math.h>

namespace coopa {
namespace physx {
namespace cloth {

/**
 * @brief How many times one particle is re-swept against the whole collider set per substep.
 *
 * Projecting a particle out of one collider can put it inside another the sweep has already
 * passed, so the sweep repeats until the particle is clear or this many passes have run. Bodies
 * are built from capsules that overlap at every joint, which makes that the normal case rather
 * than a corner one -- see stage 7's note in solve_cloth().
 *
 * Four is where the measurement stopped improving on the walk-cycle garment demo; the cost is
 * only paid by particles actually in contact, since a pass that corrects nothing breaks the loop.
 * Deeply wedged particles (genuinely inside two colliders with no gap between them) are not
 * guaranteed to satisfy both, and nothing can make them -- the bound is there so they cannot
 * spin.
 */
inline constexpr uint32_t k_collider_passes = 4;

/**
 * @brief How many XPBD substeps one solve of `dt` seconds should run, so that the substep itself
 *        never grows past `internal_h`.
 *
 * Every caller sizes its substep COUNT from the frame length rather than dividing the frame into a
 * fixed count. That is what keeps the substep at `internal_h` (1/240 s by default) and the cost per
 * SECOND constant as the frame rate varies -- 4 substeps at 60 Hz, 2 at 144, 8 at 30, all of them
 * 4.17 ms long.
 *
 * The cap is the part worth explaining, because the obvious two answers are both wrong:
 *
 *   - **A small fixed cap (this was 16) stretches the substep.** Past the cap a longer frame no
 *     longer buys more substeps, so it lengthens each one instead -- and every margin the collision
 *     stage is sized against is a distance travelled in ONE substep: the collider's own advance
 *     (ClothCollider::sweep divided by the count), the particle's travel between projections, and
 *     the ClothParams::thickness standoff that is why an analytic shape needs no particle-side CCD.
 *     Measured on `assets/scenes/physics/cloth_demo` at a 0.25 s frame (16 substeps of 15.6 ms, a
 *     ball advancing 6.25 cm per substep against a 3 cm standoff): upper-cap coverage on a driven
 *     ball falls from 81% at rest to 43%, i.e. the sheet slides off.
 *   - **Dropping the excess time is not available to cloth**, even though that is exactly what the
 *     rigid loop does with `PhysicsConfig::max_substeps` and its accumulator. A cloth's drivers are
 *     positioned from OUTSIDE the solve -- `ClothAnchor::world_position` from its body's live pose,
 *     and each collider's end pose from the body it snapshots -- so they always carry a whole
 *     frame of motion. Shorten only the interval and the sheet is decoupled from its own anchors:
 *     measured on the same scene and frame, the anchor completing a 1 m move inside a 0.067 s solve
 *     reads back as 47.5 m/s and tears the sheet off entirely (0% coverage), which is worse than
 *     the stretched substep it was meant to fix. Nor can the lag be repaid next frame -- the anchor
 *     re-resolves to the live pose, so the debt arrives as one jump either way.
 *
 * So the count is allowed to keep growing, and the cap is DERIVED: `step()` already clamps a frame
 * to `util::k_max_frame_time`, so covering that at `internal_h` is the most that can ever be asked.
 * `util::k_max_cloth_substeps` remains only as an absolute ceiling for a pathological
 * `PhysicsConfig::cloth_substeps`. The cost of a genuine 0.25 s hitch is therefore ~60 substeps in
 * one frame instead of 16 -- real, paid only on a frame that is already a hitch, and the price of
 * the sheet still being on the body afterwards.
 *
 * Below `k_max_cloth_substeps * internal_h` (66.7 ms at defaults) this returns exactly what the
 * plain clamp always did, so every frame rate a host can actually hold is unchanged.
 *
 * @param dt         Frame length the solve will cover, seconds.
 * @param internal_h The authored substep length, `fixed_dt / cloth_substeps`.
 * @return Substep count, at least 1.
 */
inline uint32_t frame_substeps(float dt, float internal_h) {
    const uint32_t cap = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::ceil(util::k_max_frame_time / internal_h)), 1u,
        util::k_max_cloth_substeps);
    return std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(dt / internal_h)), 1u, cap);
}

/**
 * @struct ClothSolverScratch
 * @brief Reusable per-solve working memory. One instance per PhysicsWorld, resized on first use.
 */
struct ClothSolverScratch {
    std::vector<glm::vec3> accel;          /**< Per-particle aerodynamic acceleration. */
    std::vector<glm::vec3> contact_normal; /**< Accumulated contact normal; zero means no contact. */
    std::vector<float> contact_depth;      /**< Total correction applied this substep, metres. */
    /** @brief Weighted sum of the contacting surfaces' velocities, and the weights, so stage 8
     *         can resolve friction in the surface's frame rather than the world's. */
    std::vector<glm::vec3> contact_velocity;
    std::vector<float> contact_weight;
    std::vector<float> anim_lambda;        /**< Anim-drive Lagrange multipliers, one per particle. */

    /** @brief Colliders re-posed for the substep being solved; see solve_cloth()'s stage 7. */
    std::vector<ClothCollider> posed;

    // --- Uniform spatial hash, used only when ClothParams::self_collision is on ---
    std::vector<uint32_t> cell_of;      /**< Hash bucket per particle. */
    std::vector<uint32_t> cell_counts;  /**< Bucket populations, then prefix sums. */
    std::vector<uint32_t> sorted;       /**< Particle indices grouped by bucket, ascending within. */

    /** @brief Grows every buffer to fit `particles`, preserving the "resize once, reuse forever"
     *         contract -- the `!=` test (rather than `<`) mirrors PhysicsWorld's scratch vectors. */
    void ensure(std::size_t particles, std::size_t buckets) {
        if (accel.size() != particles) {
            accel.resize(particles);
            contact_normal.resize(particles);
            contact_depth.resize(particles);
            contact_velocity.resize(particles);
            contact_weight.resize(particles);
            anim_lambda.resize(particles);
            cell_of.resize(particles);
            sorted.resize(particles);
        }
        if (cell_counts.size() != buckets + 1) cell_counts.resize(buckets + 1);
    }
};

/**
 * @struct SerialDispatch
 * @brief Default work dispatcher: runs the whole range inline on the calling thread.
 *
 * solve_cloth() is templated on a dispatcher so PhysicsWorld can hand it a JobEngine-backed one
 * without cloth_solver.h ever including the job system, and so tests can drive it with no job
 * engine at all. A dispatcher is called as `dispatch(count, fn)` and must invoke `fn(begin, end)`
 * over a partition of [0, count) -- in any order and on any thread, because every stage that goes
 * through it writes only to indices inside its own sub-range.
 */
struct SerialDispatch {
    template <typename Fn>
    void operator()(std::size_t count, Fn&& fn) const {
        if (count > 0) fn(std::size_t{0}, count);
    }
};

namespace detail {

/** @brief Hashes integer cell coordinates into [0, buckets). The three primes are the standard
 *         Teschner et al. (2003) spatial-hash constants; `buckets` is always a power of two, so
 *         the mask is exact and unbiased. */
inline uint32_t hash_cell(int32_t x, int32_t y, int32_t z, uint32_t buckets) {
    const uint32_t h = static_cast<uint32_t>(x) * 73856093u ^
                       static_cast<uint32_t>(y) * 19349663u ^
                       static_cast<uint32_t>(z) * 83492791u;
    return h & (buckets - 1u);
}

/** @brief Smallest power of two >= `n`, minimum 16. Sizing the hash table at ~2x the particle
 *         count keeps the average bucket under one entry, which is what makes the 27-cell scan
 *         cheap enough to run every substep. */
inline uint32_t hash_bucket_count(std::size_t n) {
    uint32_t b = 16;
    while (b < n * 2u && b < (1u << 22)) b <<= 1;
    return b;
}

/**
 * @brief Projects one XPBD distance constraint. Returns immediately when both endpoints are
 *        pinned (w == 0), which is also what stops a division by zero at zero compliance.
 *
 * `taut_unit` and `slack_unit` are compliance/h^2 BEFORE the constraint's own inverse mass is
 * folded in; this is the only place that happens, and it is what makes an authored compliance
 * mean the same thing on any cloth. See ClothParams::bend_compliance for the argument and the
 * resulting scale.
 *
 * The two are separate because fabric is not a spring. A woven sheet is nearly inextensible in
 * TENSION and carries essentially no load in COMPRESSION -- squeeze it edge-on and it buckles
 * instead of pushing back. One symmetric compliance has to pick a side, and picking "rigid" (the
 * right answer for tension) makes the sheet incapable of draping over anything convex at all:
 * wrapping a sphere means every hoop around it must shorten, a sheet that cannot shorten a hoop
 * has nowhere to put the excess material, and what it does instead is stand off the surface as a
 * taut cone tangent to it -- measured on the ball demo at 0.6 m of air under a sheet that should
 * have been lying on it. Softening tension instead buys the same drape and a rubber sheet. A
 * bend constraint passes the same value for both, since its resistance to shortening IS the
 * bending stiffness.
 */
inline void project_distance(std::vector<ClothParticle>& particles, ClothConstraint& k,
                             float taut_unit, float slack_unit) {
    ClothParticle& pa = particles[k.a];
    ClothParticle& pb = particles[k.b];
    const float w = pa.inv_mass + pb.inv_mass;
    if (w <= 0.0f) return;

    glm::vec3 d = pa.position - pb.position;
    const float len = glm::length(d);
    if (len < util::k_epsilon) return; // coincident: the gradient is undefined, skip this substep
    const glm::vec3 n = d / len;

    const float c = len - k.rest_length;
    const float alpha = (c < 0.0f ? slack_unit : taut_unit) * w;
    const float dlambda = (-c - alpha * k.lambda) / (w + alpha);
    k.lambda += dlambda;

    const glm::vec3 correction = dlambda * n;
    pa.position += pa.inv_mass * correction;
    pb.position -= pb.inv_mass * correction;
}

} // namespace detail

/**
 * @brief Advances one cloth by `h` seconds, in `substeps` XPBD substeps.
 *
 * @tparam Dispatch    Work dispatcher; see SerialDispatch.
 * @param c            Cloth to advance, mutated in place. Anchors must already have their
 *                     `world_position`/`prev_world_position` resolved for this step.
 * @param colliders    Rigid shapes to collide against, snapshotted at this step's poses.
 * @param gravity      World gravity vector (m/s^2), before ClothParams::gravity_scale.
 * @param substeps     Cloth substeps within `h`; clamped to at least 1. PhysicsWorld sizes this
 *                     per call so the resulting substep length stays constant as the frame rate
 *                     varies -- see PhysicsWorld::step_cloths_().
 * @param iterations   Constraint iterations per substep; clamped to at least 1.
 * @param h            Step length in seconds (one rendered frame, as passed to PhysicsWorld::step).
 * @param scratch      Reusable working memory, resized on demand.
 * @param dispatch     Work dispatcher for the parallelisable per-particle and per-batch stages.
 */
template <typename Dispatch>
inline void solve_cloth(Cloth& c, const std::vector<ClothCollider>& colliders,
                        const glm::vec3& gravity, uint32_t substeps, uint32_t iterations,
                        float h, ClothSolverScratch& scratch, Dispatch&& dispatch) {
    const std::size_t n = c.particles.size();
    if (n == 0 || h <= 0.0f) return;

    const uint32_t steps = std::max<uint32_t>(1, c.params.substeps ? c.params.substeps : substeps);
    const uint32_t iters = std::max<uint32_t>(1, c.params.iterations ? c.params.iterations : iterations);
    const float hs = h / static_cast<float>(steps);
    const float inv_hs = 1.0f / hs;

    const bool self_collision = c.params.self_collision && c.params.self_distance > util::k_epsilon;
    const uint32_t buckets = self_collision ? detail::hash_bucket_count(n) : 16u;
    scratch.ensure(n, buckets);

    // --- Skin channels (character cloth). All gates resolved once, outside the substep loop. ---
    const bool has_skin = !c.skin.empty() && c.skin.max_distance.size() == n &&
                          c.skin_targets.position.size() == n;
    const bool has_drive = has_skin && c.skin.anim_drive.size() == n &&
                           c.params.anim_drive_compliance > 0.0f;
    const bool has_backstop = has_skin && c.skin.backstop_radius.size() == n &&
                              c.skin.backstop_distance.size() == n &&
                              c.skin_targets.normal.size() == n;
    if (has_skin) {
        // First solve after a (re)build: no previous targets exist yet, so seed the lerp's start
        // at the current targets rather than lerping from zero-initialised garbage.
        if (c.skin_targets.prev_position.size() != n) {
            c.skin_targets.prev_position = c.skin_targets.position;
        }
        if (has_backstop && c.skin_targets.prev_normal.size() != n) {
            c.skin_targets.prev_normal = c.skin_targets.normal;
        }
    }

    // Compliance is divided by hs^2 exactly once per solve: it is constant across substeps and
    // iterations, and this is the only place the timestep enters the constraint math at all --
    // which is what makes XPBD stiffness timestep-independent (see ClothParams' doc). The
    // constraint's own inverse mass is folded in per constraint, inside project_distance().
    const float inv_h2 = 1.0f / (hs * hs);
    const float stretch_alpha = c.params.stretch_compliance * inv_h2;
    const float compress_alpha =
        std::max(c.params.compression_compliance, c.params.stretch_compliance) * inv_h2;
    // Shear is symmetric: a diagonal shortens scissoring one way and lengthens scissoring the
    // other, and both are the same weave giving way.
    const float shear_alpha = c.params.shear_compliance * inv_h2;
    const float bend_alpha = c.params.bend_compliance * inv_h2;

    // Deterministic gust: two incommensurable sinusoids over the cloth's own accumulated sim
    // time. Not rand() -- world_state_hash() must stay reproducible across runs (see
    // PhysicsWorld::world_state_hash).
    glm::vec3 wind = c.params.wind;
    if (c.params.wind_turbulence > 0.0f) {
        const float t = c.time;
        wind += c.params.wind_turbulence *
                glm::vec3(std::sin(t * 1.7f) * std::cos(t * 0.9f),
                          std::sin(t * 1.1f + 1.3f) * std::cos(t * 1.9f),
                          std::sin(t * 0.7f + 2.7f) * std::cos(t * 1.3f));
    }

    // Does any collider move across this frame? If none does, the substep re-posing below is
    // skipped entirely and the colliders are used as given -- the static case pays nothing.
    bool any_swept = false;
    for (const ClothCollider& collider : colliders) {
        if (collider.is_swept()) { any_swept = true; break; }
    }
    if (any_swept) scratch.posed = colliders;

    const glm::vec3 base_accel = gravity * c.params.gravity_scale + c.params.external_acceleration;
    const bool aero = (c.params.air_drag > 0.0f || c.params.air_lift > 0.0f) && !c.triangles.empty();
    const float max_velocity2 = c.params.max_velocity * c.params.max_velocity;

    for (uint32_t step = 0; step < steps; ++step) {
        // --- 1. Aerodynamics ------------------------------------------------------------------
        // Force on a triangle moving through still air, split into a component along the face
        // normal (drag -- what makes a sheet parachute) and one in the plane (lift -- what makes
        // it slip sideways and flutter). The |dot(v,n)| factor is the projected area the triangle
        // presents to the flow, so an edge-on triangle correctly feels almost nothing.
        std::fill(scratch.accel.begin(), scratch.accel.end(), glm::vec3(0.0f));
        if (aero) {
            for (std::size_t t = 0; t + 2 < c.triangles.size(); t += 3) {
                const uint32_t i0 = c.triangles[t], i1 = c.triangles[t + 1], i2 = c.triangles[t + 2];
                const glm::vec3& p0 = c.particles[i0].position;
                const glm::vec3 e1 = c.particles[i1].position - p0;
                const glm::vec3 e2 = c.particles[i2].position - p0;
                const glm::vec3 cross = glm::cross(e1, e2);
                const float twice_area = glm::length(cross);
                if (twice_area < util::k_epsilon) continue;
                const glm::vec3 face_n = cross / twice_area;

                const glm::vec3 v = (c.particles[i0].velocity + c.particles[i1].velocity +
                                     c.particles[i2].velocity) * (1.0f / 3.0f) - wind;
                const float vn = glm::dot(v, face_n);
                const glm::vec3 vt = v - vn * face_n;
                const glm::vec3 force = -0.5f * twice_area * std::abs(vn) *
                                        (c.params.air_drag * vn * face_n + c.params.air_lift * vt);
                // Force -> acceleration happens per particle below, via that particle's own
                // inv_mass; a third of the triangle's force goes to each corner.
                const glm::vec3 share = force * (1.0f / 3.0f);
                scratch.accel[i0] += share;
                scratch.accel[i1] += share;
                scratch.accel[i2] += share;
            }
        }

        // --- 2. Integrate ---------------------------------------------------------------------
        const float damping_factor = 1.0f / (1.0f + c.params.damping * hs);
        dispatch(n, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                ClothParticle& p = c.particles[i];
                p.prev_position = p.position;
                if (p.inv_mass == 0.0f) continue;
                p.velocity += (base_accel + scratch.accel[i] * p.inv_mass) * hs;
                p.velocity *= damping_factor;
                p.position += p.velocity * hs;
            }
        });

        // --- 3. Anchors -----------------------------------------------------------------------
        // Lerp across the substeps so a fast anchor body drags the sheet smoothly; see
        // ClothAnchor::prev_world_position.
        const float alpha_t = static_cast<float>(step + 1) / static_cast<float>(steps);
        for (const ClothAnchor& a : c.anchors) {
            if (a.particle >= n) continue;
            c.particles[a.particle].position =
                glm::mix(a.prev_world_position, a.world_position, alpha_t);
        }

        // --- 3b. Skin-driven particles --------------------------------------------------------
        // A particle painted with max_distance <= eps is fully skin-driven: the builder gave it
        // inv_mass == 0 (so constraints treat it as infinitely massive, exactly like an anchor)
        // and its position is written outright from the lerped skinned target. Gated on the
        // channel value rather than inv_mass so anchored particles keep their own stage-3 path.
        if (has_skin) {
            dispatch(n, [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) {
                    if (c.skin.max_distance[i] > util::k_epsilon) continue;
                    c.particles[i].position = glm::mix(c.skin_targets.prev_position[i],
                                                       c.skin_targets.position[i], alpha_t);
                }
            });
        }

        // --- 4. Constraints -------------------------------------------------------------------
        for (ClothConstraint& k : c.stretch) k.lambda = 0.0f;
        for (ClothConstraint& k : c.shear) k.lambda = 0.0f;
        for (ClothConstraint& k : c.bend) k.lambda = 0.0f;
        if (has_drive) std::fill(scratch.anim_lambda.begin(), scratch.anim_lambda.end(), 0.0f);

        for (uint32_t it = 0; it < iters; ++it) {
            for (const ConstraintBatch& b : c.stretch_batches) {
                dispatch(b.end - b.begin, [&](std::size_t begin, std::size_t end) {
                    for (std::size_t i = begin; i < end; ++i) {
                        detail::project_distance(c.particles, c.stretch[b.begin + i],
                                                 stretch_alpha, compress_alpha);
                    }
                });
            }
            for (const ConstraintBatch& b : c.shear_batches) {
                dispatch(b.end - b.begin, [&](std::size_t begin, std::size_t end) {
                    for (std::size_t i = begin; i < end; ++i) {
                        detail::project_distance(c.particles, c.shear[b.begin + i], shear_alpha,
                                                 shear_alpha);
                    }
                });
            }
            for (const ConstraintBatch& b : c.bend_batches) {
                dispatch(b.end - b.begin, [&](std::size_t begin, std::size_t end) {
                    for (std::size_t i = begin; i < end; ++i) {
                        detail::project_distance(c.particles, c.bend[b.begin + i], bend_alpha,
                                                 bend_alpha);
                    }
                });
            }
            // Anim drive: a per-particle XPBD spring toward the lerped skinned target, with the
            // painted gain dividing the compliance (stiffer where painted closer to 1). One
            // particle per constraint, so the whole pass is trivially disjoint and dispatchable.
            if (has_drive) {
                dispatch(n, [&](std::size_t begin, std::size_t end) {
                    for (std::size_t i = begin; i < end; ++i) {
                        ClothParticle& p = c.particles[i];
                        if (p.inv_mass == 0.0f) continue;
                        const float drive = c.skin.anim_drive[i];
                        if (drive <= util::k_epsilon) continue;
                        const glm::vec3 target = glm::mix(c.skin_targets.prev_position[i],
                                                          c.skin_targets.position[i], alpha_t);
                        glm::vec3 d = p.position - target;
                        const float len = glm::length(d);
                        if (len < util::k_epsilon) continue;
                        // Mass-normalised exactly as project_distance() is, so a painted drive
                        // pulls a heavy garment and a light one back at the same rate.
                        const float alpha =
                            c.params.anim_drive_compliance * p.inv_mass / (drive * hs * hs);
                        const float dlambda =
                            (-len - alpha * scratch.anim_lambda[i]) / (p.inv_mass + alpha);
                        scratch.anim_lambda[i] += dlambda;
                        p.position += p.inv_mass * dlambda * (d / len);
                    }
                });
            }
        }

        // --- 5. Tethers -----------------------------------------------------------------------
        // One tether per particle, and every anchor endpoint is pinned (never written here), so
        // the whole list is particle-disjoint and safe to dispatch.
        if (!c.tethers.empty()) {
            dispatch(c.tethers.size(), [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) {
                    const ClothTether& t = c.tethers[i];
                    ClothParticle& p = c.particles[t.particle];
                    if (p.inv_mass == 0.0f) continue;
                    const glm::vec3 anchor = c.particles[t.anchor].position;
                    glm::vec3 d = p.position - anchor;
                    const float len = glm::length(d);
                    if (len <= t.max_length || len < util::k_epsilon) continue;
                    p.position = anchor + d * (t.max_length / len);
                }
            });
        }

        // --- 5b. Skin limits ------------------------------------------------------------------
        // Max-distance: hard-clamp each particle into its painted sphere around the skinned
        // target (the garment's leash). Backstop: push it out of the sphere tucked behind the
        // skinned surface (the body under the garment). Both are projections onto spheres whose
        // centres are per-particle, so the pass is particle-disjoint and dispatchable. They sit
        // before self/rigid collision -- see the stage-order doc at the top of this file.
        if (has_skin) {
            const float md_scale = glm::max(0.0f, c.params.max_distance_scale);
            dispatch(n, [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) {
                    ClothParticle& p = c.particles[i];
                    if (p.inv_mass == 0.0f) continue;
                    const glm::vec3 target = glm::mix(c.skin_targets.prev_position[i],
                                                      c.skin_targets.position[i], alpha_t);

                    const float r = c.skin.max_distance[i] * md_scale;
                    glm::vec3 d = p.position - target;
                    const float len = glm::length(d);
                    if (len > r) {
                        p.position = (len > util::k_epsilon) ? target + d * (r / len) : target;
                    }

                    if (has_backstop) {
                        const float br = c.skin.backstop_radius[i];
                        if (br <= util::k_epsilon) continue;
                        const glm::vec3 nrm = glm::mix(c.skin_targets.prev_normal[i],
                                                       c.skin_targets.normal[i], alpha_t);
                        const float nlen = glm::length(nrm);
                        if (nlen < util::k_epsilon) continue;
                        const glm::vec3 centre =
                            target - (nrm / nlen) * (c.skin.backstop_distance[i] + br);
                        glm::vec3 db = p.position - centre;
                        const float dlen = glm::length(db);
                        if (dlen >= br) continue;
                        // A particle exactly at the centre has no radial; eject it along the
                        // skinned normal, the direction "out of the body" by construction.
                        p.position = (dlen > util::k_epsilon) ? centre + db * (br / dlen)
                                                              : centre + (nrm / nlen) * br;
                    }
                }
            });
        }

        // --- 6. Self-collision ----------------------------------------------------------------
        // Serial by construction: a pair push writes both particles, so no index partition makes
        // it disjoint. It is also the only stage that is ever the bottleneck, which is why it is
        // opt-in (ClothParams::self_collision).
        if (self_collision) {
            const float d_min = c.params.self_distance;
            const float inv_cell = 1.0f / d_min;

            std::fill(scratch.cell_counts.begin(), scratch.cell_counts.end(), 0u);
            for (std::size_t i = 0; i < n; ++i) {
                const glm::vec3& p = c.particles[i].position;
                const uint32_t cell = detail::hash_cell(
                    static_cast<int32_t>(std::floor(p.x * inv_cell)),
                    static_cast<int32_t>(std::floor(p.y * inv_cell)),
                    static_cast<int32_t>(std::floor(p.z * inv_cell)), buckets);
                scratch.cell_of[i] = cell;
                ++scratch.cell_counts[cell];
            }
            // Counting sort into bucket-major order. Deterministic: particles land in ascending
            // index order within each bucket regardless of anything else.
            uint32_t running = 0;
            for (uint32_t b = 0; b < buckets; ++b) {
                const uint32_t count = scratch.cell_counts[b];
                scratch.cell_counts[b] = running;
                running += count;
            }
            scratch.cell_counts[buckets] = running;
            for (std::size_t i = 0; i < n; ++i) {
                scratch.sorted[scratch.cell_counts[scratch.cell_of[i]]++] = static_cast<uint32_t>(i);
            }
            // cell_counts[b] now holds the END of bucket b, so bucket b spans
            // [b ? cell_counts[b-1] : 0, cell_counts[b]).

            for (std::size_t i = 0; i < n; ++i) {
                ClothParticle& pi = c.particles[i];
                if (pi.inv_mass == 0.0f) continue;
                const glm::vec3 base = pi.position * inv_cell;
                const int32_t cx = static_cast<int32_t>(std::floor(base.x));
                const int32_t cy = static_cast<int32_t>(std::floor(base.y));
                const int32_t cz = static_cast<int32_t>(std::floor(base.z));

                for (int32_t dz = -1; dz <= 1; ++dz) {
                for (int32_t dy = -1; dy <= 1; ++dy) {
                for (int32_t dx = -1; dx <= 1; ++dx) {
                    const uint32_t b = detail::hash_cell(cx + dx, cy + dy, cz + dz, buckets);
                    const uint32_t first = (b == 0) ? 0u : scratch.cell_counts[b - 1];
                    const uint32_t last = scratch.cell_counts[b];
                    for (uint32_t s = first; s < last; ++s) {
                        const uint32_t j = scratch.sorted[s];
                        if (j <= i) continue; // each pair once
                        // Skip particles that are already directly constrained to each other --
                        // self-collision pushing them to d_min while a structural constraint pulls
                        // them to rest_length is a fight neither wins, and the sheet buzzes. On a
                        // grid the 1-ring is exactly the Chebyshev-1 neighbourhood, so this is an
                        // O(1) index test rather than an adjacency lookup.
                        if (c.columns > 0) {
                            const int32_t xi = static_cast<int32_t>(i % c.columns);
                            const int32_t yi = static_cast<int32_t>(i / c.columns);
                            const int32_t xj = static_cast<int32_t>(j % c.columns);
                            const int32_t yj = static_cast<int32_t>(j / c.columns);
                            if (std::abs(xi - xj) <= 1 && std::abs(yi - yj) <= 1) continue;
                        }
                        ClothParticle& pj = c.particles[j];
                        const float w = pi.inv_mass + pj.inv_mass;
                        if (w <= 0.0f) continue;
                        glm::vec3 d = pi.position - pj.position;
                        const float len2 = glm::dot(d, d);
                        if (len2 >= d_min * d_min || len2 < util::k_epsilon) continue;
                        const float len = std::sqrt(len2);
                        const glm::vec3 push = (d / len) * ((d_min - len) / w);
                        pi.position += push * pi.inv_mass;
                        pj.position -= push * pj.inv_mass;
                    }
                }}}
            }
        }

        // --- 7. Collisions --------------------------------------------------------------------
        // LAST of the position stages, and that ordering is load-bearing: every earlier stage --
        // constraints, tethers, and self-collision above -- can push a particle back into a body,
        // and whichever stage runs last is the one that actually holds. Self-collision in
        // particular can displace a particle by up to half of self_distance (5 cm on this scene's
        // sheet), which would otherwise wipe out the whole `thickness` standoff with nothing left
        // to re-project it that substep. Visible interpenetration costs far more than the fraction
        // of a millimetre of constraint violation this trade gives up.
        std::fill(scratch.contact_normal.begin(), scratch.contact_normal.end(), glm::vec3(0.0f));
        std::fill(scratch.contact_depth.begin(), scratch.contact_depth.end(), 0.0f);
        std::fill(scratch.contact_velocity.begin(), scratch.contact_velocity.end(), glm::vec3(0.0f));
        std::fill(scratch.contact_weight.begin(), scratch.contact_weight.end(), 0.0f);

        // **A moving collider is advanced ACROSS the substeps, not teleported to its end pose.**
        // The caller hands over one displacement for the whole frame; holding the shape at the far
        // end of it for every substep means a body travelling 4 m/s arrives 6.7 cm inside the
        // sheet on the first substep, and PBD derives velocity as (p - prev)/hs -- so projecting
        // that intrusion out in one 1/240 s substep reads as 16 m/s of cloth. Measured on the ball
        // demo before this: a peak particle speed of 13.7 m/s against a ball doing 4, on the first
        // frame the ball moved, which threw the sheet off it within half a second.
        //
        // Advancing the pose by one substep's worth at a time bounds the intrusion to what the
        // collider actually travels in that substep, so the derived velocity comes out at the
        // collider's own speed -- which is the physically right answer, and the same reason
        // stage 3 lerps anchors rather than snapping them. The remaining per-substep displacement
        // still rides in `sweep`, so the projection covers the volume crossed WITHIN the substep
        // too (exact for a sphere, a two-pose probe for a box or capsule).
        const std::vector<ClothCollider>* active = &colliders;
        if (any_swept) {
            const float inv_steps = 1.0f / static_cast<float>(steps);
            const float t0 = static_cast<float>(step) * inv_steps;
            const float t1 = static_cast<float>(step + 1) * inv_steps;
            for (std::size_t i = 0; i < colliders.size(); ++i) {
                const ClothCollider& src = colliders[i];
                ClothCollider& dst = scratch.posed[i];
                dst.position = src.position + src.sweep * t0;
                dst.sweep = src.sweep * inv_steps;
                dst.orientation = glm::slerp(src.orientation, src.end_orientation, t0);
                dst.end_orientation = glm::slerp(src.orientation, src.end_orientation, t1);
                // `bounds` stays the frame-wide union it was built as: a reject box only ever has
                // to be conservative, and re-deriving it per substep would cost more than the few
                // extra narrow-phase calls it saves.
            }
            active = &scratch.posed;
        }
        const std::vector<ClothCollider>& step_colliders = *active;

        // One sweep of every awake particle against every collider, re-swept per particle until
        // that particle is clear of all of them.
        //
        // **The re-sweep is what makes overlapping colliders work**, and a body is nothing but
        // overlapping colliders: a garment meets a whole skeleton of capsules that overlap at
        // every joint, so projecting a particle out of the thigh frequently puts it inside the
        // shin, which the single pass had already visited and would never revisit. The particle
        // then ends the substep inside a limb no matter how many substeps run, because every
        // substep makes the same mistake. Measured on the walk-cycle demo: up to 4.2 cm left
        // inside a shin, on six different bones.
        //
        // Each pass is particle-local, so this stays dispatchable and deterministic, and costs
        // nothing for the overwhelming majority of particles -- a pass that applies no correction
        // breaks out, and a particle touching nothing is decided by the AABB rejects alone.
        auto project_against_colliders = [&]() {
            dispatch(n, [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) {
                    ClothParticle& p = c.particles[i];
                    if (p.inv_mass == 0.0f) continue;
                    for (uint32_t pass = 0; pass < k_collider_passes; ++pass) {
                        bool moved = false;
                        for (const ClothCollider& collider : step_colliders) {
                            // Cheap AABB reject first: the sheet spans many colliders' worth of
                            // space, so most (particle, collider) pairs are decided by six
                            // compares. The thickness margin is applied inline rather than via
                            // AABB::expand(), which would construct a temporary per particle per
                            // collider.
                            const glm::vec3& lo = collider.bounds.min;
                            const glm::vec3& hi = collider.bounds.max;
                            const float m = c.params.thickness;
                            if (p.position.x < lo.x - m || p.position.x > hi.x + m ||
                                p.position.y < lo.y - m || p.position.y > hi.y + m ||
                                p.position.z < lo.z - m || p.position.z > hi.z + m) {
                                continue;
                            }
                            glm::vec3 normal(0.0f);
                            float depth = 0.0f;
                            // prev_position is this substep's start, set at stage 2 -- so the swept
                            // crossing test against a mesh covers every way the particle could have
                            // ended up across the surface since: integration, constraints, tethers
                            // and self-collision alike, not just its ballistic motion.
                            if (!project_particle(collider, c.params.thickness, p.position, normal,
                                                  depth, &p.prev_position)) {
                                continue;
                            }
                            moved = true;
                            scratch.contact_normal[i] += normal;
                            scratch.contact_depth[i] += depth;
                            scratch.contact_velocity[i] += collider.velocity_at(p.position);
                            scratch.contact_weight[i] += 1.0f;
                        }
                        if (!moved) break;
                    }
                }
            });
        };
        if (!colliders.empty()) project_against_colliders();

        // --- 7b. Surface collision ------------------------------------------------------------
        // The same projection, applied to points BETWEEN particles (see ClothSurfaceSample):
        // what stops a collider showing through a triangle whose corners are both outside it.
        // Each sample's correction is distributed back to its particles by weight, mass-
        // weighted so a pinned corner takes none of it, and its contact is accumulated the same
        // way so friction still applies. Batches are particle-disjoint, so this dispatches.
        if (c.params.collide_surface && !c.surface_samples.empty() && !step_colliders.empty()) {
            for (const ConstraintBatch& sb : c.surface_batches) {
                dispatch(sb.end - sb.begin, [&](std::size_t begin, std::size_t end) {
                    for (std::size_t s = begin; s < end; ++s) {
                        const ClothSurfaceSample& vs = c.surface_samples[sb.begin + s];
                        ClothParticle& pa = c.particles[vs.a];
                        ClothParticle& pb = c.particles[vs.b];
                        ClothParticle& pc = c.particles[vs.c];
                        // Mass-weighted denominator: a sample whose particles are all pinned
                        // cannot move, and must not divide by zero trying.
                        const float denom = vs.wa * vs.wa * pa.inv_mass + vs.wb * vs.wb * pb.inv_mass +
                                            vs.wc * vs.wc * pc.inv_mass;
                        if (denom <= 0.0f) continue;

                        glm::vec3 p = vs.wa * pa.position + vs.wb * pb.position + vs.wc * pc.position;
                        const glm::vec3 start = vs.wa * pa.prev_position + vs.wb * pb.prev_position +
                                                vs.wc * pc.prev_position;
                        const glm::vec3 before = p;
                        glm::vec3 normal_sum(0.0f), velocity_sum(0.0f);
                        float depth_sum = 0.0f, weight_sum = 0.0f;
                        for (const ClothCollider& collider : step_colliders) {
                            const glm::vec3& lo = collider.bounds.min;
                            const glm::vec3& hi = collider.bounds.max;
                            const float m = c.params.thickness;
                            if (p.x < lo.x - m || p.x > hi.x + m || p.y < lo.y - m ||
                                p.y > hi.y + m || p.z < lo.z - m || p.z > hi.z + m) {
                                continue;
                            }
                            glm::vec3 normal(0.0f);
                            float depth = 0.0f;
                            if (!project_particle(collider, c.params.thickness, p, normal, depth, &start)) {
                                continue;
                            }
                            normal_sum += normal;
                            depth_sum += depth;
                            velocity_sum += collider.velocity_at(p);
                            weight_sum += 1.0f;
                        }
                        if (depth_sum <= 0.0f) continue;

                        const glm::vec3 correction = p - before;
                        pa.position += correction * (vs.wa * pa.inv_mass / denom);
                        pb.position += correction * (vs.wb * pb.inv_mass / denom);
                        pc.position += correction * (vs.wc * pc.inv_mass / denom);
                        // Share the contact so stage 8's friction sees it, in the same
                        // proportion the correction was shared.
                        scratch.contact_normal[vs.a] += normal_sum * vs.wa;
                        scratch.contact_normal[vs.b] += normal_sum * vs.wb;
                        scratch.contact_normal[vs.c] += normal_sum * vs.wc;
                        scratch.contact_depth[vs.a] += depth_sum * vs.wa;
                        scratch.contact_depth[vs.b] += depth_sum * vs.wb;
                        scratch.contact_depth[vs.c] += depth_sum * vs.wc;
                        scratch.contact_velocity[vs.a] += velocity_sum * vs.wa;
                        scratch.contact_velocity[vs.b] += velocity_sum * vs.wb;
                        scratch.contact_velocity[vs.c] += velocity_sum * vs.wc;
                        scratch.contact_weight[vs.a] += weight_sum * vs.wa;
                        scratch.contact_weight[vs.b] += weight_sum * vs.wb;
                        scratch.contact_weight[vs.c] += weight_sum * vs.wc;
                    }
                });
            }

            // --- 7c. Particles again, LAST ----------------------------------------------------
            // 7b guarantees its SAMPLE points are outside the collider, not the particles it moved
            // to get them there: a sample's correction is shared along one direction, so a corner
            // can be carried into a neighbouring collider (the same overlapping-capsule geometry
            // the re-sweep above exists for). A particle inside a limb is a mesh VERTEX inside a
            // limb -- the most visible failure there is -- whereas re-running the particle stage
            // gives back only a fraction of a triangle-interior standoff. Same reasoning as the
            // stage-order note above: whichever stage runs last is the one that holds.
            project_against_colliders();
        }

        // --- 8. Finalize velocities + friction ------------------------------------------------
        dispatch(n, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                ClothParticle& p = c.particles[i];
                p.velocity = (p.position - p.prev_position) * inv_hs;

                const float depth = scratch.contact_depth[i];
                if (depth > 0.0f) {
                    const glm::vec3 nsum = scratch.contact_normal[i];
                    const float nlen = glm::length(nsum);
                    if (nlen > util::k_epsilon) {
                        const glm::vec3 nrm = nsum / nlen;
                        // Both the inelastic clamp and the friction below are about the motion
                        // the contact actually has to absorb, which is motion RELATIVE TO THE
                        // SURFACE. Against a still collider this is the velocity itself and
                        // nothing changes; against a moving one it is the difference, which is
                        // the whole point: a sheet riding a ball driven at 4 m/s is not sliding
                        // on it at 4 m/s, and braking it toward world-zero scrubs the fabric off
                        // the back of the ball (measured: upper-cap coverage 91% -> 10% in 2.5 s).
                        const float w = scratch.contact_weight[i];
                        const glm::vec3 surface =
                            w > util::k_epsilon ? scratch.contact_velocity[i] / w : glm::vec3(0.0f);
                        glm::vec3 rel = p.velocity - surface;
                        const float vn = glm::dot(rel, nrm);
                        // Kill any residual inward velocity (cloth contacts are inelastic; a
                        // bouncing sheet looks like rubber).
                        glm::vec3 vt = rel - vn * nrm;
                        if (vn < 0.0f) rel -= vn * nrm;

                        // Coulomb friction. `depth` is the penetration the projection just
                        // removed, so depth/hs is precisely the normal velocity the contact had to
                        // absorb -- i.e. a velocity-space stand-in for the normal impulse. A
                        // particle resting under gravity accumulates depth = g*hs^2 each substep,
                        // giving a tangential budget of friction*g*hs per substep, i.e. a
                        // deceleration of exactly friction*g. That is textbook Coulomb friction,
                        // which is why this proxy is used rather than a tuned damping factor.
                        const float budget = c.params.friction * depth * inv_hs;
                        const float vt_len = glm::length(vt);
                        if (vt_len > util::k_epsilon) {
                            rel -= vt * std::min(1.0f, budget / vt_len);
                        }
                        p.velocity = rel + surface;
                    }
                }

                const float v2 = glm::dot(p.velocity, p.velocity);
                if (v2 > max_velocity2) {
                    p.velocity *= c.params.max_velocity / std::sqrt(v2);
                }
            }
        });

        c.time += hs;
    }

    // Anchors have reached their targets; next step's lerp starts from here.
    for (ClothAnchor& a : c.anchors) a.prev_world_position = a.world_position;
    if (has_skin) {
        c.skin_targets.prev_position = c.skin_targets.position;
        if (has_backstop) c.skin_targets.prev_normal = c.skin_targets.normal;
    }

    c.bounds = compute_bounds(c);
}

/** @brief Serial convenience overload -- what test.cpp and any job-engine-less caller uses. */
inline void solve_cloth(Cloth& c, const std::vector<ClothCollider>& colliders,
                        const glm::vec3& gravity, uint32_t substeps, uint32_t iterations,
                        float h, ClothSolverScratch& scratch) {
    solve_cloth(c, colliders, gravity, substeps, iterations, h, scratch, SerialDispatch{});
}

} // namespace cloth
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_CLOTH_CLOTH_SOLVER_H

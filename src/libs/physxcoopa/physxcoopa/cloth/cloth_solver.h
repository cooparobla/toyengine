/**
 * @file cloth_solver.h
 * @brief The XPBD cloth substep loop: integrate, anchor, project constraints, collide, re-derive
 *        velocities.
 *
 * Stage order per cloth substep, and why it is this order:
 *
 *   1. aerodynamics   per-triangle drag/lift accumulated into a per-particle acceleration
 *   2. integrate      v += a*hs; v *= 1/(1+damping*hs); prev = p; p += v*hs
 *   3. anchors        pinned particles written outright from their body's pose
 *   4. constraints    stretch then bend, batch by batch, `iterations` times
 *   5. tethers        one-sided max-distance clamp to the nearest anchor
 *   6. self-collision optional particle-vs-particle separation
 *   7. collisions     project out of every candidate rigid shape
 *   8. finalize       v = (p - prev)/hs, then Coulomb friction at recorded contacts
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
 * @struct ClothSolverScratch
 * @brief Reusable per-solve working memory. One instance per PhysicsWorld, resized on first use.
 */
struct ClothSolverScratch {
    std::vector<glm::vec3> accel;          /**< Per-particle aerodynamic acceleration. */
    std::vector<glm::vec3> contact_normal; /**< Accumulated contact normal; zero means no contact. */
    std::vector<float> contact_depth;      /**< Total correction applied this substep, metres. */

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

/** @brief Projects one XPBD distance constraint. Returns immediately when both endpoints are
 *         pinned (w == 0), which is also what stops a division by zero at zero compliance. */
inline void project_distance(std::vector<ClothParticle>& particles, ClothConstraint& k,
                             float alpha) {
    ClothParticle& pa = particles[k.a];
    ClothParticle& pb = particles[k.b];
    const float w = pa.inv_mass + pb.inv_mass;
    if (w <= 0.0f) return;

    glm::vec3 d = pa.position - pb.position;
    const float len = glm::length(d);
    if (len < util::k_epsilon) return; // coincident: the gradient is undefined, skip this substep
    const glm::vec3 n = d / len;

    const float c = len - k.rest_length;
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

    // Compliance is divided by hs^2 exactly once per solve: it is constant across substeps and
    // iterations, and this is the only place the timestep enters the constraint math at all --
    // which is what makes XPBD stiffness timestep-independent (see ClothParams' doc).
    const float stretch_alpha = c.params.stretch_compliance / (hs * hs);
    const float bend_alpha = c.params.bend_compliance / (hs * hs);

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

        // --- 4. Constraints -------------------------------------------------------------------
        for (ClothConstraint& k : c.stretch) k.lambda = 0.0f;
        for (ClothConstraint& k : c.bend) k.lambda = 0.0f;

        for (uint32_t it = 0; it < iters; ++it) {
            for (const ConstraintBatch& b : c.stretch_batches) {
                dispatch(b.end - b.begin, [&](std::size_t begin, std::size_t end) {
                    for (std::size_t i = begin; i < end; ++i) {
                        detail::project_distance(c.particles, c.stretch[b.begin + i], stretch_alpha);
                    }
                });
            }
            for (const ConstraintBatch& b : c.bend_batches) {
                dispatch(b.end - b.begin, [&](std::size_t begin, std::size_t end) {
                    for (std::size_t i = begin; i < end; ++i) {
                        detail::project_distance(c.particles, c.bend[b.begin + i], bend_alpha);
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
        if (!colliders.empty()) {
            dispatch(n, [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) {
                    ClothParticle& p = c.particles[i];
                    if (p.inv_mass == 0.0f) continue;
                    for (const ClothCollider& collider : colliders) {
                        // Cheap AABB reject first: the sheet spans many colliders' worth of space,
                        // so most (particle, collider) pairs are decided by six compares. The
                        // thickness margin is applied inline rather than via AABB::expand(), which
                        // would construct a temporary per particle per collider.
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
                        // ended up across the surface since: integration, constraints, tethers and
                        // self-collision alike, not just its ballistic motion.
                        if (!project_particle(collider, c.params.thickness, p.position, normal, depth,
                                              &p.prev_position)) {
                            continue;
                        }
                        scratch.contact_normal[i] += normal;
                        scratch.contact_depth[i] += depth;
                    }
                }
            });
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
                        const float vn = glm::dot(p.velocity, nrm);
                        // Kill any residual inward velocity (cloth contacts are inelastic; a
                        // bouncing sheet looks like rubber).
                        glm::vec3 vt = p.velocity - vn * nrm;
                        if (vn < 0.0f) p.velocity -= vn * nrm;

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
                            p.velocity -= vt * std::min(1.0f, budget / vt_len);
                        }
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

/**
 * @file strand_solver.h
 * @brief The XPBD substep loop for hair: `solve_strands()` and its allocate-once scratch.
 *
 * ## One substep
 *
 * ```text
 * for s in 0..substeps:                     hs = h / substeps
 *   1. aerodynamics     wind drag -> per-particle acceleration
 *   2. integrate        v += a*hs; v *= 1/(1+damping*hs); prev = p; p += v*hs
 *   3. roots            pinned particles written from the strand's root, lerped across substeps
 *   4. stretch          XPBD distance constraints, batch by batch, `iterations` times
 *   5. global shape     toward the combed pose carried by the live root frame
 *   6. local shape      restores each segment's rest angle to the one before it
 *   7. collisions       project out of every candidate rigid shape
 *   8. finalize         v = (p - prev)/hs, then Coulomb friction at recorded contacts
 * ```
 *
 * The ORDER of 4-7 is the part that is easy to get wrong. Collisions run **last of the position
 * stages**, exactly as in cloth: the stretch and the two shape constraints can all push a
 * particle back into a body, so whichever runs last is the one that holds, and a strand visibly
 * sunk into a shoulder costs far more than a fraction of a millimetre of constraint violation.
 *
 * The two shape constraints run **after** stretch rather than before, because they are soft
 * blends toward a target and would otherwise be undone by the stiff distance projection that
 * followed them.
 *
 * ## Why velocities are re-derived, not tracked
 *
 * Stage 8 recomputes velocity from the positions rather than accumulating impulses through the
 * projections. That is the defining property of PBD: every correction automatically becomes a
 * velocity change, with no per-stage bookkeeping that could disagree with where the particles
 * actually ended up.
 */

#ifndef PHYSXCOOPA_HAIR_STRAND_SOLVER_H
#define PHYSXCOOPA_HAIR_STRAND_SOLVER_H

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <physxcoopa/cloth/cloth_collision.h>
#include <physxcoopa/cloth/cloth_solver.h>
#include <physxcoopa/hair/strand.h>
#include <physxcoopa/util/math.h>

namespace coopa {
namespace physx {
namespace hair {

/**
 * @brief The step the shape constraints' strengths are authored against: one frame at 60 Hz.
 *
 * `global_shape` and `local_shape` mean "this fraction of the way back toward the groomed pose
 * per 1/60 s", whatever the substep length actually is. See solve_strands().
 */
constexpr float k_reference_step = 1.0f / 60.0f;

/**
 * @struct StrandSolverScratch
 * @brief Allocate-once workspace, so a step allocates nothing.
 */
struct StrandSolverScratch {
    std::vector<glm::vec3> contact_normal;  ///< Per particle, this substep's contact normal.
    std::vector<float>     contact_depth;   ///< Per particle, how far it was pushed out.
    std::vector<uint8_t>   contacted;       ///< Per particle, whether stage 7 touched it.

    void ensure(std::size_t n) {
        if (contact_normal.size() >= n) return;
        contact_normal.resize(n);
        contact_depth.resize(n);
        contacted.resize(n);
    }
};

namespace detail {

/**
 * The smallest rotation taking `from` to `to`, both expected unit.
 *
 * Used by the local shape constraint to carry a rest offset by however much the segment BEFORE
 * it has turned, which is what "restore the bend angle" means without committing to a frame
 * convention (and so without the roll ambiguity picking an up-vector would introduce).
 */
inline glm::quat rotation_between(const glm::vec3& from, const glm::vec3& to) {
    const float d = glm::clamp(glm::dot(from, to), -1.0f, 1.0f);
    if (d > 0.999999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.999999f) {
        // Exactly opposed: every axis perpendicular to `from` is equally correct, so pick a
        // stable one rather than letting a cross product of parallel vectors decide.
        glm::vec3 axis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), from);
        if (glm::dot(axis, axis) < 1e-8f) axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), from);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 axis = glm::cross(from, to);
    const float len = glm::length(axis);
    if (len < 1e-8f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::angleAxis(std::acos(d), axis / len);
}

} // namespace detail

/**
 * @brief Advances one strand set by `h` seconds.
 *
 * @param set        The set, mutated in place. Every strand's `root_position`/`root_orientation`
 *                   must already hold this frame's target, and `prev_root_*` the last one.
 * @param colliders  Rigid shapes to collide against, snapshotted at this step's poses.
 * @param gravity    World gravity (m/s^2), before `StrandParams::gravity_scale`.
 * @param substeps   Substeps within `h`; clamped to at least 1. A per-set override wins.
 * @param iterations Constraint iterations per substep; clamped to at least 1.
 * @param h          Step length in seconds -- one rendered frame.
 * @param scratch    Reusable working memory, resized on demand.
 */
inline void solve_strands(StrandSet& set, const std::vector<cloth::ClothCollider>& colliders,
                          const glm::vec3& gravity, uint32_t substeps, uint32_t iterations,
                          float h, StrandSolverScratch& scratch) {
    const std::size_t n = set.particles.size();
    if (n == 0 || set.strands.empty() || h <= 0.0f) return;

    const uint32_t steps = std::max<uint32_t>(1, set.params.substeps ? set.params.substeps : substeps);
    const uint32_t iters = std::max<uint32_t>(1, set.params.iterations ? set.params.iterations : iterations);
    const float hs = h / static_cast<float>(steps);
    const float inv_hs = 1.0f / hs;
    scratch.ensure(n);

    // Compliance enters the constraint maths exactly once per solve: it is constant across
    // substeps and iterations, and XPBD scales it by 1/hs^2.
    const float alpha = set.params.stretch_compliance * inv_hs * inv_hs;

    const glm::vec3 accel = gravity * set.params.gravity_scale + set.params.external_acceleration;
    const float max_speed2 = set.params.max_velocity * set.params.max_velocity;

    // Both shape constraints are authored as "fraction of the way back per 1/60 s" and converted
    // to THIS substep's length here, so the TOTAL pull per frame is the authored number whatever
    // the substep count. Applying the authored fraction once per substep instead compounds it:
    // 0.22 at four substeps becomes a 63% pull per frame, so a groom stiffens as the substep
    // count rises and the same hair feels different on two machines. Same class of bug cloth
    // avoids by damping with 1/(1 + damping*h) rather than (1 - damping*h).
    //
    // This equalises the STRENGTH, not the settled pose: a positional projection interleaved with
    // gravity integration still wins slightly more when it runs more often, so a strand hangs a
    // little higher at eight substeps than at one. That residue is small and bounded; the
    // compounding it replaces was a factor of three.
    const float substep_fraction = hs / k_reference_step;
    const float global_per_substep =
        1.0f - std::pow(1.0f - glm::clamp(set.params.global_shape, 0.0f, 1.0f), substep_fraction);
    const float local_per_substep =
        1.0f - std::pow(1.0f - glm::clamp(set.params.local_shape, 0.0f, 1.0f), substep_fraction);

    for (uint32_t s = 0; s < steps; ++s) {
        // How far through the frame this substep ENDS, for lerping the roots.
        const float t = static_cast<float>(s + 1) / static_cast<float>(steps);

        // --- 1 + 2: aerodynamics and integration ---------------------------------------------
        for (std::size_t i = 0; i < n; ++i) {
            cloth::ClothParticle& p = set.particles[i];
            if (p.inv_mass <= 0.0f) continue;

            glm::vec3 a = accel;
            if (set.params.air_drag > 0.0f) {
                glm::vec3 wind = set.params.wind;
                if (set.params.wind_turbulence > 0.0f) {
                    // Keyed on position and the set's own accumulated time, so the same scene
                    // replays identically -- never on a frame counter or a global clock.
                    const float phase = set.time * 2.3f + p.position.x * 1.7f +
                                        p.position.y * 2.1f + p.position.z * 1.3f;
                    wind += set.params.wind_turbulence * glm::length(set.params.wind) *
                            glm::vec3(std::sin(phase), std::sin(phase * 1.37f + 2.1f),
                                      std::sin(phase * 0.81f + 4.2f));
                }
                // Drag toward the air's velocity, not a push along the wind: that is what makes
                // hair trail behind a head that moves through still air.
                a += (wind - p.velocity) * set.params.air_drag;
            }

            p.velocity += a * hs;
            p.velocity *= 1.0f / (1.0f + set.params.damping * hs);
            const float v2 = glm::dot(p.velocity, p.velocity);
            if (v2 > max_speed2) p.velocity *= set.params.max_velocity / std::sqrt(v2);
            p.prev_position = p.position;
            p.position += p.velocity * hs;
        }

        // --- 3: roots ------------------------------------------------------------------------
        // Lerped across the substeps rather than jumped at the first, so a head that turns fast
        // does not whip-crack its hair. Same reason cloth lerps its anchors.
        for (Strand& strand : set.strands) {
            if (strand.count == 0) continue;
            const glm::vec3 root = glm::mix(strand.prev_root_position, strand.root_position, t);
            cloth::ClothParticle& p = set.particles[strand.first];
            p.prev_position = p.position;
            p.position = root;
            p.velocity = glm::vec3(0.0f);
        }

        // --- 4: stretch ----------------------------------------------------------------------
        for (cloth::ClothConstraint& k : set.stretch) k.lambda = 0.0f;
        for (uint32_t it = 0; it < iters; ++it) {
            for (const cloth::ConstraintBatch& batch : set.stretch_batches) {
                for (uint32_t ci = batch.begin; ci < batch.end; ++ci) {
                    cloth::detail::project_distance(set.particles, set.stretch[ci], alpha);
                }
            }
        }

        // --- 5: global shape -----------------------------------------------------------------
        // Toward where this particle was combed, carried by the root's LIVE orientation. Falls
        // off toward the tip so hair swings from its ends instead of moving as one board.
        if (set.params.global_shape > 0.0f) {
            for (const Strand& strand : set.strands) {
                if (strand.count < 2) continue;
                const glm::quat orientation =
                    glm::slerp(strand.prev_root_orientation, strand.root_orientation, t);
                const glm::vec3 root = glm::mix(strand.prev_root_position, strand.root_position, t);
                const float inv_last = 1.0f / static_cast<float>(strand.count - 1);
                for (uint32_t k = 1; k < strand.count; ++k) {
                    const std::size_t i = strand.first + k;
                    cloth::ClothParticle& p = set.particles[i];
                    if (p.inv_mass <= 0.0f) continue;
                    const float along = static_cast<float>(k) * inv_last;
                    const float weight = global_per_substep *
                                         std::pow(1.0f - along, set.params.global_shape_falloff);
                    if (weight <= 0.0f) continue;
                    const glm::vec3 target = root + orientation * set.rest_local[i];
                    p.position = glm::mix(p.position, target, glm::min(weight, 1.0f));
                }
            }
        }

        // --- 6: local shape ------------------------------------------------------------------
        // Restores the ANGLE each segment makes with the one before it, which is what a curl is.
        // The correction is split between the two particles so it adds no net momentum; the root
        // takes none of it, being pinned.
        if (set.params.local_shape > 0.0f) {
            const float k_local = local_per_substep;
            for (const Strand& strand : set.strands) {
                if (strand.count < 3) continue;
                for (uint32_t k = 1; k + 1 < strand.count; ++k) {
                    const std::size_t prev = strand.first + k - 1;
                    const std::size_t cur  = strand.first + k;
                    const std::size_t next = strand.first + k + 1;

                    // How far the segment BEFORE this one has turned away from its rest
                    // direction. For k == 1 that segment is the root's own, so a turning head
                    // carries the whole curl with it -- no special case needed.
                    const glm::vec3 rest_prev = set.rest_offset[cur];
                    const glm::vec3 live_prev = set.particles[cur].position - set.particles[prev].position;
                    const float rest_len = glm::length(rest_prev);
                    const float live_len = glm::length(live_prev);
                    if (rest_len <= 1e-8f || live_len <= 1e-8f) continue;
                    const glm::quat turn =
                        detail::rotation_between(rest_prev / rest_len, live_prev / live_len);

                    const glm::vec3 target = set.particles[cur].position + turn * set.rest_offset[next];
                    const glm::vec3 delta  = (target - set.particles[next].position) * k_local;

                    cloth::ClothParticle& a = set.particles[cur];
                    cloth::ClothParticle& b = set.particles[next];
                    const float w = a.inv_mass + b.inv_mass;
                    if (w <= 0.0f) continue;
                    b.position += delta * (b.inv_mass / w);
                    a.position -= delta * (a.inv_mass / w);
                }
            }
        }

        // --- 7: collisions, LAST of the position stages --------------------------------------
        std::fill(scratch.contacted.begin(), scratch.contacted.begin() + static_cast<long>(n), 0);
        if (!colliders.empty()) {
            for (std::size_t i = 0; i < n; ++i) {
                cloth::ClothParticle& p = set.particles[i];
                if (p.inv_mass <= 0.0f) continue;   // a pinned root inside a collider stays there
                for (const cloth::ClothCollider& collider : colliders) {
                    glm::vec3 normal(0.0f);
                    float depth = 0.0f;
                    if (cloth::project_particle(collider, set.params.thickness, p.position, normal,
                                                depth, &p.prev_position)) {
                        // Record the DEEPEST contact's normal for stage 8's friction, not merely
                        // the last one in the list: with two colliders touching one particle (a
                        // head and a neck it rests against, say) the last-wins order is arbitrary,
                        // while the deepest is the one actually holding the particle in place.
                        // The position correction from every contact still applies, in order.
                        if (scratch.contacted[i] == 0 || depth > scratch.contact_depth[i]) {
                            scratch.contact_normal[i] = normal;
                            scratch.contact_depth[i] = depth;
                        }
                        scratch.contacted[i] = 1;
                    }
                }
            }
        }

        // --- 8: finalize + friction ----------------------------------------------------------
        for (std::size_t i = 0; i < n; ++i) {
            cloth::ClothParticle& p = set.particles[i];
            if (p.inv_mass <= 0.0f) continue;
            p.velocity = (p.position - p.prev_position) * inv_hs;

            if (scratch.contacted[i] != 0 && set.params.friction > 0.0f) {
                // Coulomb, positionally: the tangential part of this substep's motion is damped
                // by at most the normal correction times the friction coefficient, which is what
                // lets hair rest on a shoulder instead of sliding straight off it.
                const glm::vec3& normal = scratch.contact_normal[i];
                const glm::vec3 motion = p.position - p.prev_position;
                const glm::vec3 tangential = motion - normal * glm::dot(motion, normal);
                const float t_len = glm::length(tangential);
                if (t_len > util::k_epsilon) {
                    const float limit = set.params.friction * scratch.contact_depth[i];
                    const float scale = glm::max(0.0f, 1.0f - glm::min(limit, t_len) / t_len);
                    p.position -= tangential * (1.0f - scale);
                    p.velocity = (p.position - p.prev_position) * inv_hs;
                }
            }
        }

        set.time += hs;
    }

    set.bounds = compute_bounds(set);
}

} // namespace hair
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_HAIR_STRAND_SOLVER_H

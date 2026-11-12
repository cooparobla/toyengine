/**
 * @file solver.h
 * @brief Sequential-impulse split-impulse solver: warm start, base velocity iterations,
 *        a final restitution correction, position correction, then island-based sleep/wake.
 *
 * Split-impulse, not Baumgarte-in-the-velocity-solver: the velocity solve below carries zero
 * positional bias -- all positional correction happens in solve_position(), separately, after
 * velocity has already converged. Pairing a velocity-solver bias with a separate position pass
 * (the more common "textbook" scheme) double-corrects, since the bias injects energy the
 * position pass then has to absorb again -- visible as stacks that gently "breathe."
 *
 * Restitution ordering: the main velocity iterations always target 0 (non-penetration +
 * friction), converging a base resting/sliding solution across every contact; restitution is
 * then applied as a small number of FINAL iterations on top of that base, not mixed into it
 * and not run before it -- see solve()'s comment for why either of those orderings quietly
 * cancels the bounce it was supposed to produce.
 */

#ifndef PHYSXCOOPA_DYNAMICS_SOLVER_H
#define PHYSXCOOPA_DYNAMICS_SOLVER_H

#include <physxcoopa/dynamics/body.h>
#include <physxcoopa/dynamics/island.h>
#include <physxcoopa/dynamics/joint.h>
#include <physxcoopa/collision/manifold.h>
#include <physxcoopa/util/config.h>
#include <physxcoopa/util/math.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coopa {
namespace physx {
namespace dynamics {

/**
 * @brief Packs an ordered body-index pair and a feature id into one warm-start cache key.
 *
 * Ordering by (min,max) index means the same physical contact hashes identically regardless
 * of which body happens to be manifold.a vs manifold.b this step.
 */
inline uint64_t pack_warm_start_key(uint32_t index_a, uint32_t index_b, uint32_t feature_id) {
    uint32_t lo = std::min(index_a, index_b);
    uint32_t hi = std::max(index_a, index_b);
    uint64_t key = (static_cast<uint64_t>(lo) << 40) ^ (static_cast<uint64_t>(hi) << 12) ^ feature_id;
    return key;
}

/**
 * @struct SolverState
 * @brief Solver state that persists across steps -- currently just the warm-start impulse
 *        cache. Owned by PhysicsWorld, passed into solve() by reference each substep.
 *
 * The cache is a hash map keyed by an integer (not a pointer), and is only ever touched via
 * point lookups/inserts by a specific computed key -- never iterated for output -- so it does
 * not violate the "no pointer-keyed hash map may be iterated inside step_fixed" determinism
 * rule (see the plan's "Determinism and parallelism").
 */
struct SolverState {
    struct CachedImpulse {
        float normal = 0.0f;
        float tangent[2] = {0.0f, 0.0f};
    };
    std::unordered_map<uint64_t, CachedImpulse> impulses;
};

/**
 * @brief Applies a world-space impulse at `point` to the pair (A,B): -impulse to A, +impulse
 *        to B (impulse is defined along the manifold's A-to-B normal convention). No-op on
 *        the side(s) that aren't an AWAKE dynamic body.
 *
 * The `awake` half of that guard is load-bearing, not defensive: every caller (warm_start(),
 * solve_velocity_pass()) already zeroes a sleeping body's inv_mass/inv_inertia before deriving
 * the impulse's magnitude, correctly treating it as immovable on its side of the contact. If
 * this function applied that impulse to a sleeping body's velocity anyway (checking only
 * `type == Dynamic`), the body would end up with a nonzero velocity the solve
 * never accounted for, silently, without ever being marked awake -- integrate_velocities()
 * skips a sleeping body, so nothing visibly moves yet, but the stale velocity sits there until
 * something later wakes the body, which then pops/twitches with this leftover, non-physical
 * value already baked in.
 */
inline void apply_impulse_pair(Body& a, Body& b, const glm::vec3& point, const glm::vec3& impulse) {
    if (a.type == BodyType::Dynamic && a.awake) {
        a.linear_velocity -= impulse * a.inv_mass;
        a.angular_velocity -= a.inv_inertia_world * glm::cross(point - a.position, impulse);
    }
    if (b.type == BodyType::Dynamic && b.awake) {
        b.linear_velocity += impulse * b.inv_mass;
        b.angular_velocity += b.inv_inertia_world * glm::cross(point - b.position, impulse);
    }
}

/**
 * @brief Applies a pure angular impulse (no linear component, no moment arm) to both bodies of
 *        a pair -- the joint solver's counterpart to apply_impulse_pair() above, for the
 *        angular-only constraints a hinge needs (axis alignment, angle limits) that have no
 *        associated world point the way a contact or a joint's point constraint does. Same
 *        Dynamic-and-awake gating, same a-=/b+= sign convention.
 */
inline void apply_angular_impulse_pair(Body& a, Body& b, const glm::vec3& angular_impulse) {
    if (a.type == BodyType::Dynamic && a.awake) {
        a.angular_velocity -= a.inv_inertia_world * angular_impulse;
    }
    if (b.type == BodyType::Dynamic && b.awake) {
        b.angular_velocity += b.inv_inertia_world * angular_impulse;
    }
}

/**
 * @brief Captures each contact point's pre-solve approach velocity and derives its
 *        restitution target, before any impulse (warm-start or otherwise) is applied this step.
 *
 * `restitution_bias` is left at 0 (a resting contact) unless the approach speed exceeds
 * config.restitution_threshold -- below that, restitution is deliberately zero regardless of
 * the material's configured value, which is what stops resting contacts from micro-bouncing.
 */
inline void capture_restitution_bias(std::vector<Body>& bodies, std::vector<collision::ContactManifold>& manifolds,
                                      const util::PhysicsConfig& config) {
    for (auto& m : manifolds) {
        if (!m.valid || m.is_trigger) continue;
        Body& a = bodies[m.a.index];
        Body& b = bodies[m.b.index];
        for (uint8_t i = 0; i < m.count; ++i) {
            auto& p = m.points[i];
            glm::vec3 ra = p.position - a.position;
            glm::vec3 rb = p.position - b.position;
            glm::vec3 va = a.linear_velocity + glm::cross(a.angular_velocity, ra);
            glm::vec3 vb = b.linear_velocity + glm::cross(b.angular_velocity, rb);
            float v_approach = glm::dot(vb - va, m.normal);
            p.restitution_bias = (-v_approach > config.restitution_threshold) ? -m.restitution * v_approach : 0.0f;
        }
    }
}

/** @brief Seeds each point's impulse accumulators from the previous step's cache (if any
 *         point with the same key existed) and immediately re-applies that impulse, so the
 *         first velocity iteration this step starts from last step's converged solution
 *         instead of zero -- the single biggest quality difference between a settling stack
 *         and one that jitters forever. */
inline void warm_start(std::vector<Body>& bodies, std::vector<collision::ContactManifold>& manifolds,
                        SolverState& state) {
    for (auto& m : manifolds) {
        if (!m.valid || m.is_trigger) continue;
        Body& a = bodies[m.a.index];
        Body& b = bodies[m.b.index];
        bool a_active = a.type == BodyType::Dynamic && a.awake;
        bool b_active = b.type == BodyType::Dynamic && b.awake;
        if (!a_active && !b_active) continue; // a sleeping body has no gravity to counteract
                                               // this step -- re-applying its last impulse
                                               // every step would accelerate it forever
        glm::vec3 t1, t2;
        util::orthonormal_basis(m.normal, t1, t2);

        for (uint8_t i = 0; i < m.count; ++i) {
            auto& p = m.points[i];
            uint64_t key = pack_warm_start_key(m.a.index, m.b.index, p.feature_id);
            auto it = state.impulses.find(key);
            if (it != state.impulses.end()) {
                p.normal_impulse = it->second.normal;
                p.tangent_impulse[0] = it->second.tangent[0];
                p.tangent_impulse[1] = it->second.tangent[1];
            } else {
                p.normal_impulse = 0.0f;
                p.tangent_impulse[0] = 0.0f;
                p.tangent_impulse[1] = 0.0f;
            }
            glm::vec3 impulse = m.normal * p.normal_impulse + t1 * p.tangent_impulse[0] + t2 * p.tangent_impulse[1];
            if (glm::dot(impulse, impulse) > 0.0f) apply_impulse_pair(a, b, p.position, impulse);
        }
    }
}

/**
 * @brief One Gauss-Seidel velocity-iteration pass over every manifold point: solves the
 *        clamped normal constraint (with restitution when `use_restitution`), then Coulomb
 *        friction against the just-updated accumulated normal impulse, applying each point's
 *        impulse immediately (so later points/manifolds see its effect this same pass).
 *
 * Sequential (not Jacobi) application is load-bearing here, not just the simpler option: a
 * box resting on a slope needs friction (acting at the contact face, offset from the center
 * of mass along the normal) to redistribute normal force asymmetrically across the contact
 * points -- more on the downhill edge, less on the uphill edge -- which is what actually
 * keeps a physically-flush box from rotating/sliding under a friction coefficient above
 * tan(slope angle). That redistribution is a real physical effect (shifting the torque
 * reference point from the contact patch to the center of mass, which sits off the patch
 * along the normal, turns a symmetric tangential force into a net torque -- see the
 * accompanying plan notes), and it converges far faster when each point reacts to the
 * immediately-preceding points' impulses than when all points solve from one shared
 * pre-pass snapshot.
 *
 * `order_offset` rotates which point in each manifold is solved first across calls (see
 * solve()) -- for a manifold with no genuine asymmetric-load requirement (e.g. a box resting
 * flat on another box), sequential order alone gives the first-solved point a consistent
 * advantage that shows up as a small torque bias, which does not average out over repeated
 * iterations in the SAME order; cycling the starting point through all of a manifold's points
 * (rather than just reversing, which only swaps a 2-way bias rather than cancelling it) does,
 * while still preserving the sequential coupling the slope case above depends on.
 *
 * Never applies positional bias -- see the file doc for why that's the whole point. The one
 * exception, in spirit if not in mechanism: a SPECULATIVE point (ContactPoint::separation > 0,
 * see its own doc and collision/sat.h's `allow_separated`) targets `-separation / h` instead of
 * 0/`restitution_bias` -- not a positional-correction bias (nothing here moves position; this
 * is still a pure velocity constraint), but a velocity CEILING that permits closing the gap by
 * at most `separation` over this one substep, which is exactly what stops a fast body from
 * tunneling through a thin target before the two shapes ever actually overlap. An ordinary
 * point's `separation` is always 0, so for it `target` is just 0/`restitution_bias`.
 */
inline void solve_velocity_pass(std::vector<Body>& bodies, std::vector<collision::ContactManifold>& manifolds,
                                 bool use_restitution, uint32_t order_offset, float h) {
    for (auto& m : manifolds) {
        if (!m.valid || m.is_trigger) continue;
        Body& a = bodies[m.a.index];
        Body& b = bodies[m.b.index];
        bool a_dyn = a.type == BodyType::Dynamic && a.awake;
        bool b_dyn = b.type == BodyType::Dynamic && b.awake;
        if (!a_dyn && !b_dyn) continue;

        float inv_mass_a = a_dyn ? a.inv_mass : 0.0f;
        float inv_mass_b = b_dyn ? b.inv_mass : 0.0f;
        glm::mat3 inv_ia = a_dyn ? a.inv_inertia_world : glm::mat3(0.0f);
        glm::mat3 inv_ib = b_dyn ? b.inv_inertia_world : glm::mat3(0.0f);

        glm::vec3 t1, t2;
        util::orthonormal_basis(m.normal, t1, t2);

        for (uint8_t k = 0; k < m.count; ++k) {
            uint8_t i = static_cast<uint8_t>((k + order_offset) % m.count);
            auto& p = m.points[i];
            glm::vec3 ra = p.position - a.position;
            glm::vec3 rb = p.position - b.position;

            // --- Normal constraint ---
            {
                glm::vec3 ra_x_n = glm::cross(ra, m.normal);
                glm::vec3 rb_x_n = glm::cross(rb, m.normal);
                float k_normal = inv_mass_a + inv_mass_b +
                                  glm::dot(ra_x_n, inv_ia * ra_x_n) +
                                  glm::dot(rb_x_n, inv_ib * rb_x_n);
                if (k_normal > util::k_epsilon) {
                    glm::vec3 va = a.linear_velocity + glm::cross(a.angular_velocity, ra);
                    glm::vec3 vb = b.linear_velocity + glm::cross(b.angular_velocity, rb);
                    float vn = glm::dot(vb - va, m.normal);
                    float target = p.separation > 0.0f ? -p.separation / h
                                                         : (use_restitution ? p.restitution_bias : 0.0f);
                    float lambda = -(vn - target) / k_normal;
                    float new_impulse = std::max(p.normal_impulse + lambda, 0.0f);
                    float delta = new_impulse - p.normal_impulse;
                    p.normal_impulse = new_impulse;
                    apply_impulse_pair(a, b, p.position, m.normal * delta);
                }
            }

            // --- Friction (Coulomb, bounded by the just-updated accumulated normal impulse) ---
            const glm::vec3* tangents[2] = {&t1, &t2};
            for (int t = 0; t < 2; ++t) {
                const glm::vec3& dir = *tangents[t];
                glm::vec3 ra_x_t = glm::cross(ra, dir);
                glm::vec3 rb_x_t = glm::cross(rb, dir);
                float k_t = inv_mass_a + inv_mass_b +
                            glm::dot(ra_x_t, inv_ia * ra_x_t) +
                            glm::dot(rb_x_t, inv_ib * rb_x_t);
                if (k_t <= util::k_epsilon) continue;

                glm::vec3 va = a.linear_velocity + glm::cross(a.angular_velocity, ra);
                glm::vec3 vb = b.linear_velocity + glm::cross(b.angular_velocity, rb);
                float vt = glm::dot(vb - va, dir);
                float lambda_t = -vt / k_t;
                float max_friction = m.friction * p.normal_impulse;
                float old_impulse = p.tangent_impulse[t];
                float new_impulse = std::clamp(old_impulse + lambda_t, -max_friction, max_friction);
                float delta = new_impulse - old_impulse;
                p.tangent_impulse[t] = new_impulse;
                apply_impulse_pair(a, b, p.position, dir * delta);
            }
        }
    }
}

/**
 * @brief Linear-only non-linear-Gauss-Seidel position correction: nudges body positions
 *        directly along each manifold's normal, tracking how much correction has already
 *        been consumed via each body's cumulative displacement since `ref_position` so
 *        successive iterations converge rather than repeatedly over-correcting the same
 *        original penetration value.
 *
 * v1 simplification: rotational correction is not applied, only translation -- acceptable
 * for the shapes/scenarios v1 targets, and does not inject velocity (only position moves).
 */
inline void solve_position(std::vector<Body>& bodies, std::vector<collision::ContactManifold>& manifolds,
                            const util::PhysicsConfig& config) {
    std::vector<glm::vec3> ref_position(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i) ref_position[i] = bodies[i].position;

    for (uint32_t iter = 0; iter < config.position_iterations; ++iter) {
        for (auto& m : manifolds) {
            if (!m.valid || m.is_trigger) continue;
            Body& a = bodies[m.a.index];
            Body& b = bodies[m.b.index];
            bool a_dyn = a.type == BodyType::Dynamic && a.awake;
            bool b_dyn = b.type == BodyType::Dynamic && b.awake;
            if (!a_dyn && !b_dyn) continue;

            float inv_mass_a = a_dyn ? a.inv_mass : 0.0f;
            float inv_mass_b = b_dyn ? b.inv_mass : 0.0f;
            float k = inv_mass_a + inv_mass_b;
            if (k <= util::k_epsilon) continue;

            glm::vec3 da = a.position - ref_position[m.a.index];
            glm::vec3 db = b.position - ref_position[m.b.index];

            for (uint8_t i = 0; i < m.count; ++i) {
                auto& p = m.points[i];
                float current_penetration = p.penetration - glm::dot(db - da, m.normal);
                float correction_mag = std::max(0.0f, current_penetration - config.linear_slop) * config.position_correction;
                correction_mag = std::min(correction_mag, config.max_linear_correction);
                if (correction_mag <= 0.0f) continue;

                glm::vec3 correction = m.normal * (correction_mag / k);
                if (a_dyn) { a.position -= correction * inv_mass_a; da = a.position - ref_position[m.a.index]; }
                if (b_dyn) { b.position += correction * inv_mass_b; db = b.position - ref_position[m.b.index]; }
            }
        }
    }
}

// --- Joints ---
//
// A Joint is an explicit, persistent object (not rediscovered by broadphase every substep), so
// unlike contacts its impulse accumulators live directly on the struct -- no warm-start
// key/hash-map needed (see Joint's own doc). The three passes below otherwise mirror the contact
// solver's shape exactly: warm start, a velocity pass (zero positional bias, split-impulse, same
// as contacts), then a separate NGS position-correction pass.
//
// Rows per JointType:
//   every type   point constraint (one 3x3 block -- see point_block_mass())
//   Hinge        axis alignment (2 rows) + optional one-sided angle limit (1 row)
//   ConeTwist    one-sided swing-cone limit (1 row) + one-sided twist limit (1 row)
//   Ball         nothing more

/**
 * @brief Twist angle (radians) of `b` relative to `a` about the hinge axis, measured from
 *        `rest_relative_rotation` (the relative orientation at joint creation) -- standard
 *        swing-twist decomposition: `delta = (inverse(rot_a) * rot_b) * inverse(rest)` is B's
 *        rotation relative to A since creation, expressed in A's own local frame (where
 *        `local_axis_a` -- unrotating, since it's already local -- is the reference axis); its
 *        TWIST component about that axis is `2 * atan2(dot(delta.xyz, axis), delta.w)`. Exact
 *        when `delta` is a pure rotation about `axis`; the axis-alignment constraint keeps the
 *        two hinge axes close enough to parallel every substep that the swing (non-twist)
 *        component this ignores stays negligible in practice. ConeTwist's twist limit measures
 *        its twist the same way (there the swing is real, and this is exactly the twist part of
 *        the swing-twist decomposition `delta = swing * twist`).
 */
inline float hinge_current_angle(const glm::quat& rot_a, const glm::quat& rot_b, const glm::vec3& local_axis_a,
                                  const glm::quat& rest_relative_rotation) {
    glm::quat rel = glm::inverse(rot_a) * rot_b;
    glm::quat delta = rel * glm::inverse(rest_relative_rotation);
    glm::vec3 axis = glm::normalize(local_axis_a);
    glm::vec3 delta_v(delta.x, delta.y, delta.z);
    float w = std::clamp(delta.w, -1.0f, 1.0f);
    // Shortest arc: q and -q are the same rotation, and only the w >= 0 one gives an angle in
    // (-pi, pi] (otherwise a small twist the other way reads as ~2 pi).
    if (w < 0.0f) { w = -w; delta_v = -delta_v; }
    return 2.0f * std::atan2(glm::dot(delta_v, axis), w);
}

/** @brief World twist axes of a cone-twist joint's two bodies (unit). */
inline void cone_twist_axes(const Body& a, const Body& b, const Joint& j, glm::vec3& ta, glm::vec3& tb) {
    ta = glm::normalize(a.orientation * j.local_axis_a);
    tb = glm::normalize(b.orientation * j.local_axis_b);
}

/**
 * @brief Swing state of a cone-twist joint: the angle between the two twist axes, and the unit
 *        axis rotating `b` about which INCREASES it (`cross(ta, tb)`, with a fallback
 *        perpendicular when the axes are parallel or anti-parallel).
 */
inline float cone_swing(const glm::vec3& ta, const glm::vec3& tb, glm::vec3& swing_axis) {
    glm::vec3 c = glm::cross(ta, tb);
    float s = glm::length(c);
    float angle = std::atan2(s, glm::dot(ta, tb));
    if (s > 1e-6f) {
        swing_axis = c / s;
    } else {
        glm::vec3 p, q;
        util::orthonormal_basis(ta, p, q);
        swing_axis = p;
    }
    return angle;
}

/** @brief Current ConeTwist twist angle (radians) -- hinge_current_angle() about `local_axis_a`. */
inline float cone_twist_angle(const Body& a, const Body& b, const Joint& j) {
    return hinge_current_angle(a.orientation, b.orientation, j.local_axis_a, j.rest_relative_rotation);
}

/**
 * @brief One one-sided angular limit row: `dir` is the world axis along which positive relative
 *        angular velocity (b minus a) moves AWAY from the limit, and `c` the remaining angle to it
 *        (positive inside the allowed range, negative past it). Same clamped-impulse pattern as a
 *        contact's normal constraint (`new = max(old + lambda, 0)`), and SPECULATIVE like one:
 *        inside the range the row lets the joint close at most the remaining gap this substep
 *        (`cdot >= -c / h`), so a fast spin stops AT the limit instead of overshooting it by a
 *        substep's worth of rotation and leaning on position correction to come back.
 */
inline void solve_angular_limit_row(Body& a, Body& b, const glm::mat3& inv_ia, const glm::mat3& inv_ib,
                                     const glm::vec3& dir, float c, float inv_h, float& accumulated) {
    float k_eff = glm::dot(dir, inv_ia * dir) + glm::dot(dir, inv_ib * dir);
    if (k_eff <= util::k_epsilon) return;
    float cdot = glm::dot(b.angular_velocity - a.angular_velocity, dir);
    float lambda = -(cdot + std::max(c, 0.0f) * inv_h) / k_eff;
    float new_impulse = std::max(accumulated + lambda, 0.0f);
    float delta = new_impulse - accumulated;
    accumulated = new_impulse;
    apply_angular_impulse_pair(a, b, dir * delta);
}

/**
 * @brief The side of a [lo, hi] angle range `angle` is nearer to, as solve_angular_limit_row()
 *        inputs: `sign` +1 for the lower limit (moving away = increasing), -1 for the upper, and
 *        `c` the signed remaining angle to that limit.
 */
inline void nearest_limit_side(float angle, float lo, float hi, float& sign, float& c) {
    float c_lo = angle - lo;
    float c_hi = hi - angle;
    if (c_lo <= c_hi) { sign = 1.0f; c = c_lo; }
    else { sign = -1.0f; c = c_hi; }
}

/**
 * @brief The point constraint's 3x3 effective-mass matrix `K` (relative anchor velocity change per
 *        unit impulse): `(m_a^-1 + m_b^-1) I - [r_a]x I_a^-1 [r_a]x - [r_b]x I_b^-1 [r_b]x`.
 *
 * Solving the three point rows as ONE block (`impulse = K^-1 * -cdot`) rather than as three
 * independent scalar rows along world X/Y/Z matters for anchors far off the centre of mass: the
 * lever arms couple the axes (a push along X spins the body, which moves the anchor along Y), so
 * per-axis rows fight each other and a chain of them (a ragdoll) is left visibly stretched after
 * a hard hit. False when the pair has no effective mass (both sides immovable).
 */
inline bool point_block_mass(const glm::vec3& ra, const glm::vec3& rb, float inv_mass_a, float inv_mass_b,
                             const glm::mat3& inv_ia, const glm::mat3& inv_ib, glm::mat3& k) {
    auto skew = [](const glm::vec3& v) {
        // glm is column-major: m[col][row]. skew(v) * x == cross(v, x).
        return glm::mat3(glm::vec3(0.0f, v.z, -v.y), glm::vec3(-v.z, 0.0f, v.x), glm::vec3(v.y, -v.x, 0.0f));
    };
    const glm::mat3 sa = skew(ra), sb = skew(rb);
    k = glm::mat3(inv_mass_a + inv_mass_b) - sa * inv_ia * sa - sb * inv_ib * sb;
    return std::abs(glm::determinant(k)) > 1e-12f;
}

/**
 * @brief NGS rotation of the pair that changes b's rotation RELATIVE to a by about `angle`
 *        radians about unit world axis `n` (split by inverse inertia, like the linear case splits
 *        by inverse mass). Clamped by the caller.
 */
inline void apply_angular_position_correction(Body& a, Body& b, bool a_dyn, bool b_dyn, const glm::mat3& inv_ia,
                                               const glm::mat3& inv_ib, const glm::vec3& n, float angle) {
    float k = glm::dot(n, inv_ia * n) + glm::dot(n, inv_ib * n);
    if (k <= util::k_epsilon) return;
    glm::vec3 correction = n * (angle / k);
    if (a_dyn) {
        glm::vec3 delta_rot = -(inv_ia * correction);
        glm::quat dq(0.0f, delta_rot.x, delta_rot.y, delta_rot.z);
        a.orientation = glm::normalize(a.orientation + 0.5f * dq * a.orientation);
    }
    if (b_dyn) {
        glm::vec3 delta_rot = inv_ib * correction;
        glm::quat dq(0.0f, delta_rot.x, delta_rot.y, delta_rot.z);
        b.orientation = glm::normalize(b.orientation + 0.5f * dq * b.orientation);
    }
}

/** @brief Re-applies each joint's own persistent impulse accumulators (point + hinge axis-
 *         alignment; NOT the limit impulses -- see solve_joint_velocity_pass()'s doc for why
 *         those aren't warm-started) so the first velocity iteration starts from last step's
 *         converged solution instead of zero. Same "skip if neither side is an awake dynamic
 *         body" gate as warm_start() for contacts, for the identical reason (a sleeping body has
 *         no gravity to counteract this step; re-applying its last impulse every step would
 *         accelerate it forever). */
inline void warm_start_joints(std::vector<Body>& bodies, std::vector<Joint>& joints) {
    for (auto& j : joints) {
        if (!j.valid || !j.enabled) continue;
        j.limit_impulse = 0.0f; // limits start every step cold -- see solve_joint_velocity_pass()
        j.swing_impulse = 0.0f;
        Body& a = bodies[j.a.index];
        Body& b = bodies[j.b.index];
        bool a_active = a.type == BodyType::Dynamic && a.awake;
        bool b_active = b.type == BodyType::Dynamic && b.awake;
        if (!a_active && !b_active) continue;

        glm::vec3 ra = a.orientation * j.local_anchor_a;
        glm::vec3 rb = b.orientation * j.local_anchor_b;

        glm::vec3 point_impulse = j.point_impulse; // world X/Y/Z components
        if (glm::dot(point_impulse, point_impulse) > 0.0f) {
            if (a_active) {
                a.linear_velocity -= point_impulse * a.inv_mass;
                a.angular_velocity -= a.inv_inertia_world * glm::cross(ra, point_impulse);
            }
            if (b_active) {
                b.linear_velocity += point_impulse * b.inv_mass;
                b.angular_velocity += b.inv_inertia_world * glm::cross(rb, point_impulse);
            }
        }

        if (j.type != JointType::Hinge) continue;
        glm::vec3 world_axis_a = glm::normalize(a.orientation * j.local_axis_a);
        glm::vec3 p, q;
        util::orthonormal_basis(world_axis_a, p, q);
        glm::vec3 axis_impulse = p * j.axis_impulse.x + q * j.axis_impulse.y;
        if (glm::dot(axis_impulse, axis_impulse) > 0.0f) apply_angular_impulse_pair(a, b, axis_impulse);
    }
}

/**
 * @brief One Gauss-Seidel velocity-iteration pass over every joint.
 *
 * Every type runs the point constraint (target 0, no clamp -- an equality constraint, unlike a
 * contact's one-sided normal), solved as one 3x3 block (see point_block_mass()). A Hinge
 * adds the axis-alignment constraint (2 scalar, angular-only, also target-0 equality --
 * `util::orthonormal_basis()` builds the perpendicular pair `p`/`q` the same way the contact
 * solver's friction tangent basis does), and, when `use_limits`, a ONE-SIDED angle-limit
 * constraint using the exact same clamped-impulse pattern as a contact's normal constraint
 * (`new_impulse = max(old+lambda, 0)`), just for an angular DOF instead of linear. A ConeTwist
 * adds two more rows of that same one-sided kind: the swing cone (about `cross(ta, tb)`, active
 * once the twist axes diverge past `swing_limit`) and the twist range (about the twist axis).
 *
 * The point constraint computes its OWN per-body moment arms (`ra`/`rb`, the two anchors --
 * generally different points until the constraint has converged) rather than calling
 * apply_impulse_pair() with a single shared point: that fits a contact (one shared contact
 * point) but not a not-yet-converged joint anchor pair, so this applies impulses directly here
 * using each body's own `ra`/`rb`. `point_impulse` accumulates the block's world-space impulse.
 *
 * The limit rows are speculative (see solve_angular_limit_row()): each always runs against the
 * nearer end of its range, and only produces impulse once the joint would reach that end within
 * the substep. Their impulses are deliberately NOT warm-started (see warm_start_joints()) --
 * limit contact is edge-triggered (a door swinging shut hits its limit occasionally, not every
 * step), so there's no steady-state impulse worth carrying across steps the way the point/axis
 * constraints have; each step's accumulator starts at 0 (see warm_start_joints()).
 *
 * Never applies positional bias -- same split-impulse reasoning as solve_velocity_pass() for
 * contacts; drift closure is solve_joint_position()'s job, run once after every velocity
 * iteration finishes (mirrors solve()'s own contact ordering).
 */
inline void solve_joint_velocity_pass(std::vector<Body>& bodies, std::vector<Joint>& joints, float h,
                                       bool reverse = false) {
    const float inv_h = h > 0.0f ? 1.0f / h : 0.0f;
    const std::size_t count = joints.size();
    for (std::size_t n = 0; n < count; ++n) {
        Joint& j = joints[reverse ? count - 1 - n : n];
        if (!j.valid || !j.enabled) continue;
        Body& a = bodies[j.a.index];
        Body& b = bodies[j.b.index];
        bool a_dyn = a.type == BodyType::Dynamic && a.awake;
        bool b_dyn = b.type == BodyType::Dynamic && b.awake;
        if (!a_dyn && !b_dyn) continue;

        float inv_mass_a = a_dyn ? a.inv_mass : 0.0f;
        float inv_mass_b = b_dyn ? b.inv_mass : 0.0f;
        glm::mat3 inv_ia = a_dyn ? a.inv_inertia_world : glm::mat3(0.0f);
        glm::mat3 inv_ib = b_dyn ? b.inv_inertia_world : glm::mat3(0.0f);

        // --- Angular limits first, point last: the point rows are equalities that must hold,
        // the limits are inequalities a little leftover error in is invisible -- solving the
        // equality last in each pass leaves the residual on the forgiving rows. ---
        if (j.type == JointType::Hinge) {
            // --- Axis-alignment constraint ---
            glm::vec3 world_axis_a = glm::normalize(a.orientation * j.local_axis_a);
            glm::vec3 p, q;
            util::orthonormal_basis(world_axis_a, p, q);
            const glm::vec3 kPerp[2] = {p, q};
            for (int k = 0; k < 2; ++k) {
                const glm::vec3& n = kPerp[k];
                float k_eff = glm::dot(n, inv_ia * n) + glm::dot(n, inv_ib * n);
                if (k_eff <= util::k_epsilon) continue;

                float cdot = glm::dot(b.angular_velocity - a.angular_velocity, n);
                float lambda = -cdot / k_eff;
                j.axis_impulse[k] += lambda;
                apply_angular_impulse_pair(a, b, n * lambda);
            }

            // --- Angle limit (optional, one-sided) ---
            if (j.use_limits) {
                float angle = hinge_current_angle(a.orientation, b.orientation, j.local_axis_a, j.rest_relative_rotation);
                // +axis: the angle INCREASES (away from the lower limit); -axis: it decreases.
                float sign, c;
                nearest_limit_side(angle, j.min_angle, j.max_angle, sign, c);
                solve_angular_limit_row(a, b, inv_ia, inv_ib, world_axis_a * sign, c, inv_h, j.limit_impulse);
            }
        } else if (j.type == JointType::ConeTwist) {
            glm::vec3 ta, tb;
            cone_twist_axes(a, b, j, ta, tb);
            if (j.has_swing_limit()) {
                glm::vec3 swing_axis;
                float swing = cone_swing(ta, tb, swing_axis);
                solve_angular_limit_row(a, b, inv_ia, inv_ib, -swing_axis, j.swing_limit - swing, inv_h, j.swing_impulse);
            }
            if (j.has_twist_limit()) {
                float angle = cone_twist_angle(a, b, j);
                glm::vec3 twist_axis = ta + tb;
                float len = glm::length(twist_axis);
                twist_axis = len > 1e-6f ? twist_axis / len : ta;
                float sign, c;
                nearest_limit_side(angle, j.twist_min, j.twist_max, sign, c);
                solve_angular_limit_row(a, b, inv_ia, inv_ib, twist_axis * sign, c, inv_h, j.limit_impulse);
            }
        }

        // --- Point constraint (every type), as one 3x3 block ---
        glm::vec3 ra = a.orientation * j.local_anchor_a;
        glm::vec3 rb = b.orientation * j.local_anchor_b;
        glm::mat3 k_block;
        if (point_block_mass(ra, rb, inv_mass_a, inv_mass_b, inv_ia, inv_ib, k_block)) {
            glm::vec3 va = a.linear_velocity + glm::cross(a.angular_velocity, ra);
            glm::vec3 vb = b.linear_velocity + glm::cross(b.angular_velocity, rb);
            glm::vec3 impulse = glm::inverse(k_block) * (va - vb);
            j.point_impulse += impulse;
            if (a_dyn) {
                a.linear_velocity -= impulse * inv_mass_a;
                a.angular_velocity -= inv_ia * glm::cross(ra, impulse);
            }
            if (b_dyn) {
                b.linear_velocity += impulse * inv_mass_b;
                b.angular_velocity += inv_ib * glm::cross(rb, impulse);
            }
        }
    }
}

/**
 * @brief NGS position correction for joints: closes point-constraint drift (linear, same
 *        cumulative-error-vs-slop pattern as solve_position() for contacts), a hinge's axis-
 *        alignment drift, and any accumulated limit violation (hinge angle, cone swing, twist).
 *        Angular corrections are needed here, unlike solve_position()'s contacts (which
 *        deliberately skip rotational correction): a misalignment or limit overshoot has no
 *        OTHER mechanism to pull it back, since the velocity pass only stops the drift RATE from
 *        growing, never corrects an already-accumulated error.
 *
 * Reuses config.linear_slop/position_correction/max_linear_correction for the angular case too
 * (radians instead of meters) rather than adding new angular-specific tunables -- both are
 * already small, sane tolerances (0.005 rad =~ 0.29 degrees) and this keeps the joint API
 * surface minimal.
 *
 * The point correction uses the same 3x3 block effective mass as the velocity pass and moves
 * AND rotates both bodies -- for a lever-arm anchor (every ragdoll bone: the anchor sits at one
 * end) translation alone would leave a chain of bones drifting apart under gravity.
 */
inline void solve_joint_position(std::vector<Body>& bodies, std::vector<Joint>& joints,
                                  const util::PhysicsConfig& config) {
    for (auto& j : joints) {
        if (!j.valid || !j.enabled) continue;
        Body& a = bodies[j.a.index];
        Body& b = bodies[j.b.index];
        bool a_dyn = a.type == BodyType::Dynamic && a.awake;
        bool b_dyn = b.type == BodyType::Dynamic && b.awake;
        if (!a_dyn && !b_dyn) continue;

        float inv_mass_a = a_dyn ? a.inv_mass : 0.0f;
        float inv_mass_b = b_dyn ? b.inv_mass : 0.0f;
        glm::mat3 inv_ia = a_dyn ? a.inv_inertia_world : glm::mat3(0.0f);
        glm::mat3 inv_ib = b_dyn ? b.inv_inertia_world : glm::mat3(0.0f);

        if (j.type == JointType::Hinge) {
            // --- Axis-alignment drift ---
            glm::vec3 world_axis_a = glm::normalize(a.orientation * j.local_axis_a);
            glm::vec3 world_axis_b = glm::normalize(b.orientation * j.local_axis_b);
            glm::vec3 axis_error = glm::cross(world_axis_a, world_axis_b);
            float axis_error_len = glm::length(axis_error);
            if (axis_error_len > config.linear_slop) {
                glm::vec3 n = axis_error / axis_error_len;
                float correction_mag = std::min((axis_error_len - config.linear_slop) * config.position_correction,
                                                 config.max_linear_correction);
                // b's axis is rotated away from a's about n: undo it (relative rotation -mag).
                apply_angular_position_correction(a, b, a_dyn, b_dyn, inv_ia, inv_ib, n, -correction_mag);
            }

            // --- Angle-limit overshoot ---
            if (j.use_limits) {
                float angle = hinge_current_angle(a.orientation, b.orientation, j.local_axis_a, j.rest_relative_rotation);
                float over = angle < j.min_angle ? angle - j.min_angle : (angle > j.max_angle ? angle - j.max_angle : 0.0f);
                if (std::abs(over) > config.linear_slop) {
                    float mag = std::min((std::abs(over) - config.linear_slop) * config.position_correction,
                                         config.max_linear_correction);
                    glm::vec3 axis = glm::normalize(a.orientation * j.local_axis_a);
                    apply_angular_position_correction(a, b, a_dyn, b_dyn, inv_ia, inv_ib, axis, over > 0.0f ? -mag : mag);
                }
            }
        } else if (j.type == JointType::ConeTwist) {
            glm::vec3 ta, tb;
            cone_twist_axes(a, b, j, ta, tb);
            if (j.has_swing_limit()) {
                glm::vec3 swing_axis;
                float over = cone_swing(ta, tb, swing_axis) - j.swing_limit;
                if (over > config.linear_slop) {
                    float mag = std::min((over - config.linear_slop) * config.position_correction,
                                         config.max_linear_correction);
                    apply_angular_position_correction(a, b, a_dyn, b_dyn, inv_ia, inv_ib, swing_axis, -mag);
                    cone_twist_axes(a, b, j, ta, tb);
                }
            }
            if (j.has_twist_limit()) {
                float angle = cone_twist_angle(a, b, j);
                float over = angle < j.twist_min ? angle - j.twist_min : (angle > j.twist_max ? angle - j.twist_max : 0.0f);
                if (std::abs(over) > config.linear_slop) {
                    float mag = std::min((std::abs(over) - config.linear_slop) * config.position_correction,
                                         config.max_linear_correction);
                    glm::vec3 twist_axis = ta + tb;
                    float len = glm::length(twist_axis);
                    twist_axis = len > 1e-6f ? twist_axis / len : ta;
                    apply_angular_position_correction(a, b, a_dyn, b_dyn, inv_ia, inv_ib, twist_axis, over > 0.0f ? -mag : mag);
                }
            }
        }

        // --- Point constraint drift (every type), last -- see the velocity pass's ordering note ---
        glm::vec3 ra = a.orientation * j.local_anchor_a;
        glm::vec3 rb = b.orientation * j.local_anchor_b;
        glm::vec3 error = (b.position + rb) - (a.position + ra);
        float error_len = glm::length(error);
        glm::mat3 k_block;
        if (error_len > config.linear_slop && point_block_mass(ra, rb, inv_mass_a, inv_mass_b, inv_ia, inv_ib, k_block)) {
            float correction_mag = std::min((error_len - config.linear_slop) * config.position_correction,
                                             config.max_linear_correction);
            // The positional "impulse" P that closes `correction_mag` of the gap through the
            // same block effective mass as the velocity pass (K * P = closure), applied to both
            // bodies' positions AND orientations -- for a lever-arm anchor (every ragdoll bone)
            // translation alone cannot close the gap without fighting the body's other joints.
            glm::vec3 p = glm::inverse(k_block) * ((error / error_len) * correction_mag);
            if (a_dyn) {
                a.position += p * inv_mass_a;
                glm::vec3 dr = inv_ia * glm::cross(ra, p);
                glm::quat dq(0.0f, dr.x, dr.y, dr.z);
                a.orientation = glm::normalize(a.orientation + 0.5f * dq * a.orientation);
            }
            if (b_dyn) {
                b.position -= p * inv_mass_b;
                glm::vec3 dr = -(inv_ib * glm::cross(rb, p));
                glm::quat dq(0.0f, dr.x, dr.y, dr.z);
                b.orientation = glm::normalize(b.orientation + 0.5f * dq * b.orientation);
            }
        }
    }
}

/**
 * @brief The joints' position pass, run on the INTEGRATED poses at the end of a substep (see
 *        solve()). Same iteration count as the contacts' position pass: a chain of joints (a
 *        ragdoll) closes one link's drift partly by opening its neighbour's, so a single pass
 *        converges slowly.
 */
inline void solve_joints_post_integrate(std::vector<Body>& bodies, std::vector<Joint>& joints,
                                        const util::PhysicsConfig& config) {
    if (joints.empty()) return;
    for (uint32_t iter = 0; iter < std::max(1u, config.position_iterations); ++iter) {
        solve_joint_position(bodies, joints, config);
    }
}

/**
 * @brief Rebuilds the warm-start cache from this step's fully-solved manifolds, so next
 *        step's warm_start() has something to seed from.
 */
inline void rebuild_warm_start_cache(std::vector<collision::ContactManifold>& manifolds, SolverState& state) {
    state.impulses.clear();
    for (auto& m : manifolds) {
        if (!m.valid || m.is_trigger) continue;
        for (uint8_t i = 0; i < m.count; ++i) {
            const auto& p = m.points[i];
            uint64_t key = pack_warm_start_key(m.a.index, m.b.index, p.feature_id);
            SolverState::CachedImpulse cached;
            cached.normal = p.normal_impulse;
            cached.tangent[0] = p.tangent_impulse[0];
            cached.tangent[1] = p.tangent_impulse[1];
            state.impulses[key] = cached;
        }
    }
}

/** @brief Wakes `dyn` if `kin` is a moving Kinematic body -- shared by update_islands_and_sleep()'s
 *         manifold-based and joint-based wake-rule passes (see that function's own doc for the
 *         "linear OR angular" reasoning). A no-op unless `kin` is Kinematic and `dyn` is a
 *         sleeping Dynamic body. */
inline void wake_if_kinematic_moving(Body& kin, Body& dyn, const util::PhysicsConfig& config) {
    if (kin.type != BodyType::Kinematic || dyn.type != BodyType::Dynamic || dyn.awake) return;
    bool linear_moving = glm::dot(kin.linear_velocity, kin.linear_velocity) > config.sleep_linear * config.sleep_linear;
    bool angular_moving = glm::dot(kin.angular_velocity, kin.angular_velocity) > config.sleep_angular * config.sleep_angular;
    if (linear_moving || angular_moving) {
        dyn.awake = true;
        dyn.sleep_timer = 0.0f;
    }
}

/**
 * @brief Builds this step's islands, updates per-body sleep timers, applies the kinematic
 *        wake rule, and puts fully-settled islands to sleep.
 *
 * Wake rule (beyond "new contact / applied force / teleport"): a sleeping dynamic body
 * touching a kinematic body whose derived velocity is non-zero wakes immediately, regardless
 * of contact freshness -- otherwise a box asleep on a stationary platform that starts moving
 * would hover in place while the platform slides out from under it. Joints get the identical
 * treatment (a door hinged to a moving kinematic frame must wake too) via the same
 * wake_if_kinematic_moving() helper, not a separate rule.
 */
inline void update_islands_and_sleep(std::vector<Body>& bodies, const std::vector<bool>& alive,
                                      std::vector<collision::ContactManifold>& manifolds,
                                      std::vector<Joint>& joints,
                                      const util::PhysicsConfig& config, float h) {
    IslandUnionFind uf(bodies.size());

    for (auto& m : manifolds) {
        if (!m.valid) continue;
        bool a_dyn = bodies[m.a.index].type == BodyType::Dynamic;
        bool b_dyn = bodies[m.b.index].type == BodyType::Dynamic;
        if (a_dyn && b_dyn) uf.unite(m.a.index, m.b.index);
    }
    for (auto& j : joints) {
        if (!j.valid || !j.enabled) continue;
        bool a_dyn = bodies[j.a.index].type == BodyType::Dynamic;
        bool b_dyn = bodies[j.b.index].type == BodyType::Dynamic;
        if (a_dyn && b_dyn) uf.unite(j.a.index, j.b.index);
    }

    for (uint32_t i = 0; i < bodies.size(); ++i) {
        if (!alive[i]) continue;
        Body& b = bodies[i];
        if (b.type != BodyType::Dynamic || !b.awake) continue;
        float lin2 = glm::dot(b.linear_velocity, b.linear_velocity);
        float ang2 = glm::dot(b.angular_velocity, b.angular_velocity);
        if (lin2 < config.sleep_linear * config.sleep_linear && ang2 < config.sleep_angular * config.sleep_angular) {
            b.sleep_timer += h;
        } else {
            b.sleep_timer = 0.0f;
        }
    }

    // Kinematic wake rule -- see wake_if_kinematic_moving()'s doc for the "linear OR angular"
    // reasoning (a spinner rotating in place has ~zero linear_velocity but is very much moving).
    for (auto& m : manifolds) {
        if (!m.valid) continue;
        wake_if_kinematic_moving(bodies[m.a.index], bodies[m.b.index], config);
        wake_if_kinematic_moving(bodies[m.b.index], bodies[m.a.index], config);
    }
    for (auto& j : joints) {
        if (!j.valid || !j.enabled) continue;
        wake_if_kinematic_moving(bodies[j.a.index], bodies[j.b.index], config);
        wake_if_kinematic_moving(bodies[j.b.index], bodies[j.a.index], config);
    }

    // Island-level sleep decision: an island sleeps only once EVERY member's own timer has
    // reached sleep_time (the least-still body in the island gates the whole group) -- lets a
    // multi-body stack sleep as a unit instead of the bottom sleeping while the top settles.
    std::unordered_map<uint32_t, float> island_min_timer;
    for (uint32_t i = 0; i < bodies.size(); ++i) {
        if (!alive[i]) continue;
        Body& b = bodies[i];
        if (b.type != BodyType::Dynamic || !b.awake) continue;
        uint32_t root = uf.find(i);
        auto it = island_min_timer.find(root);
        if (it == island_min_timer.end() || b.sleep_timer < it->second) island_min_timer[root] = b.sleep_timer;
    }
    for (uint32_t i = 0; i < bodies.size(); ++i) {
        if (!alive[i]) continue;
        Body& b = bodies[i];
        if (b.type != BodyType::Dynamic || !b.awake) continue;
        uint32_t root = uf.find(i);
        if (island_min_timer[root] >= config.sleep_time) {
            b.awake = false;
            b.linear_velocity = glm::vec3(0.0f);
            b.angular_velocity = glm::vec3(0.0f);
        }
    }
}

/**
 * @brief Wakes every sleeping dynamic body jointed (directly or through a chain) to an awake one.
 *
 * The joint passes treat a sleeping body as immovable, so a body woken alone (a kinematic ram
 * nudging one bone of a sleeping ragdoll) would be pinned between the ram and "static"
 * neighbours -- the joint and the contact then fight with unbounded impulses and the ragdoll
 * explodes. An island wakes as a whole instead, like it sleeps as a whole. Repeats until
 * nothing changes (one sweep per chain link at worst; a no-op sweep when nothing woke).
 */
inline void wake_jointed_islands(std::vector<Body>& bodies, std::vector<Joint>& joints) {
    bool changed = true;
    for (std::size_t pass = 0; changed && pass <= joints.size(); ++pass) {
        changed = false;
        for (auto& j : joints) {
            if (!j.valid || !j.enabled) continue;
            Body& a = bodies[j.a.index];
            Body& b = bodies[j.b.index];
            if (a.type != BodyType::Dynamic || b.type != BodyType::Dynamic || a.awake == b.awake) continue;
            Body& sleeper = a.awake ? b : a;
            sleeper.wake();
            changed = true;
        }
    }
}

/**
 * @brief Runs the full solve: warm start, restitution capture, velocity iterations, relax
 *        iterations, position correction, cache rebuild, then islands/sleep.
 *
 * @param bodies    Body array (mutated in place).
 * @param alive     Parallel liveness flags.
 * @param manifolds This step's narrowphase output (mutated: impulse accumulators, positions).
 * @param state     Cross-step warm-start cache.
 * @param config    Solver tunables.
 * @param h         Substep length in seconds.
 */
inline void solve(std::vector<Body>& bodies, const std::vector<bool>& alive,
                   std::vector<collision::ContactManifold>& manifolds, std::vector<Joint>& joints,
                   SolverState& state, const util::PhysicsConfig& config, float h) {
    wake_jointed_islands(bodies, joints);
    capture_restitution_bias(bodies, manifolds, config);
    warm_start(bodies, manifolds, state);
    warm_start_joints(bodies, joints);

    // Main pass targets 0 (non-penetration + friction), converging the base resting/sliding
    // solution across every contact. Restitution is applied AFTERWARD, as a final correction
    // on top of that converged base -- if it ran first (or were re-targeted to 0 afterward),
    // a later zero-target pass would cancel the very bounce it just added, since nothing
    // distinguishes "this point is genuinely separating on purpose" from "this point should
    // be resting" once both have been driven toward the same target. Matches how Box2D
    // applies restitution: a single dedicated correction after the main solve, never undone.
    //
    // Joints solve every velocity iteration too (both the base and relax passes -- a joint has
    // no restitution concept, so use_restitution doesn't affect it, but running it every
    // iteration lets it converge jointly with whatever contacts are pushing on the same bodies,
    // e.g. a stack of boxes leaning against a door).
    //
    // The joint pass alternates direction each iteration (symmetric Gauss-Seidel): in a chain or
    // tree of joints (a ragdoll) a forward-only sweep carries a disturbance at one end to the
    // other within one iteration but needs many to carry it back, so a hard hit on a ragdoll's
    // chest otherwise leaves the joints visibly stretched for several frames.
    for (uint32_t i = 0; i < config.velocity_iterations; ++i) {
        solve_velocity_pass(bodies, manifolds, /*use_restitution=*/false, /*order_offset=*/i, h);
        solve_joint_velocity_pass(bodies, joints, h, /*reverse=*/(i & 1u) != 0);
    }
    for (uint32_t i = 0; i < config.relax_iterations; ++i) {
        solve_velocity_pass(bodies, manifolds, /*use_restitution=*/true, /*order_offset=*/i, h);
        solve_joint_velocity_pass(bodies, joints, h, /*reverse=*/(i & 1u) != 0);
    }

    solve_position(bodies, manifolds, config);
    // Joint drift is closed mainly AFTER integration (solve_joints_post_integrate(), called by
    // PhysicsWorld::step_fixed()), Box2D-style: most of a fast-spinning ragdoll's joint drift is
    // created BY the integration (anchors move on arcs, velocities only match them to first
    // order). One pass here as well re-closes what the contact position pass just opened (a
    // foot pushed out of the ground drags its ankle off the shin) -- this pose becomes the
    // substep's prev_position, which render interpolation draws.
    solve_joint_position(bodies, joints, config);
    rebuild_warm_start_cache(manifolds, state);
    update_islands_and_sleep(bodies, alive, manifolds, joints, config, h);
}

} // namespace dynamics
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_DYNAMICS_SOLVER_H

/**
 * @file manifold.h
 * @brief Contact point/manifold types produced by narrowphase and consumed by the solver.
 */

#ifndef PHYSXCOOPA_COLLISION_MANIFOLD_H
#define PHYSXCOOPA_COLLISION_MANIFOLD_H

#include <physxcoopa/dynamics/body.h>

#include <glm/glm.hpp>

#include <cstdint>

namespace coopa {
namespace physx {
namespace collision {

/**
 * @struct ContactPoint
 * @brief One point of contact within a manifold.
 *
 * `normal_impulse`/`tangent_impulse` persist across steps via the solver's warm-start cache,
 * keyed by (body pair, feature_id) -- see dynamics/solver.h. `restitution_bias` is transient,
 * recomputed fresh every step from the pre-solve approach velocity; it is never warm-started.
 */
struct ContactPoint {
    glm::vec3 position{0.0f};
    float penetration = 0.0f;

    /** @brief Signed gap for a SPECULATIVE (not-yet-touching, CCD) point only -- positive means
     *         still separated by this distance along `ContactManifold::normal`. Zero for every
     *         ordinary point (`add_point()`), where `penetration` is the field that drives the
     *         solver instead; a speculative point (`add_speculative_point()`) leaves
     *         `penetration` at 0 and sets this instead. See solve_velocity_pass()'s doc in
     *         dynamics/solver.h for the distinct velocity-target branch this feeds
     *         (`v_n >= -separation/h`), and sat_test_obb_obb()'s `allow_separated` parameter in
     *         collision/sat.h for how it's produced. */
    float separation = 0.0f;

    float normal_impulse = 0.0f;
    float tangent_impulse[2] = {0.0f, 0.0f};

    /** @brief Stable-across-steps id used to match this point to its predecessor for warm
     *         starting -- see the plan's per-pair feature-id scheme (Phase 4) and the
     *         position-based-matching fallback for pairs without a natural combinatorial id. */
    uint32_t feature_id = 0;

    /** @brief Target approach velocity for restitution (`-e * v_approach`, captured once
     *         before any impulse this step is applied), or 0 for a resting contact. Transient
     *         solver scratch -- never warm-started. */
    float restitution_bias = 0.0f;
};

/**
 * @struct ContactManifold
 * @brief All contact points between one pair of bodies this step.
 *
 * `normal` points from body `a` toward body `b`. `valid` supports the pre-sized-vector layout
 * the plan specifies for a future job-parallel narrowphase (write by index, `valid` flag,
 * compact serially -- never push_back from a worker); v1 uses it just to mark manifolds
 * invalidated mid-step by a destroyed body (see PhysicsWorld::destroy_body).
 */
struct ContactManifold {
    dynamics::BodyId a;
    dynamics::BodyId b;
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    ContactPoint points[4];
    uint8_t count = 0;
    float friction = 0.0f;
    float restitution = 0.0f;
    bool is_trigger = false;
    bool valid = false;

    /** @brief Appends a contact point (up to 4); additional points beyond that are dropped. */
    void add_point(const glm::vec3& position, float penetration, uint32_t feature_id) {
        if (count >= 4) return;
        ContactPoint& p = points[count++];
        p = ContactPoint{};
        p.position = position;
        p.penetration = penetration;
        p.feature_id = feature_id;
    }

    /** @brief Appends a SPECULATIVE point -- not yet overlapping (`penetration` stays 0), but
     *         close enough and closing fast enough this substep that the solver should
     *         constrain it anyway to prevent tunneling before the shapes actually touch. See
     *         ContactPoint::separation's doc. */
    void add_speculative_point(const glm::vec3& position, float separation, uint32_t feature_id) {
        if (count >= 4) return;
        ContactPoint& p = points[count++];
        p = ContactPoint{};
        p.position = position;
        p.separation = separation;
        p.feature_id = feature_id;
    }
};

} // namespace collision
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COLLISION_MANIFOLD_H

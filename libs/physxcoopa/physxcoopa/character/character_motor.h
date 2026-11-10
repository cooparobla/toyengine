/**
 * @file character_motor.h
 * @brief A kinematic capsule character motor: one call moves a capsule by a desired
 *        displacement through a PhysicsWorld, colliding, sliding, stepping and snapping the way
 *        a walking character should. Pure algorithm over PhysicsWorld's queries -- no scene
 *        types, no state between calls (the caller keeps velocity, grounded state and timers).
 *
 * Each move() runs, in order:
 *
 *   1. **Depenetrate.** compute_penetration() against everything the filter accepts; the
 *      deepest overlap is pushed out first, re-querying, up to `max_depenetration_iterations`.
 *      A character only ever starts a move overlapping something because something moved into
 *      it (a body pushed by the solver, a platform that rose) -- the sweeps below assume a
 *      clear start.
 *   2. **Collide and slide, horizontally.** The displacement's horizontal part is swept up to
 *      `max_slide_iterations` times; each hit stops the capsule `skin` short of the surface and
 *      redirects what is left along it. A WALKABLE surface (normal within `slope_limit_deg` of
 *      up) is followed, keeping the horizontal speed, so a ramp is climbed; any other surface
 *      -- a wall, or a slope steeper than the limit -- has its normal flattened to horizontal
 *      first, so it acts as a vertical wall and can never be climbed by sliding up it.
 *   3. **Step up.** If that horizontal slide was blocked while grounded, it is retried from
 *      `step_height` higher (a sweep up, then the same slide, then a sweep down to land). The
 *      stepped result wins only if it got at least as far and landed on walkable ground above
 *      the start, on a ledge no taller than step_height (measured from the ground under the
 *      start pose), so a stair riser at or under step_height is climbed and a taller one blocks.
 *   4. **Collide and slide, vertically** (gravity, or a jump). Landing on walkable ground stops
 *      it; a steep slope redirects it along the slope, so a character on one slides down.
 *   5. **Ground probe and snap.** Unless the move is a jump, a short sweep down finds the ground
 *      under the capsule -- `snap_distance` long when the caller was grounded at the start of
 *      the move (walking down a ramp or off a stair then stays glued to it), just past the skin
 *      otherwise (resting contact).
 *
 * The capsule keeps a `skin` gap to every surface it is swept against. That gap is what lets a
 * grounded capsule slide along its floor (a cast that only grazes a surface it does not
 * approach never hits it -- see query/sweep.h's initial-overlap rules) and keeps float error
 * from leaving it interpenetrating.
 *
 * Walkability of a contact: the capsule's round bottom touching a stair edge reports a sloped
 * normal even though it stands on a flat tread. Ground decisions (landing, probing, the step's
 * landing) therefore fall back to a short raycast down just past the contact point, and accept
 * the contact when the surface there is walkable. The horizontal slide deliberately does NOT
 * use that fallback: riding up a riser's edge would climb risers taller than step_height.
 *
 * Moving platforms: when the ground is a kinematic body, MoveResult::ground_velocity is that
 * body's velocity at the ground point (linear plus angular), so the caller can add it to next
 * frame's displacement and ride along. Kinematic velocity is derived from the body's last
 * transform delta (see PhysicsSystem::sync_transforms_in_()), so it is one physics step old.
 *
 * Conventions: Z-up world, capsule axis Z, `position` is the capsule's CENTRE. Trigger shapes
 * never block (the filter's include_triggers is forced off). Pass a filter whose `ignore` is
 * the character's own body. Only valid between physics steps, like every query.
 */

#ifndef PHYSXCOOPA_CHARACTER_CHARACTER_MOTOR_H
#define PHYSXCOOPA_CHARACTER_CHARACTER_MOTOR_H

#include <physxcoopa/world.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace coopa {
namespace physx {
namespace character {

/**
 * @struct MotorSettings
 * @brief The capsule and the walking rules. All distances in metres.
 */
struct MotorSettings {
    float radius = 0.3f;            ///< Capsule radius.
    float height = 1.8f;            ///< Total capsule height, end caps included (CapsuleCollider's convention).
    float step_height = 0.35f;      ///< Tallest ledge walked up without jumping.
    float slope_limit_deg = 45.0f;  ///< Steeper surfaces are walls: not stood on, not climbed.
    float skin = 0.02f;             ///< Gap kept to every surface.
    float snap_distance = 0.3f;     ///< How far down a grounded character is pulled to stay on the ground.
    int max_slide_iterations = 4;
    int max_depenetration_iterations = 4;
};

/** @brief World up. The motor is written for the Z-up world (its capsule axis is Z). */
inline const glm::vec3 k_up{0.0f, 0.0f, 1.0f};

/**
 * @struct MotorHit
 * @brief One surface the capsule was swept into during a move.
 */
struct MotorHit {
    glm::vec3 point{0.0f};               ///< Contact point on the surface.
    glm::vec3 normal{0.0f, 0.0f, 1.0f};  ///< Surface normal, toward the capsule.
    glm::vec3 direction{0.0f};           ///< Unit direction the capsule was moving in.
    float remaining = 0.0f;              ///< Displacement still to go when it hit (m).
    bool walkable = false;               ///< Treated as ground (a walkable slope, or a landing on a
                                         ///< ledge edge); false for walls, steep slopes, ceilings.
    dynamics::BodyId body;
    uint32_t shape_index = 0xFFFFFFFFu;
};

/**
 * @struct MoveOptions
 * @brief Per-call state the caller carries between moves.
 */
struct MoveOptions {
    bool was_grounded = false;  ///< Grounded at the end of the previous move: enables step-up and ground snap.
    bool allow_step = true;
    bool allow_snap = true;
    /** @brief The move is a jump (or other deliberate take-off): no ground probe or snap, so it
     *         leaves the ground. Without it, an upward move still probes when `was_grounded`
     *         (a character carried up by a lift stays grounded on it). */
    bool jumping = false;
    /**
     * @brief Which way is up for this move (unit length): what "horizontal", "ground", "step up"
     *        and the capsule's axis are measured against. +Z, the world's up, by default; a
     *        character on a planet passes the planet's up where it stands
     *        (`PhysicsWorld::up_at()`). At exactly +Z the motor runs its original arithmetic.
     */
    glm::vec3 up = k_up;
};

/**
 * @struct MoveResult
 * @brief Where the capsule ended up and what it is standing on.
 */
struct MoveResult {
    glm::vec3 position{0.0f};                ///< Final capsule centre.
    bool grounded = false;                   ///< Standing on walkable ground.
    glm::vec3 ground_normal{0.0f, 0.0f, 1.0f};
    glm::vec3 ground_point{0.0f};
    dynamics::BodyId ground_body;            ///< Invalid when not grounded.
    uint32_t ground_shape = 0xFFFFFFFFu;
    glm::vec3 ground_velocity{0.0f};         ///< A kinematic ground body's velocity at ground_point; else zero.
    bool hit_ceiling = false;                ///< An upward move was stopped by something above.
    bool stepped = false;                    ///< The horizontal move went up a step.
    bool snapped = false;                    ///< Ground snap pulled the capsule down.
    glm::vec3 depenetration{0.0f};           ///< How far step 1 pushed the capsule out of overlaps.
    std::vector<MotorHit> collisions;        ///< Every surface swept into, in order.
};

/** @brief The motor's capsule as a collision shape (centred, axis Z). */
inline collision::Shape motor_capsule(const MotorSettings& s) {
    const float r = std::max(1e-3f, s.radius);
    const float half = std::max(0.0f, 0.5f * s.height - r);
    return collision::Shape::make_capsule(r, half, 2);
}

/** @brief True if a surface with this normal can be stood on under `s.slope_limit_deg`. */
inline bool is_walkable(const glm::vec3& normal, const MotorSettings& s) {
    return glm::dot(normal, k_up) >= std::cos(glm::radians(std::clamp(s.slope_limit_deg, 0.0f, 89.9f))) - 1e-4f;
}

/** @brief `is_walkable()` against an `up` of the caller's (a planet's, say). */
inline bool is_walkable(const glm::vec3& normal, const MotorSettings& s, const glm::vec3& up) {
    return glm::dot(normal, up) >= std::cos(glm::radians(std::clamp(s.slope_limit_deg, 0.0f, 89.9f))) - 1e-4f;
}

/** @brief The rotation that stands the motor's Z-axis capsule along `up` (identity for +Z). */
inline glm::quat motor_orientation(const glm::vec3& up) {
    if (up == k_up) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::quat(k_up, glm::normalize(up));   // the shortest arc from +Z to up
}

namespace detail {

struct Ctx {
    const PhysicsWorld& world;
    const collision::Shape shape;
    const query::QueryFilter& filter;
    const MotorSettings& s;
    glm::vec3 up = k_up;                                   ///< The move's up (MoveOptions::up).
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};         ///< The capsule stood along `up`.
};

inline bool cast(const Ctx& c, const glm::vec3& pos, const glm::vec3& dir, float dist, query::RaycastHit& hit) {
    return c.world.shape_cast(c.shape, pos, c.orientation, dir, dist, hit, c.filter);
}

inline glm::vec3 horizontal(const glm::vec3& v) { return glm::vec3(v.x, v.y, 0.0f); }

/** @brief `v` less its part along `up` -- exactly `horizontal(v)` for +Z. */
inline glm::vec3 horizontal(const glm::vec3& v, const glm::vec3& up) {
    if (up == k_up) return horizontal(v);
    return v - up * glm::dot(v, up);
}

/**
 * @brief Whether `hit` (a contact under/beside the capsule centred at `center`) is ground the
 *        character can stand on, and the walkable normal there. Falls back to a raycast just
 *        past the contact for a round-bottom-on-an-edge contact -- see the file doc.
 */
inline bool ground_at(const Ctx& c, const query::RaycastHit& hit, const glm::vec3& center, glm::vec3& normal,
                      float* surface_height = nullptr) {
    const glm::vec3& up = c.up;
    if (is_walkable(hit.normal, c.s, up)) {
        normal = hit.normal;
        if (surface_height) *surface_height = glm::dot(hit.point, up);
        return true;
    }
    if (glm::dot(hit.normal, up) <= 0.02f) return false; // a wall or a ceiling, not an edge
    glm::vec3 out = horizontal(hit.point - center, up);
    const float len = glm::length(out);
    out = len > 1e-5f ? out / len : glm::vec3(0.0f);
    geometry::Ray ray;
    ray.origin = hit.point + out * 0.02f + up * 0.05f;
    ray.direction = -up;
    ray.max_distance = 0.1f;
    query::RaycastHit rh;
    if (!c.world.raycast(ray, rh, c.filter)) return false;
    if (!is_walkable(rh.normal, c.s, up)) return false;
    normal = rh.normal;
    if (surface_height) *surface_height = glm::dot(rh.point, up);
    return true;
}

/** @brief Removes from `v` its component into the plane `n` (only if it points into it). */
inline glm::vec3 clip_into(const glm::vec3& v, const glm::vec3& n) {
    const float d = glm::dot(v, n);
    return d < 0.0f ? v - n * d : v;
}

struct SlideOut {
    glm::vec3 pos{0.0f};
    bool hit_ceiling = false;
    bool landed = false;           ///< Vertical slide only: stopped on walkable ground.
    bool blocked = false;          ///< Horizontal slide only: hit a wall/steep surface.
    std::vector<MotorHit> hits;
};

/**
 * @brief Collide-and-slide of `delta` from `pos`. `horizontal_phase` applies the slope rules of
 *        step 2 (walkable surfaces followed, others flattened to walls); otherwise step 4's
 *        (walkable ground stops a downward move, anything else redirects it).
 */
inline SlideOut slide(const Ctx& c, glm::vec3 pos, glm::vec3 delta, bool horizontal_phase) {
    SlideOut out;
    const float total = glm::length(delta);
    if (total < 1e-7f) { out.pos = pos; return out; }
    const glm::vec3 intent = delta / total;
    glm::vec3 first_plane(0.0f);
    bool have_plane = false;

    for (int i = 0; i < std::max(1, c.s.max_slide_iterations); ++i) {
        const float len = glm::length(delta);
        if (len < 1e-6f) break;
        const glm::vec3 dir = delta / len;
        query::RaycastHit hit;
        if (!cast(c, pos, dir, len + c.s.skin, hit)) {
            pos += delta;
            delta = glm::vec3(0.0f);
            break;
        }
        // Back off so the gap along the surface NORMAL is `skin` (backing off `skin` along the
        // travel direction would leave a grazing approach almost touching).
        const float approach = std::max(0.05f, -glm::dot(dir, hit.normal));
        const float travel = std::clamp(hit.distance - c.s.skin / approach, 0.0f, len);
        pos += dir * travel;
        glm::vec3 remaining = dir * (len - travel);

        MotorHit mh;
        mh.point = hit.point;
        mh.normal = hit.normal;
        mh.direction = dir;
        mh.remaining = len - travel;
        mh.body = hit.body;
        mh.shape_index = hit.shape_index;
        mh.walkable = is_walkable(hit.normal, c.s, c.up);
        out.hits.push_back(mh);

        glm::vec3 n = hit.normal;
        const float n_up = glm::dot(n, c.up);
        if (n_up < -0.3f && glm::dot(dir, c.up) > 0.0f) out.hit_ceiling = true;

        if (horizontal_phase) {
            if (is_walkable(n, c.s, c.up)) {
                // A ramp: follow it, keeping the horizontal speed (walking up a ramp isn't slower).
                const float h_len = glm::length(horizontal(remaining, c.up));
                glm::vec3 p = remaining - n * glm::dot(remaining, n);
                const float p_len = glm::length(horizontal(p, c.up));
                remaining = p_len > 1e-6f ? p * (h_len / p_len) : glm::vec3(0.0f);
            } else {
                out.blocked = true;
                const glm::vec3 nh = horizontal(n, c.up);
                if (glm::length(nh) > 1e-4f) n = glm::normalize(nh); // steep slope -> vertical wall
                remaining = clip_into(remaining, n);
                // Never let a wall or ceiling lift the character.
                if (c.up == k_up) {
                    if (remaining.z > 0.0f) remaining.z = 0.0f;
                } else {
                    const float lift = glm::dot(remaining, c.up);
                    if (lift > 0.0f) remaining -= c.up * lift;
                }
            }
        } else {
            glm::vec3 ground_n;
            if (glm::dot(dir, c.up) < 0.0f && ground_at(c, hit, pos, ground_n)) {
                out.hits.back().walkable = true;
                out.landed = true;
                break;
            }
            if (out.hit_ceiling) break;
            remaining = remaining - n * glm::dot(remaining, n);
        }

        // Two planes: move along their crease, never back into the first.
        if (have_plane && glm::dot(remaining, first_plane) < 0.0f) {
            glm::vec3 crease = glm::cross(first_plane, n);
            const float cl = glm::length(crease);
            remaining = cl > 1e-4f ? (crease / cl) * glm::dot(remaining, crease / cl) : glm::vec3(0.0f);
        }
        first_plane = n;
        have_plane = true;
        // Never turn back against the requested direction (it only jitters in a corner).
        if (glm::dot(remaining, intent) <= 1e-6f) break;
        delta = remaining;
    }
    out.pos = pos;
    return out;
}

} // namespace detail

/**
 * @brief Moves the capsule centred at `position` by `displacement` -- see the file doc for the
 *        five steps.
 *
 * @param world        The world to collide against (only queried, never changed).
 * @param position     Capsule centre at the start of the move.
 * @param displacement Desired movement this frame (velocity * dt).
 * @param filter       Which shapes block; set `ignore` to the character's own body. Triggers
 *                     never block whatever `include_triggers` says.
 * @param settings     The capsule and walking rules.
 * @param options      Grounded state carried from the previous move.
 */
inline MoveResult move(const PhysicsWorld& world, const glm::vec3& position, const glm::vec3& displacement,
                       const query::QueryFilter& filter, const MotorSettings& settings,
                       const MoveOptions& options = {}) {
    query::QueryFilter f = filter;
    f.include_triggers = false;
    const glm::vec3 k_up = options.up;   // this move's up; shadows the world's +Z below
    const detail::Ctx c{world, motor_capsule(settings), f, settings, k_up, motor_orientation(k_up)};
    const glm::quat identity = c.orientation;

    MoveResult r;
    glm::vec3 pos = position;

    // 1. Depenetrate, deepest first.
    for (int i = 0; i < settings.max_depenetration_iterations; ++i) {
        const std::vector<query::Penetration> pens = world.compute_penetration(c.shape, pos, identity, f);
        if (pens.empty()) break;
        const query::Penetration* deepest = &pens.front();
        for (const query::Penetration& p : pens) if (p.depth > deepest->depth) deepest = &p;
        if (deepest->depth < 1e-5f) break;
        const glm::vec3 push = deepest->normal * (deepest->depth + 1e-3f);
        pos += push;
        r.depenetration += push;
    }

    const glm::vec3 horiz = detail::horizontal(displacement, k_up);
    const float vert = glm::dot(displacement, k_up);

    // 2./3. Horizontal slide, with a step-up retry when blocked.
    if (glm::length(horiz) > 1e-7f) {
        detail::SlideOut a = detail::slide(c, pos, horiz, true);
        bool use_a = true;
        if (a.blocked && options.was_grounded && options.allow_step && settings.step_height > 0.0f) {
            query::RaycastHit up_hit;
            float rise = settings.step_height;
            if (detail::cast(c, pos, k_up, settings.step_height + settings.skin, up_hit))
                rise = std::clamp(up_hit.distance - settings.skin, 0.0f, settings.step_height);
            if (rise > 1e-3f) {
                // Step forward at least a little past the skin gap: a capsule stopped against a
                // riser is `skin` short of it, and a slower step than that would never bring its
                // round bottom over the edge to land on (it would stall at the riser forever).
                const float h_len = glm::length(horiz);
                const float min_step = 2.0f * settings.skin + 0.01f;
                const glm::vec3 b_horiz = h_len < min_step ? horiz * (min_step / h_len) : horiz;
                detail::SlideOut b = detail::slide(c, pos + k_up * rise, b_horiz, true);
                const float progress_a = glm::length(detail::horizontal(a.pos - pos, k_up));
                const float progress_b = glm::length(detail::horizontal(b.pos - pos, k_up));
                query::RaycastHit down_hit;
                glm::vec3 land_n;
                float ledge = 0.0f;
                // The ledge stepped onto, measured from the ground under the start pose: what
                // decides "too tall to step", even when the round bottom lands on its edge.
                const float ground_z = glm::dot(pos, k_up) - 0.5f * settings.height - settings.skin;
                // Taken when it got at least as far AND actually landed higher: a riser met at
                // the very end of the move is still stepped onto now, not next frame (a lost
                // frame would cost the caller its speed), while a tall wall -- where the raised
                // slide is blocked just the same and drops back to the floor -- is left to A.
                if (progress_b >= progress_a - 1e-5f &&
                    detail::cast(c, b.pos, -k_up, rise + 2.0f * settings.skin, down_hit) &&
                    detail::ground_at(c, down_hit, b.pos, land_n, &ledge) &&
                    ledge - ground_z > 1e-3f && ledge - ground_z <= settings.step_height + 1e-4f) {
                    b.pos -= k_up * std::max(0.0f, down_hit.distance - settings.skin);
                    pos = b.pos;
                    r.collisions.insert(r.collisions.end(), b.hits.begin(), b.hits.end());
                    r.stepped = true;
                    use_a = false;
                }
            }
        }
        if (use_a) {
            pos = a.pos;
            r.hit_ceiling |= a.hit_ceiling;
            r.collisions.insert(r.collisions.end(), a.hits.begin(), a.hits.end());
        }
    }

    // 4. Vertical slide.
    bool landed = false;
    if (std::abs(vert) > 1e-7f) {
        detail::SlideOut v = detail::slide(c, pos, k_up * vert, false);
        pos = v.pos;
        landed = v.landed;
        r.hit_ceiling |= v.hit_ceiling;
        r.collisions.insert(r.collisions.end(), v.hits.begin(), v.hits.end());
    }

    // 5. Ground probe / snap. Never for a jump, nor for any upward move that didn't start on
    //    the ground (still rising from a jump).
    if (!options.jumping && (vert <= 1e-6f || options.was_grounded)) {
        const bool snap = options.was_grounded && options.allow_snap && !r.stepped;
        const float probe = 2.0f * settings.skin + (snap ? settings.snap_distance : 0.0f) + (landed ? settings.skin : 0.0f);
        query::RaycastHit g;
        glm::vec3 gn;
        if (detail::cast(c, pos, -k_up, probe, g) && detail::ground_at(c, g, pos, gn)) {
            // Settle at exactly the skin gap: down onto the ground, or up by at most the skin
            // when a grazing slide left the capsule closer than that.
            const float drop = std::max(g.distance - settings.skin, -settings.skin);
            if (std::abs(drop) > 1e-5f) {
                pos -= k_up * drop;
                r.snapped = drop > 2.0f * settings.skin;
            }
            r.grounded = true;
            r.ground_normal = gn;
            r.ground_point = g.point;
            r.ground_body = g.body;
            r.ground_shape = g.shape_index;
        }
    }

    if (r.grounded && r.ground_body.is_valid()) {
        if (const dynamics::Body* b = world.get_body(r.ground_body)) {
            if (b->type == dynamics::BodyType::Kinematic)
                r.ground_velocity = b->linear_velocity + glm::cross(b->angular_velocity, r.ground_point - b->position);
        }
    }
    r.position = pos;
    return r;
}

} // namespace character
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_CHARACTER_CHARACTER_MOTOR_H

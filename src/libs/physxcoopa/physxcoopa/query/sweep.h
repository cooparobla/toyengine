/**
 * @file sweep.h
 * @brief Exact shape casts and penetration (minimum-translation) queries between one convex
 *        query shape (Sphere, Capsule or Box) and one placed target shape -- the pairwise math
 *        behind PhysicsWorld::shape_cast()/sphere_cast()/box_cast()/capsule_cast() and
 *        PhysicsWorld::compute_penetration(). Still no GJK/EPA (shape.h's ShapeType doc):
 *
 * - Sphere/Capsule casts use CONSERVATIVE ADVANCEMENT on the exact closest distance between
 *   the cast shape's core (a point or segment) and the target (collision/distance.h, plus the
 *   segment-segment closest points capsules already use). Under pure translation the distance
 *   between two convex sets is a convex function of the travelled distance `t`, and its slope
 *   is -dot(dir, n) for `n` the unit direction between the closest points -- so each step
 *   `t += d / -dot(dir, n)` is a Newton step on a convex function from the left: it can never
 *   pass the true time of impact, and it stops once the gap is under k_sweep_tolerance. A
 *   Box cast against a Sphere or Capsule target runs the same loop with the roles swapped
 *   (the target's core against the moving box).
 * - Box-vs-Box and Box-vs-triangle casts are polytope pairs, swept with the separating-axis
 *   test instead: each candidate axis gives the interval of `t` over which the two
 *   projections overlap, and the shapes overlap exactly when every interval does, so the time
 *   of impact is the latest entry time (provided it precedes the earliest exit).
 * - A TriangleMesh target is swept per triangle, over only the triangles its BVH reports for
 *   the cast's swept volume (geometry::MeshBVH::sweep()).
 *
 * Initial overlap: a target the cast shape already touches or overlaps at its start pose is
 * reported at distance 0 only when the cast direction leads further into it
 * (dot(dir, separation normal) < 0) -- `started_inside` set when it genuinely overlaps by more
 * than k_sweep_tolerance. Moving out of or along such a target ignores it, so a shape resting
 * against a surface can still be swept along or away from it (what a character motor's
 * collide-and-slide relies on). For a mesh this decision is made per triangle.
 *
 * No ConvexHull support, on EITHER side of a sweep -- not attempted, not approximated. Every
 * routine here is built around a cast shape's "core" (a point/segment plus radius, time-marched
 * by conservative advancement) or a box's fixed 3 face axes / 3 edge directions (swept SAT); a
 * hull has neither, and generalizing swept_sat_() to a variable, per-hull axis set the way
 * sat_hull.h generalized the STATIONARY case would be real new work, not a drop-in. Per the
 * project's own precedent for exactly this situation (no speculative/CCD support for Capsule
 * pairs either, "a tractable follow-up"), sweep_shape() below rejects a hull on either side
 * outright rather than quietly computing a wrong time of impact against a zeroed-out core.
 * compute_penetration() (PhysicsWorld, via convex_penetration()/mesh_penetrations() in this
 * file) is unaffected -- stationary overlap already dispatches through generate_contacts()
 * and mesh_triangle_contacts(), both of which DO have real ConvexHull branches.
 */

#ifndef PHYSXCOOPA_QUERY_SWEEP_H
#define PHYSXCOOPA_QUERY_SWEEP_H

#include <physxcoopa/collision/shape.h>
#include <physxcoopa/collision/narrowphase.h>
#include <physxcoopa/collision/mesh_contact.h>
#include <physxcoopa/collision/distance.h>
#include <physxcoopa/geometry/ray.h>
#include <physxcoopa/util/math.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace coopa {
namespace physx {
namespace query {

/** @brief Gap below which a sweep counts as touching -- conservative advancement's stopping
 *         distance, and the depth an initial overlap must exceed to count as `started_inside`. */
inline constexpr float k_sweep_tolerance = 1e-4f;

/**
 * @struct SweepResult
 * @brief One cast-vs-target result: distance travelled to first contact, the contact normal
 *        (unit, from the target's surface toward the cast shape -- it opposes the motion), a
 *        contact point on the target, and whether the cast shape already overlapped the target
 *        at its start pose.
 */
struct SweepResult {
    float distance = 0.0f;
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec3 point{0.0f};
    bool started_inside = false;
};

// --- Internals ---

/** @brief A cast shape's world-space geometry at its start pose: the core segment (a == b for
 *         a sphere) plus radius for Sphere/Capsule, or the OBB for Box. */
struct CastGeometry_ {
    collision::ShapeType type = collision::ShapeType::Sphere;
    glm::vec3 a{0.0f}, b{0.0f};
    float radius = 0.0f;
    geometry::OBB box;
};

inline CastGeometry_ cast_geometry_(const collision::Shape& s, const glm::vec3& pos, const glm::quat& rot) {
    CastGeometry_ g;
    g.type = s.type;
    switch (s.type) {
        case collision::ShapeType::Sphere: {
            geometry::Sphere sph = collision::world_sphere(s, pos, rot);
            g.a = g.b = sph.center;
            g.radius = sph.radius;
            break;
        }
        case collision::ShapeType::Capsule: {
            geometry::Capsule cap = collision::world_capsule(s, pos, rot);
            g.a = cap.a;
            g.b = cap.b;
            g.radius = cap.radius;
            break;
        }
        case collision::ShapeType::Box:
            g.box = collision::world_obb(s, pos, rot);
            break;
        case collision::ShapeType::TriangleMesh:
            break;
    }
    return g;
}

/** @brief Core segment + radius of a Sphere or Capsule TARGET (cast_geometry_ works for either
 *         role -- this just names the intent at call sites). */
inline CastGeometry_ target_core_(const collision::Shape& s, const glm::vec3& pos, const glm::quat& rot) {
    return cast_geometry_(s, pos, rot);
}

/** @brief The 8 corners of an OBB. */
inline void obb_corners_(const geometry::OBB& box, glm::vec3 out[8]) {
    glm::vec3 ax, ay, az;
    box.axes(ax, ay, az);
    int k = 0;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2)
                out[k++] = box.center + (float(sx) * box.half_extents.x) * ax + (float(sy) * box.half_extents.y) * ay +
                           (float(sz) * box.half_extents.z) * az;
}

/** @brief Outcome of advance_(): a hit at `t`, no hit within range, or contact/overlap already
 *         at the start pose (the caller resolves that case with a penetration normal). */
enum class Advance_ { Hit, Miss, AtStart };

/**
 * @brief Conservative-advancement loop (see the file doc). `dist_at(t, n, p)` returns the gap
 *        between cast shape and target with the cast shape moved by `dir * t` (<= 0 when they
 *        overlap), writing the unit normal `n` (target toward cast shape) and a point `p` on
 *        the target. AtStart is returned when the gap at t=0 is already within tolerance.
 */
template <typename DistFn>
inline Advance_ advance_(DistFn&& dist_at, const glm::vec3& dir, float max_distance, float& toi,
                         glm::vec3& n, glm::vec3& p) {
    float t = 0.0f;
    float d = dist_at(t, n, p);
    if (d <= k_sweep_tolerance) return Advance_::AtStart;
    for (int iter = 0; iter < 64; ++iter) {
        float closing = -glm::dot(dir, n);
        if (closing <= 1e-6f) return Advance_::Miss; // convex distance never decreases from here
        t += d / closing;
        if (t > max_distance) return Advance_::Miss;
        d = dist_at(t, n, p);
        if (d <= k_sweep_tolerance) break;
    }
    toi = t; // converged (or, after the iteration cap, a still-conservative early contact)
    return Advance_::Hit;
}

/**
 * @brief Swept separating-axis test for two convex polytopes under translation.
 *        `project_a(L, lo, hi)` / `project_b(L, lo, hi)` give each shape's projection onto axis
 *        `L` at the start pose; the cast shape (A) moves along `dir`. On a hit `toi` is the
 *        latest axis entry time (negative when they already overlap) and `axis` that axis,
 *        oriented from B toward A. When they already overlap, `mtv_axis`/`mtv_depth` give the
 *        minimum-overlap axis (B toward A) for the AtStart case.
 */
template <typename ProjA, typename ProjB>
inline Advance_ swept_sat_(const glm::vec3* axes, int axis_count, ProjA&& project_a, ProjB&& project_b,
                           const glm::vec3& dir, float max_distance, float& toi, glm::vec3& axis,
                           glm::vec3& mtv_axis, float& mtv_depth) {
    float t_first = -std::numeric_limits<float>::max();
    float t_last = std::numeric_limits<float>::max();
    mtv_depth = std::numeric_limits<float>::max();
    for (int k = 0; k < axis_count; ++k) {
        const glm::vec3& L = axes[k];
        float a0, a1, b0, b1;
        project_a(L, a0, a1);
        project_b(L, b0, b1);
        float v = glm::dot(dir, L);

        float overlap = std::min(a1 - b0, b1 - a0);
        if (overlap < mtv_depth) {
            mtv_depth = overlap;
            mtv_axis = (a0 + a1) >= (b0 + b1) ? L : -L;
        }

        if (std::abs(v) < 1e-7f) {
            // No motion along this axis: separated (or merely touching) forever, or never.
            if (overlap <= k_sweep_tolerance) return Advance_::Miss;
            continue;
        }
        float enter = v > 0.0f ? (b0 - a1) / v : (b1 - a0) / v;
        float exit = v > 0.0f ? (b1 - a0) / v : (b0 - a1) / v;
        if (enter > t_first) {
            t_first = enter;
            axis = v > 0.0f ? -L : L;
        }
        t_last = std::min(t_last, exit);
        if (t_first > t_last || t_first > max_distance || t_last < 0.0f) return Advance_::Miss;
    }
    if (t_first <= k_sweep_tolerance) return Advance_::AtStart;
    toi = t_first;
    return Advance_::Hit;
}

/** @brief Adds `L` (normalized) to `axes` unless it is degenerate (parallel edge pair). */
inline void push_axis_(glm::vec3* axes, int& count, const glm::vec3& L) {
    float len2 = glm::dot(L, L);
    if (len2 < 1e-10f) return;
    axes[count++] = L / std::sqrt(len2);
}

/** @brief Projection of an OBB onto unit axis `L`. */
inline void project_obb_(const geometry::OBB& box, const glm::vec3 box_axes[3], const glm::vec3& L, float& lo, float& hi) {
    float c = glm::dot(box.center, L);
    float r = box.half_extents.x * std::abs(glm::dot(box_axes[0], L)) +
              box.half_extents.y * std::abs(glm::dot(box_axes[1], L)) +
              box.half_extents.z * std::abs(glm::dot(box_axes[2], L));
    lo = c - r;
    hi = c + r;
}

/**
 * @brief A representative contact point for a polytope sweep: the cast box's support feature
 *        (its corners nearest the target along `-n`) at the time of impact, averaged, then
 *        snapped onto the target via `closest_on_target`. Exact for a corner-on-face contact;
 *        the centre of the cast box's touching face/edge, projected onto the target, otherwise.
 */
template <typename ClosestFn>
inline glm::vec3 polytope_contact_point_(const geometry::OBB& box_at_toi, const glm::vec3& n, ClosestFn&& closest_on_target) {
    glm::vec3 corners[8];
    obb_corners_(box_at_toi, corners);
    float lo = std::numeric_limits<float>::max();
    for (const glm::vec3& c : corners) lo = std::min(lo, glm::dot(c, n));
    glm::vec3 sum(0.0f);
    int count = 0;
    float eps = 1e-3f * std::max({box_at_toi.half_extents.x, box_at_toi.half_extents.y, box_at_toi.half_extents.z, 1e-3f});
    for (const glm::vec3& c : corners) {
        if (glm::dot(c, n) <= lo + eps) {
            sum += c;
            ++count;
        }
    }
    return closest_on_target(sum / float(std::max(count, 1)));
}

/**
 * @brief Swept SAT of cast box `box` (moving along `dir`) against triangle (v0, v1, v2):
 *        13 axes -- the triangle normal, the box's 3 face axes, and the 9 edge-edge crosses.
 */
inline Advance_ sweep_box_triangle_(const geometry::OBB& box, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                    const glm::vec3& dir, float max_distance, float& toi, glm::vec3& n,
                                    glm::vec3& mtv_axis, float& mtv_depth) {
    glm::vec3 bx[3];
    box.axes(bx[0], bx[1], bx[2]);
    const glm::vec3 edges[3] = {v1 - v0, v2 - v1, v0 - v2};
    glm::vec3 axes[13];
    int count = 0;
    push_axis_(axes, count, glm::cross(edges[0], edges[1]));
    for (int i = 0; i < 3; ++i) push_axis_(axes, count, bx[i]);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) push_axis_(axes, count, glm::cross(bx[i], edges[j]));

    auto project_box = [&](const glm::vec3& L, float& lo, float& hi) { project_obb_(box, bx, L, lo, hi); };
    auto project_tri = [&](const glm::vec3& L, float& lo, float& hi) {
        float p0 = glm::dot(v0, L), p1 = glm::dot(v1, L), p2 = glm::dot(v2, L);
        lo = std::min({p0, p1, p2});
        hi = std::max({p0, p1, p2});
    };
    return swept_sat_(axes, count, project_box, project_tri, dir, max_distance, toi, n, mtv_axis, mtv_depth);
}

/** @brief Swept SAT of cast box `a` (moving along `dir`) against box `b`: 15 axes. */
inline Advance_ sweep_box_box_(const geometry::OBB& a, const geometry::OBB& b, const glm::vec3& dir, float max_distance,
                               float& toi, glm::vec3& n, glm::vec3& mtv_axis, float& mtv_depth) {
    glm::vec3 ax[3], bx[3];
    a.axes(ax[0], ax[1], ax[2]);
    b.axes(bx[0], bx[1], bx[2]);
    glm::vec3 axes[15];
    int count = 0;
    for (int i = 0; i < 3; ++i) push_axis_(axes, count, ax[i]);
    for (int i = 0; i < 3; ++i) push_axis_(axes, count, bx[i]);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) push_axis_(axes, count, glm::cross(ax[i], bx[j]));
    auto project_a = [&](const glm::vec3& L, float& lo, float& hi) { project_obb_(a, ax, L, lo, hi); };
    auto project_b = [&](const glm::vec3& L, float& lo, float& hi) { project_obb_(b, bx, L, lo, hi); };
    return swept_sat_(axes, count, project_a, project_b, dir, max_distance, toi, n, mtv_axis, mtv_depth);
}

/**
 * @brief Minimum-translation direction for a segment core lying (partly) INSIDE a box -- the
 *        one case closest-point math can't orient (the closest points coincide). Tests the
 *        box's 3 face axes and the 3 crosses of the segment with them, the axes a segment-vs-box
 *        separation can need; depth includes the core's `radius`.
 */
inline void segment_box_mtv_(const glm::vec3& p0, const glm::vec3& p1, float radius, const geometry::OBB& box,
                             glm::vec3& normal, float& depth) {
    glm::vec3 bx[3];
    box.axes(bx[0], bx[1], bx[2]);
    glm::vec3 axes[6];
    int count = 0;
    for (int i = 0; i < 3; ++i) push_axis_(axes, count, bx[i]);
    for (int i = 0; i < 3; ++i) push_axis_(axes, count, glm::cross(p1 - p0, bx[i]));
    depth = std::numeric_limits<float>::max();
    normal = glm::vec3(0.0f, 0.0f, 1.0f);
    for (int k = 0; k < count; ++k) {
        const glm::vec3& L = axes[k];
        float s0 = glm::dot(p0, L), s1 = glm::dot(p1, L);
        float b0, b1;
        project_obb_(box, bx, L, b0, b1);
        float lo = std::min(s0, s1), hi = std::max(s0, s1);
        float overlap = std::min(hi - b0, b1 - lo) + radius;
        if (overlap < depth) {
            depth = overlap;
            normal = (lo + hi) >= (b0 + b1) ? L : -L;
        }
    }
}

// --- Penetration ---

/**
 * @brief Penetration of convex query shape `probe` (at `pos`/`rot`) into convex target
 *        `target` (at `tpos`/`trot`, not a TriangleMesh -- see mesh_penetrations()).
 *
 * @param normal Output: unit direction to move the probe to separate (target toward probe).
 * @param depth  Output: distance to move it along `normal`.
 * @param point  Output: a representative contact point.
 * @return True if they overlap by a positive depth.
 */
inline bool convex_penetration(const collision::Shape& probe, const glm::vec3& pos, const glm::quat& rot,
                               const collision::Shape& target, const glm::vec3& tpos, const glm::quat& trot,
                               glm::vec3& normal, float& depth, glm::vec3& point) {
    using collision::ShapeType;
    // Capsule-vs-Box, either way round: generate_contacts' alternating-projection closest points
    // can't orient a core that has entered the box, so handle the pair here.
    bool probe_core_vs_box = probe.type == ShapeType::Capsule && target.type == ShapeType::Box;
    bool box_vs_target_core = probe.type == ShapeType::Box && target.type == ShapeType::Capsule;
    if (probe_core_vs_box || box_vs_target_core) {
        CastGeometry_ core = probe_core_vs_box ? cast_geometry_(probe, pos, rot) : target_core_(target, tpos, trot);
        geometry::OBB box = probe_core_vs_box ? collision::world_obb(target, tpos, trot) : collision::world_obb(probe, pos, rot);
        glm::vec3 on_seg, on_box;
        float dist = std::sqrt(collision::closest_points_segment_box(core.a, core.b, box, on_seg, on_box));
        if (dist >= core.radius) return false;
        if (dist > util::k_epsilon) {
            normal = (on_seg - on_box) / dist; // box toward core
            depth = core.radius - dist;
        } else {
            segment_box_mtv_(core.a, core.b, core.radius, box, normal, depth);
        }
        point = on_box;
        if (!probe_core_vs_box) normal = -normal; // the probe is the box: push it away from the core
        return depth > 0.0f;
    }

    collision::ContactManifold m;
    if (!collision::generate_contacts(dynamics::BodyId{}, probe, pos, rot, dynamics::BodyId{}, target, tpos, trot, m))
        return false;
    normal = -m.normal; // generate_contacts' normal points probe(a) -> target(b)
    depth = 0.0f;
    glm::vec3 sum(0.0f);
    for (uint8_t k = 0; k < m.count; ++k) {
        depth = std::max(depth, m.points[k].penetration);
        sum += m.points[k].position;
    }
    point = m.count > 0 ? sum / float(m.count) : tpos;
    return depth > 0.0f;
}

/**
 * @brief Per-triangle penetrations of convex `probe` into TriangleMesh `mesh_shape`, through
 *        the same BVH-narrowed, internal-edge-corrected per-triangle tests contact generation
 *        uses (collision::mesh_triangle_contacts()). Calls `emit(normal, depth, point)` for each
 *        contact point found -- typically several per triangle for a box (one per embedded
 *        corner); the caller merges them.
 */
template <typename Fn>
inline void mesh_penetrations(const collision::Shape& probe, const glm::vec3& pos, const glm::quat& rot,
                              const collision::Shape& mesh_shape, const glm::vec3& mesh_pos, const glm::quat& mesh_rot,
                              Fn&& emit) {
    if (!mesh_shape.mesh) return;
    const glm::mat4 mesh_transform = collision::mesh_world_transform(mesh_shape, mesh_pos, mesh_rot);
    const glm::quat mesh_world_rot = mesh_rot * mesh_shape.local_rotation;
    std::vector<uint32_t> candidates;
    collision::mesh_contact_candidates(probe, pos, rot, mesh_shape, mesh_transform, candidates);
    for (uint32_t tri : candidates) {
        collision::mesh_triangle_contacts(probe, pos, rot, *mesh_shape.mesh, mesh_transform, mesh_world_rot, tri,
                                          [&](const glm::vec3& point, const glm::vec3& normal, float penetration, uint32_t) {
                                              if (penetration > 0.0f) emit(normal, penetration, point);
                                          });
    }
}

// --- Sweeps ---

/**
 * @brief Resolves a sweep that starts touching/overlapping its target (Advance_::AtStart):
 *        reports a distance-0 hit only when `dir` leads into the target (see the file doc).
 */
inline bool resolve_at_start_(const glm::vec3& dir, const glm::vec3& normal, float depth, const glm::vec3& point,
                              SweepResult& out) {
    if (glm::dot(dir, normal) >= -1e-4f) return false; // moving out of / along it
    out.distance = 0.0f;
    out.normal = normal;
    out.point = point;
    out.started_inside = depth > k_sweep_tolerance;
    return true;
}

/**
 * @brief Casts convex `shape` (at `pos`/`rot`, translating along unit `dir` up to
 *        `max_distance`) against one convex target (Sphere/Box/Capsule) at `tpos`/`trot`.
 */
inline bool sweep_convex(const collision::Shape& shape, const glm::vec3& pos, const glm::quat& rot,
                         const glm::vec3& dir, float max_distance,
                         const collision::Shape& target, const glm::vec3& tpos, const glm::quat& trot,
                         SweepResult& out) {
    using collision::ShapeType;
    CastGeometry_ cast = cast_geometry_(shape, pos, rot);
    float toi = 0.0f;
    glm::vec3 n(0.0f, 0.0f, 1.0f), p(0.0f);
    glm::vec3 mtv_axis(0.0f, 0.0f, 1.0f);
    float mtv_depth = 0.0f;
    const bool polytopes = shape.type == ShapeType::Box && target.type == ShapeType::Box;
    Advance_ result;

    if (polytopes) {
        geometry::OBB tb = collision::world_obb(target, tpos, trot);
        result = sweep_box_box_(cast.box, tb, dir, max_distance, toi, n, mtv_axis, mtv_depth);
        if (result == Advance_::Hit) {
            geometry::OBB moved = cast.box;
            moved.center += dir * toi;
            p = polytope_contact_point_(moved, n, [&](const glm::vec3& q) { return geometry::closest_point_on_obb(q, tb); });
        } else if (result == Advance_::AtStart) {
            p = geometry::closest_point_on_obb(cast.box.center, tb);
        }
    } else if (shape.type == ShapeType::Box) {
        // Moving box vs a Sphere/Capsule target's core.
        CastGeometry_ core = target_core_(target, tpos, trot);
        auto dist_at = [&](float t, glm::vec3& normal, glm::vec3& point) {
            geometry::OBB moved = cast.box;
            moved.center += dir * t;
            glm::vec3 on_seg, on_box;
            float dist = std::sqrt(collision::closest_points_segment_box(core.a, core.b, moved, on_seg, on_box));
            if (dist > util::k_epsilon) normal = (on_box - on_seg) / dist; // target core toward box
            point = on_seg + normal * core.radius;
            return dist - core.radius;
        };
        result = advance_(dist_at, dir, max_distance, toi, n, p);
    } else {
        // Moving Sphere/Capsule core vs any convex target.
        auto dist_at = [&](float t, glm::vec3& normal, glm::vec3& point) {
            glm::vec3 a = cast.a + dir * t, b = cast.b + dir * t;
            glm::vec3 on_cast, on_target;
            float target_radius = 0.0f;
            if (target.type == ShapeType::Box) {
                collision::closest_points_segment_box(a, b, collision::world_obb(target, tpos, trot), on_cast, on_target);
            } else {
                CastGeometry_ core = target_core_(target, tpos, trot);
                geometry::closest_points_segment_segment(a, b, core.a, core.b, on_cast, on_target);
                target_radius = core.radius;
            }
            glm::vec3 diff = on_cast - on_target;
            float dist = glm::length(diff);
            if (dist > util::k_epsilon) normal = diff / dist;
            point = on_target + normal * target_radius;
            return dist - cast.radius - target_radius;
        };
        result = advance_(dist_at, dir, max_distance, toi, n, p);
    }

    if (result == Advance_::Miss) return false;
    if (result == Advance_::AtStart) {
        // Box-box already has its minimum-overlap axis from the SAT; every other pair takes the
        // penetration normal when genuinely overlapping, else (touching within tolerance) the
        // closest-point normal at t=0.
        glm::vec3 normal = polytopes ? mtv_axis : n;
        glm::vec3 point = p;
        float depth = polytopes ? mtv_depth : 0.0f;
        glm::vec3 pen_normal, pen_point;
        float pen_depth = 0.0f;
        if (!polytopes && convex_penetration(shape, pos, rot, target, tpos, trot, pen_normal, pen_depth, pen_point)) {
            normal = pen_normal;
            point = pen_point;
            depth = pen_depth;
        }
        return resolve_at_start_(dir, normal, depth, point, out);
    }
    out.distance = toi;
    out.normal = n;
    out.point = p;
    out.started_inside = false;
    return true;
}

/**
 * @brief For a sweep hit at `on_tri` on a mesh triangle: the raw closest-point `normal`, unless
 *        the point lies on an edge shared with a coplanar neighbour (an internal edge of a
 *        flat, tessellated surface), where the face normal is the only meaningful direction --
 *        the sweep form of mesh_contact.h's ghost-collision correction. Genuine creases and
 *        silhouettes keep the exact raw normal.
 */
inline glm::vec3 sweep_mesh_normal_(const glm::vec3& raw, const glm::vec3& on_tri, const glm::vec3& v0, const glm::vec3& v1,
                                    const glm::vec3& v2, const glm::vec3& face_normal,
                                    const geometry::TriangleMesh& mesh, const glm::quat& mesh_world_rot, uint32_t tri) {
    int edge = collision::classify_triangle_edge_(collision::barycentric_(on_tri, v0, v1, v2));
    if (edge < 0) return raw; // face interior (raw is already +-face_normal) or a vertex
    uint32_t neighbor = mesh.triangle_neighbor(tri, edge);
    if (neighbor == geometry::k_no_neighbor) return raw;
    glm::vec3 neighbor_normal = glm::normalize(mesh_world_rot * mesh.triangle_normal(neighbor));
    if (glm::dot(face_normal, neighbor_normal) <= 0.999f) return raw;
    return glm::dot(raw, face_normal) >= 0.0f ? face_normal : -face_normal;
}

/**
 * @brief Casts convex `shape` against a TriangleMesh target, per triangle over the BVH's
 *        candidates for the swept volume, keeping the earliest contact.
 */
inline bool sweep_mesh(const collision::Shape& shape, const glm::vec3& pos, const glm::quat& rot,
                       const glm::vec3& dir, float max_distance,
                       const collision::Shape& mesh_shape, const glm::vec3& mesh_pos, const glm::quat& mesh_rot,
                       SweepResult& out) {
    using collision::ShapeType;
    if (!mesh_shape.mesh) return false;
    const geometry::TriangleMesh& mesh = *mesh_shape.mesh;
    const glm::mat4 mesh_transform = collision::mesh_world_transform(mesh_shape, mesh_pos, mesh_rot);
    const glm::quat mesh_world_rot = mesh_rot * mesh_shape.local_rotation;
    const CastGeometry_ cast = cast_geometry_(shape, pos, rot);

    // Candidate triangles: the cast shape's start bounds swept along `dir`, in mesh space.
    // Dividing the direction by mesh_scale keeps the local ray parameter equal to world
    // distance (same trick as raycast_shape()'s mesh case).
    geometry::AABB local_start = collision::mesh_local_bounds(collision::world_bounds(shape, pos, rot), mesh_transform);
    float inv_scale = mesh_shape.mesh_scale > 1e-8f ? 1.0f / mesh_shape.mesh_scale : 1.0f;
    geometry::Ray local_ray;
    local_ray.origin = local_start.center();
    local_ray.direction = glm::inverse(mesh_world_rot) * dir * inv_scale;
    local_ray.max_distance = max_distance;

    bool found = false;
    float best = max_distance;
    mesh.sweep(local_start, local_ray, [&](uint32_t tri) {
        if (found && best <= 0.0f) return; // nothing can beat a distance-0 hit
        glm::vec3 lv0, lv1, lv2;
        mesh.triangle_vertices(tri, lv0, lv1, lv2);
        glm::vec3 v0 = glm::vec3(mesh_transform * glm::vec4(lv0, 1.0f));
        glm::vec3 v1 = glm::vec3(mesh_transform * glm::vec4(lv1, 1.0f));
        glm::vec3 v2 = glm::vec3(mesh_transform * glm::vec4(lv2, 1.0f));
        glm::vec3 face_normal = glm::normalize(mesh_world_rot * mesh.triangle_normal(tri));

        float toi = 0.0f;
        glm::vec3 n(0.0f, 0.0f, 1.0f), p(0.0f);
        Advance_ result;
        SweepResult candidate;
        if (shape.type == ShapeType::Box) {
            glm::vec3 mtv_axis(0.0f, 0.0f, 1.0f);
            float mtv_depth = 0.0f;
            result = sweep_box_triangle_(cast.box, v0, v1, v2, dir, best, toi, n, mtv_axis, mtv_depth);
            if (result == Advance_::Miss) return;
            if (result == Advance_::AtStart) {
                glm::vec3 point = collision::closest_point_on_triangle(cast.box.center, v0, v1, v2);
                if (!resolve_at_start_(dir, mtv_axis, mtv_depth, point, candidate)) return;
            } else {
                geometry::OBB moved = cast.box;
                moved.center += dir * toi;
                candidate.distance = toi;
                candidate.normal = n;
                candidate.point = polytope_contact_point_(moved, n, [&](const glm::vec3& q) {
                    return collision::closest_point_on_triangle(q, v0, v1, v2);
                });
            }
        } else {
            auto dist_at = [&](float t, glm::vec3& normal, glm::vec3& point) {
                glm::vec3 on_seg, on_tri;
                float dist = std::sqrt(collision::closest_points_segment_triangle(cast.a + dir * t, cast.b + dir * t,
                                                                                  v0, v1, v2, on_seg, on_tri));
                if (dist > util::k_epsilon) {
                    normal = (on_seg - on_tri) / dist;
                } else {
                    // The core pierces the triangle: push toward whichever side the core's centre is on.
                    glm::vec3 centre = 0.5f * (cast.a + cast.b) + dir * t;
                    normal = glm::dot(centre - v0, face_normal) >= 0.0f ? face_normal : -face_normal;
                }
                point = on_tri;
                return dist - cast.radius;
            };
            result = advance_(dist_at, dir, best, toi, n, p);
            if (result == Advance_::Miss) return;
            glm::vec3 normal = sweep_mesh_normal_(n, p, v0, v1, v2, face_normal, mesh, mesh_world_rot, tri);
            if (result == Advance_::AtStart) {
                glm::vec3 n0, p0;
                float depth = -dist_at(0.0f, n0, p0);
                if (!resolve_at_start_(dir, normal, depth, p, candidate)) return;
            } else {
                candidate.distance = toi;
                candidate.normal = normal;
                candidate.point = p;
            }
        }
        if (!found || candidate.distance < best) {
            best = candidate.distance;
            out = candidate;
            found = true;
        }
    });
    return found;
}

/**
 * @brief Casts convex `shape` (Sphere, Capsule or Box, at `pos`/`rot`) along unit `dir` up to
 *        `max_distance` against `target` at `tpos`/`trot`, any shape type. False for a
 *        TriangleMesh cast shape (mesh-vs-anything casts are not supported), and false for a
 *        ConvexHull on EITHER side -- see this file's doc for why that is a deliberate,
 *        documented gap rather than an oversight.
 */
inline bool sweep_shape(const collision::Shape& shape, const glm::vec3& pos, const glm::quat& rot,
                        const glm::vec3& dir, float max_distance,
                        const collision::Shape& target, const glm::vec3& tpos, const glm::quat& trot,
                        SweepResult& out) {
    if (shape.type == collision::ShapeType::TriangleMesh || shape.type == collision::ShapeType::ConvexHull ||
        target.type == collision::ShapeType::ConvexHull || max_distance < 0.0f) return false;
    if (target.type == collision::ShapeType::TriangleMesh)
        return sweep_mesh(shape, pos, rot, dir, max_distance, target, tpos, trot, out);
    return sweep_convex(shape, pos, rot, dir, max_distance, target, tpos, trot, out);
}

} // namespace query
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_QUERY_SWEEP_H

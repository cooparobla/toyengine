/**
 * @file sat_hull.h
 * @brief Separating Axis Theorem contact generation generalized from boxes (collision/sat.h) to
 *        arbitrary convex hulls: hull-hull, hull-box, hull-sphere, hull-capsule.
 *
 * SAT only -- no GJK, no EPA, no randomized convergence. See geometry/convex_hull.h's file doc
 * for why: GJK/EPA give no stable feature id for warm-starting, which is the one thing a
 * resting-contact solver cannot do without. A face axis (from either hull) or an edge-edge
 * cross axis always has a nameable, stable identity (a face index or an ordered edge pair), the
 * same property collision/sat.h's box-box routine already relies on -- this file is that
 * routine's generalization, not a different approach.
 *
 * A SEPARATE file from sat.h, deliberately: the box-box fast path (6+9 fixed axes, fixed-size
 * stack buffers) stays untouched and independently measurable. Hulls cost more per axis test
 * (projecting a variable-size vertex set instead of 3 dot products against half-extents), which
 * is the whole reason build_convex_hull()'s `max_vertices` cap exists.
 *
 * No speculative/CCD support for any hull pair, same precedent narrowphase.h already sets for
 * Capsule pairs ("capsule speculative support is a tractable follow-up") -- not attempted here.
 */

#ifndef PHYSXCOOPA_COLLISION_SAT_HULL_H
#define PHYSXCOOPA_COLLISION_SAT_HULL_H

#include <physxcoopa/geometry/convex_hull.h>
#include <physxcoopa/geometry/obb.h>
#include <physxcoopa/geometry/sphere.h>
#include <physxcoopa/geometry/capsule.h>
#include <physxcoopa/collision/clip.h>
#include <physxcoopa/collision/manifold.h>
#include <physxcoopa/collision/shape.h>
#include <physxcoopa/util/math.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace coopa {
namespace physx {
namespace collision {

/** @brief A hull posed in the world: the hull plus where it is. Avoids copying vertices --
 *         world-space positions are derived on demand by the functions below. */
struct PosedHull {
    const geometry::ConvexHull* hull = nullptr;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float scale = 1.0f;

    glm::vec3 world_vertex(std::uint32_t i) const {
        return position + rotation * (hull->vertices[i] * scale);
    }
    glm::vec3 world_face_normal(std::size_t f) const {
        return rotation * glm::vec3(hull->face_planes[f]);
    }
};

/** @brief Instances a ConvexHull Shape at a body's current world pose -- the hull analog of
 *         world_sphere()/world_obb()/world_capsule() in shape.h (kept here rather than there
 *         since PosedHull is this file's type). */
inline PosedHull world_posed_hull(const Shape& s, const glm::vec3& pos, const glm::quat& rot) {
    PosedHull h;
    h.hull = s.hull;
    h.position = pos + rot * s.local_center;
    h.rotation = rot * s.local_rotation;
    h.scale = s.hull_scale;
    return h;
}

// --- Internals ---

namespace detail {

inline std::vector<glm::vec3> hull_world_vertices_(const PosedHull& h) {
    std::vector<glm::vec3> out(h.hull->vertices.size());
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = h.world_vertex(std::uint32_t(i));
    return out;
}

struct Interval_ { float lo, hi; };

inline Interval_ project_(const std::vector<glm::vec3>& pts, const glm::vec3& axis) {
    Interval_ r{std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    for (const glm::vec3& p : pts) {
        float d = glm::dot(p, axis);
        r.lo = std::min(r.lo, d);
        r.hi = std::max(r.hi, d);
    }
    return r;
}

/**
 * @brief One-sided SAT test along one face's own outward unit normal `n`: the correct
 *        generalization of a BOX's two-sided, antipodal-pair-combining per-UNIQUE-axis test
 *        (sat.h's box-box routine, which only ever tests 3 axes for a box because each axis
 *        already stands for both of a box's opposite faces) to an individual hull FACE, which
 *        has no antipodal partner to pair with.
 *
 * Testing a face axis as an undirected two-sided interval -- `min(ia.hi-ib.lo, ib.hi-ia.lo)`,
 * the way the box shortcut safely does for a box's 3 unique axes -- ties two OPPOSITE hull
 * faces at the same score whenever the other body sits squarely along that axis (the common
 * resting/face-face case), making the "winning" face arbitrary: half the time the chosen
 * reference face points AWAY from the other body, and the contact-plane reconstruction built
 * from its true outward normal then clips against the wrong half-space entirely (observed as a
 * grossly inflated, nonsensical penetration depth -- the bug the hull-vs-box/hull-vs-hull
 * reproduction tests in narrowphase_test.cpp exist to catch).
 *
 * The one-sided form has no such ambiguity: `self`'s own extreme along its own face normal is
 * always exactly that face's plane constant (by hull convexity), so `other`'s nearest point
 * minus that constant is a correctly-SIGNED separation -- positive means a genuine separating
 * axis (this hull face provably does not touch `other` at all), never a false tie with the
 * opposite face.
 *
 * @param overlap Set to the one-sided penetration depth (only meaningful when this returns
 *                true; negative separations are collapsed to a true early-exit instead).
 * @return False if this face is a genuine separating axis (the caller should treat that as
 *         proof the two hulls do not overlap at all).
 */
inline bool face_overlap_(const std::vector<glm::vec3>& self_pts, const std::vector<glm::vec3>& other_pts,
                          const glm::vec3& n, float& overlap) {
    const float d = project_(self_pts, n).hi;
    const float other_min = project_(other_pts, n).lo;
    overlap = d - other_min;
    return overlap >= 0.0f;
}

/** @brief Clips a polygon of up to `cap` vertices against one half-space, same convention as
 *         clip.h's clip_polygon() (kept: dot(normal, p) <= d) but with a caller-chosen capacity
 *         -- a merged hull face can have far more than clip.h's box-sized k_max_clip_vertices. */
template <int Cap>
inline void clip_polygon_n_(const glm::vec3* in, int in_count, const ClipPlane& plane, glm::vec3* out, int& out_count) {
    out_count = 0;
    if (in_count == 0) return;
    for (int i = 0; i < in_count; ++i) {
        const glm::vec3& current = in[i];
        const glm::vec3& prev = in[(i - 1 + in_count) % in_count];
        float d_current = glm::dot(plane.normal, current) - plane.d;
        float d_prev = glm::dot(plane.normal, prev) - plane.d;
        bool current_inside = d_current <= 0.0f;
        bool prev_inside = d_prev <= 0.0f;
        if (current_inside != prev_inside) {
            float t = d_prev / (d_prev - d_current);
            if (out_count < Cap) out[out_count++] = prev + t * (current - prev);
        }
        if (current_inside && out_count < Cap) out[out_count++] = current;
    }
}

/** @brief Hard cap on a clipped hull-face polygon: a face can have at most as many vertices as
 *         the hull's own `max_vertices` cap (64 by default), and clipping against its own edge
 *         count of side planes can at most double that in the worst case. */
inline constexpr int k_hull_clip_cap = 160;

/** @brief Outward in-plane edge normal for a CCW polygon (viewed from +face_normal): the
 *         direction a Sutherland-Hodgman side plane needs. Derivation in the file's own
 *         development notes: for a CCW square facing +Z, edge (0,0)->(1,0)'s outward normal is
 *         -Y, which is cross(edge_dir, face_normal), not the other order. */
inline glm::vec3 outward_edge_normal_(const glm::vec3& edge_dir, const glm::vec3& face_normal) {
    glm::vec3 n = glm::cross(edge_dir, face_normal);
    float len = glm::length(n);
    return len > 1e-12f ? n / len : face_normal;
}

/** @brief Builds the N side clip planes for one hull face, analogous to clip.h's
 *         obb_side_planes() but for an arbitrary-sided world-space polygon. */
inline int hull_face_side_planes_(const PosedHull& h, std::size_t face, ClipPlane* planes, int cap) {
    std::uint32_t start = h.hull->face_starts[face];
    std::uint32_t end = h.hull->face_starts[face + 1];
    int n = int(end - start);
    glm::vec3 normal = h.world_face_normal(face);
    int count = 0;
    for (int i = 0; i < n && count < cap; ++i) {
        glm::vec3 v0 = h.world_vertex(h.hull->face_vertices[start + i]);
        glm::vec3 v1 = h.world_vertex(h.hull->face_vertices[start + (i + 1) % n]);
        glm::vec3 edge_dir = v1 - v0;
        glm::vec3 outward = outward_edge_normal_(edge_dir, normal);
        planes[count++] = ClipPlane{outward, glm::dot(outward, v0)};
    }
    return count;
}

/** @brief World-space polygon vertices of one hull face. */
inline int hull_face_polygon_(const PosedHull& h, std::size_t face, glm::vec3* out, int cap) {
    std::uint32_t start = h.hull->face_starts[face];
    std::uint32_t end = h.hull->face_starts[face + 1];
    int n = std::min(int(end - start), cap);
    for (int i = 0; i < n; ++i) out[i] = h.world_vertex(h.hull->face_vertices[start + i]);
    return n;
}

} // namespace detail

/**
 * @struct HullSatResult
 * @brief The minimum-penetration axis found across a hull-hull SAT test: `normal` always points
 *        from A toward B.
 */
struct HullSatResult {
    float penetration = 0.0f;
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    enum class Kind { FaceA, FaceB, EdgeEdge } kind = Kind::FaceA;
    int face_a = -1, face_b = -1; /**< Valid for Kind::FaceA / Kind::FaceB respectively. */
    int edge_a = -1, edge_b = -1; /**< Valid for Kind::EdgeEdge. */
};

/**
 * @brief SAT between two convex hulls over A's face normals, B's face normals, and the unique
 *        edge-edge crosses -- the direct generalization of sat_test_obb_obb()'s 6+9 axes to
 *        however many faces/edges each hull actually has.
 *
 * Face axes use the ONE-SIDED test (detail::face_overlap_()'s doc) since an individual hull
 * face, unlike a box's unique axis, has no antipodal partner to pair with. Edge-edge axes stay
 * the symmetric two-interval test with a sign correction -- a cross product has no natural
 * "outward" direction, the same reasoning sat.h's box-box edge-edge branch already relies on.
 *
 * @return False the first axis proves separation (no speculative/CCD support for hull pairs).
 */
inline bool sat_test_hull_hull(const PosedHull& a, const PosedHull& b, HullSatResult& result) {
    std::vector<glm::vec3> pts_a = detail::hull_world_vertices_(a);
    std::vector<glm::vec3> pts_b = detail::hull_world_vertices_(b);

    float best_overlap = std::numeric_limits<float>::max();
    bool found = false;
    constexpr float k_edge_bias = 0.01f;

    // `self_is_a` says which hull owns this face -- `normal` is self's own outward normal, so
    // the contact normal (always A toward B) is `normal` when self is A, `-normal` when self is B.
    auto consider_face = [&](const std::vector<glm::vec3>& self_pts, const std::vector<glm::vec3>& other_pts,
                             const glm::vec3& normal_raw, bool self_is_a, HullSatResult::Kind kind, int fa,
                             int fb) -> bool {
        float len2 = glm::dot(normal_raw, normal_raw);
        if (len2 < 1e-10f) return true; // degenerate face normal: skip, not a failure
        glm::vec3 n = normal_raw / std::sqrt(len2);
        float overlap = 0.0f;
        if (!detail::face_overlap_(self_pts, other_pts, n, overlap)) return false; // a true separating axis
        if (overlap < best_overlap) {
            best_overlap = overlap;
            result.penetration = overlap;
            result.normal = self_is_a ? n : -n;
            result.kind = kind;
            result.face_a = fa;
            result.face_b = fb;
            result.edge_a = -1;
            result.edge_b = -1;
            found = true;
        }
        return true;
    };

    for (std::size_t f = 0; f < a.hull->face_count(); ++f)
        if (!consider_face(pts_a, pts_b, a.world_face_normal(f), true, HullSatResult::Kind::FaceA, int(f), -1))
            return false;
    for (std::size_t f = 0; f < b.hull->face_count(); ++f)
        if (!consider_face(pts_b, pts_a, b.world_face_normal(f), false, HullSatResult::Kind::FaceB, -1, int(f)))
            return false;
    for (std::size_t i = 0; i < a.hull->edges.size(); ++i) {
        glm::vec3 ea_dir = a.world_vertex(a.hull->edges[i].y) - a.world_vertex(a.hull->edges[i].x);
        for (std::size_t j = 0; j < b.hull->edges.size(); ++j) {
            glm::vec3 eb_dir = b.world_vertex(b.hull->edges[j].y) - b.world_vertex(b.hull->edges[j].x);
            glm::vec3 axis_raw = glm::cross(ea_dir, eb_dir);
            float len2 = glm::dot(axis_raw, axis_raw);
            if (len2 < 1e-10f) continue; // degenerate (parallel edges): skip, not a failure
            glm::vec3 axis = axis_raw / std::sqrt(len2);
            detail::Interval_ ia = detail::project_(pts_a, axis);
            detail::Interval_ ib = detail::project_(pts_b, axis);
            float overlap = std::min(ia.hi - ib.lo, ib.hi - ia.lo);
            if (overlap < 0.0f) return false; // separated: short-circuit, no speculative support
            float score = overlap + k_edge_bias;
            if (score < best_overlap) {
                best_overlap = score;
                result.penetration = overlap;
                result.normal = (ia.lo + ia.hi) >= (ib.lo + ib.hi) ? -axis : axis;
                result.kind = HullSatResult::Kind::EdgeEdge;
                result.face_a = -1;
                result.face_b = -1;
                result.edge_a = int(i);
                result.edge_b = int(j);
                found = true;
            }
        }
    }
    return found;
}

/**
 * @brief Generates a face-face or edge-edge contact manifold for two overlapping hulls.
 *
 * Feature ids are disjoint from sat.h's box-box scheme (which tops out under 0x3000) and from
 * each other: face-face packs `0x40000000 | (ref_face << 16) | (incident_vertex_index << 8) |
 * i`, where `ref_face` already carries which body owned it implicitly (A's and B's face
 * indices are never compared against each other, only used to rebuild the SAME manifold next
 * step for the SAME touching faces); edge-edge uses `0x50000000 | (edge_a << 16) | edge_b`.
 * Both are stable across steps for a fixed pair of touching faces/edges -- what warm-starting
 * needs.
 */
inline bool generate_hull_hull_contacts(const PosedHull& a, const PosedHull& b, ContactManifold& out) {
    HullSatResult r;
    if (!sat_test_hull_hull(a, b, r)) return false;
    out = ContactManifold{};
    out.normal = r.normal;

    if (r.kind == HullSatResult::Kind::EdgeEdge) {
        glm::vec3 ea0 = a.world_vertex(a.hull->edges[r.edge_a].x), ea1 = a.world_vertex(a.hull->edges[r.edge_a].y);
        glm::vec3 eb0 = b.world_vertex(b.hull->edges[r.edge_b].x), eb1 = b.world_vertex(b.hull->edges[r.edge_b].y);
        glm::vec3 c1, c2;
        geometry::closest_points_segment_segment(ea0, ea1, eb0, eb1, c1, c2);
        uint32_t feature_id = 0x50000000u | (uint32_t(r.edge_a) << 16) | uint32_t(r.edge_b);
        out.add_point(0.5f * (c1 + c2), r.penetration, feature_id);
        return true;
    }

    bool ref_is_a = r.kind == HullSatResult::Kind::FaceA;
    const PosedHull& ref = ref_is_a ? a : b;
    const PosedHull& inc = ref_is_a ? b : a;
    std::size_t ref_face = ref_is_a ? std::size_t(r.face_a) : std::size_t(r.face_b);

    glm::vec3 ref_normal = ref.world_face_normal(ref_face);
    // Incident face: the other hull's face most anti-parallel to the reference normal.
    std::size_t inc_face = 0;
    float most_anti_parallel = std::numeric_limits<float>::max();
    for (std::size_t f = 0; f < inc.hull->face_count(); ++f) {
        float dp = glm::dot(inc.world_face_normal(f), ref_normal);
        if (dp < most_anti_parallel) {
            most_anti_parallel = dp;
            inc_face = f;
        }
    }

    glm::vec3 poly[detail::k_hull_clip_cap], poly2[detail::k_hull_clip_cap];
    int count = detail::hull_face_polygon_(inc, inc_face, poly, detail::k_hull_clip_cap);

    ClipPlane planes[detail::k_hull_clip_cap];
    int plane_count = detail::hull_face_side_planes_(ref, ref_face, planes, detail::k_hull_clip_cap);
    for (int p = 0; p < plane_count && count > 0; ++p) {
        int next_count = 0;
        detail::clip_polygon_n_<detail::k_hull_clip_cap>(poly, count, planes[p], poly2, next_count);
        for (int i = 0; i < next_count; ++i) poly[i] = poly2[i];
        count = next_count;
    }
    if (count == 0) return false;

    float ref_d = glm::dot(ref_normal, ref.world_vertex(ref.hull->face_vertices[ref.hull->face_starts[ref_face]]));
    struct Candidate { glm::vec3 pos; float penetration; };
    Candidate candidates[detail::k_hull_clip_cap];
    int kept = 0;
    for (int i = 0; i < count; ++i) {
        float penetration = -(glm::dot(ref_normal, poly[i]) - ref_d);
        if (penetration > -util::k_epsilon) {
            candidates[kept].pos = poly[i];
            candidates[kept].penetration = std::max(penetration, 0.0f);
            ++kept;
        }
    }
    if (kept == 0) return false;
    if (kept > 4) {
        std::sort(candidates, candidates + kept,
                  [](const Candidate& x, const Candidate& y) { return x.penetration > y.penetration; });
        kept = 4;
    }
    for (int i = 0; i < kept; ++i) {
        uint32_t feature_id = 0x40000000u | (uint32_t(ref_face) << 16) | (uint32_t(inc_face) << 8) | uint32_t(i & 0xFF);
        out.add_point(candidates[i].pos, candidates[i].penetration, feature_id);
    }
    return out.count > 0;
}

/**
 * @brief Generates a contact manifold for a hull `a` against a world-posed box `b`.
 *
 * Reuses the SAME asymmetric-interval SAT core as hull-hull, with the box's 3 axes/8 corners
 * standing in for a second hull's faces/vertices -- a box IS a convex hull with 3 unique face
 * axes (each axis covers 2 opposite faces, so SAT only needs the axis, not both signs) and 3
 * unique edge directions. `out.normal` points from `a` (the hull) toward `b` (the box).
 */
inline bool generate_hull_box_contacts(const PosedHull& a, const geometry::OBB& b, ContactManifold& out) {
    std::vector<glm::vec3> pts_a = detail::hull_world_vertices_(a);
    glm::vec3 box_axes[3];
    b.axes(box_axes[0], box_axes[1], box_axes[2]);
    glm::vec3 box_corners[8];
    int k = 0;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2)
                box_corners[k++] = b.center + (float(sx) * b.half_extents.x) * box_axes[0] +
                                    (float(sy) * b.half_extents.y) * box_axes[1] + (float(sz) * b.half_extents.z) * box_axes[2];
    std::vector<glm::vec3> pts_b(box_corners, box_corners + 8);

    float best_score = std::numeric_limits<float>::max();
    bool found = false;
    constexpr float k_edge_bias = 0.01f;
    enum class Kind { FaceA, FaceB, EdgeEdge } kind = Kind::FaceA;
    int face_a = -1, box_axis = -1, edge_a = -1, edge_b = -1;
    float best_penetration = 0.0f;
    glm::vec3 best_normal(0.0f, 0.0f, 1.0f);

    auto consider = [&](const glm::vec3& axis_raw, float bias, Kind k2, int fa, int ba, int ea, int eb) -> bool {
        float len2 = glm::dot(axis_raw, axis_raw);
        if (len2 < 1e-10f) return true;
        glm::vec3 axis = axis_raw / std::sqrt(len2);
        detail::Interval_ ia = detail::project_(pts_a, axis);
        detail::Interval_ ib = detail::project_(pts_b, axis);
        float overlap = std::min(ia.hi - ib.lo, ib.hi - ia.lo);
        if (overlap < 0.0f) return false;
        float score = overlap + bias;
        if (score < best_score) {
            best_score = score;
            best_penetration = overlap;
            best_normal = (ia.lo + ia.hi) >= (ib.lo + ib.hi) ? -axis : axis;
            kind = k2;
            face_a = fa;
            box_axis = ba;
            edge_a = ea;
            edge_b = eb;
            found = true;
        }
        return true;
    };

    // Hull A's own faces: the ONE-SIDED test (detail::face_overlap_()'s doc) -- unlike the
    // box's 3 unique axes below (each already standing for both of a pair of opposite faces),
    // an individual hull face has no antipodal partner, so the two-sided `consider()` above
    // would tie two opposite faces at the same score and pick between them arbitrarily.
    for (std::size_t f = 0; f < a.hull->face_count(); ++f) {
        glm::vec3 n = a.world_face_normal(f);
        float overlap = 0.0f;
        if (!detail::face_overlap_(pts_a, pts_b, n, overlap)) return false;
        if (overlap < best_score) {
            best_score = overlap;
            best_penetration = overlap;
            best_normal = n;
            kind = Kind::FaceA;
            face_a = int(f);
            box_axis = -1;
            edge_a = -1;
            edge_b = -1;
            found = true;
        }
    }
    for (int i = 0; i < 3; ++i)
        if (!consider(box_axes[i], 0.0f, Kind::FaceB, -1, i, -1, -1)) return false;
    for (std::size_t i = 0; i < a.hull->edges.size(); ++i) {
        glm::vec3 ea_dir = a.world_vertex(a.hull->edges[i].y) - a.world_vertex(a.hull->edges[i].x);
        for (int j = 0; j < 3; ++j)
            if (!consider(glm::cross(ea_dir, box_axes[j]), k_edge_bias, Kind::EdgeEdge, -1, -1, int(i), j)) return false;
    }
    if (!found) return false;

    out = ContactManifold{};
    out.normal = best_normal;

    if (kind == Kind::EdgeEdge) {
        glm::vec3 ea0 = a.world_vertex(a.hull->edges[edge_a].x), ea1 = a.world_vertex(a.hull->edges[edge_a].y);
        // Box edge `edge_b` (its axis index): pick the specific edge nearest the hull edge by
        // choosing the box edge along that axis closest to the hull edge's midpoint, same
        // technique sat.h's box-box edge branch uses (project the midpoint delta onto the two
        // OTHER box axes to choose which of the 4 parallel edges).
        glm::vec3 mid = 0.5f * (ea0 + ea1);
        glm::vec3 d = mid - b.center;
        int aj = (edge_b + 1) % 3, ak = (edge_b + 2) % 3;
        float sj = glm::dot(d, box_axes[aj]) >= 0.0f ? 1.0f : -1.0f;
        float sk = glm::dot(d, box_axes[ak]) >= 0.0f ? 1.0f : -1.0f;
        glm::vec3 edge_center = b.center + box_axes[aj] * (sj * b.half_extents[aj]) + box_axes[ak] * (sk * b.half_extents[ak]);
        glm::vec3 eb0 = edge_center - box_axes[edge_b] * b.half_extents[edge_b];
        glm::vec3 eb1 = edge_center + box_axes[edge_b] * b.half_extents[edge_b];
        glm::vec3 c1, c2;
        geometry::closest_points_segment_segment(ea0, ea1, eb0, eb1, c1, c2);
        uint32_t feature_id = 0x60000000u | (uint32_t(edge_a) << 8) | uint32_t(edge_b);
        out.add_point(0.5f * (c1 + c2), best_penetration, feature_id);
        return true;
    }

    if (kind == Kind::FaceA) {
        glm::vec3 ref_normal = a.world_face_normal(std::size_t(face_a));
        // Incident: the box face most anti-parallel to the reference normal.
        int inc_axis = 0;
        float inc_sign = 1.0f;
        float most_anti_parallel = std::numeric_limits<float>::max();
        for (int ax = 0; ax < 3; ++ax)
            for (float s : {1.0f, -1.0f}) {
                float dp = glm::dot(box_axes[ax] * s, ref_normal);
                if (dp < most_anti_parallel) {
                    most_anti_parallel = dp;
                    inc_axis = ax;
                    inc_sign = s;
                }
            }
        glm::vec3 inc_corners[4], inc_normal;
        obb_face(b, inc_axis, inc_sign, inc_corners, inc_normal);

        glm::vec3 poly[detail::k_hull_clip_cap], poly2[detail::k_hull_clip_cap];
        int count = 4;
        for (int i = 0; i < 4; ++i) poly[i] = inc_corners[i];
        ClipPlane planes[detail::k_hull_clip_cap];
        int plane_count = detail::hull_face_side_planes_(a, std::size_t(face_a), planes, detail::k_hull_clip_cap);
        for (int p = 0; p < plane_count && count > 0; ++p) {
            int next_count = 0;
            detail::clip_polygon_n_<detail::k_hull_clip_cap>(poly, count, planes[p], poly2, next_count);
            for (int i = 0; i < next_count; ++i) poly[i] = poly2[i];
            count = next_count;
        }
        if (count == 0) return false;

        std::size_t start = a.hull->face_starts[face_a];
        float ref_d = glm::dot(ref_normal, a.world_vertex(a.hull->face_vertices[start]));
        struct Candidate { glm::vec3 pos; float penetration; };
        Candidate candidates[detail::k_hull_clip_cap];
        int kept = 0;
        for (int i = 0; i < count; ++i) {
            float penetration = -(glm::dot(ref_normal, poly[i]) - ref_d);
            if (penetration > -util::k_epsilon) {
                candidates[kept].pos = poly[i];
                candidates[kept].penetration = std::max(penetration, 0.0f);
                ++kept;
            }
        }
        if (kept == 0) return false;
        if (kept > 4) {
            std::sort(candidates, candidates + kept,
                      [](const Candidate& x, const Candidate& y) { return x.penetration > y.penetration; });
            kept = 4;
        }
        for (int i = 0; i < kept; ++i) {
            uint32_t feature_id = 0x40000000u | (uint32_t(face_a) << 16) | uint32_t(i & 0xFF);
            out.add_point(candidates[i].pos, candidates[i].penetration, feature_id);
        }
        return out.count > 0;
    }

    // kind == Kind::FaceB: reference is the box, incident is the hull's most-anti-parallel face.
    glm::vec3 desired_outward = -best_normal; // best_normal points hull(a) -> box(b); box's outward ref face points away from a
    float ref_sign = glm::dot(box_axes[box_axis], desired_outward) >= 0.0f ? 1.0f : -1.0f;
    glm::vec3 ref_corners[4], ref_normal;
    obb_face(b, box_axis, ref_sign, ref_corners, ref_normal);

    std::size_t inc_face = 0;
    float most_anti_parallel = std::numeric_limits<float>::max();
    for (std::size_t f = 0; f < a.hull->face_count(); ++f) {
        float dp = glm::dot(a.world_face_normal(f), ref_normal);
        if (dp < most_anti_parallel) {
            most_anti_parallel = dp;
            inc_face = f;
        }
    }
    glm::vec3 poly[detail::k_hull_clip_cap], poly2[detail::k_hull_clip_cap];
    int count = detail::hull_face_polygon_(a, inc_face, poly, detail::k_hull_clip_cap);

    ClipPlane planes[4];
    obb_side_planes(b, box_axis, planes);
    for (int p = 0; p < 4 && count > 0; ++p) {
        int next_count = 0;
        detail::clip_polygon_n_<detail::k_hull_clip_cap>(poly, count, planes[p], poly2, next_count);
        for (int i = 0; i < next_count; ++i) poly[i] = poly2[i];
        count = next_count;
    }
    if (count == 0) return false;

    float ref_d = glm::dot(ref_normal, ref_corners[0]);
    struct Candidate { glm::vec3 pos; float penetration; };
    Candidate candidates[detail::k_hull_clip_cap];
    int kept = 0;
    for (int i = 0; i < count; ++i) {
        float penetration = -(glm::dot(ref_normal, poly[i]) - ref_d);
        if (penetration > -util::k_epsilon) {
            candidates[kept].pos = poly[i];
            candidates[kept].penetration = std::max(penetration, 0.0f);
            ++kept;
        }
    }
    if (kept == 0) return false;
    if (kept > 4) {
        std::sort(candidates, candidates + kept,
                  [](const Candidate& x, const Candidate& y) { return x.penetration > y.penetration; });
        kept = 4;
    }
    for (int i = 0; i < kept; ++i) {
        uint32_t feature_id = 0x70000000u | (uint32_t(box_axis) << 16) | (uint32_t(inc_face) << 8) | uint32_t(i & 0xFF);
        out.add_point(candidates[i].pos, candidates[i].penetration, feature_id);
    }
    return out.count > 0;
}

/**
 * @brief Closest point on a convex polygon to `point`, projected into the polygon's own plane.
 *
 * `point` is projected onto the plane first (via `normal`, the plane's unit normal), then
 * tested against every edge's own outward in-plane normal (outward_edge_normal_(), the same
 * convention hull_face_side_planes_() builds its clip planes with): inside every edge, the
 * projection itself is the closest point -- this is the common case (a sphere resting squarely
 * on a face, nowhere near an edge) and the one case the edge-only version of this function
 * used to get wrong, since it tested boundary segments only and never asked whether the
 * projection was already interior. Only when `point` projects outside the polygon does the
 * closest point fall on a boundary edge, which is the loop below.
 *
 * O(n) in the polygon's vertex count, which build_convex_hull()'s `max_vertices` keeps small.
 */
inline glm::vec3 closest_point_on_convex_polygon_(const glm::vec3* poly, int n, const glm::vec3& normal,
                                                   const glm::vec3& point) {
    const float plane_d = glm::dot(normal, poly[0]);
    const glm::vec3 proj = point - normal * (glm::dot(normal, point) - plane_d);

    bool inside = true;
    for (int i = 0; i < n && inside; ++i) {
        const glm::vec3 a = poly[i], b = poly[(i + 1) % n];
        const glm::vec3 outward = detail::outward_edge_normal_(b - a, normal);
        if (glm::dot(outward, proj - a) > 1e-6f) inside = false;
    }
    if (inside) return proj;

    glm::vec3 best = poly[0];
    float best_dist2 = std::numeric_limits<float>::max();
    for (int i = 0; i < n; ++i) {
        glm::vec3 a = poly[i], b = poly[(i + 1) % n];
        glm::vec3 ab = b - a;
        float len2 = glm::dot(ab, ab);
        float t = len2 > 1e-12f ? std::clamp(glm::dot(proj - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        glm::vec3 p = a + ab * t;
        float d2 = glm::dot(proj - p, proj - p);
        if (d2 < best_dist2) {
            best_dist2 = d2;
            best = p;
        }
    }
    return best;
}

/**
 * @brief Hull-vs-sphere closest-point test.
 *
 * APPROXIMATION, documented rather than hidden: when the sphere's center is outside the hull,
 * this picks the single MOST-VIOLATED face plane and clamps the projection to that face's own
 * polygon -- exact whenever the true closest feature belongs to that face (the common case: a
 * sphere resting on or poking a face or its near edges), but not a full closest-point-on-a-
 * polytope solve, so a sphere center deep in a hull's far vertex region can pick a slightly
 * wrong normal. The inside case (center inside the hull) pushes out along the LEAST-violated
 * (nearest) face, mirroring sphere_vs_box()'s inside branch.
 *
 * @param normal      Output: unit normal from the hull's surface toward the sphere's center --
 *                     EXCEPT in the fully-inside case (see below), where the only useful
 *                     direction is the opposite one.
 * @param penetration Output: overlap depth.
 * @param point       Output: contact point, on the hull's surface.
 * @return True if the sphere overlaps the hull.
 */
inline bool hull_vs_sphere(const PosedHull& a, const geometry::Sphere& b, glm::vec3& normal, float& penetration,
                           glm::vec3& point) {
    std::size_t best_face = 0;
    float best_dist = -std::numeric_limits<float>::max();
    bool any_positive = false;
    for (std::size_t f = 0; f < a.hull->face_count(); ++f) {
        glm::vec3 n = a.world_face_normal(f);
        float d = glm::dot(n, a.world_vertex(a.hull->face_vertices[a.hull->face_starts[f]]));
        float dist = glm::dot(n, b.center) - d;
        if (dist > 0.0f) any_positive = true;
        if (dist > best_dist) {
            best_dist = dist;
            best_face = f;
        }
    }
    glm::vec3 n = a.world_face_normal(best_face);
    glm::vec3 poly[detail::k_hull_clip_cap];
    int count = detail::hull_face_polygon_(a, best_face, poly, detail::k_hull_clip_cap);

    if (!any_positive) {
        // Inside the hull: push out along the nearest face, clamped to its polygon (exact when
        // the orthogonal projection lands inside the face, which it does whenever the center is
        // closer to that face than to any edge it doesn't share -- see sphere_vs_box()'s analog).
        // `normal` here is the direction FROM the center OUT to that face (point - center, not
        // the usual center - point): sphere_vs_box's own inside branch sets its normal the same
        // way (outward through the near face), and the reverse sign would push a fully-embedded
        // sphere IN the wrong direction -- not merely a non-minimal correction but, for a hull
        // that extends further on the opposite side, one that can still land inside.
        point = closest_point_on_convex_polygon_(poly, count, n, b.center);
        glm::vec3 diff = point - b.center;
        float dist = glm::length(diff);
        normal = dist > util::k_epsilon ? diff / dist : n;
        penetration = b.radius + (-best_dist);
        return true;
    }

    point = closest_point_on_convex_polygon_(poly, count, n, b.center);
    glm::vec3 diff = b.center - point;
    float dist = glm::length(diff);
    if (dist > b.radius) return false;
    normal = dist > util::k_epsilon ? diff / dist : n;
    penetration = b.radius - dist;
    return true;
}

/**
 * @brief Hull-vs-capsule contact generation: closest point between the hull's surface
 *        (hull_vs_sphere()'s per-face approximation, same caveat) and the capsule's core
 *        segment, sampled at both endpoints and their midpoint -- cheap and adequate for a
 *        capsule shape, which narrowphase.h's own capsule_vs_box() also treats via endpoint
 *        sampling rather than a true segment-vs-polytope solve.
 */
inline bool hull_vs_capsule(const PosedHull& a, const geometry::Capsule& b, ContactManifold& out) {
    out = ContactManifold{};
    glm::vec3 samples[3] = {b.a, b.b, 0.5f * (b.a + b.b)};
    glm::vec3 best_normal(0.0f, 0.0f, 1.0f);
    float best_pen = 0.0f;
    bool hit = false;
    for (int i = 0; i < 3; ++i) {
        geometry::Sphere probe{samples[i], b.radius};
        glm::vec3 n, p;
        float pen;
        if (hull_vs_sphere(a, probe, n, pen, p)) {
            out.add_point(p, pen, uint32_t(0x80000000u | uint32_t(i)));
            if (pen > best_pen) {
                best_pen = pen;
                best_normal = n;
            }
            hit = true;
        }
    }
    if (!hit) return false;
    out.normal = best_normal;
    return true;
}

} // namespace collision
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COLLISION_SAT_HULL_H

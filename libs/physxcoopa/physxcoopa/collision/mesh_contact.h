/**
 * @file mesh_contact.h
 * @brief Convex-shape-vs-static-triangle-mesh contact generation, with internal-edge
 *        correction: the classic ghost-collision bug where a box sliding across a flat mesh
 *        floor catches on the shared edge between two coplanar triangles, because a naive
 *        per-triangle contact normal points along the edge rather than the surface.
 *
 * Approach: query the mesh's BVH with the convex shape's bounds for candidate triangles
 * (mesh_contact_candidates()), then run a per-triangle contact test on each
 * (mesh_triangle_contacts()). Two different per-triangle contact tests feed the same
 * correction step, because Sphere/Capsule and Box need genuinely different
 * penetration measurements (see box_vs_triangle_contacts_'s doc for why a box has no
 * meaningful "closest point on surface" once it's overlapping at all):
 * - Sphere/Capsule: closest point on their core (a point or segment) to the triangle, offset
 *   by the shape's radius -- sphere_or_capsule_vs_triangle_contact_.
 * - Box: every corner tested directly against the triangle's plane, one contact per embedded
 *   corner -- box_vs_triangle_contacts_.
 * - ConvexHull: generate_hull_mesh_contacts_() generalizes the box path above to a variable-count
 *   vertex array -- every hull vertex tested the same way a box corner is.
 *
 * Either test also answers the case a shape should never be in but ends up in anyway: through
 * the surface, with nothing on the far side of a terrain's skin to push it back. A shape whose
 * core (or, for a box, centre) has crossed a face it is directly under is EMBEDDED, and gets one
 * contact out along that face's normal by the whole distance it reaches past it. Without it such
 * a shape reports no contact at all and simply keeps going, which is what a body falling through
 * the ground is.
 *
 * Either way, the resulting normal is then corrected: if the shape approaches from genuinely
 * outside the triangle's face (the common case -- resting/sliding on a surface), use the
 * triangle's OWN face normal rather than the raw closest-point direction, which is what stops
 * the direction from ever pointing along a shared internal edge. Only when the shape is
 * meaningfully on the "wrong" side of this triangle's face (a genuine silhouette edge/corner,
 * e.g. the mesh's boundary) does the raw direction survive.
 */

#ifndef PHYSXCOOPA_COLLISION_MESH_CONTACT_H
#define PHYSXCOOPA_COLLISION_MESH_CONTACT_H

#include <physxcoopa/geometry/triangle_mesh.h>
#include <physxcoopa/geometry/obb.h>
#include <physxcoopa/geometry/sphere.h>
#include <physxcoopa/geometry/capsule.h>
#include <physxcoopa/collision/shape.h>
#include <physxcoopa/collision/manifold.h>
#include <physxcoopa/collision/sat_hull.h>
#include <physxcoopa/util/math.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace coopa {
namespace physx {
namespace collision {

/**
 * @brief Closest point on triangle (a,b,c) to `p` (Ericson, "Real-Time Collision Detection",
 *        5.1.5 -- the standard barycentric-region case analysis).
 */
inline glm::vec3 closest_point_on_triangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return a + v * ab;
    }

    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return a + w * ac;
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b);
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return a + ab * v + ac * w;
}

/**
 * @brief Per-triangle contact for a Sphere or Capsule: both have a "core" (a point or a
 *        segment) offset from the actual surface by a constant radius, so penetration is
 *        simply `radius - distance(core, triangle)` -- exactly how sphere_vs_box and
 *        capsule_vs_box already work elsewhere in narrowphase. `on_shape` is the closest point
 *        on the core (not the shape's swept surface); the capsule case alternates between the
 *        segment and the triangle a few times, since both are convex sets needing mutual
 *        projection to converge on their true closest points.
 *
 * @return True if `radius - distance(core, triangle) > 0`.
 */
inline bool sphere_or_capsule_vs_triangle_contact_(const Shape& shape, const glm::vec3& pos, const glm::quat& rot,
                                                    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                                    glm::vec3& on_shape, glm::vec3& on_tri, float& penetration) {
    if (shape.type == ShapeType::Sphere) {
        on_shape = world_sphere(shape, pos, rot).center;
        on_tri = closest_point_on_triangle(on_shape, v0, v1, v2);
    } else {
        geometry::Capsule cap = world_capsule(shape, pos, rot);
        on_tri = (v0 + v1 + v2) * (1.0f / 3.0f);
        for (int i = 0; i < 6; ++i) {
            on_shape = geometry::closest_point_on_segment(on_tri, cap.a, cap.b);
            on_tri = closest_point_on_triangle(on_shape, v0, v1, v2);
        }
    }
    float radius = shape.type == ShapeType::Sphere ? shape.radius : shape.capsule_radius;
    float dist = glm::length(on_shape - on_tri);
    penetration = radius - dist;
    return penetration > 0.0f;
}

/**
 * @brief Per-triangle contact for a Box: a box has no core-radius offset, so the
 *        closest-point-on-surface trick above doesn't work -- once a box's face has
 *        overlapped a triangle's plane at all, the "closest point on the box" to any point on
 *        that triangle is already inside the box (distance 0), destroying the penetration
 *        signal immediately rather than growing it as the box sinks in further.
 *
 * Instead: test all 8 corners against the triangle's own plane directly (a convex-vs-halfspace
 * test per corner, the same one SAT reduces to along a single fixed axis) and emit one contact
 * per embedded corner -- a box resting flat generates all 4 corners on its underside as one
 * consistent 4-point manifold, matching what box-vs-box's face clipping produces, rather than
 * an arbitrarily-chosen single corner that would inject spurious torque as the box rocks between
 * which corner is "deepest" from one step to the next. This only tests the triangle's own
 * face-normal axis (not a full per-triangle SAT with the box's face/edge axes too), which is
 * sufficient for the resting/sliding-on-a-surface case this loop exists for; a box approaching
 * a mesh edge/corner from the side falls to the closest-point-based `on_tri` (already clamped
 * into the finite triangle) and the internal-edge correction below, same as the sphere/capsule
 * path.
 *
 * One-sided: a triangle whose plane the box's CENTRE is behind contributes nothing. The
 * per-corner plane test has no idea which side the box came from, so without this a box resting
 * on a ledge reads its corners as "behind" every wall face below that ledge (and every tilted
 * face of a rounded lip) -- by up to the box's whole size, within the generous footprint -- and
 * each such corner pushes the box sideways out of a wall it never touched. On a mesh with cliffs
 * (a tile terrain's chunk colliders) that fed energy in every step until the box flew off. A box
 * genuinely pressed into a surface keeps its centre in front of it; one that has already
 * tunnelled past its middle is embedded rather than resting, which the corner test cannot
 * resolve sanely at all -- generate_box_mesh_contacts_() handles that case on its own, with a
 * single recovery contact out along the face normal.
 *
 * The contact point is the CORNER (moved halfway out along the face normal), not the closest
 * point on the triangle: with the generous footprint a corner may be accepted by a triangle it
 * is not over (the next one along, or the border triangle of a neighbouring mesh), and the
 * triangle's clamped closest point would then put a support where the box has none -- enough to
 * hold a box balanced on an edge on perfectly flat ground.
 *
 * @param emit Called as `emit(corner_index, corner, penetration, offset)` for each embedded
 *             corner. `corner_index` (0-7) has bit 0 set for the +x half-extent, bit 1 for +y,
 *             bit 2 for +z; `offset` is how far the corner's plane projection lies outside the
 *             triangle (0 when the corner is over it).
 */
template <typename Fn>
inline void box_vs_triangle_contacts_(const Shape& shape, const glm::vec3& pos, const glm::quat& rot,
                                       const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                       const glm::vec3& face_normal, Fn&& emit) {
    geometry::OBB box = world_obb(shape, pos, rot);
    glm::vec3 ax, ay, az;
    box.axes(ax, ay, az);
    const glm::vec3& he = box.half_extents;
    float footprint_limit = glm::length(he);
    if (glm::dot(box.center - v0, face_normal) <= 0.0f) return;   // behind the face: see the doc

    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                glm::vec3 corner = box.center + (float(sx) * he.x) * ax + (float(sy) * he.y) * ay + (float(sz) * he.z) * az;
                float signed_dist = glm::dot(corner - v0, face_normal);
                if (signed_dist >= 0.0f) continue;

                glm::vec3 on_tri = closest_point_on_triangle(corner, v0, v1, v2);

                // Reject triangles this corner isn't actually over: the plane test above doesn't
                // know about the triangle's finite extent, so an unrelated triangle sharing the
                // same plane would otherwise report the same penetration. Compare against the
                // corner's own plane projection, not the corner itself, since the corner is
                // offset from the plane by the penetration depth along `face_normal` -- that
                // offset must not count against the in-plane footprint test.
                glm::vec3 plane_point = corner - signed_dist * face_normal;
                if (glm::length(plane_point - on_tri) > footprint_limit) continue;

                emit(static_cast<uint32_t>(((sx + 1) / 2) | (((sy + 1) / 2) << 1) | (((sz + 1) / 2) << 2)), corner,
                     -signed_dist, glm::length(plane_point - on_tri));
            }
        }
    }
}

/**
 * @brief Per-triangle contact for a ConvexHull: the direct generalization of
 *        box_vs_triangle_contacts_() from a box's fixed 8 corners to a hull's variable-count
 *        vertex array -- every hull vertex tested against the triangle's plane the same way a
 *        box corner is, same one-sided "centre behind the face" rejection, same in-plane
 *        footprint check against the triangle's finite extent (sized to the hull's own local
 *        bounds half-diagonal rather than `glm::length(he)`).
 *
 * Used by mesh_triangle_contacts()'s per-triangle dispatch, which is what
 * PhysicsWorld::compute_penetration() calls (via query::mesh_penetrations()) -- unlike
 * generate_hull_mesh_contacts_() below, this has no embedded-recovery fallback, exactly
 * mirroring box_vs_triangle_contacts_()'s own scope (see that function's doc for why that is
 * sufficient here and handled separately for the manifold-building path).
 *
 * @param emit Called as `emit(vertex_index, vertex, penetration, offset)` per embedded vertex.
 */
template <typename Fn>
inline void hull_vs_triangle_contacts_(const Shape& shape, const glm::vec3& pos, const glm::quat& rot,
                                        const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                        const glm::vec3& face_normal, Fn&& emit) {
    if (!shape.hull) return;
    const PosedHull posed = world_posed_hull(shape, pos, rot);
    const geometry::ConvexHull& hull = *posed.hull;
    const glm::vec3 hull_center = posed.position + posed.rotation * (hull.centroid * posed.scale);
    if (glm::dot(hull_center - v0, face_normal) <= 0.0f) return;   // behind the face: see the doc

    const float footprint_limit = glm::length(hull.local_bounds.extents()) * 0.5f * posed.scale;
    for (std::uint32_t c = 0; c < hull.vertices.size(); ++c) {
        const glm::vec3 vertex = posed.world_vertex(c);
        const float signed_dist = glm::dot(vertex - v0, face_normal);
        if (signed_dist >= 0.0f) continue;

        const glm::vec3 on_tri = closest_point_on_triangle(vertex, v0, v1, v2);
        const glm::vec3 plane_point = vertex - signed_dist * face_normal;
        if (glm::length(plane_point - on_tri) > footprint_limit) continue;

        emit(c, vertex, -signed_dist, glm::length(plane_point - on_tri));
    }
}

/** @brief Barycentric weights of `p` w.r.t. triangle (a,b,c): {weight of a, weight of b, weight of c}. */
inline glm::vec3 barycentric_(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 v0 = b - a, v1 = c - a, v2 = p - a;
    float d00 = glm::dot(v0, v0), d01 = glm::dot(v0, v1), d11 = glm::dot(v1, v1);
    float d20 = glm::dot(v2, v0), d21 = glm::dot(v2, v1);
    float denom = d00 * d11 - d01 * d01;
    if (std::abs(denom) < util::k_epsilon) return glm::vec3(1.0f, 0.0f, 0.0f);
    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    return glm::vec3(1.0f - v - w, v, w);
}

/**
 * @brief Which of triangle `tri`'s edges (0: v0-v1, 1: v1-v2, 2: v2-v0) barycentric point `bary`
 *        lies on, or -1 for a face-interior point or a vertex (two weights near zero at once --
 *        deliberately left uncorrected by edge adjacency, see generate_mesh_contacts).
 */
inline int classify_triangle_edge_(const glm::vec3& bary) {
    constexpr float k_edge_eps = 1e-3f;
    bool near_a = bary.x < k_edge_eps, near_b = bary.y < k_edge_eps, near_c = bary.z < k_edge_eps;
    if (near_c && !near_a && !near_b) return 0;
    if (near_a && !near_b && !near_c) return 1;
    if (near_b && !near_a && !near_c) return 2;
    return -1;
}

/**
 * @brief Internal-edge correction: clamps a raw closest-point normal into a normal
 *        consistent with the mesh's actual surface, using the edge adjacency built by
 *        TriangleMeshLoader.
 *
 * - Face-interior (or vertex) contacts: unambiguous, but still prefer the face normal whenever
 *   the shape is genuinely on its outward side -- only a true silhouette keeps the raw direction.
 * - Boundary edges (no neighbour, `k_no_neighbor`): no constraint -- these are the mesh's
 *   perimeter, where the raw closest-feature direction is the only correct answer.
 * - Nearly-coplanar neighbours (the classic ghost-collision case -- a flat floor tessellated
 *   into many triangles): always trust this triangle's own face normal, which is what stops
 *   the shared edge from ever producing a normal that points along the surface instead of
 *   away from it.
 * - Genuine creases (a real corner): clamp to whichever of the two adjacent face normals the
 *   raw direction is closer to, rather than either an edge-tangent direction or an
 *   unconstrained raw normal.
 */
inline glm::vec3 correct_mesh_normal_(int edge, const glm::vec3& face_normal, const glm::vec3& neighbor_normal,
                                       bool has_neighbor, const glm::vec3& raw_normal) {
    if (edge < 0) {
        return glm::dot(raw_normal, face_normal) > 0.0f ? face_normal : raw_normal;
    }
    if (!has_neighbor) return raw_normal;

    float coplanarity = glm::dot(face_normal, neighbor_normal);
    if (coplanarity > 0.999f) return face_normal;

    return glm::dot(raw_normal, face_normal) >= glm::dot(raw_normal, neighbor_normal) ? face_normal : neighbor_normal;
}

/**
 * @brief World transform of a TriangleMesh shape's local-space vertices (rotation, uniform
 *        mesh_scale, translation) at its owning body's pose -- the same matrix
 *        collision::world_bounds() fits the mesh's bounds with.
 */
inline glm::mat4 mesh_world_transform(const Shape& mesh_shape, const glm::vec3& mesh_pos, const glm::quat& mesh_rot) {
    glm::mat4 m(glm::mat3_cast(mesh_rot * mesh_shape.local_rotation) * mesh_shape.mesh_scale);
    m[3] = glm::vec4(mesh_pos + mesh_rot * mesh_shape.local_center, 1.0f);
    return m;
}

/**
 * @brief Maps a world-space AABB into a mesh shape's local (pre-scale) vertex space -- the
 *        frame its MeshBVH was built in. Exact-under-rotation refit (AABB::transform), so the
 *        result is conservative: it may admit a few extra triangles, never drop one.
 */
inline geometry::AABB mesh_local_bounds(const geometry::AABB& world_box, const glm::mat4& mesh_transform) {
    return world_box.transform(glm::inverse(mesh_transform));
}

/**
 * @brief Candidate triangles of `mesh_shape` that convex `shape` (at `pos`/`rot`) could produce
 *        a contact against, from the mesh BVH, sorted ascending so the per-triangle loop visits
 *        them in exactly the order a brute-force 0..N-1 scan would (the manifold keeps the first
 *        4 points and the last-touched triangle's normal, so order is part of the result).
 *
 * The probe's world AABB is grown for a Box by 1.5x its half-diagonal: box_vs_triangle_contacts_
 * accepts a triangle up to that half-diagonal away IN-PLANE from an embedded corner's
 * projection (its footprint test), plus however deep the corner has sunk -- so a triangle just
 * outside the box's own bounds can still contribute a contact. A ConvexHull gets the same
 * margin (its own half-diagonal), for the same reason via hull_vs_triangle_contacts_()'s
 * analogous footprint test. Sphere/Capsule contacts need real overlap, so their bounds are used
 * as-is.
 */
inline void mesh_contact_candidates(const Shape& shape, const glm::vec3& pos, const glm::quat& rot,
                                     const Shape& mesh_shape, const glm::mat4& mesh_transform,
                                     std::vector<uint32_t>& out) {
    out.clear();
    geometry::AABB probe = world_bounds(shape, pos, rot);
    if (shape.type == ShapeType::Box) probe = probe.expand(1.5f * glm::length(shape.half_extents));
    else if (shape.type == ShapeType::ConvexHull && shape.hull)
        probe = probe.expand(1.5f * 0.5f * glm::length(shape.hull->local_bounds.extents()) * shape.hull_scale);
    mesh_shape.mesh->query(mesh_local_bounds(probe, mesh_transform), [&](uint32_t tri) { out.push_back(tri); });
    std::sort(out.begin(), out.end());
}

/**
 * @brief Per-triangle contact generation shared by generate_mesh_contacts() and
 *        PhysicsWorld::compute_penetration(): runs the shape-appropriate test against triangle
 *        `tri` (see this file's doc), applies the internal-edge correction, and calls
 *        `emit(point, normal, penetration, feature_id)` per contact point -- `normal` pointing
 *        from the mesh's surface toward the convex shape, `point` midway into the overlap.
 *
 * @param mesh_transform mesh_world_transform() of `mesh_shape`.
 * @param mesh_world_rot The mesh's full world orientation (body rotation * local_rotation),
 *                       which rotates its stored face normals.
 */
template <typename Fn>
inline void mesh_triangle_contacts(const Shape& shape, const glm::vec3& pos, const glm::quat& rot,
                                    const geometry::TriangleMesh& mesh, const glm::mat4& mesh_transform,
                                    const glm::quat& mesh_world_rot, uint32_t tri, Fn&& emit) {
    glm::vec3 lv0, lv1, lv2;
    mesh.triangle_vertices(tri, lv0, lv1, lv2);
    glm::vec3 v0 = glm::vec3(mesh_transform * glm::vec4(lv0, 1.0f));
    glm::vec3 v1 = glm::vec3(mesh_transform * glm::vec4(lv1, 1.0f));
    glm::vec3 v2 = glm::vec3(mesh_transform * glm::vec4(lv2, 1.0f));
    glm::vec3 face_normal = glm::normalize(mesh_world_rot * mesh.triangle_normal(tri));
    const geometry::TriangleAdjacency adjacency = mesh.triangle_adjacency(tri);

    // Shared per-point path: classify where on this triangle the point landed, correct the
    // normal via adjacency, and emit. `raw_normal` is the fallback used only for a genuine
    // silhouette/crease (edge/vertex region with no coplanar neighbour); a box's corner-based
    // test always passes its own face_normal here (see box_vs_triangle_contacts_'s doc for why
    // the box case has no meaningful raw direction).
    auto emit_point = [&](const glm::vec3& on_tri, float penetration, const glm::vec3& raw_normal,
                          uint32_t feature_id) {
        glm::vec3 bary = barycentric_(on_tri, v0, v1, v2);
        int edge = classify_triangle_edge_(bary);

        glm::vec3 neighbor_normal(0.0f);
        bool has_neighbor = false;
        if (edge >= 0) {
            uint32_t neighbor_tri = adjacency.neighbor[edge];
            if (neighbor_tri != geometry::k_no_neighbor) {
                has_neighbor = true;
                neighbor_normal = glm::normalize(mesh_world_rot * mesh.triangle_normal(neighbor_tri));
            }
        }

        glm::vec3 normal = correct_mesh_normal_(edge, face_normal, neighbor_normal, has_neighbor, raw_normal);
        emit(on_tri + normal * (penetration * 0.5f), normal, penetration, feature_id);
    };

    if (shape.type == ShapeType::Box) {
        // A box's normal is always the face normal: the internal-edge correction, given the face
        // normal as the raw direction, returns it in every case. The low 3 bits of the feature id
        // name the box CORNER, so generate_mesh_contacts() can keep one contact per corner.
        box_vs_triangle_contacts_(shape, pos, rot, v0, v1, v2, face_normal,
                                   [&](uint32_t corner_index, const glm::vec3& corner, float penetration, float) {
                                       emit(corner + face_normal * (penetration * 0.5f), face_normal, penetration,
                                            (tri << 3) | (corner_index & 0x7u));
                                   });
    } else if (shape.type == ShapeType::ConvexHull) {
        // Same reasoning as the box branch above, generalized -- see hull_vs_triangle_contacts_'s
        // doc. The low 7 bits of the feature id name the hull VERTEX (max_vertices defaults to
        // 64, fits in 7 bits with room to spare for any realistic cap).
        hull_vs_triangle_contacts_(shape, pos, rot, v0, v1, v2, face_normal,
                                    [&](uint32_t vertex_index, const glm::vec3& vertex, float penetration, float) {
                                        emit(vertex + face_normal * (penetration * 0.5f), face_normal, penetration,
                                             (tri << 7) | (vertex_index & 0x7Fu));
                                    });
    } else {
        glm::vec3 on_shape, on_tri;
        float penetration = 0.0f;
        const bool touching =
            sphere_or_capsule_vs_triangle_contact_(shape, pos, rot, v0, v1, v2, on_shape, on_tri, penetration);
        // Which side of the face the shape's CORE is on. The distance the test above measures is
        // unsigned, so once the core crosses the surface the penetration it reports shrinks back
        // towards zero while the raw direction flips to point INTO the mesh -- a shape pushed
        // out of the ground downwards, and then nothing to report at all once it is a radius
        // past. Signed, the same two cases the box path has: pressed in, or embedded.
        const float core_side = glm::dot(on_shape - v0, face_normal);
        if (core_side < 0.0f) {
            // Embedded, and only worth a contact if the core is genuinely under THIS triangle --
            // not merely behind its extended plane, which a shape standing beside a step is.
            const glm::vec3 under = on_shape - core_side * face_normal;
            const glm::vec3 bary = barycentric_(under, v0, v1, v2);
            if (bary.x < -1e-4f || bary.y < -1e-4f || bary.z < -1e-4f) return;
            const float radius = shape.type == ShapeType::Sphere ? shape.radius : shape.capsule_radius;
            const float exit = radius - core_side;   // the whole way out, core_side <= 0
            emit(under + face_normal * (exit * 0.5f), face_normal, exit, tri);
            return;
        }
        if (!touching) return;
        glm::vec3 diff = on_shape - on_tri; // points from triangle toward shape
        float dist = glm::length(diff);
        glm::vec3 raw_normal = dist > util::k_epsilon ? diff / dist : face_normal;
        emit_point(on_tri, penetration, raw_normal, tri);
    }
}

/**
 * @brief generate_mesh_contacts() for a Box: the two feature pairs that support a box on a
 *        triangle mesh, reduced to a well-spread 4-point manifold.
 *
 * - **Box corners over a triangle**: a corner behind the triangle's plane whose projection
 *   lies inside the triangle. Exactly inside -- not within box_vs_triangle_contacts_()'s
 *   footprint -- because a corner within the footprint of a triangle it is NOT over is behind
 *   that triangle's EXTENDED plane: on a rounded lip or a step a metre away that is a phantom
 *   contact, which props a resting box up on an edge or kicks it sideways.
 * - **Triangle vertices inside the box**: what holds a box up on a triangle smaller than its
 *   face (a pillar top, a fine tessellation) when no corner is over any triangle. Depth: how far
 *   the box reaches behind the triangle's plane.
 *
 * A triangle whose plane the box's centre is behind contributes neither (see
 * box_vs_triangle_contacts_()). Each corner keeps its least-penetrating triangle and each mesh
 * vertex its first; the manifold is then the deepest point, the point farthest from it, the one
 * farthest from that line and the one farthest from all three -- the usual spread reduction,
 * in candidate order so it is deterministic -- under the penetration-weighted mean normal.
 * Feature ids: a corner is its index (0-7), a vertex 8 + its first triangle * 3 + slot.
 *
 * - **Recovery**, and only when neither feature pair found anything at all: a box whose CENTRE
 *   has ended up behind a face it is directly under is embedded in the mesh, and both tests
 *   above are blind to it -- a corner behind the plane needs the centre in front to count, and
 *   a mesh vertex inside the box needs the triangle to have been tested at all. Without this it
 *   stays embedded for ever: nothing in the step after it reports a contact either, so a body
 *   that tunnelled once (one fast frame, a spawn inside geometry, a joint pulling a bone
 *   through) simply keeps sinking. One contact is emitted, pushing out along the face normal of
 *   whichever containing triangle is the SHORTEST way out, by the full distance the box reaches
 *   behind that face -- the solver's per-iteration correction cap (PhysicsConfig::
 *   max_linear_correction) is what keeps that from being a pop. Containment is the centre's
 *   projection landing INSIDE the triangle, not merely behind its extended plane, so a box
 *   resting in front of a step is never pushed out through the step. Feature id: the triangle,
 *   with the top bit set, so it never aliases a corner's or a vertex's.
 */
inline bool generate_box_mesh_contacts_(const Shape& shape, const glm::vec3& pos, const glm::quat& rot,
                                        const geometry::TriangleMesh& mesh, const glm::mat4& mesh_transform,
                                        const glm::quat& mesh_world_rot, ContactManifold& out, bool use_bvh,
                                        const Shape& mesh_shape) {
    struct Candidate {
        glm::vec3 point{0.0f}, normal{0.0f}, key{0.0f};
        float     penetration = 0.0f;
        uint32_t  feature_id = 0;
    };
    const geometry::OBB box = world_obb(shape, pos, rot);
    glm::vec3 ax, ay, az;
    box.axes(ax, ay, az);
    const glm::vec3& he = box.half_extents;
    glm::vec3 corner[8];
    for (uint32_t c = 0; c < 8; ++c) {
        corner[c] = box.center + ((c & 1u) ? he.x : -he.x) * ax + ((c & 2u) ? he.y : -he.y) * ay +
                    ((c & 4u) ? he.z : -he.z) * az;
    }
    const float inside_eps = 1e-4f * std::max(1.0f, glm::length(he));

    Candidate corners[8];
    bool corner_valid[8] = {};
    std::vector<Candidate> vertices;   // small: the vertices of the few triangles under a box
    Candidate recovery;                // the shortest way out for an embedded box; see the doc
    bool recovery_valid = false;

    auto test = [&](uint32_t tri) {
        glm::vec3 lv[3];
        mesh.triangle_vertices(tri, lv[0], lv[1], lv[2]);
        glm::vec3 v[3];
        for (int k = 0; k < 3; ++k) v[k] = glm::vec3(mesh_transform * glm::vec4(lv[k], 1.0f));
        const glm::vec3 n = glm::normalize(mesh_world_rot * mesh.triangle_normal(tri));
        const float center_dist = glm::dot(box.center - v[0], n);
        if (center_dist <= 0.0f) {
            // Behind the face. Embedded if the centre is under this triangle itself, which is
            // the one case worth a contact -- see this function's doc.
            const glm::vec3 under = box.center - center_dist * n;
            const glm::vec3 bary = barycentric_(under, v[0], v[1], v[2]);
            if (bary.x < -1e-4f || bary.y < -1e-4f || bary.z < -1e-4f) return;
            const float support = std::abs(glm::dot(he.x * ax, n)) + std::abs(glm::dot(he.y * ay, n)) +
                                  std::abs(glm::dot(he.z * az, n));
            const float exit = support - center_dist;
            if (recovery_valid && recovery.penetration <= exit) return;
            recovery_valid = true;
            recovery = Candidate{under, n, under, exit, 0x80000000u | tri};
            return;
        }

        float depth = 0.0f;   // how far the box reaches behind this triangle's plane
        float dist[8];
        for (uint32_t c = 0; c < 8; ++c) {
            dist[c] = glm::dot(corner[c] - v[0], n);
            depth = std::max(depth, -dist[c]);
        }
        if (depth <= 0.0f) return;

        // Corners over the triangle.
        for (uint32_t c = 0; c < 8; ++c) {
            if (dist[c] >= 0.0f) continue;
            const glm::vec3 plane_point = corner[c] - dist[c] * n;
            const glm::vec3 bary = barycentric_(plane_point, v[0], v[1], v[2]);
            if (bary.x < -1e-4f || bary.y < -1e-4f || bary.z < -1e-4f) continue;
            const float pen = -dist[c];
            if (corner_valid[c] && corners[c].penetration <= pen) continue;
            corner_valid[c] = true;
            corners[c] = Candidate{corner[c] + n * (pen * 0.5f), n, corner[c], pen, c};
        }
        // Triangle vertices inside the box.
        for (int k = 0; k < 3; ++k) {
            const glm::vec3 d = v[k] - box.center;
            if (std::abs(glm::dot(d, ax)) > he.x + inside_eps || std::abs(glm::dot(d, ay)) > he.y + inside_eps ||
                std::abs(glm::dot(d, az)) > he.z + inside_eps) continue;
            bool seen = false;
            for (const Candidate& c : vertices) seen = seen || glm::length(c.key - v[k]) < inside_eps;
            if (seen) continue;
            vertices.push_back(Candidate{v[k] - n * (depth * 0.5f), n, v[k], depth, 8u + tri * 3u + static_cast<uint32_t>(k)});
        }
    };
    if (use_bvh) {
        thread_local std::vector<uint32_t> candidates;
        mesh_contact_candidates(shape, pos, rot, mesh_shape, mesh_transform, candidates);
        for (uint32_t tri : candidates) test(tri);
    } else {
        for (size_t tri = 0; tri < mesh.triangle_count(); ++tri) test(static_cast<uint32_t>(tri));
    }

    std::vector<Candidate> all;
    for (uint32_t c = 0; c < 8; ++c) if (corner_valid[c]) all.push_back(corners[c]);
    all.insert(all.end(), vertices.begin(), vertices.end());
    if (all.empty()) {
        if (!recovery_valid) return false;
        out.add_point(recovery.point, recovery.penetration, recovery.feature_id);
        out.normal = recovery.normal;
        return true;
    }

    // Reduce to four well-spread points.
    std::size_t pick[4];
    std::size_t picked = 0;
    auto taken = [&](std::size_t i) { return std::find(pick, pick + picked, i) != pick + picked; };
    auto best = [&](auto&& score) {
        std::size_t arg = all.size();
        float top = -1.0f;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (taken(i)) continue;
            const float sc = score(all[i]);
            if (sc > top + 1e-6f) { top = sc; arg = i; }
        }
        if (arg < all.size() && top > 1e-6f) pick[picked++] = arg;
    };
    {
        std::size_t deepest = 0;
        for (std::size_t i = 1; i < all.size(); ++i) {
            if (all[i].penetration > all[deepest].penetration + 1e-6f) deepest = i;
        }
        pick[picked++] = deepest;
    }
    const glm::vec3 p0 = all[pick[0]].point;
    best([&](const Candidate& c) { return glm::length(c.point - p0); });
    if (picked == 2) {
        const glm::vec3 p1 = all[pick[1]].point;
        best([&](const Candidate& c) { return glm::length(glm::cross(p1 - p0, c.point - p0)); });
    }
    if (picked == 3) {
        const glm::vec3 p1 = all[pick[1]].point, p2 = all[pick[2]].point;
        best([&](const Candidate& c) {
            return std::min({glm::length(c.point - p0), glm::length(c.point - p1), glm::length(c.point - p2)});
        });
    }
    std::sort(pick, pick + picked);

    glm::vec3 normal(0.0f);
    for (std::size_t i = 0; i < picked; ++i) {
        const Candidate& c = all[pick[i]];
        out.add_point(c.point, c.penetration, c.feature_id);
        normal += c.normal * std::max(c.penetration, 1e-4f);
    }
    const float len = glm::length(normal);
    out.normal = len > 1e-6f ? normal / len : all[pick[0]].normal;
    return true;
}

/**
 * @brief generate_mesh_contacts() for a ConvexHull: the direct generalization of
 *        generate_box_mesh_contacts_() from a box's fixed 8 corners to a hull's variable-count
 *        vertex array -- same two feature pairs, same spread reduction, same embedded-recovery
 *        fallback, just sized to `hull.vertices.size()` instead of 8.
 *
 * - **Hull vertices over a triangle**: a hull vertex behind the triangle's plane whose
 *   projection lies inside the triangle. Generalizes box_vs_triangle_contacts_()'s corner test.
 * - **Triangle vertices inside the hull**: generalizes the box's 3-axis half-extent clamp to
 *   checking all of the hull's face-plane inequalities at once -- a point is inside a convex
 *   hull exactly when it is on the inward side of every face.
 * - **Recovery**: the hull's centre (not a geometric average of vertices -- the solid's own
 *   centroid, which build_convex_hull() already computes) behind a face it is directly under
 *   pushes out along that face's normal by the hull's own support width, the same `max_v
 *   dot(v - center, n)` quantity that reduces to a box's `sum|he_i * dot(axis_i, n)|` when the
 *   vertex set is a box's 8 corners.
 *
 * Feature ids stay disjoint from the box scheme (this is always a different Shape::type, so
 * never confused within one call, but kept distinct on principle): a hull vertex keeps its own
 * index (0..max_vertices-1); a triangle vertex is `hull.vertices.size() + tri * 3 + slot`; the
 * recovery contact is `0x80000000 | tri`, same convention as the box path.
 */
inline bool generate_hull_mesh_contacts_(const Shape& shape, const glm::vec3& pos, const glm::quat& rot,
                                         const geometry::TriangleMesh& mesh, const glm::mat4& mesh_transform,
                                         const glm::quat& mesh_world_rot, ContactManifold& out, bool use_bvh,
                                         const Shape& mesh_shape) {
    struct Candidate {
        glm::vec3 point{0.0f}, normal{0.0f}, key{0.0f};
        float     penetration = 0.0f;
        uint32_t  feature_id = 0;
    };
    if (!shape.hull) return false;
    const PosedHull posed = world_posed_hull(shape, pos, rot);
    const geometry::ConvexHull& hull = *posed.hull;
    std::vector<glm::vec3> verts = detail::hull_world_vertices_(posed);
    const glm::vec3 hull_center = posed.position + posed.rotation * (hull.centroid * posed.scale);
    const float inside_eps = 1e-4f * std::max(1.0f, glm::length(hull.local_bounds.extents()) * posed.scale);

    std::vector<Candidate> corners(verts.size());
    std::vector<bool> corner_valid(verts.size(), false);
    std::vector<Candidate> vertices;   // small: the vertices of the few triangles under the hull
    Candidate recovery;                // the shortest way out for an embedded hull; see the doc
    bool recovery_valid = false;

    auto test = [&](uint32_t tri) {
        glm::vec3 lv[3];
        mesh.triangle_vertices(tri, lv[0], lv[1], lv[2]);
        glm::vec3 v[3];
        for (int k = 0; k < 3; ++k) v[k] = glm::vec3(mesh_transform * glm::vec4(lv[k], 1.0f));
        const glm::vec3 n = glm::normalize(mesh_world_rot * mesh.triangle_normal(tri));
        const float center_dist = glm::dot(hull_center - v[0], n);
        if (center_dist <= 0.0f) {
            // Behind the face. Embedded if the centre is under this triangle itself, same test
            // as the box path's recovery branch.
            const glm::vec3 under = hull_center - center_dist * n;
            const glm::vec3 bary = barycentric_(under, v[0], v[1], v[2]);
            if (bary.x < -1e-4f || bary.y < -1e-4f || bary.z < -1e-4f) return;
            float support = 0.0f;
            for (const glm::vec3& hv : verts) support = std::max(support, glm::dot(hv - hull_center, n));
            const float exit = support - center_dist;
            if (recovery_valid && recovery.penetration <= exit) return;
            recovery_valid = true;
            recovery = Candidate{under, n, under, exit, 0x80000000u | tri};
            return;
        }

        float depth = 0.0f;   // how far the hull reaches behind this triangle's plane
        thread_local std::vector<float> dist;
        dist.resize(verts.size());
        for (std::size_t c = 0; c < verts.size(); ++c) {
            dist[c] = glm::dot(verts[c] - v[0], n);
            depth = std::max(depth, -dist[c]);
        }
        if (depth <= 0.0f) return;

        // Hull vertices over the triangle.
        for (std::size_t c = 0; c < verts.size(); ++c) {
            if (dist[c] >= 0.0f) continue;
            const glm::vec3 plane_point = verts[c] - dist[c] * n;
            const glm::vec3 bary = barycentric_(plane_point, v[0], v[1], v[2]);
            if (bary.x < -1e-4f || bary.y < -1e-4f || bary.z < -1e-4f) continue;
            const float pen = -dist[c];
            if (corner_valid[c] && corners[c].penetration <= pen) continue;
            corner_valid[c] = true;
            corners[c] = Candidate{verts[c] + n * (pen * 0.5f), n, verts[c], pen, uint32_t(c)};
        }
        // Triangle vertices inside the hull: a point is inside a convex hull exactly when it is
        // on the inward side of every one of its face planes.
        for (int k = 0; k < 3; ++k) {
            bool inside = true;
            for (std::size_t f = 0; f < hull.face_count() && inside; ++f) {
                const glm::vec3 fn = posed.world_face_normal(f);
                const float fd = glm::dot(fn, posed.world_vertex(hull.face_vertices[hull.face_starts[f]]));
                if (glm::dot(fn, v[k]) - fd > inside_eps) inside = false;
            }
            if (!inside) continue;
            bool seen = false;
            for (const Candidate& c : vertices) seen = seen || glm::length(c.key - v[k]) < inside_eps;
            if (seen) continue;
            vertices.push_back(Candidate{v[k] - n * (depth * 0.5f), n, v[k],
                                         depth, uint32_t(verts.size()) + tri * 3u + uint32_t(k)});
        }
    };
    if (use_bvh) {
        thread_local std::vector<uint32_t> candidates;
        mesh_contact_candidates(shape, pos, rot, mesh_shape, mesh_transform, candidates);
        for (uint32_t tri : candidates) test(tri);
    } else {
        for (size_t tri = 0; tri < mesh.triangle_count(); ++tri) test(static_cast<uint32_t>(tri));
    }

    std::vector<Candidate> all;
    for (std::size_t c = 0; c < verts.size(); ++c) if (corner_valid[c]) all.push_back(corners[c]);
    all.insert(all.end(), vertices.begin(), vertices.end());
    if (all.empty()) {
        if (!recovery_valid) return false;
        out.add_point(recovery.point, recovery.penetration, recovery.feature_id);
        out.normal = recovery.normal;
        return true;
    }

    // Reduce to four well-spread points -- identical procedure to generate_box_mesh_contacts_.
    std::size_t pick[4];
    std::size_t picked = 0;
    auto taken = [&](std::size_t i) { return std::find(pick, pick + picked, i) != pick + picked; };
    auto best = [&](auto&& score) {
        std::size_t arg = all.size();
        float top = -1.0f;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (taken(i)) continue;
            const float sc = score(all[i]);
            if (sc > top + 1e-6f) { top = sc; arg = i; }
        }
        if (arg < all.size() && top > 1e-6f) pick[picked++] = arg;
    };
    {
        std::size_t deepest = 0;
        for (std::size_t i = 1; i < all.size(); ++i) {
            if (all[i].penetration > all[deepest].penetration + 1e-6f) deepest = i;
        }
        pick[picked++] = deepest;
    }
    const glm::vec3 p0 = all[pick[0]].point;
    best([&](const Candidate& c) { return glm::length(c.point - p0); });
    if (picked == 2) {
        const glm::vec3 p1 = all[pick[1]].point;
        best([&](const Candidate& c) { return glm::length(glm::cross(p1 - p0, c.point - p0)); });
    }
    if (picked == 3) {
        const glm::vec3 p1 = all[pick[1]].point, p2 = all[pick[2]].point;
        best([&](const Candidate& c) {
            return std::min({glm::length(c.point - p0), glm::length(c.point - p1), glm::length(c.point - p2)});
        });
    }
    std::sort(pick, pick + picked);

    glm::vec3 normal(0.0f);
    for (std::size_t i = 0; i < picked; ++i) {
        const Candidate& c = all[pick[i]];
        out.add_point(c.point, c.penetration, c.feature_id);
        normal += c.normal * std::max(c.penetration, 1e-4f);
    }
    const float len = glm::length(normal);
    out.normal = len > 1e-6f ? normal / len : all[pick[0]].normal;
    return true;
}

/**
 * @brief Generates a manifold between a convex Shape and a static TriangleMesh shape.
 *
 * @param shape       The convex (Sphere/Box/Capsule/ConvexHull) shape.
 * @param pos, rot    That shape's world pose.
 * @param mesh_shape  The TriangleMesh shape (its own pose is `mesh_pos`/`mesh_rot`).
 * @param out         Output manifold, normal pointing from the mesh's surface toward the
 *                     convex shape -- caller (generate_contacts) applies the final A->B sign.
 * @param use_bvh     True (the default) narrows the triangles tested to the mesh BVH's
 *                     candidates for the shape's bounds (mesh_contact_candidates()); false
 *                     scans every triangle -- the brute-force reference, kept for tests.
 *                     Both visit triangles in ascending index order, so they agree exactly.
 * @return True if any triangle produced a contact point.
 */
inline bool generate_mesh_contacts(const Shape& shape, const glm::vec3& pos, const glm::quat& rot,
                                    const Shape& mesh_shape, const glm::vec3& mesh_pos, const glm::quat& mesh_rot,
                                    ContactManifold& out, bool use_bvh = true) {
    out = ContactManifold{};
    if (!mesh_shape.mesh) return false;
    const geometry::TriangleMesh& mesh = *mesh_shape.mesh;

    const glm::mat4 mesh_transform = mesh_world_transform(mesh_shape, mesh_pos, mesh_rot);
    const glm::quat mesh_world_rot = mesh_rot * mesh_shape.local_rotation;

    if (shape.type == ShapeType::Box) {
        return generate_box_mesh_contacts_(shape, pos, rot, mesh, mesh_transform, mesh_world_rot, out, use_bvh, mesh_shape);
    }
    if (shape.type == ShapeType::ConvexHull) {
        return generate_hull_mesh_contacts_(shape, pos, rot, mesh, mesh_transform, mesh_world_rot, out, use_bvh, mesh_shape);
    }

    bool any_hit = false;
    glm::vec3 best_normal(0.0f, 0.0f, 1.0f);
    auto emit = [&](const glm::vec3& point, const glm::vec3& normal, float penetration, uint32_t feature_id) {
        if (out.count >= 4) return;
        out.add_point(point, penetration, feature_id);
        best_normal = normal;
        any_hit = true;
    };

    if (use_bvh) {
        // Reused per thread: narrowphase runs this job-parallel, one pair per call, never nested.
        thread_local std::vector<uint32_t> candidates;
        mesh_contact_candidates(shape, pos, rot, mesh_shape, mesh_transform, candidates);
        for (uint32_t tri : candidates) {
            if (out.count >= 4) break;
            mesh_triangle_contacts(shape, pos, rot, mesh, mesh_transform, mesh_world_rot, tri, emit);
        }
    } else {
        for (size_t tri = 0; tri < mesh.triangle_count(); ++tri) {
            if (out.count >= 4) break;
            mesh_triangle_contacts(shape, pos, rot, mesh, mesh_transform, mesh_world_rot, static_cast<uint32_t>(tri), emit);
        }
    }

    if (!any_hit) return false;
    out.normal = best_normal;
    return true;
}

} // namespace collision
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COLLISION_MESH_CONTACT_H

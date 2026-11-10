/**
 * @file convex_hull.h
 * @brief A convex polyhedron (merged polygon faces, unique edges, volume/centroid) plus a
 *        deterministic builder from a point cloud.
 *
 * No GJK, no EPA, no randomized convergence anywhere in this file or in anything that consumes
 * it (collision/sat_hull.h). `libs/physxcoopa/plans/2026-08-17_physxcoopa-realtime-physics.md`
 * excludes GJK/EPA from v1 because EPA produces no stable feature id (its output face depends
 * on polytope-expansion order) and because a randomized-convergence test is a CI flake risk --
 * neither objection applies to a SAT narrowphase over a deterministically-built hull, which is
 * what this type exists for. The builder below is a plain incremental hull construction (add
 * the farthest outside point, replace the faces it sees, stitch the horizon) with a fixed
 * tie-break (farthest point wins; ties break toward the lower vertex index) -- the same point
 * cloud always produces the same hull, vertex for vertex.
 */

#ifndef PHYSXCOOPA_GEOMETRY_CONVEX_HULL_H
#define PHYSXCOOPA_GEOMETRY_CONVEX_HULL_H

#include <physxcoopa/geometry/aabb.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace coopa {
namespace physx {
namespace geometry {

/**
 * @struct ConvexHull
 * @brief A convex polyhedron: vertices, merged polygon faces with their planes, and the unique
 *        edge directions SAT needs. Built once (build_convex_hull()), read many times.
 */
struct ConvexHull {
    std::vector<glm::vec3> vertices;
    /** @brief xyz = outward unit normal, w = plane offset d (dot(n, p) == d for p on the face). */
    std::vector<glm::vec4> face_planes;
    /** @brief Face i spans [face_starts[i], face_starts[i + 1]) of face_vertices. Size is
     *         face_count() + 1. */
    std::vector<std::uint32_t> face_starts;
    /** @brief Vertex indices of every face, CCW about that face's outward normal. */
    std::vector<std::uint32_t> face_vertices;
    /** @brief Unique undirected vertex-index pairs, one per physical edge -- what SAT's
     *         edge-edge axes cross. */
    std::vector<glm::uvec2> edges;
    glm::vec3 centroid{0.0f};
    float volume = 0.0f;
    AABB local_bounds;

    std::size_t face_count() const { return face_planes.size(); }

    /** @brief The farthest vertex along `dir` -- the support function SAT and sweeps both
     *         need. Brute force over `vertices`, which the builder's `max_vertices` cap keeps
     *         small (64 by default). */
    glm::vec3 support(const glm::vec3& dir) const {
        glm::vec3 best = vertices.empty() ? glm::vec3(0.0f) : vertices[0];
        float best_dot = vertices.empty() ? 0.0f : glm::dot(best, dir);
        for (std::size_t i = 1; i < vertices.size(); ++i) {
            float d = glm::dot(vertices[i], dir);
            if (d > best_dot) {
                best_dot = d;
                best = vertices[i];
            }
        }
        return best;
    }

    /** @brief World-space AABB of this hull posed at `pos`/`rot` with uniform `scale`, by
     *         transforming `local_bounds`' 8 corners -- the same exact-under-rotation technique
     *         AABB::transform()/OBB::bounds() already use, so a hull shape's broadphase bound
     *         is computed the same way every other shape's is. */
    AABB bounds(const glm::vec3& pos, const glm::quat& rot, float scale) const {
        glm::mat4 m(glm::mat3_cast(rot) * scale);
        m[3] = glm::vec4(pos, 1.0f);
        return local_bounds.transform(m);
    }
};

// --- Builder internals ---

namespace detail {

/** @brief One triangular face during incremental construction: vertex indices CCW about the
 *         outward normal, plus the cached plane. */
struct HullFace_ {
    std::uint32_t a = 0, b = 0, c = 0;
    glm::vec3 normal{0.0f};
    float d = 0.0f;
};

inline void compute_face_plane_(HullFace_& f, const std::vector<glm::vec3>& pts) {
    glm::vec3 n = glm::cross(pts[f.b] - pts[f.a], pts[f.c] - pts[f.a]);
    float len = glm::length(n);
    f.normal = len > 1e-12f ? n / len : glm::vec3(0.0f, 0.0f, 1.0f);
    f.d = glm::dot(f.normal, pts[f.a]);
}

/** @brief Deterministic farthest-point reduction: keeps `target` of `points`, starting from the
 *         point farthest from the centroid, then greedily adding whichever remaining point is
 *         farthest from everything already kept. Ties break toward the lower source index. */
inline std::vector<glm::vec3> farthest_point_subset_(const glm::vec3* points, std::size_t count,
                                                       std::size_t target) {
    std::vector<glm::vec3> kept;
    kept.reserve(target);
    glm::vec3 centroid(0.0f);
    for (std::size_t i = 0; i < count; ++i) centroid += points[i];
    centroid /= float(count);

    std::vector<bool> used(count, false);
    std::size_t first = 0;
    float best = -1.0f;
    for (std::size_t i = 0; i < count; ++i) {
        float d2 = glm::dot(points[i] - centroid, points[i] - centroid);
        if (d2 > best) {
            best = d2;
            first = i;
        }
    }
    kept.push_back(points[first]);
    used[first] = true;

    std::vector<float> min_dist2(count, std::numeric_limits<float>::max());
    while (kept.size() < target) {
        std::size_t pick = count;
        float pick_dist = -1.0f;
        for (std::size_t i = 0; i < count; ++i) {
            if (used[i]) continue;
            glm::vec3 diff = points[i] - kept.back();
            min_dist2[i] = std::min(min_dist2[i], glm::dot(diff, diff));
            if (min_dist2[i] > pick_dist) {
                pick_dist = min_dist2[i];
                pick = i;
            }
        }
        if (pick == count) break; // fewer distinct points than target
        kept.push_back(points[pick]);
        used[pick] = true;
    }
    return kept;
}

/** @brief 2D convex hull (Andrew's monotone chain) of points already projected into a face's own
 *         plane, used to merge coplanar triangles into one polygon. Returns indices into `pts`
 *         in CCW order. Deterministic: lexicographic sort, no ties left to hash order. */
inline std::vector<std::uint32_t> convex_hull_2d_(const std::vector<glm::vec2>& pts) {
    std::vector<std::uint32_t> order(pts.size());
    for (std::uint32_t i = 0; i < pts.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        if (pts[a].x != pts[b].x) return pts[a].x < pts[b].x;
        return pts[a].y < pts[b].y;
    });
    order.erase(std::unique(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
                    return glm::dot(pts[a] - pts[b], pts[a] - pts[b]) < 1e-14f;
                }),
                order.end());
    if (order.size() < 3) return order;

    auto cross2 = [&](std::uint32_t o, std::uint32_t a, std::uint32_t b) {
        glm::vec2 oa = pts[a] - pts[o], ob = pts[b] - pts[o];
        return oa.x * ob.y - oa.y * ob.x;
    };

    std::vector<std::uint32_t> lower;
    for (std::uint32_t idx : order) {
        while (lower.size() >= 2 && cross2(lower[lower.size() - 2], lower.back(), idx) <= 0.0f)
            lower.pop_back();
        lower.push_back(idx);
    }
    std::vector<std::uint32_t> upper;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        std::uint32_t idx = *it;
        while (upper.size() >= 2 && cross2(upper[upper.size() - 2], upper.back(), idx) <= 0.0f)
            upper.pop_back();
        upper.push_back(idx);
    }
    lower.pop_back();
    upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return lower;
}

} // namespace detail

/**
 * @brief Builds the convex hull of a point cloud. Deterministic: no RNG, no random restarts.
 *
 * Construction is incremental: seed a tetrahedron from the extreme points, then repeatedly find
 * the point farthest outside any current face, delete every face it can see, and stitch the
 * horizon with new faces to that point. Coplanar triangles are merged into real polygon faces
 * afterward -- without this a hull of a cube would keep ~12 triangular faces instead of 6, and
 * SAT over near-duplicate face normals is exactly the near-tie flicker sat_test_obb_obb() already
 * dampens for boxes.
 *
 * @param tolerance Planarity/merge epsilon in metres; points within it of an existing face are
 *                  not treated as outside it, and faces whose normals agree within it (and whose
 *                  shared edge is convex) are merged.
 * @param max_vertices Hard cap (default 64). Over it, a deterministic farthest-point subset of
 *                     the INPUT is kept before building, which keeps the hull an approximation
 *                     shaped like a superset of the true hull rather than an arbitrary subset.
 * @return False for a degenerate cloud -- fewer than 4 points, all collinear, or all coplanar (a
 *         flat cut face, a cloth, a plane). Callers must have a fallback; a sliced sliver really
 *         can come out coplanar.
 */
inline bool build_convex_hull(const glm::vec3* points, std::size_t count, ConvexHull& out,
                               float tolerance = 1e-5f, std::size_t max_vertices = 64) {
    using detail::HullFace_;
    out = ConvexHull{};
    if (count < 4) return false;

    std::vector<glm::vec3> cloud;
    if (count > max_vertices) {
        cloud = detail::farthest_point_subset_(points, count, max_vertices);
    } else {
        cloud.assign(points, points + count);
    }
    if (cloud.size() < 4) return false;

    // Seed tetrahedron: min-x point, farthest from it, farthest from that line, farthest from
    // that plane -- a fixed, deterministic sequence of extremal picks.
    std::size_t p0 = 0;
    for (std::size_t i = 1; i < cloud.size(); ++i) {
        if (cloud[i].x < cloud[p0].x ||
            (cloud[i].x == cloud[p0].x && (cloud[i].y < cloud[p0].y ||
                                           (cloud[i].y == cloud[p0].y && cloud[i].z < cloud[p0].z))))
            p0 = i;
    }
    std::size_t p1 = p0;
    float best1 = -1.0f;
    for (std::size_t i = 0; i < cloud.size(); ++i) {
        float d2 = glm::dot(cloud[i] - cloud[p0], cloud[i] - cloud[p0]);
        if (d2 > best1) {
            best1 = d2;
            p1 = i;
        }
    }
    if (best1 < tolerance * tolerance) return false; // every point coincident

    std::size_t p2 = p0;
    float best2 = -1.0f;
    glm::vec3 line_dir = glm::normalize(cloud[p1] - cloud[p0]);
    for (std::size_t i = 0; i < cloud.size(); ++i) {
        glm::vec3 to_pt = cloud[i] - cloud[p0];
        glm::vec3 perp = to_pt - line_dir * glm::dot(to_pt, line_dir);
        float d2 = glm::dot(perp, perp);
        if (d2 > best2) {
            best2 = d2;
            p2 = i;
        }
    }
    if (best2 < tolerance * tolerance) return false; // collinear

    glm::vec3 seed_normal = glm::normalize(glm::cross(cloud[p1] - cloud[p0], cloud[p2] - cloud[p0]));
    float seed_d = glm::dot(seed_normal, cloud[p0]);
    std::size_t p3 = p0;
    float best3 = 0.0f;
    for (std::size_t i = 0; i < cloud.size(); ++i) {
        float dist = std::abs(glm::dot(seed_normal, cloud[i]) - seed_d);
        if (dist > best3) {
            best3 = dist;
            p3 = i;
        }
    }
    if (best3 < tolerance) return false; // coplanar

    // Four faces of the seed tetrahedron, each wound CCW as seen from outside (the 4th point of
    // the tetrahedron lies behind every face).
    std::uint32_t ia = std::uint32_t(p0), ib = std::uint32_t(p1), ic = std::uint32_t(p2), id = std::uint32_t(p3);
    auto make_face = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t opposite) {
        HullFace_ f{a, b, c};
        detail::compute_face_plane_(f, cloud);
        if (glm::dot(f.normal, cloud[opposite]) - f.d > 0.0f) std::swap(f.b, f.c);
        detail::compute_face_plane_(f, cloud);
        return f;
    };
    std::vector<HullFace_> faces;
    faces.push_back(make_face(ia, ib, ic, id));
    faces.push_back(make_face(ia, ib, id, ic));
    faces.push_back(make_face(ib, ic, id, ia));
    faces.push_back(make_face(ia, ic, id, ib));

    std::vector<bool> used(cloud.size(), false);
    used[ia] = used[ib] = used[ic] = used[id] = true;

    // Incremental insertion: find the point farthest outside any face, remove every face it can
    // see, stitch the horizon with new faces to that point. O(points * faces) -- fine, since
    // max_vertices keeps both small (<=64 by default).
    for (;;) {
        std::size_t best_point = cloud.size();
        float best_dist = tolerance;
        for (std::size_t i = 0; i < cloud.size(); ++i) {
            if (used[i]) continue;
            float furthest = 0.0f;
            for (const HullFace_& f : faces) furthest = std::max(furthest, glm::dot(f.normal, cloud[i]) - f.d);
            if (furthest > best_dist) {
                best_dist = furthest;
                best_point = i;
            }
        }
        if (best_point == cloud.size()) break;

        std::uint32_t pt = std::uint32_t(best_point);
        std::vector<bool> visible(faces.size(), false);
        for (std::size_t i = 0; i < faces.size(); ++i)
            visible[i] = glm::dot(faces[i].normal, cloud[pt]) - faces[i].d > tolerance;

        // Directed-edge ownership among the visible faces, to find horizon edges: a directed
        // edge (a->b) owned by a visible face is a horizon edge unless its twin (b->a) is also
        // owned by a visible face.
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> owner;
        for (std::size_t i = 0; i < faces.size(); ++i) {
            if (!visible[i]) continue;
            const HullFace_& f = faces[i];
            owner[{f.a, f.b}] = i;
            owner[{f.b, f.c}] = i;
            owner[{f.c, f.a}] = i;
        }
        std::vector<std::pair<std::uint32_t, std::uint32_t>> horizon;
        for (const auto& kv : owner) {
            std::pair<std::uint32_t, std::uint32_t> twin{kv.first.second, kv.first.first};
            if (owner.find(twin) == owner.end()) horizon.push_back(kv.first);
        }

        std::vector<HullFace_> next_faces;
        next_faces.reserve(faces.size());
        for (std::size_t i = 0; i < faces.size(); ++i)
            if (!visible[i]) next_faces.push_back(faces[i]);
        for (const auto& edge : horizon) {
            HullFace_ f{edge.first, edge.second, pt};
            detail::compute_face_plane_(f, cloud);
            next_faces.push_back(f);
        }
        faces = std::move(next_faces);
        used[pt] = true;
    }

    // --- Merge coplanar faces into real polygons ---
    std::vector<bool> merged(faces.size(), false);
    std::vector<std::vector<std::uint32_t>> polygons; // vertex indices, CCW, per merged face
    std::vector<glm::vec3> polygon_normals;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        if (merged[i]) continue;
        std::vector<std::size_t> group{i};
        merged[i] = true;
        for (std::size_t j = i + 1; j < faces.size(); ++j) {
            if (merged[j]) continue;
            if (glm::dot(faces[i].normal, faces[j].normal) > 1.0f - tolerance &&
                std::abs(faces[i].d - faces[j].d) < std::max(tolerance, 1.0f) * tolerance * 100.0f) {
                group.push_back(j);
                merged[j] = true;
            }
        }
        glm::vec3 n = faces[i].normal;
        float d = faces[i].d;
        // Collect the group's unique vertices and project them into the face's own 2D plane.
        std::vector<std::uint32_t> verts;
        for (std::size_t g : group) {
            const HullFace_& f = faces[g];
            for (std::uint32_t v : {f.a, f.b, f.c})
                if (std::find(verts.begin(), verts.end(), v) == verts.end()) verts.push_back(v);
        }
        glm::vec3 u = std::abs(n.x) < 0.9f ? glm::normalize(glm::cross(n, glm::vec3(1, 0, 0)))
                                            : glm::normalize(glm::cross(n, glm::vec3(0, 1, 0)));
        glm::vec3 v = glm::cross(n, u);
        glm::vec3 origin = n * d;
        std::vector<glm::vec2> proj(verts.size());
        for (std::size_t k = 0; k < verts.size(); ++k) {
            glm::vec3 rel = cloud[verts[k]] - origin;
            proj[k] = glm::vec2(glm::dot(rel, u), glm::dot(rel, v));
        }
        std::vector<std::uint32_t> hull2d = detail::convex_hull_2d_(proj);
        if (hull2d.size() < 3) continue; // degenerate sliver face; drop it
        std::vector<std::uint32_t> poly(hull2d.size());
        for (std::size_t k = 0; k < hull2d.size(); ++k) poly[k] = verts[hull2d[k]];
        polygons.push_back(std::move(poly));
        polygon_normals.push_back(n);
    }

    // --- Flatten into the output's compact arrays, and collect the hull's own vertex set ---
    std::vector<std::uint32_t> remap(cloud.size(), UINT32_MAX);
    for (const auto& poly : polygons)
        for (std::uint32_t v : poly)
            if (remap[v] == UINT32_MAX) {
                remap[v] = std::uint32_t(out.vertices.size());
                out.vertices.push_back(cloud[v]);
            }

    out.face_starts.push_back(0);
    std::map<std::pair<std::uint32_t, std::uint32_t>, bool> edge_seen;
    for (std::size_t i = 0; i < polygons.size(); ++i) {
        const auto& poly = polygons[i];
        float d = glm::dot(polygon_normals[i], cloud[poly[0]]);
        out.face_planes.push_back(glm::vec4(polygon_normals[i], d));
        for (std::size_t k = 0; k < poly.size(); ++k) {
            std::uint32_t ra = remap[poly[k]];
            std::uint32_t rb = remap[poly[(k + 1) % poly.size()]];
            out.face_vertices.push_back(ra);
            std::uint32_t lo = std::min(ra, rb), hi = std::max(ra, rb);
            auto key = std::make_pair(lo, hi);
            if (!edge_seen[key]) {
                edge_seen[key] = true;
                out.edges.push_back(glm::uvec2(lo, hi));
            }
        }
        out.face_starts.push_back(std::uint32_t(out.face_vertices.size()));
    }
    if (out.face_count() < 4 || out.vertices.size() < 4) return false;

    // --- Volume and centroid, by tetrahedron decomposition about the cloud's mean vertex ---
    glm::vec3 ref(0.0f);
    for (const glm::vec3& p : out.vertices) ref += p;
    ref /= float(out.vertices.size());
    glm::vec3 weighted(0.0f);
    float total_volume = 0.0f;
    for (std::size_t i = 0; i < polygons.size(); ++i) {
        std::uint32_t start = out.face_starts[i];
        std::uint32_t end = out.face_starts[i + 1];
        glm::vec3 v0 = out.vertices[out.face_vertices[start]];
        for (std::uint32_t k = start + 1; k + 1 < end; ++k) {
            glm::vec3 v1 = out.vertices[out.face_vertices[k]];
            glm::vec3 v2 = out.vertices[out.face_vertices[k + 1]];
            float vol6 = glm::dot(v0 - ref, glm::cross(v1 - ref, v2 - ref));
            glm::vec3 tet_centroid = (ref + v0 + v1 + v2) * 0.25f;
            total_volume += vol6;
            weighted += tet_centroid * vol6;
        }
    }
    out.volume = std::abs(total_volume) / 6.0f;
    out.centroid = total_volume != 0.0f ? weighted / total_volume : ref;

    out.local_bounds = AABB{};
    for (const glm::vec3& p : out.vertices) {
        out.local_bounds.min = glm::min(out.local_bounds.min, p);
        out.local_bounds.max = glm::max(out.local_bounds.max, p);
    }
    return true;
}

} // namespace geometry
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_GEOMETRY_CONVEX_HULL_H

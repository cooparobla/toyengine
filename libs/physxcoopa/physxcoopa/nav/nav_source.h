/**
 * @file nav_source.h
 * @brief Navigation input geometry: snapshots of physics shapes (SourceShape), gathering them
 *        from a PhysicsWorld, and turning each into world-space triangles for rasterization.
 *
 * Sources are copied out of the PhysicsWorld on the calling thread, so a background tile
 * rebuild never reads live physics state. The one thing a snapshot does not own is a mesh
 * collider's TriangleMesh, or a hull collider's ConvexHull (both non-owning pointers into an
 * asset or a ConvexCollider's shared_ptr), which is why NavBaker gathers a tile's triangles on
 * the calling thread before handing an async rebuild to a job (see nav_baker.h).
 */

#ifndef PHYSXCOOPA_NAV_NAV_SOURCE_H
#define PHYSXCOOPA_NAV_NAV_SOURCE_H

#include <physxcoopa/nav/nav_types.h>
#include <physxcoopa/collision/sat_hull.h>
#include <physxcoopa/collision/shape.h>
#include <physxcoopa/dynamics/body.h>
#include <physxcoopa/geometry/aabb.h>
#include <physxcoopa/geometry/obb.h>
#include <physxcoopa/geometry/triangle_mesh.h>
#include <physxcoopa/world.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace coopa {
namespace physx {
namespace nav {

/**
 * @struct SourceShape
 * @brief One collider's geometry as navigation sees it: a world-placed Shape plus how its
 *        surfaces count.
 */
struct SourceShape {
    collision::Shape shape;               /**< World-scale shape (mesh pointer non-owning). */
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    geometry::AABB bounds;                /**< World bounds. */
    /** @brief Area its walkable (flat enough) surfaces get. k_area_null makes it a pure
     *         obstacle: it blocks and carves, but nothing can stand on it. */
    uint8_t area = k_area_walkable;
    /** @brief Stable identity across gathers (body index/generation + shape slot). */
    uint64_t key = 0;
    /** @brief Hash of everything that changes the rasterized result -- see fingerprint(). */
    uint64_t fingerprint = 0;
};

/**
 * @struct SourceVolume
 * @brief An oriented box that relabels the area of every span whose floor lies inside it
 *        (NavVolume component). k_area_null cuts a hole.
 */
struct SourceVolume {
    geometry::OBB box;
    uint8_t area = k_area_walkable;
    uint64_t fingerprint = 0;

    geometry::AABB bounds() const { return box.bounds(); }
    bool contains(const glm::vec3& p) const {
        glm::vec3 local = glm::inverse(box.orientation) * (p - box.center);
        return std::fabs(local.x) <= box.half_extents.x && std::fabs(local.y) <= box.half_extents.y &&
               std::fabs(local.z) <= box.half_extents.z;
    }
};

namespace detail {

inline uint64_t hash_mix_(uint64_t h, const void* data, std::size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

template <typename T>
inline uint64_t hash_value_(uint64_t h, const T& v) {
    return hash_mix_(h, &v, sizeof(T));
}

} // namespace detail

/** @brief Hash of a source's rasterization-relevant state. Bitwise on floats: a body that has
 *         not moved hashes identically, which is the only property change detection needs. */
inline uint64_t fingerprint(const SourceShape& s) {
    uint64_t h = 1469598103934665603ull;
    const collision::Shape& sh = s.shape;
    h = detail::hash_value_(h, sh.type);
    h = detail::hash_value_(h, sh.local_center);
    h = detail::hash_value_(h, sh.local_rotation);
    switch (sh.type) {
        case collision::ShapeType::Sphere: h = detail::hash_value_(h, sh.radius); break;
        case collision::ShapeType::Box: h = detail::hash_value_(h, sh.half_extents); break;
        case collision::ShapeType::Capsule:
            h = detail::hash_value_(h, sh.capsule_radius);
            h = detail::hash_value_(h, sh.capsule_half_height);
            h = detail::hash_value_(h, sh.capsule_axis);
            break;
        case collision::ShapeType::TriangleMesh:
            h = detail::hash_value_(h, sh.mesh);
            h = detail::hash_value_(h, sh.mesh_scale);
            break;
        case collision::ShapeType::ConvexHull:
            h = detail::hash_value_(h, sh.hull);
            h = detail::hash_value_(h, sh.hull_scale);
            break;
    }
    h = detail::hash_value_(h, s.position);
    h = detail::hash_value_(h, s.rotation);
    h = detail::hash_value_(h, s.area);
    return h;
}

inline uint64_t fingerprint(const SourceVolume& v) {
    uint64_t h = 1469598103934665603ull;
    h = detail::hash_value_(h, v.box.center);
    h = detail::hash_value_(h, v.box.half_extents);
    h = detail::hash_value_(h, v.box.orientation);
    h = detail::hash_value_(h, v.area);
    return h;
}

/**
 * @struct SourceFilter
 * @brief Which physics shapes gather_sources() takes and how they count.
 */
struct SourceFilter {
    /** @brief Physics layers that contribute (bit per layer). */
    uint32_t layer_mask = ~0u;
    /**
     * @brief Optional per-shape override, called for every candidate that passed the default
     *        rules plus every dynamic body's shape (which the default rules reject). Return
     *        false to skip the shape; otherwise adjust `out.area`. `default_include` says what
     *        the default rules decided.
     */
    std::function<bool(dynamics::BodyId, uint32_t shape_index, const dynamics::Body&, bool default_include,
                       SourceShape& out)> classify;
};

/**
 * @brief Snapshots every contributing shape in `world` into `out` (cleared first).
 *
 * Default rules: enabled, non-trigger shapes on a layer in `filter.layer_mask`, owned by a
 * static or kinematic body. Dynamic bodies move, so they are left to SourceFilter::classify
 * (NavSystem includes a dynamic body once it has come to rest and its NavModifier asks to carve).
 */
inline void gather_sources(const PhysicsWorld& world, const SourceFilter& filter, std::vector<SourceShape>& out) {
    out.clear();
    world.for_each_body([&](dynamics::BodyId id, const dynamics::Body& body) {
        for (uint32_t slot : world.shapes_of(id)) {
            const collision::Shape* shape = world.get_shape_at(slot);
            if (!shape || !shape->enabled || shape->is_trigger) continue;
            if (shape->layer < 32 && !((filter.layer_mask >> shape->layer) & 1u)) continue;
            bool include = body.type != dynamics::BodyType::Dynamic;
            if (shape->type == collision::ShapeType::TriangleMesh && !shape->mesh) continue;
            if (shape->type == collision::ShapeType::ConvexHull && !shape->hull) continue;

            SourceShape s;
            s.shape = *shape;
            s.position = body.position;
            s.rotation = body.orientation;
            s.area = k_area_walkable;
            s.key = (static_cast<uint64_t>(id.index) << 40) ^ (static_cast<uint64_t>(id.generation) << 20) ^ slot;
            if (filter.classify) {
                if (!filter.classify(id, slot, body, include, s)) continue;
            } else if (!include) {
                continue;
            }
            s.bounds = collision::world_bounds(s.shape, s.position, s.rotation);
            s.fingerprint = fingerprint(s);
            out.push_back(s);
        }
    });
}

// --- Triangulation --------------------------------------------------------------------------

namespace detail {

inline bool overlaps_xy_(const geometry::AABB& a, const geometry::AABB& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y;
}

inline void push_tri_if_overlaps_(std::vector<glm::vec3>& out, const glm::vec3& a, const glm::vec3& b,
                                  const glm::vec3& c, const geometry::AABB& clip) {
    geometry::AABB tb;
    tb.min = glm::min(a, glm::min(b, c));
    tb.max = glm::max(a, glm::max(b, c));
    if (!overlaps_xy_(tb, clip)) return;
    out.push_back(a);
    out.push_back(b);
    out.push_back(c);
}

/** @brief Tessellated capsule (a sphere when `half_height` is 0) around `center`, long axis
 *         `axis` of `rot`: a 16-segment profile sweep, 4 rings per hemisphere. */
inline void emit_capsule_(std::vector<glm::vec3>& out, const glm::vec3& center, const glm::quat& rot, int axis,
                          float radius, float half_height, const geometry::AABB& clip) {
    constexpr int k_segments = 16;
    constexpr int k_hemi_rings = 4;
    glm::vec3 ax(0.0f), u(0.0f), v(0.0f);
    ax[axis] = 1.0f;
    u[(axis + 1) % 3] = 1.0f;
    v[(axis + 2) % 3] = 1.0f;
    ax = rot * ax;
    u = rot * u;
    v = rot * v;

    // Profile (height along the axis, ring radius), bottom pole to top pole. The equator
    // appears twice -- once per hemisphere -- which is exactly the cylinder band of a capsule.
    glm::vec2 profile[2 * (k_hemi_rings + 1)];
    int n = 0;
    for (int i = 0; i <= k_hemi_rings; ++i) {
        float phi = -glm::half_pi<float>() + glm::half_pi<float>() * static_cast<float>(i) / k_hemi_rings;
        profile[n++] = {std::sin(phi) * radius - half_height, std::cos(phi) * radius};
    }
    for (int i = 0; i <= k_hemi_rings; ++i) {
        float phi = glm::half_pi<float>() * static_cast<float>(i) / k_hemi_rings;
        profile[n++] = {std::sin(phi) * radius + half_height, std::cos(phi) * radius};
    }
    auto point = [&](int ring, int seg) {
        float theta = glm::two_pi<float>() * static_cast<float>(seg) / static_cast<float>(k_segments);
        return center + ax * profile[ring].x + u * (std::cos(theta) * profile[ring].y) +
               v * (std::sin(theta) * profile[ring].y);
    };
    for (int ring = 0; ring + 1 < n; ++ring) {
        for (int seg = 0; seg < k_segments; ++seg) {
            int s1 = (seg + 1) % k_segments;
            glm::vec3 a = point(ring, seg), b = point(ring, s1), c = point(ring + 1, s1), d = point(ring + 1, seg);
            push_tri_if_overlaps_(out, a, b, c, clip);
            push_tri_if_overlaps_(out, a, c, d, clip);
        }
    }
}

} // namespace detail

/**
 * @brief Appends `src`'s world-space triangles (3 vertices each) whose XY footprint overlaps
 *        `clip` to `out`. Boxes and hulls are exact (hull faces fan-triangulated); spheres and
 *        capsules are tessellated (16 x 8); meshes are culled through their BVH so a large
 *        terrain mesh only emits the tile's triangles.
 */
inline void emit_triangles(const SourceShape& src, const geometry::AABB& clip, std::vector<glm::vec3>& out) {
    if (!detail::overlaps_xy_(src.bounds, clip)) return;
    const collision::Shape& s = src.shape;
    switch (s.type) {
        case collision::ShapeType::Box: {
            geometry::OBB obb = collision::world_obb(s, src.position, src.rotation);
            glm::mat3 r = glm::mat3_cast(obb.orientation);
            glm::vec3 c[8];
            for (int i = 0; i < 8; ++i) {
                glm::vec3 l((i & 1) ? obb.half_extents.x : -obb.half_extents.x,
                            (i & 2) ? obb.half_extents.y : -obb.half_extents.y,
                            (i & 4) ? obb.half_extents.z : -obb.half_extents.z);
                c[i] = obb.center + r * l;
            }
            static constexpr int k_faces[6][4] = {
                {0, 2, 3, 1}, {4, 5, 7, 6}, // -z, +z
                {0, 1, 5, 4}, {2, 6, 7, 3}, // -y, +y
                {0, 4, 6, 2}, {1, 3, 7, 5}, // -x, +x
            };
            for (const auto& f : k_faces) {
                detail::push_tri_if_overlaps_(out, c[f[0]], c[f[1]], c[f[2]], clip);
                detail::push_tri_if_overlaps_(out, c[f[0]], c[f[2]], c[f[3]], clip);
            }
            break;
        }
        case collision::ShapeType::Sphere: {
            geometry::Sphere sph = collision::world_sphere(s, src.position, src.rotation);
            detail::emit_capsule_(out, sph.center, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 2, sph.radius, 0.0f, clip);
            break;
        }
        case collision::ShapeType::Capsule: {
            glm::vec3 center = src.position + src.rotation * s.local_center;
            detail::emit_capsule_(out, center, src.rotation * s.local_rotation, s.capsule_axis, s.capsule_radius,
                                  s.capsule_half_height, clip);
            break;
        }
        case collision::ShapeType::TriangleMesh: {
            if (!s.mesh) break;
            glm::quat rot = src.rotation * s.local_rotation;
            glm::vec3 center = src.position + src.rotation * s.local_center;
            float scale = s.mesh_scale;
            glm::mat3 r = glm::mat3_cast(rot);
            // The clip box into mesh-local space. Its z range is unbounded, so clamp it to the
            // mesh's own world bounds first -- transforming an infinite corner gives NaN.
            geometry::AABB wclip = clip;
            wclip.min.z = std::max(clip.min.z, src.bounds.min.z);
            wclip.max.z = std::min(clip.max.z, src.bounds.max.z);
            glm::mat3 inv_r = glm::transpose(r);
            float inv_scale = scale != 0.0f ? 1.0f / scale : 0.0f;
            geometry::AABB local;
            for (int i = 0; i < 8; ++i) {
                glm::vec3 corner((i & 1) ? wclip.max.x : wclip.min.x, (i & 2) ? wclip.max.y : wclip.min.y,
                                 (i & 4) ? wclip.max.z : wclip.min.z);
                glm::vec3 l = inv_r * (corner - center) * inv_scale;
                local.min = glm::min(local.min, l);
                local.max = glm::max(local.max, l);
            }
            const geometry::TriangleMesh& mesh = *s.mesh;
            mesh.query(local, [&](uint32_t tri) {
                glm::vec3 v0, v1, v2;
                mesh.triangle_vertices(tri, v0, v1, v2);
                detail::push_tri_if_overlaps_(out, center + r * (v0 * scale), center + r * (v1 * scale),
                                              center + r * (v2 * scale), clip);
            });
            break;
        }
        case collision::ShapeType::ConvexHull: {
            if (!s.hull) break;
            const collision::PosedHull posed = collision::world_posed_hull(s, src.position, src.rotation);
            const geometry::ConvexHull& hull = *posed.hull;
            for (std::size_t f = 0; f < hull.face_count(); ++f) {
                const std::uint32_t start = hull.face_starts[f];
                const std::uint32_t end = hull.face_starts[f + 1];
                const glm::vec3 v0 = posed.world_vertex(hull.face_vertices[start]);
                for (std::uint32_t i = start + 1; i + 1 < end; ++i) {
                    detail::push_tri_if_overlaps_(out, v0, posed.world_vertex(hull.face_vertices[i]),
                                                  posed.world_vertex(hull.face_vertices[i + 1]), clip);
                }
            }
            break;
        }
    }
}

} // namespace nav
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_NAV_NAV_SOURCE_H

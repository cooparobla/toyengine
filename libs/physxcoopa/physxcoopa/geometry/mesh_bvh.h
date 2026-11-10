/**
 * @file mesh_bvh.h
 * @brief Static BVH over a triangle mesh's triangles, built once at load.
 *
 * v1 simplification: top-down median-split on the longest axis (partition triangles by
 * centroid, not a full surface-area-heuristic bucket search) -- cheaper to build and simpler
 * to get right than true SAH, at the cost of a somewhat less balanced tree for pathological
 * meshes. Query/raycast correctness is unaffected either way.
 */

#ifndef PHYSXCOOPA_GEOMETRY_MESH_BVH_H
#define PHYSXCOOPA_GEOMETRY_MESH_BVH_H

#include <physxcoopa/geometry/aabb.h>
#include <physxcoopa/geometry/ray.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace coopa {
namespace physx {
namespace geometry {

/**
 * @class MeshBVH
 * @brief Maps a world/object-space AABB or ray query to a set of candidate triangle indices.
 */
class MeshBVH {
public:
    MeshBVH() = default;

    /**
     * @brief Builds the tree from a mesh's vertex/index arrays. Safe to call with zero
     *        triangles (the tree is then simply empty).
     */
    void build(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices) {
        nodes_.clear();
        size_t tri_count = indices.size() / 3;
        tri_order_.resize(tri_count);
        if (tri_count == 0) return;

        std::vector<AABB> tri_bounds(tri_count);
        std::vector<glm::vec3> centroids(tri_count);
        for (size_t t = 0; t < tri_count; ++t) {
            tri_order_[t] = static_cast<uint32_t>(t);
            const glm::vec3& v0 = vertices[indices[t * 3 + 0]];
            const glm::vec3& v1 = vertices[indices[t * 3 + 1]];
            const glm::vec3& v2 = vertices[indices[t * 3 + 2]];
            AABB b;
            b.min = glm::min(v0, glm::min(v1, v2));
            b.max = glm::max(v0, glm::max(v1, v2));
            tri_bounds[t] = b;
            centroids[t] = (v0 + v1 + v2) * (1.0f / 3.0f);
        }
        root_ = build_recursive_(0, tri_count, tri_bounds, centroids);
    }

    /** @brief Invokes `fn(uint32_t triangle_index)` for every triangle whose bounds overlap `bounds`. */
    template <typename Fn>
    void query(const AABB& bounds, Fn&& fn) const {
        if (nodes_.empty()) return;
        query_recursive_(root_, bounds, fn);
    }

    /** @brief Invokes `fn(uint32_t triangle_index)` for every triangle whose bounds the ray intersects. */
    template <typename Fn>
    void raycast(const Ray& ray, Fn&& fn) const {
        if (nodes_.empty()) return;
        raycast_recursive_(root_, ray, fn);
    }

    /**
     * @brief Invokes `fn(uint32_t triangle_index)` for every triangle whose bounds the box
     *        `box` could touch while translating along `ray` -- each node's bounds are grown by
     *        the box's half-extents (a Minkowski sum) and slab-tested against `ray`, whose
     *        origin should be the box's center. Tighter than query()-ing the whole swept AABB
     *        for a long diagonal cast, which is what shape casts against a mesh use it for.
     */
    template <typename Fn>
    void sweep(const AABB& box, const Ray& ray, Fn&& fn) const {
        if (nodes_.empty()) return;
        sweep_recursive_(root_, (box.max - box.min) * 0.5f, ray, fn);
    }

    bool empty() const { return nodes_.empty(); }

private:
    struct Node {
        AABB bounds;
        int32_t left = -1;
        int32_t right = -1;
        uint32_t tri_start = 0;
        uint32_t tri_count = 0;
        bool is_leaf() const { return left < 0; }
    };

    int32_t build_recursive_(size_t start, size_t end, std::vector<AABB>& tri_bounds, std::vector<glm::vec3>& centroids) {
        AABB bounds;
        for (size_t i = start; i < end; ++i) bounds = AABB::merge(bounds, tri_bounds[tri_order_[i]]);

        size_t count = end - start;
        int32_t idx = static_cast<int32_t>(nodes_.size());
        nodes_.push_back(Node{});
        nodes_[idx].bounds = bounds;

        constexpr size_t k_leaf_threshold = 4;
        if (count <= k_leaf_threshold) {
            nodes_[idx].tri_start = static_cast<uint32_t>(start);
            nodes_[idx].tri_count = static_cast<uint32_t>(count);
            return idx;
        }

        glm::vec3 extent = bounds.extents();
        int axis = 0;
        if (extent.y > extent[axis]) axis = 1;
        if (extent.z > extent[axis]) axis = 2;

        size_t mid = start + count / 2;
        std::nth_element(tri_order_.begin() + start, tri_order_.begin() + mid, tri_order_.begin() + end,
                          [&](uint32_t a, uint32_t b) { return centroids[a][axis] < centroids[b][axis]; });

        int32_t left = build_recursive_(start, mid, tri_bounds, centroids);
        int32_t right = build_recursive_(mid, end, tri_bounds, centroids);
        nodes_[idx].left = left;
        nodes_[idx].right = right;
        return idx;
    }

    template <typename Fn>
    void query_recursive_(int32_t idx, const AABB& bounds, Fn&& fn) const {
        const Node& node = nodes_[idx];
        if (!node.bounds.overlaps(bounds)) return;
        if (node.is_leaf()) {
            for (uint32_t i = 0; i < node.tri_count; ++i) fn(tri_order_[node.tri_start + i]);
        } else {
            query_recursive_(node.left, bounds, fn);
            query_recursive_(node.right, bounds, fn);
        }
    }

    template <typename Fn>
    void raycast_recursive_(int32_t idx, const Ray& ray, Fn&& fn) const {
        const Node& node = nodes_[idx];
        float t;
        if (!ray.intersect(node.bounds, t)) return;
        if (node.is_leaf()) {
            for (uint32_t i = 0; i < node.tri_count; ++i) fn(tri_order_[node.tri_start + i]);
        } else {
            raycast_recursive_(node.left, ray, fn);
            raycast_recursive_(node.right, ray, fn);
        }
    }

    template <typename Fn>
    void sweep_recursive_(int32_t idx, const glm::vec3& half, const Ray& ray, Fn&& fn) const {
        const Node& node = nodes_[idx];
        AABB grown;
        grown.min = node.bounds.min - half;
        grown.max = node.bounds.max + half;
        float t;
        if (!grown.contains(ray.origin) && !ray.intersect(grown, t)) return;
        if (node.is_leaf()) {
            for (uint32_t i = 0; i < node.tri_count; ++i) fn(tri_order_[node.tri_start + i]);
        } else {
            sweep_recursive_(node.left, half, ray, fn);
            sweep_recursive_(node.right, half, ray, fn);
        }
    }

    std::vector<Node> nodes_;
    std::vector<uint32_t> tri_order_;
    int32_t root_ = -1;
};

} // namespace geometry
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_GEOMETRY_MESH_BVH_H

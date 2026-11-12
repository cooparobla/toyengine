/**
 * @file triangle_mesh.h
 * @brief Format-agnostic static triangle mesh: welded vertices, per-triangle normals and
 *        edge adjacency. Construction is from raw arrays only: from_triangles() welds and
 *        builds adjacency from an unwelded triangle list (runtime geometry -- a streamed
 *        terrain chunk, say); asset loading is physxcoopa::loaders::TriangleMeshLoader, which
 *        parses and fan-triangulates a mesh YAML and then calls from_triangles().
 */

#ifndef PHYSXCOOPA_GEOMETRY_TRIANGLE_MESH_H
#define PHYSXCOOPA_GEOMETRY_TRIANGLE_MESH_H

#include <physxcoopa/geometry/aabb.h>
#include <physxcoopa/geometry/height_grid.h>
#include <physxcoopa/geometry/mesh_bvh.h>
#include <physxcoopa/geometry/ray.h>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coopa {
namespace physx {
namespace geometry {

namespace detail {

/** @brief Packs two triangle-vertex indices into an order-independent 64-bit edge key. */
inline uint64_t edge_key(uint32_t a, uint32_t b) {
    uint32_t lo = a < b ? a : b;
    uint32_t hi = a < b ? b : a;
    return (static_cast<uint64_t>(lo) << 32) | static_cast<uint64_t>(hi);
}

/** @brief Packs a quantized 3D grid cell into a 64-bit key for the welding spatial hash. */
inline uint64_t cell_key(int32_t x, int32_t y, int32_t z) {
    auto to_u = [](int32_t v) { return static_cast<uint64_t>(static_cast<uint32_t>(v)) & 0x1FFFFFu; };
    return (to_u(x) << 42) | (to_u(y) << 21) | to_u(z);
}

} // namespace detail

/** @brief Sentinel neighbour-triangle index meaning "no neighbour" (boundary edge). */
inline constexpr uint32_t k_no_neighbor = 0xFFFFFFFFu;

/**
 * @struct TriangleAdjacency
 * @brief Per-triangle neighbour info, indexed by triangle. Edge `i` of a triangle runs from
 *        vertex `i` to vertex `(i+1)%3`; `neighbor[i]` is the triangle sharing that edge, or
 *        k_no_neighbor for a mesh-boundary edge.
 */
struct TriangleAdjacency {
    uint32_t neighbor[3] = {k_no_neighbor, k_no_neighbor, k_no_neighbor};
};

/**
 * @class TriangleMesh
 * @brief A static, welded triangle mesh with precomputed per-triangle normals and adjacency.
 *
 * Used as the collision geometry for MeshCollider. Non-convex mesh colliders are static-only
 * (see RigidbodyComponent's bind-time check) -- this class itself has no notion of that
 * restriction, it is purely geometric storage.
 */
class TriangleMesh {
public:
    TriangleMesh() = default;

    /**
     * @brief Builds a TriangleMesh from already-welded vertex/index data plus precomputed
     *        per-triangle normals and adjacency.
     *
     * @param vertices  Welded vertex positions.
     * @param indices   Triangle list, 3 indices per triangle.
     * @param normals   One normal per triangle (indices.size()/3 entries).
     * @param adjacency One TriangleAdjacency per triangle.
     */
    TriangleMesh(std::vector<glm::vec3> vertices,
                 std::vector<uint32_t> indices,
                 std::vector<glm::vec3> normals,
                 std::vector<TriangleAdjacency> adjacency)
        : vertices_(std::move(vertices)),
          indices_(std::move(indices)),
          normals_(std::move(normals)),
          adjacency_(std::move(adjacency)) {
        bounds_ = compute_bounds_();
        bvh_.build(vertices_, indices_);
    }

    /**
     * @brief Builds a collision mesh from an UNWELDED triangle list: welds coincident vertices
     *        (epsilon relative to the mesh's own bounds), drops triangles welding made
     *        degenerate, and computes face normals, edge adjacency and the BVH.
     *
     * Welding is not an optimisation: render and exported meshes split vertices per face
     * corner (or per normal), and edge adjacency only finds shared edges between welded
     * vertices. An edge owned by more than two triangles is left unconnected.
     *
     * @param positions          Vertex positions (any duplication).
     * @param indices            Triangle list, 3 indices per triangle, into `positions`.
     * @param non_manifold_edges Optional: receives the number of edges left unconnected
     *                           because more than two triangles share them.
     */
    static TriangleMesh from_triangles(const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& indices,
                                       size_t* non_manifold_edges = nullptr) {
        if (non_manifold_edges) *non_manifold_edges = 0;
        if (positions.empty() || indices.size() < 3) return TriangleMesh();

        // --- Weld via a spatial hash ---
        glm::vec3 bounds_min(positions[0]), bounds_max(positions[0]);
        for (const auto& v : positions) {
            bounds_min = glm::min(bounds_min, v);
            bounds_max = glm::max(bounds_max, v);
        }
        const float diagonal = glm::length(bounds_max - bounds_min);
        const float epsilon = diagonal > 0.0f ? diagonal * 1e-4f : 1e-5f;
        const float cell_size = epsilon > 0.0f ? epsilon : 1e-5f;

        std::vector<glm::vec3> welded;
        std::vector<uint32_t> remap(positions.size());
        std::unordered_multimap<uint64_t, uint32_t> spatial_hash; // cell key -> welded index
        spatial_hash.reserve(positions.size());
        for (size_t i = 0; i < positions.size(); ++i) {
            const glm::vec3& p = positions[i];
            const glm::ivec3 cell(static_cast<int32_t>(std::floor(p.x / cell_size)),
                                  static_cast<int32_t>(std::floor(p.y / cell_size)),
                                  static_cast<int32_t>(std::floor(p.z / cell_size)));
            uint32_t found = UINT32_MAX;
            for (int dx = -1; dx <= 1 && found == UINT32_MAX; ++dx) {
                for (int dy = -1; dy <= 1 && found == UINT32_MAX; ++dy) {
                    for (int dz = -1; dz <= 1 && found == UINT32_MAX; ++dz) {
                        auto range = spatial_hash.equal_range(detail::cell_key(cell.x + dx, cell.y + dy, cell.z + dz));
                        for (auto it = range.first; it != range.second; ++it) {
                            if (glm::length(welded[it->second] - p) <= epsilon) {
                                found = it->second;
                                break;
                            }
                        }
                    }
                }
            }
            if (found == UINT32_MAX) {
                found = static_cast<uint32_t>(welded.size());
                welded.push_back(p);
                spatial_hash.emplace(detail::cell_key(cell.x, cell.y, cell.z), found);
            }
            remap[i] = found;
        }

        // --- Remap triangles, dropping any made degenerate by welding ---
        std::vector<uint32_t> out_indices;
        out_indices.reserve(indices.size());
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const uint32_t a = remap[indices[t + 0]];
            const uint32_t b = remap[indices[t + 1]];
            const uint32_t c = remap[indices[t + 2]];
            if (a == b || b == c || a == c) continue;
            const glm::vec3 n = glm::cross(welded[b] - welded[a], welded[c] - welded[a]);
            if (glm::length(n) <= 1e-8f) continue;
            out_indices.push_back(a);
            out_indices.push_back(b);
            out_indices.push_back(c);
        }
        const size_t triangle_count = out_indices.size() / 3;

        // --- Face normals ---
        std::vector<glm::vec3> normals(triangle_count);
        for (size_t t = 0; t < triangle_count; ++t) {
            const glm::vec3& a = welded[out_indices[t * 3 + 0]];
            const glm::vec3& b = welded[out_indices[t * 3 + 1]];
            const glm::vec3& c = welded[out_indices[t * 3 + 2]];
            normals[t] = glm::normalize(glm::cross(b - a, c - a));
        }

        // --- Edge adjacency: edge key -> owning (triangle, edge slot) occurrences ---
        std::vector<TriangleAdjacency> adjacency(triangle_count);
        std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> edge_owners;
        edge_owners.reserve(triangle_count * 2);
        for (uint32_t t = 0; t < triangle_count; ++t) {
            for (uint32_t e = 0; e < 3; ++e) {
                const uint32_t v0 = out_indices[t * 3 + e];
                const uint32_t v1 = out_indices[t * 3 + (e + 1) % 3];
                edge_owners[detail::edge_key(v0, v1)].push_back({t, e});
            }
        }
        for (const auto& entry : edge_owners) {
            const auto& owners = entry.second;
            if (owners.size() == 2) {
                adjacency[owners[0].first].neighbor[owners[0].second] = owners[1].first;
                adjacency[owners[1].first].neighbor[owners[1].second] = owners[0].first;
            } else if (owners.size() > 2 && non_manifold_edges) {
                ++*non_manifold_edges;
            }
            // owners.size() == 1: boundary edge, k_no_neighbor is already the default.
        }
        return TriangleMesh(std::move(welded), std::move(out_indices), std::move(normals), std::move(adjacency));
    }

    /**
     * @brief A mesh whose triangles are a height grid's (geometry/height_grid.h): nothing is
     *        stored per triangle, and vertices()/indices()/normals()/adjacency()/bvh() are empty
     *        -- reach it through the accessors below (triangle_vertices(), triangle_normal(),
     *        triangle_neighbor(), query(), raycast(), sweep()), which every collision path uses.
     */
    static TriangleMesh from_height_grid(std::shared_ptr<const HeightGrid> grid) {
        TriangleMesh m;
        m.grid_ = std::move(grid);
        if (m.grid_) m.bounds_ = m.grid_->bounds();
        return m;
    }

    /** @brief The grid behind a heightfield mesh (null for a stored mesh). */
    const HeightGrid* height_grid() const { return grid_.get(); }

    /** @brief True when there is no triangle to collide with. */
    bool empty() const { return grid_ ? false : bvh_.empty(); }

    /** @brief Triangle `tri`'s unit face normal. */
    glm::vec3 triangle_normal(uint32_t tri) const { return grid_ ? grid_->triangle_normal(tri) : normals_[tri]; }

    /** @brief The triangle sharing edge `edge` (vertex edge -> edge+1) of `tri`, or k_no_neighbor. */
    uint32_t triangle_neighbor(uint32_t tri, int edge) const {
        if (grid_) {
            uint32_t n[3];
            grid_->triangle_neighbors(tri, n);
            return n[edge];
        }
        return adjacency_[tri].neighbor[edge];
    }

    /** @brief `tri`'s three neighbours (see TriangleAdjacency). */
    TriangleAdjacency triangle_adjacency(uint32_t tri) const {
        TriangleAdjacency a;
        if (grid_) grid_->triangle_neighbors(tri, a.neighbor);
        else a = adjacency_[tri];
        return a;
    }

    /** @brief fn(tri) for every triangle whose bounds overlap `bounds` (local space). */
    template <typename Fn>
    void query(const AABB& bounds, Fn&& fn) const {
        if (grid_) grid_->query(bounds, fn);
        else bvh_.query(bounds, fn);
    }

    /** @brief fn(tri) for every triangle whose bounds the ray reaches (local space). */
    template <typename Fn>
    void raycast(const Ray& ray, Fn&& fn) const {
        if (grid_) grid_->raycast(ray, fn);
        else bvh_.raycast(ray, fn);
    }

    /** @brief fn(tri) for every triangle `box` could touch moving along `ray` (see MeshBVH::sweep). */
    template <typename Fn>
    void sweep(const AABB& box, const Ray& ray, Fn&& fn) const {
        if (grid_) grid_->sweep(box, ray, fn);
        else bvh_.sweep(box, ray, fn);
    }

    /** @brief Welded vertex positions. */
    const std::vector<glm::vec3>& vertices() const { return vertices_; }

    /** @brief Triangle list, 3 indices per triangle. */
    const std::vector<uint32_t>& indices() const { return indices_; }

    /** @brief One face normal per triangle. */
    const std::vector<glm::vec3>& normals() const { return normals_; }

    /** @brief One TriangleAdjacency per triangle. */
    const std::vector<TriangleAdjacency>& adjacency() const { return adjacency_; }

    /** @brief Number of triangles in the mesh. */
    size_t triangle_count() const { return grid_ ? grid_->triangle_count() : indices_.size() / 3; }

    /** @brief The three vertex positions of triangle `tri`. */
    void triangle_vertices(uint32_t tri, glm::vec3& v0, glm::vec3& v1, glm::vec3& v2) const {
        if (grid_) {
            grid_->triangle_vertices(tri, v0, v1, v2);
            return;
        }
        v0 = vertices_[indices_[tri * 3 + 0]];
        v1 = vertices_[indices_[tri * 3 + 1]];
        v2 = vertices_[indices_[tri * 3 + 2]];
    }

    /** @brief Object-space bounds of the whole mesh. */
    const AABB& bounds() const { return bounds_; }

    /** @brief Static BVH over this mesh's triangles, built once at construction. */
    const MeshBVH& bvh() const { return bvh_; }

private:
    AABB compute_bounds_() const {
        AABB out;
        for (const auto& v : vertices_) {
            out.min = glm::min(out.min, v);
            out.max = glm::max(out.max, v);
        }
        return out;
    }

    std::vector<glm::vec3> vertices_;
    std::vector<uint32_t> indices_;
    std::vector<glm::vec3> normals_;
    std::vector<TriangleAdjacency> adjacency_;
    AABB bounds_;
    MeshBVH bvh_;
    std::shared_ptr<const HeightGrid> grid_;
};

} // namespace geometry
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_GEOMETRY_TRIANGLE_MESH_H

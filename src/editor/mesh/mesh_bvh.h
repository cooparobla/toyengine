/**
 * @file mesh_bvh.h
 * @brief A bounding-volume hierarchy over an EditMesh's (fan-triangulated) faces.
 *
 * Ray queries in mesh-local space: face picking, the visibility test that keeps edit-mode
 * picking from grabbing elements on the far side of the mesh (Blender only selects what it
 * can see unless X-ray is on), the Loop Cut hover, and the sculpt brush's surface hit. Built
 * by median split in O(n log n); refit() keeps the tree and only recomputes boxes, which is
 * enough while sculpting moves vertices without changing topology.
 */

#ifndef TOYEDITOR_MESH_MESH_BVH_H
#define TOYEDITOR_MESH_MESH_BVH_H

#include "edit_mesh.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace toy::editor {

class TriangleBVH {
public:
    struct Hit {
        float t = 0.0f;          ///< Ray parameter (distance when `dir` is unit length).
        uint32_t face = 0;       ///< Face index in the EditMesh.
        uint32_t tri = 0;        ///< Triangle index (internal).
        glm::vec3 p{0.0f};       ///< Hit point.
        glm::vec3 normal{0.0f};  ///< Geometric normal of the hit triangle (unnormalized winding).
    };

    void build(const EditMesh& m);

    /** @brief Recomputes every box for moved vertices (same topology as the last build). */
    void refit(const EditMesh& m);

    bool empty() const { return tris_.empty(); }
    bool built() const { return built_; }

    /** @brief Nearest hit along `o + t * d` for t in (t_min, t_max). Two-sided. */
    std::optional<Hit> raycast(const EditMesh& m, glm::vec3 o, glm::vec3 d, float t_min = 0.0f, float t_max = 1e30f) const;

    /**
     * @brief Is `target` hidden from `eye` by the surface? `slack` (a fraction of the eye ->
     *        target distance) lets a vertex or edge lying ON the surface count as visible.
     */
    bool occluded(const EditMesh& m, glm::vec3 eye, glm::vec3 target, float slack = 1e-3f) const;

private:
    struct Tri { std::array<uint32_t, 3> v; uint32_t face; };
    struct Node {
        glm::vec3 lo{1e30f}, hi{-1e30f};
        uint32_t left = 0;     ///< First child (children are left, left + 1) when count == 0.
        uint32_t first = 0, count = 0;
    };

    uint32_t build_into_(const EditMesh& m, uint32_t slot, uint32_t first, uint32_t count);

    static bool slab_(const Node& n, glm::vec3 o, glm::vec3 inv, float t0, float t1);

    /** @brief Möller-Trumbore, two-sided. */
    static bool intersect_(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 o, glm::vec3 d, float& t);

    std::vector<Tri> tris_;
    std::vector<uint32_t> order_;
    std::vector<glm::vec3> centroids_;
    std::vector<Node> nodes_;
    bool built_ = false;
};

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_MESH_BVH_H

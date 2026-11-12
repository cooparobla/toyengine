/**
 * @file height_grid.h
 * @brief A regular grid of heights as a collision surface: the triangles of a heightfield
 *        terrain, never stored -- each one is computed from its four corner heights on demand.
 *
 * Used through TriangleMesh's heightfield mode (TriangleMesh::from_height_grid()), so every
 * consumer of a mesh shape -- narrowphase, raycasts, sweeps, the character motor's penetration
 * solve, cloth, the navmesh bake -- takes a heightfield unchanged. What a stored mesh keeps per
 * triangle (vertices, normal, adjacency, a BVH) is all implicit here:
 *
 *   - Cell (i, j) spans samples i..i+1, j..j+1 and holds two triangles,
 *     tri = (j * (nx - 1) + i) * 2 + half, split along its (i, j)-(i+1, j+1) diagonal:
 *       half 0: (i, j) (i+1, j) (i+1, j+1)      half 1: (i, j) (i+1, j+1) (i, j+1)
 *     both counter-clockwise seen from above, so normals point up.
 *   - Adjacency follows TriangleMesh's convention (edge e runs vertex e -> e+1) arithmetically.
 *   - Culling uses a two-level min/max pyramid (8 x 8 and 64 x 64 cells) in place of a BVH.
 *
 * Local space: sample (i, j) sits at (i * cell.x, j * cell.y, height). Immutable once built --
 * an edited terrain builds a new grid, so a navmesh bake or a query running on another thread
 * against the old one is never disturbed.
 */

#ifndef PHYSXCOOPA_GEOMETRY_HEIGHT_GRID_H
#define PHYSXCOOPA_GEOMETRY_HEIGHT_GRID_H

#include <physxcoopa/geometry/aabb.h>
#include <physxcoopa/geometry/ray.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace coopa {
namespace physx {
namespace geometry {

class HeightGrid {
public:
    HeightGrid() = default;

    /**
     * @param nx, ny  Samples per side (at least 2 each).
     * @param cell    Metres between samples along x and y.
     * @param heights nx * ny heights, row-major in y.
     */
    HeightGrid(int nx, int ny, glm::vec2 cell, std::vector<float> heights)
        : nx_(std::max(2, nx)), ny_(std::max(2, ny)), cell_(cell), h_(std::move(heights)) {
        h_.resize(static_cast<std::size_t>(nx_) * ny_, 0.0f);
        build_pyramid_();
    }

    int nx() const { return nx_; }
    int ny() const { return ny_; }
    glm::vec2 cell() const { return cell_; }
    const std::vector<float>& heights() const { return h_; }
    float at(int i, int j) const { return h_[static_cast<std::size_t>(j) * nx_ + i]; }
    glm::vec3 vertex(int i, int j) const {
        return glm::vec3(static_cast<float>(i) * cell_.x, static_cast<float>(j) * cell_.y, at(i, j));
    }

    std::size_t triangle_count() const { return static_cast<std::size_t>(nx_ - 1) * (ny_ - 1) * 2; }
    AABB bounds() const { return bounds_; }

    void triangle_vertices(uint32_t tri, glm::vec3& v0, glm::vec3& v1, glm::vec3& v2) const {
        int i, j, half;
        decode_(tri, i, j, half);
        v0 = vertex(i, j);
        if (half == 0) {
            v1 = vertex(i + 1, j);
            v2 = vertex(i + 1, j + 1);
        } else {
            v1 = vertex(i + 1, j + 1);
            v2 = vertex(i, j + 1);
        }
    }

    glm::vec3 triangle_normal(uint32_t tri) const {
        glm::vec3 v0, v1, v2;
        triangle_vertices(tri, v0, v1, v2);
        const glm::vec3 n = glm::cross(v1 - v0, v2 - v0);
        const float len = glm::length(n);
        return len > 0.0f ? n / len : glm::vec3(0.0f, 0.0f, 1.0f);
    }

    /** @brief Neighbour across edge e (vertex e -> e+1), or 0xFFFFFFFF on the grid's border. */
    void triangle_neighbors(uint32_t tri, uint32_t out[3]) const {
        int i, j, half;
        decode_(tri, i, j, half);
        const uint32_t none = 0xFFFFFFFFu;
        if (half == 0) {
            out[0] = j > 0 ? encode_(i, j - 1, 1) : none;            // bottom -> below's half 1 (its top)
            out[1] = i + 1 < nx_ - 1 ? encode_(i + 1, j, 1) : none;   // right  -> right's half 1 (its left)
            out[2] = encode_(i, j, 1);                                // diagonal
        } else {
            out[0] = encode_(i, j, 0);                                // diagonal
            out[1] = j + 1 < ny_ - 1 ? encode_(i, j + 1, 0) : none;   // top  -> above's half 0 (its bottom)
            out[2] = i > 0 ? encode_(i - 1, j, 0) : none;             // left -> left's half 0 (its right)
        }
    }

    /** @brief fn(tri) for every triangle whose cell box overlaps `box`. */
    template <typename Fn>
    void query(const AABB& box, Fn&& fn) const {
        visit_([&](const AABB& b) { return b.overlaps(box); }, fn);
    }

    /** @brief fn(tri) for every triangle whose cell box the ray reaches. */
    template <typename Fn>
    void raycast(const Ray& ray, Fn&& fn) const {
        float t;
        visit_([&](const AABB& b) { return ray.intersect(b, t); }, fn);
    }

    /** @brief fn(tri) for every triangle `box` could touch translating along `ray` (see MeshBVH::sweep). */
    template <typename Fn>
    void sweep(const AABB& box, const Ray& ray, Fn&& fn) const {
        const glm::vec3 half = (box.max - box.min) * 0.5f;
        float t;
        visit_([&](const AABB& b) {
            AABB g;
            g.min = b.min - half;
            g.max = b.max + half;
            return g.contains(ray.origin) || ray.intersect(g, t);
        }, fn);
    }

private:
    static constexpr int k_fine = 8;     ///< Cells per fine block side.
    static constexpr int k_coarse = 64;  ///< Cells per coarse block side (8 fine blocks).

    struct Range {
        float lo = std::numeric_limits<float>::max();
        float hi = std::numeric_limits<float>::lowest();
    };

    uint32_t encode_(int i, int j, int half) const {
        return static_cast<uint32_t>((j * (nx_ - 1) + i) * 2 + half);
    }
    void decode_(uint32_t tri, int& i, int& j, int& half) const {
        half = static_cast<int>(tri & 1u);
        const int cell = static_cast<int>(tri >> 1);
        i = cell % (nx_ - 1);
        j = cell / (nx_ - 1);
    }

    int cells_x_() const { return nx_ - 1; }
    int cells_y_() const { return ny_ - 1; }

    AABB box_(int ci0, int cj0, int ci1, int cj1, const Range& r) const {
        AABB b;
        b.min = glm::vec3(static_cast<float>(ci0) * cell_.x, static_cast<float>(cj0) * cell_.y, r.lo);
        b.max = glm::vec3(static_cast<float>(ci1) * cell_.x, static_cast<float>(cj1) * cell_.y, r.hi);
        return b;
    }

    void build_pyramid_() {
        const int cx = cells_x_(), cy = cells_y_();
        fine_w_ = (cx + k_fine - 1) / k_fine;
        fine_h_ = (cy + k_fine - 1) / k_fine;
        coarse_w_ = (cx + k_coarse - 1) / k_coarse;
        coarse_h_ = (cy + k_coarse - 1) / k_coarse;
        fine_.assign(static_cast<std::size_t>(fine_w_) * fine_h_, Range{});
        coarse_.assign(static_cast<std::size_t>(coarse_w_) * coarse_h_, Range{});
        bounds_ = AABB{};
        for (int j = 0; j < ny_; ++j) {
            for (int i = 0; i < nx_; ++i) {
                const float h = at(i, j);
                // A sample is a corner of up to four cells: widen each one's fine block.
                for (int dj = -1; dj <= 0; ++dj) {
                    for (int di = -1; di <= 0; ++di) {
                        const int ci = i + di, cj = j + dj;
                        if (ci < 0 || cj < 0 || ci >= cx || cj >= cy) continue;
                        Range& f = fine_[static_cast<std::size_t>(cj / k_fine) * fine_w_ + ci / k_fine];
                        f.lo = std::min(f.lo, h);
                        f.hi = std::max(f.hi, h);
                        Range& c = coarse_[static_cast<std::size_t>(cj / k_coarse) * coarse_w_ + ci / k_coarse];
                        c.lo = std::min(c.lo, h);
                        c.hi = std::max(c.hi, h);
                    }
                }
                bounds_.min = glm::min(bounds_.min, vertex(i, j));
                bounds_.max = glm::max(bounds_.max, vertex(i, j));
            }
        }
    }

    template <typename Pred, typename Fn>
    void visit_(Pred&& pred, Fn&& fn) const {
        const int cx = cells_x_(), cy = cells_y_();
        for (int gj = 0; gj < coarse_h_; ++gj) {
            for (int gi = 0; gi < coarse_w_; ++gi) {
                const int c0i = gi * k_coarse, c0j = gj * k_coarse;
                const int c1i = std::min(c0i + k_coarse, cx), c1j = std::min(c0j + k_coarse, cy);
                if (!pred(box_(c0i, c0j, c1i, c1j, coarse_[static_cast<std::size_t>(gj) * coarse_w_ + gi]))) continue;
                for (int fj = c0j / k_fine; fj * k_fine < c1j; ++fj) {
                    for (int fi = c0i / k_fine; fi * k_fine < c1i; ++fi) {
                        const int f0i = fi * k_fine, f0j = fj * k_fine;
                        const int f1i = std::min(f0i + k_fine, cx), f1j = std::min(f0j + k_fine, cy);
                        if (!pred(box_(f0i, f0j, f1i, f1j, fine_[static_cast<std::size_t>(fj) * fine_w_ + fi]))) continue;
                        for (int j = f0j; j < f1j; ++j) {
                            for (int i = f0i; i < f1i; ++i) {
                                Range r;
                                r.lo = std::min(std::min(at(i, j), at(i + 1, j)), std::min(at(i, j + 1), at(i + 1, j + 1)));
                                r.hi = std::max(std::max(at(i, j), at(i + 1, j)), std::max(at(i, j + 1), at(i + 1, j + 1)));
                                if (!pred(box_(i, j, i + 1, j + 1, r))) continue;
                                fn(encode_(i, j, 0));
                                fn(encode_(i, j, 1));
                            }
                        }
                    }
                }
            }
        }
    }

    int nx_ = 2, ny_ = 2;
    glm::vec2 cell_{1.0f};
    std::vector<float> h_;
    std::vector<Range> fine_, coarse_;
    int fine_w_ = 0, fine_h_ = 0, coarse_w_ = 0, coarse_h_ = 0;
    AABB bounds_;
};

} // namespace geometry
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_GEOMETRY_HEIGHT_GRID_H

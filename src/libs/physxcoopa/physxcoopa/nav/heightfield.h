/**
 * @file heightfield.h
 * @brief Solid heightfield: per-column lists of solid spans, filled by conservative triangle
 *        rasterization (Recast's rcRasterizeTriangle, rewritten for this module's Z-up frame).
 *
 * One Heightfield covers one tile plus its border padding. Every triangle is clipped against
 * each cell's square footprint, so a wall thinner than a cell still blocks the cells it passes
 * through, and the clipped polygon's z range becomes a solid span. Overlapping spans merge; the
 * merged span keeps the area of whichever input had the higher top, which is what makes the top
 * surface -- the one an agent stands on -- decide walkability.
 */

#ifndef PHYSXCOOPA_NAV_HEIGHTFIELD_H
#define PHYSXCOOPA_NAV_HEIGHTFIELD_H

#include <physxcoopa/nav/nav_types.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace coopa {
namespace physx {
namespace nav {

/** @brief One solid interval in a column, in z quanta. */
struct SolidSpan {
    uint16_t smin = 0;
    uint16_t smax = 0;
    uint8_t area = k_area_null;
    uint32_t next = 0xFFFFFFFFu;
};

/** @brief Largest z quantum a span may reach (k_open_ceiling is reserved for "no ceiling"). */
inline constexpr int k_max_span_height = 0xFFFE;

/**
 * @class Heightfield
 * @brief Width x height columns of solid spans, kept sorted bottom-up per column.
 */
class Heightfield {
public:
    static constexpr uint32_t k_null = 0xFFFFFFFFu;

    /** @brief Resets to `w` x `h` empty columns. `bmin` is the world corner of column (0, 0);
     *         its z is the quantization origin (NavGridParams::origin.z). Keeps allocations. */
    void reset(int w, int h, const glm::vec3& bmin, float cs, float ch) {
        width_ = w;
        height_ = h;
        bmin_ = bmin;
        cs_ = cs;
        ch_ = ch;
        heads_.assign(static_cast<std::size_t>(w) * h, k_null);
        pool_.clear();
        free_ = k_null;
    }

    int width() const { return width_; }
    int height() const { return height_; }
    uint32_t head(int x, int y) const { return heads_[static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * width_]; }
    const SolidSpan& span(uint32_t i) const { return pool_[i]; }
    SolidSpan& span(uint32_t i) { return pool_[i]; }
    std::size_t span_capacity() const { return pool_.size(); }

    /**
     * @brief Inserts [smin, smax] into column (x, y), merging every span it overlaps.
     * @param merge_threshold When the merged tops are within this many quanta, the walkable
     *        area wins (a curb's top and the road beside it are "the same" surface).
     */
    void add_span(int x, int y, uint16_t smin, uint16_t smax, uint8_t area, int merge_threshold) {
        std::size_t col = static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * width_;
        SolidSpan ns;
        ns.smin = smin;
        ns.smax = smax;
        ns.area = area;

        uint32_t prev = k_null;
        uint32_t cur = heads_[col];
        while (cur != k_null) {
            SolidSpan& c = pool_[cur];
            if (c.smin > ns.smax) break;            // new span lies wholly below cur
            if (c.smax < ns.smin) {                 // cur lies wholly below the new span
                prev = cur;
                cur = c.next;
                continue;
            }
            // Overlap: absorb cur.
            if (std::abs(static_cast<int>(c.smax) - static_cast<int>(ns.smax)) <= merge_threshold) {
                ns.area = std::max(ns.area, c.area);
            } else if (c.smax > ns.smax) {
                ns.area = c.area;
            }
            ns.smin = std::min(ns.smin, c.smin);
            ns.smax = std::max(ns.smax, c.smax);
            uint32_t next = c.next;
            free_span_(cur);
            if (prev != k_null) pool_[prev].next = next;
            else heads_[col] = next;
            cur = next;
        }
        uint32_t idx = alloc_span_();
        ns.next = cur;
        pool_[idx] = ns;
        if (prev != k_null) pool_[prev].next = idx;
        else heads_[col] = idx;
    }

    /**
     * @brief Conservatively rasterizes one triangle.
     * @param area Area its spans get (already resolved to k_area_null for a too-steep or
     *             obstacle-only triangle -- see rasterize_triangles()).
     */
    void rasterize_triangle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, uint8_t area,
                            int merge_threshold) {
        const float w = static_cast<float>(width_) * cs_;
        const float h = static_cast<float>(height_) * cs_;
        glm::vec3 tmin = glm::min(v0, glm::min(v1, v2)) - bmin_;
        glm::vec3 tmax = glm::max(v0, glm::max(v1, v2)) - bmin_;
        if (tmax.x < 0.0f || tmax.y < 0.0f || tmin.x > w || tmin.y > h) return;

        const float inv_cs = 1.0f / cs_;
        const float inv_ch = 1.0f / ch_;
        // Row/column -1 absorbs the part of the triangle hanging off the low edge, and is
        // then skipped -- clamping to 0 instead would fold that geometry into row 0.
        int y0 = std::clamp(static_cast<int>(std::floor(tmin.y * inv_cs)), -1, height_ - 1);
        int y1 = std::clamp(static_cast<int>(std::floor(tmax.y * inv_cs)), 0, height_ - 1);

        // Polygons are in heightfield-local coordinates; a triangle clipped by four planes has
        // at most 7 vertices.
        glm::vec3 in[12], row[12], rest[12], cell[12], rowrest[12];
        int nin = 3;
        in[0] = v0 - bmin_;
        in[1] = v1 - bmin_;
        in[2] = v2 - bmin_;

        for (int y = y0; y <= y1; ++y) {
            int nrow = 0, nrest = 0;
            float cy = static_cast<float>(y + 1) * cs_;
            divide_poly_(in, nin, row, nrow, rest, nrest, cy, 1);
            std::copy(rest, rest + nrest, in);
            nin = nrest;
            if (nrow < 3 || y < 0) continue;

            float minx = row[0].x, maxx = row[0].x;
            for (int i = 1; i < nrow; ++i) {
                minx = std::min(minx, row[i].x);
                maxx = std::max(maxx, row[i].x);
            }
            if (maxx < 0.0f || minx > w) continue;
            int x0 = std::clamp(static_cast<int>(std::floor(minx * inv_cs)), -1, width_ - 1);
            int x1 = std::clamp(static_cast<int>(std::floor(maxx * inv_cs)), 0, width_ - 1);

            for (int x = x0; x <= x1; ++x) {
                int ncell = 0, nrr = 0;
                float cx = static_cast<float>(x + 1) * cs_;
                divide_poly_(row, nrow, cell, ncell, rowrest, nrr, cx, 0);
                std::copy(rowrest, rowrest + nrr, row);
                nrow = nrr;
                if (ncell < 3 || x < 0) continue;

                float zmin = cell[0].z, zmax = cell[0].z;
                for (int i = 1; i < ncell; ++i) {
                    zmin = std::min(zmin, cell[i].z);
                    zmax = std::max(zmax, cell[i].z);
                }
                // The epsilon keeps a surface lying exactly on a quantum boundary (a floor at
                // z = 0 with a 0.1 cell height) from rounding a whole quantum up.
                int smin = static_cast<int>(std::floor(zmin * inv_ch + 1e-3f));
                int smax = static_cast<int>(std::ceil(zmax * inv_ch - 1e-3f));
                if (smax < 0 || smin > k_max_span_height) continue;
                smin = std::clamp(smin, 0, k_max_span_height);
                smax = std::clamp(smax, 0, k_max_span_height);
                // Spans are at least one quantum thick. Grow a flat one DOWNWARD: its top is
                // the floor an agent stands on, and must not rise.
                if (smax <= smin) {
                    if (smax > 0) smin = smax - 1;
                    else smax = smin + 1;
                }
                add_span(x, y, static_cast<uint16_t>(smin), static_cast<uint16_t>(smax), area, merge_threshold);
            }
        }
    }

    /**
     * @brief Rasterizes a triangle soup (3 vertices per triangle). Triangles steeper than
     *        `walkable_cos` (cos of the max slope, against |normal.z| so either winding works)
     *        rasterize as k_area_null: they still block, nobody stands on them.
     */
    void rasterize_triangles(const std::vector<glm::vec3>& verts, const std::vector<uint8_t>& tri_areas,
                             float walkable_cos, int merge_threshold) {
        std::size_t tri_count = verts.size() / 3;
        for (std::size_t t = 0; t < tri_count; ++t) {
            const glm::vec3& a = verts[t * 3 + 0];
            const glm::vec3& b = verts[t * 3 + 1];
            const glm::vec3& c = verts[t * 3 + 2];
            glm::vec3 n = glm::cross(b - a, c - a);
            float len = glm::length(n);
            uint8_t area = tri_areas[t];
            if (len <= 1e-12f || std::fabs(n.z) / len < walkable_cos) area = k_area_null;
            rasterize_triangle(a, b, c, area, merge_threshold);
        }
    }

    /**
     * @brief Recast's low-hanging-obstacle filter: an unwalkable span whose top sits within
     *        `climb` of the walkable span below it becomes walkable (curbs, stair nosings, the
     *        top edge of a step whose riser is a steep triangle).
     * @param areas Per-pool-index area output (resized here); the heightfield is not modified,
     *        so several agent profiles can filter the same rasterization.
     */
    void filter_low_hanging_obstacles(int climb, std::vector<uint8_t>& areas) const {
        areas.resize(pool_.size());
        for (std::size_t i = 0; i < pool_.size(); ++i) areas[i] = pool_[i].area;
        for (std::size_t col = 0; col < heads_.size(); ++col) {
            bool prev_walkable = false;
            uint8_t prev_area = k_area_null;
            uint16_t prev_top = 0;
            for (uint32_t s = heads_[col]; s != k_null; s = pool_[s].next) {
                bool walkable = pool_[s].area != k_area_null;
                if (!walkable && prev_walkable &&
                    static_cast<int>(pool_[s].smax) - static_cast<int>(prev_top) <= climb) {
                    areas[s] = prev_area;
                }
                prev_walkable = walkable;
                prev_area = pool_[s].area;
                prev_top = pool_[s].smax;
            }
        }
    }

private:
    uint32_t alloc_span_() {
        if (free_ != k_null) {
            uint32_t i = free_;
            free_ = pool_[i].next;
            return i;
        }
        pool_.push_back(SolidSpan{});
        return static_cast<uint32_t>(pool_.size() - 1);
    }

    void free_span_(uint32_t i) {
        pool_[i].area = k_area_null;
        pool_[i].smin = pool_[i].smax = 0;
        pool_[i].next = free_;
        free_ = i;
    }

    /** @brief Splits a convex polygon by the plane `p[axis] == x`: `below` gets the part with
     *         p[axis] <= x, `above` the rest. */
    static void divide_poly_(const glm::vec3* in, int nin, glm::vec3* below, int& nbelow, glm::vec3* above,
                             int& nabove, float x, int axis) {
        float d[12];
        for (int i = 0; i < nin; ++i) d[i] = x - in[i][axis];
        int m = 0, n = 0;
        for (int i = 0, j = nin - 1; i < nin; j = i, ++i) {
            bool ina = d[j] >= 0.0f;
            bool inb = d[i] >= 0.0f;
            if (ina != inb) {
                float s = d[j] / (d[j] - d[i]);
                glm::vec3 p = in[j] + (in[i] - in[j]) * s;
                below[m++] = p;
                above[n++] = p;
                if (d[i] > 0.0f) below[m++] = in[i];
                else if (d[i] < 0.0f) above[n++] = in[i];
            } else {
                if (d[i] >= 0.0f) {
                    below[m++] = in[i];
                    if (d[i] != 0.0f) continue;
                }
                above[n++] = in[i];
            }
        }
        nbelow = m;
        nabove = n;
    }

    int width_ = 0;
    int height_ = 0;
    glm::vec3 bmin_{0.0f};
    float cs_ = 0.25f;
    float ch_ = 0.1f;
    std::vector<uint32_t> heads_;
    std::vector<SolidSpan> pool_;
    uint32_t free_ = k_null;
};

} // namespace nav
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_NAV_HEIGHTFIELD_H

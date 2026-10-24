/**
 * @file caster_grid.h
 * @brief A uniform world-XY grid over draw items, so a view's culling loop tests the handful
 *        of cells its frustum touches instead of every item in the scene.
 *
 * Pure CPU math over glm types in the style of visibility.h -- no Vulkan, no scene -- rebuilt
 * from MeshGather's bounds once per frame and queried once per view (camera, each directional
 * shadow cascade, each local-light view). Cells are XY columns because every query volume is
 * either a camera frustum or a sun-aligned orthographic box: both have a compact world-XY
 * footprint, while their Z extent says little about what they contain.
 *
 * Candidates are a SUPERSET: a cell passes a conservative box test and its items still go
 * through the caller's exact per-item tests (Frustum::intersects, projected_texels, filters).
 * The grid never drops an item a flat loop would draw -- it only skips items whose whole cell
 * provably cannot intersect the frustum.
 */

#ifndef TOYENGINE_RENDER_CASTER_GRID_H
#define TOYENGINE_RENDER_CASTER_GRID_H

#include "visibility.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace toy {
namespace render {

/// A world-XY rectangle, the footprint form every grid query takes.
struct XyRect {
    glm::vec2 min{0.0f};
    glm::vec2 max{0.0f};
};

/**
 * @brief World-XY footprint of a view-projection's frustum: the 8 clip-space corners pulled
 *        back to world space ([0,1] depth range, matching Frustum::from_matrix).
 *
 * Returns an unbounded rectangle when the matrix is singular or a corner lands at (or
 * behind) w = 0 -- CasterGrid::for_candidates treats that as "visit every occupied cell",
 * which keeps the result a superset instead of guessing.
 */
inline XyRect view_proj_xy_rect(const glm::mat4& view_proj) {
    constexpr float kHuge = std::numeric_limits<float>::max();
    const XyRect unbounded{glm::vec2(-kHuge), glm::vec2(kHuge)};
    const glm::mat4 inv = glm::inverse(view_proj);
    XyRect r{glm::vec2(kHuge), glm::vec2(-kHuge)};
    for (int i = 0; i < 8; ++i) {
        const glm::vec4 ndc(i & 1 ? 1.0f : -1.0f, i & 2 ? 1.0f : -1.0f, i & 4 ? 1.0f : 0.0f, 1.0f);
        const glm::vec4 world = inv * ndc;
        if (!(world.w > 1e-6f) || !std::isfinite(world.x) || !std::isfinite(world.y)) {
            return unbounded;
        }
        const glm::vec2 p = glm::vec2(world) / world.w;
        r.min = glm::min(r.min, p);
        r.max = glm::max(r.max, p);
    }
    return r;
}

/**
 * @brief Uniform XY grid of item indices, rebuilt per frame, queried per view.
 *
 * Each item lands in the single cell holding its bounds centre; items wider than one cell in
 * XY (terrain-scale planes, water) go to a flat `large` list that every query visits, which
 * bounds how far a cell's contents can poke past its edges. The cell size defaults to the
 * terrain chunk pitch so one streamed chunk is roughly one cell.
 */
class CasterGrid {
public:
    /**
     * @brief Rebuilds the grid from this frame's item bounds. Items with `valid[i] == 0` are
     *        skipped; every other index appears in exactly one cell or in `large`.
     *
     * @param bounds    Per-item world bounds (MeshGather::bounds).
     * @param valid     Per-item validity flags (MeshGather::valid); sizes must match.
     * @param cell_size Cell pitch in metres; also the XY half-extent above which an item is
     *                  filed as `large` instead of in a cell.
     */
    void rebuild(const std::vector<WorldBounds>& bounds, const std::vector<uint8_t>& valid,
                 float cell_size = 32.0f) {
        _cell_size = cell_size;
        _cells.clear();
        _large.clear();
        const size_t n = bounds.size();
        for (size_t i = 0; i < n; ++i) {
            if (!valid[i]) continue;
            const WorldBounds& b = bounds[i];
            if (std::max(b.extent.x, b.extent.y) > cell_size) {
                _large.push_back(static_cast<uint32_t>(i));
                continue;
            }
            Cell& cell = _cells[key_for_(b.center)];
            cell.items.push_back(static_cast<uint32_t>(i));
            cell.pad  = std::max(cell.pad, std::max(b.extent.x, b.extent.y));
            cell.zmin = std::min(cell.zmin, b.center.z - b.extent.z);
            cell.zmax = std::max(cell.zmax, b.center.z + b.extent.z);
        }
    }

    /**
     * @brief Visits every item that may intersect `f`, as `fn(uint32_t item_index)`.
     *
     * Cells overlapping `rect` (grown by one cell, covering centre-filed items that lean out
     * of their cell) are box-tested against `f` and their items visited only on a pass; items
     * in `large` are always visited. When `rect` spans more cells than the grid holds -- an
     * unbounded camera footprint -- every occupied cell is box-tested instead, so the result
     * stays a superset at flat cost.
     *
     * @param f    The view's frustum; cells it provably misses are skipped wholesale.
     * @param rect The view's world-XY footprint (view_proj_xy_rect, or a light's range box).
     * @param fn   Callback receiving each candidate item index; callers run their exact
     *             per-item tests inside it.
     * @return Number of cells box-tested (the F3 overlay's cull_cells_visited).
     */
    template <class F>
    uint32_t for_candidates(const Frustum& f, const XyRect& rect, F&& fn) const {
        for (const uint32_t i : _large) fn(i);
        uint32_t visited = 0;
        auto visit_cell = [&](int64_t cx, int64_t cy, const Cell& cell) {
            ++visited;
            WorldBounds box;
            const float half = 0.5f * _cell_size + cell.pad;
            box.center = glm::vec3((static_cast<float>(cx) + 0.5f) * _cell_size,
                                   (static_cast<float>(cy) + 0.5f) * _cell_size,
                                   0.5f * (cell.zmin + cell.zmax));
            box.extent = glm::vec3(half, half, 0.5f * (cell.zmax - cell.zmin));
            if (!f.intersects(box)) return;
            for (const uint32_t i : cell.items) fn(i);
        };
        // One extra cell on each side: an item sits in the cell of its CENTRE, so a cell's
        // contents reach at most `pad <= cell_size` past its edge into a neighbour.
        const int64_t x0 = cell_coord_(rect.min.x) - 1, x1 = cell_coord_(rect.max.x) + 1;
        const int64_t y0 = cell_coord_(rect.min.y) - 1, y1 = cell_coord_(rect.max.y) + 1;
        const uint64_t span_x = static_cast<uint64_t>(x1 - x0 + 1);
        const uint64_t span_y = static_cast<uint64_t>(y1 - y0 + 1);
        if (span_x == 0 || span_y == 0 || span_x * span_y > _cells.size()) {
            for (const auto& [key, cell] : _cells) {
                visit_cell(static_cast<int32_t>(key >> 32), static_cast<int32_t>(key), cell);
            }
            return visited;
        }
        for (int64_t cy = y0; cy <= y1; ++cy) {
            for (int64_t cx = x0; cx <= x1; ++cx) {
                const auto it = _cells.find(pack_(cx, cy));
                if (it != _cells.end()) visit_cell(cx, cy, it->second);
            }
        }
        return visited;
    }

    /// Items too wide for a cell -- every query visits them.
    const std::vector<uint32_t>& large_items() const { return _large; }

    /// Number of occupied cells (tests / diagnostics).
    size_t cell_count() const { return _cells.size(); }

private:
    struct Cell {
        std::vector<uint32_t> items;
        float pad  = 0.0f;   // Widest XY half-extent filed here; grows the cell's test box.
        float zmin = std::numeric_limits<float>::max();
        float zmax = std::numeric_limits<float>::lowest();
    };

    int64_t cell_coord_(float v) const {
        // Clamp before floor: an unbounded footprint must not overflow the int conversion.
        const double c = std::floor(static_cast<double>(v) / _cell_size);
        const double lim = 1.0e9;
        return static_cast<int64_t>(std::max(-lim, std::min(lim, c)));
    }

    uint64_t key_for_(const glm::vec3& center) const {
        return pack_(cell_coord_(center.x), cell_coord_(center.y));
    }

    static uint64_t pack_(int64_t cx, int64_t cy) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(cx))) << 32) |
               static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(cy)));
    }

    float _cell_size = 32.0f;
    std::unordered_map<uint64_t, Cell> _cells;
    std::vector<uint32_t> _large;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_CASTER_GRID_H

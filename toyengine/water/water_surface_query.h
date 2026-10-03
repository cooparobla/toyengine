/**
 * @file water_surface_query.h
 * @brief CPU-side "where is the water" structure for one water body: its baked, undisplaced,
 *        world-space surface triangles bucketed into a uniform XY grid, plus the Gerstner waves
 *        layered on top -- what buoyancy (and any gameplay query) samples.
 *
 * One structure serves both kinds of water: a planar lake is simply a mesh whose baked flow is
 * zero, a river a mesh whose baked flow follows its slope (see water_flow_bake.h). Sampling is
 * a grid-cell lookup plus a handful of 2D barycentric tests, then the same wave sum the vertex
 * shader evaluates (water_waves.h), attenuated by the same baked depth.
 *
 * The surface is treated as a height field over XY: fine for lakes, oceans and rivers, which
 * never overhang. A point outside every triangle (in XY) is "not over this water body".
 */

#ifndef TOYENGINE_WATER_WATER_SURFACE_QUERY_H
#define TOYENGINE_WATER_WATER_SURFACE_QUERY_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/water/water_waves.h>

namespace toy {
namespace water {

/** @brief One baked surface vertex, world space, undisplaced by waves. */
struct WaterVertex {
    glm::vec3 position{0.0f};
    glm::vec3 flow{0.0f};      ///< World-space surface current (m/s).
    float     depth = 0.0f;    ///< Water depth below this vertex (m); see WaterSystem's bake.
    float     turbulence = 0.0f; ///< 0..1 whitewater hint (rapids, obstacle wakes).
};

/** @brief The undisplaced surface interpolated at an XY point. */
struct WaterBaseSample {
    float     height = 0.0f;
    glm::vec3 flow{0.0f};
    float     depth = 0.0f;
    float     turbulence = 0.0f;
};

/** @brief Full surface query result: base surface plus waves. */
struct WaterSample {
    float     surface_height = 0.0f; ///< World Z of the (wave-displaced) surface above the query XY.
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec3 flow{0.0f};            ///< Surface current (m/s), world space.
    float     depth = 0.0f;          ///< Bed depth below the undisplaced surface (m).
    float     turbulence = 0.0f;
};

/**
 * @class WaterSurfaceQuery
 * @brief Uniform XY grid over a water body's baked surface triangles.
 */
class WaterSurfaceQuery {
public:
    /**
     * @brief Builds the grid. `indices` are triangle triples into `vertices` (both world space).
     *        Cell size targets ~2 triangles per cell.
     */
    void build(std::vector<WaterVertex> vertices, std::vector<uint32_t> indices) {
        vertices_ = std::move(vertices);
        indices_  = std::move(indices);
        cell_start_.clear();
        cell_tris_.clear();
        if (vertices_.empty() || indices_.size() < 3) {
            valid_ = false;
            return;
        }
        min_ = glm::vec3(std::numeric_limits<float>::max());
        max_ = glm::vec3(std::numeric_limits<float>::lowest());
        for (const WaterVertex& v : vertices_) {
            min_ = glm::min(min_, v.position);
            max_ = glm::max(max_, v.position);
        }
        const std::size_t tri_count = indices_.size() / 3;
        glm::vec2 extent = glm::max(glm::vec2(max_ - min_), glm::vec2(1e-3f));
        float cell = std::sqrt(extent.x * extent.y / static_cast<float>(tri_count) * 2.0f);
        cell = std::max(cell, 1e-3f);
        dims_ = glm::ivec2(std::clamp(static_cast<int>(std::ceil(extent.x / cell)), 1, 1024),
                           std::clamp(static_cast<int>(std::ceil(extent.y / cell)), 1, 1024));
        inv_cell_ = glm::vec2(dims_) / extent;

        // Counting sort into CSR: one pass to count, one to fill.
        std::vector<uint32_t> counts(static_cast<std::size_t>(dims_.x * dims_.y) + 1, 0u);
        auto for_cells = [&](std::size_t t, auto&& fn) {
            glm::vec2 a(vertices_[indices_[t * 3 + 0]].position);
            glm::vec2 b(vertices_[indices_[t * 3 + 1]].position);
            glm::vec2 c(vertices_[indices_[t * 3 + 2]].position);
            glm::ivec2 lo = cell_of_(glm::min(a, glm::min(b, c)));
            glm::ivec2 hi = cell_of_(glm::max(a, glm::max(b, c)));
            for (int y = lo.y; y <= hi.y; ++y)
                for (int x = lo.x; x <= hi.x; ++x) fn(static_cast<std::size_t>(y * dims_.x + x));
        };
        for (std::size_t t = 0; t < tri_count; ++t) for_cells(t, [&](std::size_t c) { ++counts[c + 1]; });
        for (std::size_t i = 1; i < counts.size(); ++i) counts[i] += counts[i - 1];
        cell_start_ = counts;
        cell_tris_.resize(counts.back());
        std::vector<uint32_t> cursor(counts.begin(), counts.end() - 1);
        for (std::size_t t = 0; t < tri_count; ++t) {
            for_cells(t, [&](std::size_t c) { cell_tris_[cursor[c]++] = static_cast<uint32_t>(t); });
        }
        valid_ = true;
    }

    bool valid() const { return valid_; }
    const glm::vec3& bounds_min() const { return min_; }
    const glm::vec3& bounds_max() const { return max_; }
    const std::vector<WaterVertex>& vertices() const { return vertices_; }
    const std::vector<uint32_t>& indices() const { return indices_; }

    /** @brief True if XY `p` (expanded by `margin`) overlaps this body's XY bounds. */
    bool overlaps_xy(const glm::vec2& lo, const glm::vec2& hi) const {
        return valid_ && hi.x >= min_.x && lo.x <= max_.x && hi.y >= min_.y && lo.y <= max_.y;
    }

    /** @brief Interpolates the undisplaced surface at XY `p`; false if `p` is off the surface. */
    bool sample_base(const glm::vec2& p, WaterBaseSample& out) const {
        if (!valid_ || p.x < min_.x || p.y < min_.y || p.x > max_.x || p.y > max_.y) return false;
        glm::ivec2 c = cell_of_(p);
        std::size_t cell = static_cast<std::size_t>(c.y * dims_.x + c.x);
        for (uint32_t i = cell_start_[cell]; i < cell_start_[cell + 1]; ++i) {
            uint32_t t = cell_tris_[i];
            const WaterVertex& a = vertices_[indices_[t * 3 + 0]];
            const WaterVertex& b = vertices_[indices_[t * 3 + 1]];
            const WaterVertex& d = vertices_[indices_[t * 3 + 2]];
            glm::vec3 w;
            if (!barycentric_xy_(glm::vec2(a.position), glm::vec2(b.position), glm::vec2(d.position), p, w)) continue;
            out.height     = w.x * a.position.z + w.y * b.position.z + w.z * d.position.z;
            out.flow       = w.x * a.flow + w.y * b.flow + w.z * d.flow;
            out.depth      = w.x * a.depth + w.y * b.depth + w.z * d.depth;
            out.turbulence = w.x * a.turbulence + w.y * b.turbulence + w.z * d.turbulence;
            return true;
        }
        return false;
    }

    /**
     * @brief The wave-displaced surface above XY `p` at time `t`. Mirrors the vertex shader:
     *        the wave sum is evaluated at the undisplaced point p0 whose displaced position lands
     *        on `p` (fixed-point inversion, see water_waves.h's height_at()), with p0's own baked
     *        depth attenuation. False if `p` is off the surface.
     */
    bool sample(const glm::vec2& p, const WaveParams& waves, float t, WaterSample& out) const {
        WaterBaseSample base;
        if (!sample_base(p, base)) return false;
        out.surface_height = base.height;
        out.normal         = glm::vec3(0.0f, 0.0f, 1.0f);
        out.flow           = base.flow;
        out.depth          = base.depth;
        out.turbulence     = base.turbulence;
        if (waves.calm()) return true;

        glm::vec2 p0 = p;
        WaterBaseSample b0 = base;
        WaveSample s;
        for (int i = 0; i < 3; ++i) {
            s = evaluate(waves, p0, t, depth_attenuation(b0.depth, waves.wavelength));
            glm::vec2 next = p - glm::vec2(s.displacement);
            WaterBaseSample bn;
            if (!sample_base(next, bn)) break; // near the mesh edge: keep the last valid p0
            p0 = next;
            b0 = bn;
        }
        s = evaluate(waves, p0, t, depth_attenuation(b0.depth, waves.wavelength));
        out.surface_height = b0.height + s.displacement.z;
        out.normal         = s.normal;
        return true;
    }

private:
    glm::ivec2 cell_of_(const glm::vec2& p) const {
        glm::vec2 f = (p - glm::vec2(min_)) * inv_cell_;
        return glm::clamp(glm::ivec2(glm::floor(f)), glm::ivec2(0), dims_ - 1);
    }

    static bool barycentric_xy_(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c,
                                const glm::vec2& p, glm::vec3& w) {
        glm::vec2 v0 = b - a, v1 = c - a, v2 = p - a;
        float den = v0.x * v1.y - v1.x * v0.y;
        if (std::abs(den) < 1e-12f) return false;
        float v = (v2.x * v1.y - v1.x * v2.y) / den;
        float u = (v0.x * v2.y - v2.x * v0.y) / den;
        constexpr float eps = -1e-5f;
        if (v < eps || u < eps || v + u > 1.0f - eps) return false;
        w = glm::vec3(1.0f - v - u, v, u);
        return true;
    }

    bool valid_ = false;
    std::vector<WaterVertex> vertices_;
    std::vector<uint32_t>    indices_;
    std::vector<uint32_t>    cell_start_;
    std::vector<uint32_t>    cell_tris_;
    glm::vec3  min_{0.0f}, max_{0.0f};
    glm::ivec2 dims_{1, 1};
    glm::vec2  inv_cell_{1.0f};
};

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_WATER_SURFACE_QUERY_H

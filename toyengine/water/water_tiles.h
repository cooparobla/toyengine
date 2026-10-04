/**
 * @file water_tiles.h
 * @brief Splits a baked water surface into render tiles, each with distance LODs -- pure CPU, no
 *        GPU dependency (the result is MeshCpuData, uploaded by WaterSystem), so it is
 *        unit-testable on its own.
 *
 * Why. One mesh per water body is culled all-or-nothing and drawn at full density however far
 * away it is. Tiles let the renderer's frustum culling drop what is off screen, and each tile
 * carries its own LOD chain for the renderer's screen-size LOD selection (render/visibility.h).
 *
 * Procedural grids (WaterBody without mesh geometry) are cut into square tiles of 2^n quads.
 * LOD k keeps every 2^k-th grid row and column (always including the tile's last), so its
 * vertices are an exact SUBSET of LOD 0's: they carry LOD 0's baked depth, flow and turbulence
 * with no further baking, and two neighbouring tiles at any LODs still share their corners.
 *
 * Where LOD k may start. Two conditions, both as a distance:
 *  - Pixels: the level's vertex spacing subtends under 1/k_pixel_lod_factor radians. Over water
 *    seen at a grazing angle, triangles foreshorten to a fraction of that on screen, and
 *    sub-pixel triangles are what make the heavy water fragment shader expensive.
 *  - Cracks: mid-edge vertices of a finer neighbour are T-junctions against a coarser tile,
 *    displaced by waves the coarse edge interpolates linearly. The gap is the interpolation
 *    error of each wave over the coarse spacing, scaled by its distance fade
 *    (water_waves.h's wave_distance_fade()). It must stay under half a pixel there, so no
 *    skirts are needed (skirts would show through transparent water).
 * `lod_bias` (> 1 coarsens sooner) scales both. The renderer selects LODs by projected size, so
 * each distance is converted with an assumed 60-degree vertical field of view at 1080p; another
 * camera only shifts the switch distances.
 *
 * Arbitrary meshes (rivers, lake outlines) are bucketed into tiles by triangle centroid, and
 * each tile's LODs are simplified with meshoptimizer: meshopt_simplify keeps a SUBSET of the
 * tile's vertices (so baked attributes stay exact) and LockBorder pins every tile edge, so
 * neighbouring tiles stay stitched at any LOD pair. The same start-distance rule applies, with
 * the level's spacing estimated from its triangle count.
 */

#ifndef TOYENGINE_WATER_WATER_TILES_H
#define TOYENGINE_WATER_WATER_TILES_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <meshoptimizer.h>

#include <gfxcoopa/engine/data/mesh.h>

#include <toyengine/water/water_waves.h>

namespace toy {
namespace water {

/** @brief One render tile: its grid coordinate and its mesh (with LODs), in the body's local space. */
struct WaterTile {
    glm::ivec2 coord{0};
    coopa::gfx::engine::data::MeshCpuData data;
};

/** @brief Inputs to the LOD switch distances; see the file doc. */
struct WaterTileLodParams {
    float wave_lambdas[4] = {0.0f, 0.0f, 0.0f, 0.0f}; ///< Derived wavelengths, mesh units (0 = calm).
    float wave_amps[4]    = {0.0f, 0.0f, 0.0f, 0.0f}; ///< Their amplitudes, mesh units.
    float lod_bias = 1.0f;          ///< WaterSettings::lod_bias.
    int   max_lods = 6;
};

/// A LOD's vertex spacing must subtend under 1/this radians (~9 pixels at 1080p and a 60-degree
/// field of view, before a grazing view foreshortens it much further).
inline constexpr float k_pixel_lod_factor = 120.0f;
/// One pixel, in radians, at 1080p and a 60-degree vertical field of view.
inline constexpr float k_pixel_angle = 1.0f / 1030.0f;
/// proj[1][1] of the assumed 60-degree vertical field of view (1 / tan(30 deg)).
inline constexpr float k_assumed_p11 = 1.7320508f;

/**
 * @brief Worst T-junction gap against a LOD of vertex spacing `spacing` seen from `distance`:
 *        each wave's linear-interpolation error over the spacing, A (1 - cos(pi s / lambda)),
 *        times its distance fade.
 */
inline float water_lod_crack(float spacing, float distance, const WaterTileLodParams& p) {
    constexpr float pi = 3.14159265f;
    float e = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float lambda = p.wave_lambdas[i];
        if (lambda <= 0.0f || p.wave_amps[i] <= 0.0f) continue;
        const float x = std::min(pi * spacing / lambda, pi);
        e += p.wave_amps[i] * (1.0f - std::cos(x)) * wave_distance_fade(lambda, distance);
    }
    return e;
}

/**
 * @brief The nearest distance at which a LOD of vertex spacing `spacing` may be drawn: the
 *        pixel criterion met, and its T-junction gap under half a pixel (see the file doc).
 */
inline float water_lod_distance(float spacing, const WaterTileLodParams& p) {
    const float bias = std::max(p.lod_bias, 1e-3f);
    float d = spacing * k_pixel_lod_factor / bias;
    for (int i = 0; i < 400 && water_lod_crack(spacing, d, p) > 0.5f * k_pixel_angle * d * bias; ++i) d *= 1.05f;
    return d;
}

namespace detail {

/** @brief Fills bounds (inflated by `inflate` for the wave displacement the vertex shader adds)
 *         and rewinds every triangle of `d` so its normal faces local +Z (TransparentPass culls
 *         back faces). */
inline void finish_tile_(coopa::gfx::engine::data::MeshCpuData& d, const glm::vec3& inflate) {
    d.bounds_min = glm::vec3(std::numeric_limits<float>::max());
    d.bounds_max = glm::vec3(std::numeric_limits<float>::lowest());
    for (const auto& v : d.vertices) {
        d.bounds_min = glm::min(d.bounds_min, v.position);
        d.bounds_max = glm::max(d.bounds_max, v.position);
    }
    d.bounds_min -= inflate;
    d.bounds_max += inflate;
    auto rewind = [&](uint32_t first, uint32_t count, int32_t base) {
        for (uint32_t t = first; t + 2 < first + count; t += 3) {
            const glm::vec3& a = d.vertices[base + d.indices[t]].position;
            const glm::vec3& b = d.vertices[base + d.indices[t + 1]].position;
            const glm::vec3& c = d.vertices[base + d.indices[t + 2]].position;
            if (glm::cross(b - a, c - a).z < 0.0f) std::swap(d.indices[t + 1], d.indices[t + 2]);
        }
    };
    if (d.lods.empty()) {
        rewind(0, static_cast<uint32_t>(d.indices.size()), 0);
    } else {
        for (const auto& l : d.lods) rewind(l.first_index, l.index_count, l.vertex_offset);
    }
}

/** @brief Grid lines from `a` to `b` every `step`, always ending on `b`. */
inline std::vector<int> grid_lines_(int a, int b, int step) {
    std::vector<int> out;
    for (int x = a; x < b; x += step) out.push_back(x);
    out.push_back(b);
    return out;
}

} // namespace detail

/**
 * @brief Tiles a procedural grid of `res_x` x `res_y` quads whose (res_x+1)*(res_y+1) vertices
 *        are row-major in `verts` (LOD 0, fully baked), `quads_per_tile` per tile side.
 * @param spacing Vertex spacing of LOD 0, in the same (local) units as `lod`'s wavelengths.
 * @param inflate Bounds inflation for the wave displacement.
 */
inline std::vector<WaterTile> build_grid_tiles(const std::vector<coopa::gfx::engine::data::Vertex>& verts,
                                               int res_x, int res_y, int quads_per_tile, float spacing,
                                               const WaterTileLodParams& lod, const glm::vec3& inflate) {
    using coopa::gfx::engine::data::MeshLod;
    std::vector<WaterTile> tiles;
    const int row = res_x + 1;
    if (res_x < 1 || res_y < 1 || static_cast<int>(verts.size()) != row * (res_y + 1)) return tiles;
    const int qpt = std::max(quads_per_tile, 1);
    for (int ty = 0; ty * qpt < res_y; ++ty) {
        for (int tx = 0; tx * qpt < res_x; ++tx) {
            const int x0 = tx * qpt, x1 = std::min(x0 + qpt, res_x);
            const int y0 = ty * qpt, y1 = std::min(y0 + qpt, res_y);
            WaterTile tile;
            tile.coord = glm::ivec2(tx, ty);
            auto& d = tile.data;
            float threshold_r = 0.0f; // bounding radius for the screen-size thresholds
            for (int k = 0, step = 1; k < lod.max_lods; ++k, step *= 2) {
                // Stop once a level would have fewer than 2 quads a side (LOD 0 always exists).
                if (k > 0 && ((x1 - x0) / step < 2 || (y1 - y0) / step < 2)) break;
                const std::vector<int> xs = detail::grid_lines_(x0, x1, step);
                const std::vector<int> ys = detail::grid_lines_(y0, y1, step);
                MeshLod l;
                l.vertex_offset = static_cast<int32_t>(d.vertices.size());
                l.first_index = static_cast<uint32_t>(d.indices.size());
                for (int y : ys)
                    for (int x : xs) d.vertices.push_back(verts[static_cast<std::size_t>(y * row + x)]);
                const uint32_t w = static_cast<uint32_t>(xs.size());
                for (uint32_t j = 0; j + 1 < ys.size(); ++j) {
                    for (uint32_t i = 0; i + 1 < w; ++i) {
                        uint32_t i0 = j * w + i, i1 = i0 + 1, i2 = i0 + w, i3 = i2 + 1;
                        d.indices.insert(d.indices.end(), {i0, i1, i3, i0, i3, i2});
                    }
                }
                l.index_count = static_cast<uint32_t>(d.indices.size()) - l.first_index;
                if (k == 0) {
                    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
                    for (const auto& v : d.vertices) { lo = glm::min(lo, v.position); hi = glm::max(hi, v.position); }
                    threshold_r = 0.5f * glm::length(hi - lo + 2.0f * inflate);
                } else {
                    // screen_height_fraction == r * p11 / distance: use LOD k below the size it
                    // has at its start distance.
                    const float dist = water_lod_distance(spacing * static_cast<float>(step), lod);
                    l.screen_size = threshold_r * k_assumed_p11 / std::max(dist, 1e-3f);
                }
                d.lods.push_back(std::move(l));
            }
            detail::finish_tile_(d, inflate);
            tiles.push_back(std::move(tile));
        }
    }
    return tiles;
}

namespace detail {

/** @brief Appends simplified LODs (vertex subsets, borders locked) to a single-level tile. */
inline void simplify_tile_lods_(coopa::gfx::engine::data::MeshCpuData& d, const WaterTileLodParams& lod,
                                const glm::vec3& inflate) {
    using coopa::gfx::engine::data::MeshLod;
    using coopa::gfx::engine::data::Vertex;
    const uint32_t base_count = static_cast<uint32_t>(d.indices.size());
    if (base_count < 3 * 8) return; // too small to be worth a chain
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    float area = 0.0f;
    for (const auto& v : d.vertices) { lo = glm::min(lo, v.position); hi = glm::max(hi, v.position); }
    for (uint32_t t = 0; t + 2 < base_count; t += 3) {
        const glm::vec3 a = d.vertices[d.indices[t]].position;
        area += 0.5f * glm::length(glm::cross(d.vertices[d.indices[t + 1]].position - a,
                                              d.vertices[d.indices[t + 2]].position - a));
    }
    // Mean edge of an equilateral-ish triangle of the mean area.
    const float spacing0 = std::sqrt(2.0f * area / static_cast<float>(base_count / 3));
    const float radius = 0.5f * glm::length(hi - lo + 2.0f * inflate);
    d.lods.push_back({0u, base_count, 0, 0.0f, {}});
    std::vector<uint32_t> base(d.indices.begin(), d.indices.end());
    uint32_t prev = base_count;
    for (int k = 1; k < lod.max_lods; ++k) {
        const std::size_t target = std::max<std::size_t>(3, static_cast<std::size_t>(base_count >> k) / 3 * 3);
        std::vector<uint32_t> out(base_count);
        const std::size_t count = meshopt_simplify(out.data(), base.data(), base.size(), &d.vertices[0].position.x,
                                                   d.vertices.size(), sizeof(Vertex), target, 0.02f,
                                                   meshopt_SimplifyLockBorder, nullptr);
        // Stop when the simplifier can no longer make real progress (borders are locked).
        if (count < 3 || static_cast<float>(count) > 0.8f * static_cast<float>(prev)) break;
        out.resize(count);
        MeshLod l;
        l.first_index = static_cast<uint32_t>(d.indices.size());
        l.index_count = static_cast<uint32_t>(count);
        const float spacing = spacing0 * std::sqrt(static_cast<float>(base_count) / static_cast<float>(count));
        l.screen_size = radius * k_assumed_p11 / std::max(water_lod_distance(spacing, lod), 1e-3f);
        d.indices.insert(d.indices.end(), out.begin(), out.end());
        d.lods.push_back(std::move(l));
        prev = static_cast<uint32_t>(count);
    }
    if (d.lods.size() == 1) d.lods.clear(); // no chain after all: plain single-level mesh
}

} // namespace detail

/**
 * @brief Tiles an arbitrary triangle mesh (`verts`/`indices`, local space) into `tile_size`
 *        squares in local XY by triangle centroid, each with a simplified LOD chain (given
 *        `lod`; null = single level).
 */
inline std::vector<WaterTile> build_mesh_tiles(const std::vector<coopa::gfx::engine::data::Vertex>& verts,
                                               const std::vector<uint32_t>& indices, float tile_size,
                                               const glm::vec3& inflate, const WaterTileLodParams* lod = nullptr) {
    std::vector<WaterTile> tiles;
    if (verts.empty() || indices.size() < 3) return tiles;
    glm::vec2 lo(std::numeric_limits<float>::max());
    for (const auto& v : verts) lo = glm::min(lo, glm::vec2(v.position));
    const float ts = std::max(tile_size, 1e-3f);
    std::map<std::pair<int, int>, std::vector<uint32_t>> buckets; // ordered: deterministic tiles
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const glm::vec2 c = (glm::vec2(verts[indices[t]].position) + glm::vec2(verts[indices[t + 1]].position) +
                             glm::vec2(verts[indices[t + 2]].position)) / 3.0f;
        const glm::ivec2 cell(glm::floor((c - lo) / ts));
        buckets[{cell.y, cell.x}].push_back(static_cast<uint32_t>(t));
    }
    for (auto& [key, tris] : buckets) {
        WaterTile tile;
        tile.coord = glm::ivec2(key.second, key.first);
        auto& d = tile.data;
        std::vector<int32_t> remap(verts.size(), -1);
        for (uint32_t t : tris) {
            for (int k = 0; k < 3; ++k) {
                const uint32_t v = indices[t + k];
                if (remap[v] < 0) {
                    remap[v] = static_cast<int32_t>(d.vertices.size());
                    d.vertices.push_back(verts[v]);
                }
                d.indices.push_back(static_cast<uint32_t>(remap[v]));
            }
        }
        if (lod) detail::simplify_tile_lods_(d, *lod, inflate);
        detail::finish_tile_(d, inflate);
        tiles.push_back(std::move(tile));
    }
    return tiles;
}

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_WATER_TILES_H

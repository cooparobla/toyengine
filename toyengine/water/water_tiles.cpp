#include <toyengine/water/water_tiles.h>

#include <meshoptimizer.h>
#include <toyengine/water/water_waves.h>

namespace toy {
namespace water {

float water_lod_crack(float spacing, float distance, const WaterTileLodParams& p) {
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

float water_lod_distance(float spacing, const WaterTileLodParams& p) {
    const float bias = std::max(p.lod_bias, 1e-3f);
    float d = spacing * k_pixel_lod_factor / bias;
    for (int i = 0; i < 400 && water_lod_crack(spacing, d, p) > 0.5f * k_pixel_angle * d * bias; ++i) d *= 1.05f;
    return d;
}

} // namespace water
} // namespace toy

namespace toy {
namespace water {
namespace detail {

void finish_tile_(coopa::gfx::engine::data::MeshCpuData& d, const glm::vec3& inflate) {
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

std::vector<int> grid_lines_(int a, int b, int step) {
    std::vector<int> out;
    for (int x = a; x < b; x += step) out.push_back(x);
    out.push_back(b);
    return out;
}

} // namespace detail
} // namespace water
} // namespace toy

namespace toy {
namespace water {

std::vector<WaterTile> build_grid_tiles(const std::vector<coopa::gfx::engine::data::Vertex>& verts,
                                               int res_x, int res_y, int quads_per_tile, float spacing,
                                               const WaterTileLodParams& lod, const glm::vec3& inflate,
                                               const ParallelFor* par) {
    using coopa::gfx::engine::data::MeshLod;
    std::vector<WaterTile> tiles;
    const int row = res_x + 1;
    if (res_x < 1 || res_y < 1 || static_cast<int>(verts.size()) != row * (res_y + 1)) return tiles;
    const int qpt = std::max(quads_per_tile, 1);
    const int tiles_x = (res_x + qpt - 1) / qpt, tiles_y = (res_y + qpt - 1) / qpt;
    tiles.resize(static_cast<std::size_t>(tiles_x) * static_cast<std::size_t>(tiles_y));
    // Tiles are independent: each is built into its own slot, row-major as before.
    run_range(par, tiles.size(), [&](std::size_t begin, std::size_t end) {
        for (std::size_t ti = begin; ti < end; ++ti) {
            const int tx = static_cast<int>(ti % static_cast<std::size_t>(tiles_x));
            const int ty = static_cast<int>(ti / static_cast<std::size_t>(tiles_x));
            const int x0 = tx * qpt, x1 = std::min(x0 + qpt, res_x);
            const int y0 = ty * qpt, y1 = std::min(y0 + qpt, res_y);
            WaterTile& tile = tiles[ti];
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
        }
    });
    return tiles;
}

} // namespace water
} // namespace toy

namespace toy {
namespace water {
namespace detail {

void simplify_tile_lods_(coopa::gfx::engine::data::MeshCpuData& d, const WaterTileLodParams& lod,
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
} // namespace water
} // namespace toy

namespace toy {
namespace water {

std::vector<WaterTile> build_mesh_tiles(const std::vector<coopa::gfx::engine::data::Vertex>& verts,
                                               const std::vector<uint32_t>& indices, float tile_size,
                                               const glm::vec3& inflate, const WaterTileLodParams* lod,
                                               const ParallelFor* par) {
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
    // Buckets (in key order) become tiles independently: each into its own slot.
    std::vector<const std::pair<const std::pair<int, int>, std::vector<uint32_t>>*> order;
    order.reserve(buckets.size());
    for (const auto& b : buckets) order.push_back(&b);
    tiles.resize(order.size());
    run_range(par, order.size(), [&](std::size_t begin, std::size_t end) {
        std::vector<int32_t> remap(verts.size(), -1);
        for (std::size_t bi = begin; bi < end; ++bi) {
            const auto& [key, tris] = *order[bi];
            WaterTile& tile = tiles[bi];
            tile.coord = glm::ivec2(key.second, key.first);
            auto& d = tile.data;
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
            // Reset only what this tile touched, for the next tile in this range.
            for (uint32_t t : tris) for (int k = 0; k < 3; ++k) remap[indices[t + k]] = -1;
            if (lod) detail::simplify_tile_lods_(d, *lod, inflate);
            detail::finish_tile_(d, inflate);
        }
    });
    return tiles;
}

} // namespace water
} // namespace toy

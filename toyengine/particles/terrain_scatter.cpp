#include <toyengine/particles/terrain_scatter.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace toy {
namespace particles {

bool TerrainScatterParams::operator==(const TerrainScatterParams& o) const {
    return shape == o.shape && layer == o.layer && seed == o.seed && size.min == o.size.min && size.max == o.size.max &&
           align_to_normal == o.align_to_normal && random_spin == o.random_spin;
}

std::uint32_t TerrainScatterCache::thinned_count(std::uint32_t total, float distance, float max_distance, float fade_start) {
    if (max_distance <= 0.0f || fade_start >= 1.0f) return total;
    const float start = std::clamp(fade_start, 0.0f, 1.0f) * max_distance;
    if (distance <= start) return total;
    if (distance >= max_distance) return 0;
    const float t = (distance - start) / (max_distance - start);
    const float keep = 1.0f - t * t * (3.0f - 2.0f * t);
    return static_cast<std::uint32_t>(std::lround(static_cast<float>(total) * keep));
}

std::size_t TerrainScatterCache::instance_count() const {
    std::size_t n = 0;
    for (const auto& [key, c] : chunks_) n += c.matrices.size();
    return n;
}

TerrainScatterCache::Chunk TerrainScatterCache::build_chunk(const world::TerrainFieldView& terrain, const TerrainScatterParams& p,
                                                            int cx, int cy) {
    Chunk out;
    out.bounds_min = glm::vec3(std::numeric_limits<float>::max());
    out.bounds_max = glm::vec3(std::numeric_limits<float>::lowest());
    const world::TerrainField& field = *terrain.field;
    out.revision = field.chunk_revision(cx, cy);
    const world::TerrainFieldGeometry& g = field.geometry();
    // Chunk size in metres, whichever mode: the field counts chunks in TILES.
    const float cs = static_cast<float>(g.chunk_tiles) * g.tile_extent().x;
    const float spacing = 1.0f / std::sqrt(std::max(p.shape.density, 1e-4f));
    const glm::vec2 lo(static_cast<float>(cx) * cs, static_cast<float>(cy) * cs);
    const glm::vec2 hi = glm::min(lo + glm::vec2(cs), g.size);
    const int nx = std::max(1, static_cast<int>(std::ceil((hi.x - lo.x) / spacing)));
    const int ny = std::max(1, static_cast<int>(std::ceil((hi.y - lo.y) / spacing)));
    const float cos_min = std::cos(glm::radians(std::clamp(p.shape.slope_deg.y, 0.0f, 90.0f)));
    const float cos_max = std::cos(glm::radians(std::clamp(p.shape.slope_deg.x, 0.0f, 90.0f)));
    const float jitter = std::clamp(p.shape.jitter, 0.0f, 1.0f);

    out.matrices.reserve(static_cast<std::size_t>(nx) * ny / 2);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            // One stream per (seed, chunk, cell): placement never depends on build order.
            Rng rng(hash_u32(p.seed ^ hash_u32(static_cast<uint32_t>(cx) * 73856093u ^ static_cast<uint32_t>(cy) * 19349663u)),
                    static_cast<uint64_t>(j) * 65537u + static_cast<uint64_t>(i) + 1u);
            const float ox = 0.5f + (rng.next01() - 0.5f) * jitter;
            const float oy = 0.5f + (rng.next01() - 0.5f) * jitter;
            const float gate = rng.next01();
            const float spin = rng.next01() * 6.28318530718f;
            const float scale_t = rng.next01();
            const float size_t_ = rng.next01();
            const glm::vec2 local = lo + glm::vec2((static_cast<float>(i) + ox) * spacing, (static_cast<float>(j) + oy) * spacing);
            if (local.x >= hi.x || local.y >= hi.y) continue;

            const glm::vec3 n = field.normal_at(local);
            if (n.z < cos_min - 1e-5f || n.z > cos_max + 1e-5f) continue;
            const float h = terrain.origin.z + field.ground_at(local);
            if (h < p.shape.height.x || h > p.shape.height.y) continue;
            if (p.layer >= 0) {
                const float w = field.layer_weight_at(p.layer, local);
                // Thin paint: sparse; full paint: every candidate.
                const float threshold = p.shape.min_weight + (1.0f - p.shape.min_weight) * gate;
                if (w < threshold || w <= 0.0f) continue;
            }

            const float s = p.size.lerp(size_t_) * (p.shape.scale.x + (p.shape.scale.y - p.shape.scale.x) * scale_t);
            const glm::vec3 up = p.align_to_normal ? n : glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::vec3 pos = glm::vec3(glm::vec2(terrain.origin) + local, h) - n * p.shape.sink;
            // Frame about `up`: x from a reference axis, spun.
            glm::vec3 ref = std::abs(up.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            glm::vec3 x = glm::normalize(ref - up * glm::dot(ref, up));
            glm::vec3 y = glm::cross(up, x);
            if (p.random_spin) {
                const float c = std::cos(spin), sn = std::sin(spin);
                const glm::vec3 xr = x * c + y * sn;
                y = glm::cross(up, xr);
                x = xr;
            }
            glm::mat4 m(1.0f);
            m[0] = glm::vec4(x * s, 0.0f);
            m[1] = glm::vec4(y * s, 0.0f);
            m[2] = glm::vec4(up * s, 0.0f);
            m[3] = glm::vec4(pos, 1.0f);
            out.matrices.push_back(m);
            // Bounds: the instance's origin padded by its size (meshes are roughly unit-sized
            // about their origin; tall ones are covered by the generous vertical pad).
            out.bounds_min = glm::min(out.bounds_min, pos - glm::vec3(s, s, s));
            out.bounds_max = glm::max(out.bounds_max, pos + glm::vec3(s, s, 4.0f * s));
        }
    }
    if (out.matrices.empty()) out.bounds_min = out.bounds_max = glm::vec3(0.0f);
    // Shuffled (deterministically), so drawing any prefix -- distance thinning -- takes an even
    // random subset rather than a band of rows.
    Rng shuffle(hash_u32(p.seed * 2654435761u ^ static_cast<uint32_t>(cx) * 40503u ^ static_cast<uint32_t>(cy) * 9973u), 7u);
    for (std::size_t k = out.matrices.size(); k > 1; --k) {
        const std::size_t j = static_cast<std::size_t>(shuffle.next_u32() % static_cast<uint32_t>(k));
        std::swap(out.matrices[k - 1], out.matrices[j]);
    }
    static std::atomic<std::uint64_t> next_build{1};
    out.build_id = next_build.fetch_add(1);
    return out;
}

void TerrainScatterCache::update(const world::TerrainFieldView& terrain, const TerrainScatterParams& params,
                                 const glm::vec3& camera, float max_distance, int budget) {
    if (!terrain) {
        chunks_.clear();
        return;
    }
    const world::TerrainField& field = *terrain.field;
    if (params != params_ || field.uid() != data_uid_ || field.geometry() != geometry_ ||
        terrain.origin != origin_) {
        chunks_.clear();
        params_ = params;
        data_uid_ = field.uid();
        geometry_ = field.geometry();
        origin_ = terrain.origin;
    }

    const glm::ivec2 count = geometry_.chunk_count();
    const float cs = static_cast<float>(geometry_.chunk_tiles) * geometry_.tile_extent().x;
    const glm::vec2 cam = glm::vec2(camera) - glm::vec2(terrain.origin);
    auto chunk_distance = [&](int cx, int cy) {
        const glm::vec2 lo(static_cast<float>(cx) * cs, static_cast<float>(cy) * cs);
        const glm::vec2 d = glm::max(glm::max(lo - cam, cam - (lo + glm::vec2(cs))), glm::vec2(0.0f));
        return glm::length(d);
    };
    const float keep = max_distance > 0.0f ? max_distance * 1.2f + cs : std::numeric_limits<float>::max();
    const float want = max_distance > 0.0f ? max_distance : std::numeric_limits<float>::max();

    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (chunk_distance(it->first.first, it->first.second) > keep) it = chunks_.erase(it);
        else ++it;
    }

    // Missing or stale chunks in range, nearest first.
    std::vector<std::pair<float, std::pair<int, int>>> todo;
    for (int cy = 0; cy < count.y; ++cy) {
        for (int cx = 0; cx < count.x; ++cx) {
            const float d = chunk_distance(cx, cy);
            if (d > want) continue;
            auto it = chunks_.find({cx, cy});
            if (it != chunks_.end() && it->second.revision == field.chunk_revision(cx, cy)) continue;
            todo.push_back({d, {cx, cy}});
        }
    }
    std::sort(todo.begin(), todo.end());
    for (int k = 0; k < static_cast<int>(todo.size()) && k < std::max(1, budget); ++k) {
        const auto key = todo[static_cast<std::size_t>(k)].second;
        chunks_[key] = build_chunk(terrain, params_, key.first, key.second);
    }
}

} // namespace particles
} // namespace toy

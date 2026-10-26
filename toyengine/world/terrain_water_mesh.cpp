#include <toyengine/world/terrain_water_mesh.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace toy {
namespace world {
namespace {

constexpr int k_dx[4] = {1, -1, 0, 0};
constexpr int k_dy[4] = {0, 0, 1, -1};

/** @brief What one corner of the water surface stands at, averaged over the wet tiles on it. */
struct Corner {
    float level = 0.0f;
    float depth = 0.0f;
    float wave = 1.0f;
    bool  any = false;
};

/**
 * @brief The corner shared by tiles (tx-1..tx, ty-1..ty), averaged over whichever of them are wet.
 *
 * Addressed by global tile coordinate, never by a chunk-local one, which is what makes two
 * chunks write the same value for the edge between them.
 */
Corner corner_at(const TerrainField& field, int cx, int cy, float wave_scale_river) {
    // Two rings, both addressed by GLOBAL tile coordinate so two chunks cannot disagree.
    //
    // The inner ring is the four tiles that touch the corner, and it is what a corner inside the
    // water averages. The outer ring exists for the SKIRT: the surface is emitted one tile past
    // the water, so a skirt quad's outer corners touch no wet tile at all. Left at zero they
    // would drag the surface down to z = 0 and render as a wall of water at every shoreline, so
    // they take the body they continue -- found by widening the search rather than by asking the
    // tile that emitted the quad, because two chunks emitting the same skirt corner from
    // different tiles would then write different heights and crack the seam.
    for (int radius = 1; radius <= 2; ++radius) {
        Corner out;
        float level_sum = 0.0f, depth_sum = 0.0f, wave_sum = 0.0f;
        int n = 0;
        for (int dy = -radius; dy <= radius - 1; ++dy) {
            for (int dx = -radius; dx <= radius - 1; ++dx) {
                const glm::ivec2 t(cx + dx, cy + dy);
                if (!field.contains(t)) continue;
                const WaterCell w = field.water_at(t);
                if (!w.wet()) continue;
                level_sum += w.level;
                depth_sum += std::max(0.0f, w.level - field.ground_at_tile(t));
                wave_sum += w.kind == WaterKind::River ? wave_scale_river : 1.0f;
                ++n;
            }
        }
        if (n == 0) continue;
        out.level = level_sum / static_cast<float>(n);
        // A skirt corner is dry ground: its depth is zero, which is what fades the shore out.
        out.depth = radius == 1 ? depth_sum / static_cast<float>(n) : 0.0f;
        out.wave = wave_sum / static_cast<float>(n);
        out.any = true;
        return out;
    }
    return Corner{};
}

/** @brief The water a tile shows: its own, or -- for a dry skirt tile -- a wet neighbour's. */
WaterCell surface_of(const TerrainField& field, glm::ivec2 tile) {
    const WaterCell own = field.water_at(tile);
    if (own.wet()) return own;
    for (int k = 0; k < 4; ++k) {
        const glm::ivec2 n(tile.x + k_dx[k], tile.y + k_dy[k]);
        if (!field.contains(n)) continue;
        const WaterCell w = field.water_at(n);
        if (w.wet()) return w;   // the skirt continues the body beside it
    }
    return WaterCell{};
}

/** @brief One mesh under construction, with its corner-to-vertex table. */
struct Builder {
    ChunkWaterData* out = nullptr;
    std::unordered_map<std::uint64_t, std::uint32_t> corners;

    /** @brief The vertex for a global corner, added once per mesh so a quad edge is shared. */
    std::uint32_t vertex(const TerrainField& field, int cx, int cy, const glm::vec2& local,
                         float wave_scale_river) {
        const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
                                  static_cast<std::uint32_t>(cy);
        const auto it = corners.find(key);
        if (it != corners.end()) return it->second;
        const Corner c = corner_at(field, cx, cy, wave_scale_river);
        const std::uint32_t index = static_cast<std::uint32_t>(out->positions.size());
        out->positions.push_back(glm::vec3(local.x, local.y, c.level));
        out->depths.push_back(c.depth);
        out->wave_scales.push_back(c.wave);
        out->uvs.push_back(glm::vec2(local.x, local.y));
        corners.emplace(key, index);
        return index;
    }
};

} // namespace

void build_smooth_chunk_water(const TerrainField& field, glm::ivec2 chunk, int chunk_tiles,
                              float wave_scale_river, ChunkWaterData& still, ChunkWaterData& flowing) {
    still.clear();
    flowing.clear();
    if (field.mode() != TerrainMode::Heightmap || field.empty()) return;

    const int ct = std::max(1, chunk_tiles);
    const glm::vec2 extent = field.geometry().tile_extent();
    const glm::vec2 chunk_origin(static_cast<float>(chunk.x * ct) * extent.x,
                                 static_cast<float>(chunk.y * ct) * extent.y);

    Builder still_b{&still, {}};
    Builder flow_b{&flowing, {}};

    for (int ty = chunk.y * ct; ty < (chunk.y + 1) * ct; ++ty) {
        for (int tx = chunk.x * ct; tx < (chunk.x + 1) * ct; ++tx) {
            const glm::ivec2 tile(tx, ty);
            if (!field.contains(tile)) continue;
            // Wet tiles, and one tile further: the extra ring is the skirt, and the ground is
            // what hides it. Without it the surface would stop at a tile boundary rather than
            // at the waterline.
            const WaterCell shows = surface_of(field, tile);
            if (!shows.wet()) continue;

            Builder& b = shows.kind == WaterKind::River ? flow_b : still_b;
            std::uint32_t v[4];
            for (int c = 0; c < 4; ++c) {
                const int ox = c == 1 || c == 2 ? 1 : 0;
                const int oy = c >= 2 ? 1 : 0;
                const glm::vec2 local(static_cast<float>(tx + ox) * extent.x - chunk_origin.x,
                                      static_cast<float>(ty + oy) * extent.y - chunk_origin.y);
                v[c] = b.vertex(field, tx + ox, ty + oy, local, wave_scale_river);
            }
            // Two triangles, anticlockwise seen from above (+Z up), as the tile path emits them.
            b.out->indices.insert(b.out->indices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
        }
    }

    // A river's `u` runs downstream (water_flow_bake.h's contract). The flow is a per-tile
    // octant, so the direction is taken once for the chunk from the river tiles in it: a chunk
    // is small beside a river's bends, and one direction per chunk keeps the seam consistent.
    if (!flowing.empty()) {
        glm::vec2 downstream(0.0f);
        for (int ty = chunk.y * ct; ty < (chunk.y + 1) * ct; ++ty) {
            for (int tx = chunk.x * ct; tx < (chunk.x + 1) * ct; ++tx) {
                const glm::ivec2 tile(tx, ty);
                if (!field.contains(tile)) continue;
                const WaterCell w = field.water_at(tile);
                if (w.kind != WaterKind::River || w.flow < 1 || w.flow > 8) continue;
                downstream += flow_direction(w.flow);
            }
        }
        if (glm::length(downstream) > 1e-4f) {
            downstream = glm::normalize(downstream);
            const glm::vec2 across(-downstream.y, downstream.x);
            for (std::size_t i = 0; i < flowing.uvs.size(); ++i) {
                const glm::vec2 p = flowing.uvs[i];
                flowing.uvs[i] = glm::vec2(glm::dot(p, downstream), glm::dot(p, across));
            }
        }
    }
}

} // namespace world
} // namespace toy

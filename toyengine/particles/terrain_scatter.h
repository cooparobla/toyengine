/**
 * @file terrain_scatter.h
 * @brief Foliage, trees and rocks over a HeightmapTerrain: a ParticleSystem in `scatter` mode with
 *        `shape: terrain` places mesh instances over the terrain's surface, by paint layer, slope
 *        and height -- streamed in chunks around the camera so an open world never holds them all.
 *
 * ### Placement
 *
 * Per terrain chunk, a jittered grid with one candidate per 1/density m^2, every random number
 * seeded by (system seed, chunk, cell): the same chunk always grows the same plants, in any build
 * order. A candidate survives when the terrain there is within the slope and height limits and,
 * if a layer is named, its paint weight passes a per-candidate threshold (min_weight .. 1, so thin
 * paint grows sparse plants and full paint dense ones). Survivors sink a little into the ground,
 * align to the surface normal when asked, spin randomly and scale within the size range.
 *
 * ### Streaming
 *
 * Chunks within max_draw_distance (plus one chunk) of the camera are built, a few per frame,
 * nearest first; chunks beyond 1.2x that are dropped. A chunk whose terrain revision moved (a
 * sculpt or paint stroke inside it) is rebuilt -- live, while the stroke runs. Each built chunk is
 * one instanced mesh batch with its own bounds, so it frustum-culls on its own.
 */

#ifndef TOYENGINE_PARTICLES_TERRAIN_SCATTER_H
#define TOYENGINE_PARTICLES_TERRAIN_SCATTER_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/particles/particle_math.h>
#include <toyengine/particles/particle_shape.h>
#include <toyengine/world/heightmap_data.h>

namespace toy {
namespace particles {

/**
 * @struct TerrainScatterParams
 * @brief Everything placement depends on (a change rebuilds every chunk).
 */
struct TerrainScatterParams {
    TerrainScatterShape shape;
    int layer = -1;               ///< Paint layer index; -1 = everywhere.
    uint32_t seed = 1;
    Range size{1.0f};             ///< The system's start_size.
    bool align_to_normal = false;
    bool random_spin = true;

    bool operator==(const TerrainScatterParams& o) const;
    bool operator!=(const TerrainScatterParams& o) const { return !(*this == o); }
};

/**
 * @class TerrainScatterCache
 * @brief One system's built chunks. Pure CPU; see the file comment.
 */
class TerrainScatterCache {
public:
    struct Chunk {
        std::vector<glm::mat4> matrices;
        glm::vec3 bounds_min{0.0f}, bounds_max{0.0f};
        std::uint64_t revision = 0;   ///< The terrain chunk revision it was built from.
        std::uint64_t build_id = 0;   ///< Process-unique per build: the renderer's upload version.
    };

    /**
     * @brief How many of a chunk's instances to draw at `distance` from the camera: all of them
     *        up to `fade_start * max_distance`, then smoothly fewer, none at `max_distance`.
     *        Instances are stored shuffled, so drawing a prefix thins them evenly.
     */
    static std::uint32_t thinned_count(std::uint32_t total, float distance, float max_distance, float fade_start);

    /**
     * @brief Brings the cache in line with the terrain, the parameters and the camera: drops what
     *        changed or went out of range, then builds up to `budget` missing / stale chunks.
     * @param max_distance Draw distance (m); 0 = the whole terrain.
     */
    void update(const world::HeightmapView& terrain, const TerrainScatterParams& params, const glm::vec3& camera,
                float max_distance, int budget = 2);

    /** @brief Builds one chunk's instances (world space). Deterministic; exposed for tests. */
    static Chunk build_chunk(const world::HeightmapView& terrain, const TerrainScatterParams& params, int cx, int cy);

    const std::map<std::pair<int, int>, Chunk>& chunks() const { return chunks_; }
    std::size_t instance_count() const;
    void clear() { chunks_.clear(); }

private:
    std::map<std::pair<int, int>, Chunk> chunks_;
    TerrainScatterParams params_;
    std::uint64_t data_uid_ = 0;
    world::HeightmapGeometry geometry_;
    glm::vec3 origin_{0.0f};
};

} // namespace particles
} // namespace toy

#endif // TOYENGINE_PARTICLES_TERRAIN_SCATTER_H

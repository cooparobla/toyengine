/**
 * @file instance_cache.h
 * @brief One painter's built chunks: the world matrices a chunk's records turn into, grouped by
 *        the (prototype, mesh part) they draw as, ready to hand the renderer as instanced
 *        batches.
 *
 * The stored counterpart to `particles::TerrainScatterCache`: that one *generates* a chunk's
 * instances from density and slope rules, this one *reads* them out of an `InstanceSetData`.
 * Everything downstream is the same -- one batch per chunk so it frustum-culls on its own, a
 * `build_id` the renderer uses as its upload version, and a shuffled order so drawing a prefix
 * thins the chunk evenly with distance.
 *
 * Pure CPU: no Vulkan, no `Scene`. The matrices it holds are what the renderer points at, so
 * they must not move while a frame is in flight -- `update()` runs in the painter system's
 * `execute()`, well before render.
 */

#ifndef TOYENGINE_INSTANCE_INSTANCE_CACHE_H
#define TOYENGINE_INSTANCE_INSTANCE_CACHE_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/instance/instance_painter.h>
#include <toyengine/instance/instance_set.h>

namespace toy {
namespace instance {

/**
 * @class InstanceRenderCache
 * @brief See the file comment.
 */
class InstanceRenderCache {
public:
    /** @brief The instances of one (prototype, mesh part) inside one chunk: one instanced draw. */
    struct PartBatch {
        std::vector<glm::mat4> matrices;   ///< World space, shuffled.
        /// Per-instance foliage parent sway, shuffled in lockstep with `matrices` so entry `i`
        /// still describes instance `i`. Empty unless the prototype named a parent.
        std::vector<glm::vec4> foliage;
        /// The second half, shuffled with the other two. Empty whenever `foliage` is.
        std::vector<glm::vec4> foliage1;
        std::uint16_t prototype = 0;
        std::uint32_t part = 0;            ///< The prototype's part index, not the mesh's slot.
        glm::vec3 bounds_min{0.0f}, bounds_max{0.0f};
        std::uint64_t build_id = 0;        ///< Process-unique per build: the upload version.
        float draw_distance = 0.0f;        ///< Resolved for this prototype (0 = always drawn).
        float fade_start = 0.6f;
    };

    /** @brief One chunk, built from the records in it. */
    struct Chunk {
        std::vector<PartBatch> parts;
        std::uint64_t revision = 0;        ///< The data chunk revision it was built from.
        glm::vec3 bounds_min{0.0f}, bounds_max{0.0f};
    };

    /**
     * @brief Brings the cache in line with the data, the prototypes and the camera: drops what
     *        changed or went out of range, then builds up to `budget` missing or stale chunks,
     *        nearest first.
     *
     * @param data         The painted records.
     * @param prototypes   The painter's palette; a prototype that is not `ready()` is skipped,
     *                     and its chunks rebuilt once it loads.
     * @param camera       World position the streaming is centred on.
     * @param max_distance Furthest a chunk is kept, metres; 0 keeps every chunk.
     * @param budget       Chunks built this call.
     */
    void update(const InstanceSetData& data, const std::vector<InstancePrototype>& prototypes,
                const glm::vec3& camera, float max_distance, int budget = 2);

    /** @brief Builds one chunk. Deterministic for a given (records, prototypes); for tests. */
    static Chunk build_chunk(const InstanceSetData& data, ChunkCoord coord,
                             const std::vector<InstancePrototype>& prototypes);

    const std::unordered_map<ChunkCoord, Chunk, ChunkCoordHash>& chunks() const { return chunks_; }

    /** @brief Total instances across every built chunk and part. */
    std::size_t instance_count() const;

    void clear();

private:
    std::unordered_map<ChunkCoord, Chunk, ChunkCoordHash> chunks_;
    std::uint64_t data_uid_ = 0;
    /// Which prototypes were ready last update: one becoming ready rebuilds everything, since
    /// its parts were skipped while it loaded.
    std::vector<std::uint8_t> proto_ready_;
};

} // namespace instance
} // namespace toy

#endif // TOYENGINE_INSTANCE_INSTANCE_CACHE_H

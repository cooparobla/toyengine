/**
 * @file chunk_shaper.h
 * @brief How a terrain chunk's geometry meets the shape of the world it belongs to.
 *
 * The engine meshes a chunk in **grid space** -- a flat `[0, span]^2` patch with heights along
 * +Z -- and for the flat Z-up world it was written for, that is already the finished geometry.
 * A world with any other shape needs the patch bent onto it afterwards, and needs a handful of
 * related decisions answered differently. A `ChunkShaper` is that set of answers.
 *
 * **A null shaper is the flat world.** Every call site reads `shaper != nullptr` (or asks the
 * shaper a question whose flat answer is the default), so the flat path costs one null compare
 * and is otherwise the code it always was -- no virtual call, nothing inlined away. Only a
 * project that curves its world pays for the indirection, and it pays it once per chunk build
 * rather than per vertex.
 *
 * **Implementations must be immutable and thread-safe.** A shaper is read from
 * `ChunkBuildJob::run()`, which runs on a JobEngine worker, and the same shaper serves every
 * chunk of a terrain concurrently. Hence every method is `const` and the whole object is held
 * by `shared_ptr<const ChunkShaper>` on the TerrainComponent.
 *
 * This is deliberately NOT the same seam as the tile grid's (`neighbour()`, `tile_at()`, the
 * brush and raycast queries): that one is asked on the owner thread and by the editor, where
 * this one is asked on a worker. Keeping them apart is what lets each state a single threading
 * contract, even when one project class implements both.
 */

#ifndef TOYENGINE_WORLD_CHUNK_SHAPER_H
#define TOYENGINE_WORLD_CHUNK_SHAPER_H

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

struct TerrainParams;
struct ChunkMeshData;
struct ChunkWaterData;

/**
 * @class ChunkShaper
 * @brief Bends a chunk meshed in grid space onto the world's real shape, and answers the few
 *        build decisions that depend on that shape. Null means the flat Z-up world.
 */
class ChunkShaper {
public:
    virtual ~ChunkShaper() = default;

    /**
     * @brief Where the chunk's mesh is positioned, as its SceneObject's local position.
     *
     * Flat chunks are built around their own corner and placed on the grid. A shaper whose
     * `shape()` writes world-space positions returns the origin those positions are measured
     * from instead -- commonly the centre everything curves about.
     *
     * @param params The terrain's parameters.
     * @param coord  Which chunk.
     * @return The chunk object's position, world units.
     */
    virtual glm::vec3 chunk_origin(const TerrainParams& params, const ChunkCoord& coord) const = 0;

    /**
     * @brief Bends a chunk's finished grid-space geometry onto the world.
     *
     * Called on a worker thread, after the mesh and its water are built and before the weld, so
     * the positions it writes are what gets welded, collided against and simplified. Normals
     * must be carried through the map as well as positions, or a wall stops reading as a wall.
     *
     * @param params  The terrain's parameters.
     * @param coord   Which chunk.
     * @param mesh    The chunk's grid-space mesh, rewritten in place.
     * @param still   The chunk's still water, rewritten in place (may be empty).
     * @param flowing The chunk's flowing water, rewritten in place (may be empty).
     */
    virtual void shape(const TerrainParams& params, const ChunkCoord& coord, ChunkMeshData& mesh,
                       ChunkWaterData& still, ChunkWaterData& flowing) const = 0;

    /**
     * @brief Which vertices the simplifier must keep, so neighbouring chunks never crack.
     *
     * A chunk at a coarse level still has to meet a full-density neighbour on the vertices they
     * shared, and the two decide their levels independently -- so the seam is held by locking
     * it rather than by agreeing. The seam is the chunk's own edge, but `shape()` has already
     * moved it by the time this is asked, so only the shaper knows where it now lies.
     *
     * @param params The terrain's parameters.
     * @param coord  Which chunk.
     * @param mesh   The shaped, welded mesh.
     * @return One flag per vertex, 1 on the seam; empty when nothing needs locking.
     */
    virtual std::vector<std::uint8_t> seam_locks(const TerrainParams& params, const ChunkCoord& coord,
                                                 const ChunkMeshData& mesh) const = 0;

    /**
     * @brief Whether a `style:` LOD tier can be used (a level re-meshed with another tile set).
     *
     * A style level re-meshes from the chunk's columns and welds on its own, which `shape()`
     * would then have to be re-applied to. A shaper that cannot support that says so and the
     * tier is skipped; `field:` tiers are always built, being re-meshed from the same columns
     * and shaped like any other geometry.
     */
    virtual bool allows_style_lods() const = 0;

    /**
     * @brief Whether a chunk too small on screen may be built at its coarsest `field` tier only.
     *
     * Worth it only where chunks are never evicted, so the resident far field is paid for
     * forever. A streaming world drops what leaves view instead and has nothing to save.
     */
    virtual bool allows_coarse_builds() const = 0;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_CHUNK_SHAPER_H

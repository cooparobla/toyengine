/**
 * @file planet_chunk_shaper.h
 * @brief The ChunkShaper for a planet: bends a chunk onto the cube-sphere (planet.h).
 *
 * TRANSITIONAL HOME. This belongs to the planet project, not the engine, and moves there once
 * the rest of the planet's terrain follows. It lives here for now only so the engine's existing
 * planet tests keep proving that routing the geometry through ChunkShaper changed nothing.
 */

#ifndef TOYENGINE_WORLD_PLANET_CHUNK_SHAPER_H
#define TOYENGINE_WORLD_PLANET_CHUNK_SHAPER_H

#include <toyengine/world/chunk_shaper.h>

namespace toy {
namespace world {

/**
 * @class PlanetChunkShaper
 * @brief Bends a chunk meshed in cube-face space onto the globe.
 *
 * Stateless -- every answer comes from the `TerrainParams` it is handed, which already carry
 * the layout (`TerrainParams::layout()`). One instance therefore serves a whole planet, on
 * every worker at once.
 */
class PlanetChunkShaper final : public ChunkShaper {
public:
    /** @brief The planet's centre: every chunk's geometry is written in world space around it. */
    glm::vec3 chunk_origin(const TerrainParams& params, const ChunkCoord& coord) const override;

    /**
     * @brief Every position through `to_planet()`, every normal through that map's Jacobian.
     *
     * The water goes into a frame of its own (local Z out from the planet at the chunk's
     * centre), because over one chunk the sphere is still a height field in that frame -- which
     * is what the water system works on. See `WaterBody::local_frame`.
     */
    void shape(const TerrainParams& params, const ChunkCoord& coord, ChunkMeshData& mesh,
               ChunkWaterData& still, ChunkWaterData& flowing) const override;

    /**
     * @brief The chunk's four edges as planes through the planet's centre.
     *
     * Each edge is a curve of constant face-space x or y, and every direction along such a
     * curve is a combination of the face's outward axis and one of its own -- so each edge lies
     * in a plane through the centre, and the seam test is four dot products. That is both
     * cheaper than reading every vertex back into face space and immune to the thing that
     * breaks that: a vertex exactly on a cube edge resolves to either of the two faces meeting
     * there, and read through the wrong one its face-space coordinate is a whole face away.
     */
    std::vector<std::uint8_t> seam_locks(const TerrainParams& params, const ChunkCoord& coord,
                                         const ChunkMeshData& mesh) const override;

    /** @brief No: a style level welds independently, and the warp could not follow it. */
    bool allows_style_lods() const override { return false; }

    /** @brief Yes: a planet keeps every chunk resident, so its far field is paid for forever. */
    bool allows_coarse_builds() const override { return true; }
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_PLANET_CHUNK_SHAPER_H

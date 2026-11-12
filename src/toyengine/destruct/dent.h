/**
 * @file dent.h
 * @brief Pushing a mesh's vertices inward where a blow lands, and densifying a mesh too coarse
 *        to take one -- the geometry DamageResponse::Dent and DamageResponse::Chip carry out.
 *
 * Pure geometry, no Scene and no physics -- mirrors toyengine/destruct/'s other files: vertices
 * and a blow in, vertices (and normals) changed in place. The caller (DestructionSystem) owns
 * the per-object clone these functions mutate, publishes it as a dynamic Mesh, and re-uploads it
 * every frame -- see destruction_system.h's file doc for why a per-frame upload, not a per-hit
 * one, is the safe contract with a double-buffered vertex buffer.
 */

#ifndef TOYENGINE_DESTRUCT_DENT_H
#define TOYENGINE_DESTRUCT_DENT_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/data/mesh.h>

namespace toy {
namespace destruct {

using Vertex = coopa::gfx::engine::data::Vertex;

/** @brief 1 out to 40% of the radius (a flat floor, not a single dome), smoothstepping down to
 *         0 at the rim from there -- a dent's radial profile, with an actual crease at the edge. */
float dent_falloff(float x);

/** @brief One blow, in WORLD space -- the frame a footprint has to stay round in under a
 *         non-uniformly scaled object. */
struct DentBlow {
    glm::vec3 point{0.0f};                   ///< Where it landed.
    glm::vec3 direction{0.0f, 0.0f, -1.0f};   ///< Unit; the way the blow travels.
    float radius = 0.1f;                      ///< Footprint radius, metres.
    float depth = 0.01f;                      ///< Displacement at the centre, metres.
    float max_offset = 0.0f;                  ///< Cap on a vertex's ACCUMULATED offset, metres;
                                              ///< 0 is uncapped.
    bool force_inward = true;                 ///< Flip `direction` once, for the whole blow, if
                                              ///< it points away from the object's own centre --
                                              ///< impact_blow() takes its direction from the
                                              ///< contact impulse, which may point either way.
};

/**
 * @struct DentState
 * @brief What a dented mesh carries between blows: the rest pose, the accumulated world-space
 *        offset per UNIQUE POSITION, and the groups that keep duplicated corners together.
 *
 * Positions are grouped by position alone, not by whole Vertex: Mesh::build_cpu() welds only
 * byte-identical corners, so a cube's eight corners are twenty-four vertices with three normals
 * each. Moving those independently would split the mesh open along every face seam, so every
 * vertex sharing a position moves together.
 */
struct DentState {
    std::vector<glm::vec3>     rest;          ///< One per unique position, object space.
    std::vector<glm::vec3>     offset;        ///< Accumulated world-space push, one per unique position.
    std::vector<std::uint32_t> group_off;     ///< CSR: unique position g's members are
                                              ///< group_members[group_off[g] .. group_off[g+1]).
    std::vector<std::uint32_t> group_members; ///< ...their indices into the vertex array.
    float mean_edge = 0.0f;                   ///< Mean triangle edge length, object space -- the
                                              ///< floor a Chip's tiny footprint is widened to.

    /** @brief Builds the weld groups, the rest pose and the zeroed offsets from `vertices`'
     *         current positions -- call this once, before the mesh's first dent. */
    void build(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices);

    /** @brief True before build() has ever found a vertex, i.e. nothing to dent. */
    bool empty() const { return rest.empty(); }

    /** @brief The deepest accumulated offset over every unique position, metres -- how wrecked
     *         the mesh reads right now. */
    float max_offset() const;
};

/**
 * @brief Accumulates one blow into `state` and rewrites `vertices`' positions and normals from
 *        the rest pose.
 *
 * Weighting and displacement happen in WORLD space, so the footprint stays round under a
 * non-uniform scale: each affected rest position is transformed to world, weighted by
 * dent_falloff() on its world distance from `blow.point`, and pushed by `blow.direction *
 * blow.depth * weight`, accumulated into `state.offset` and clamped to `blow.max_offset`. The
 * new local position is `rest + mat3(inv_world) * offset` -- a displacement is a difference of
 * two points, so it takes the inverse world matrix's LINEAR part, not the inverse-transpose a
 * normal would (that rule is for covectors; this is not one).
 *
 * Normals are recomputed over the WHOLE mesh, per vertex INDEX from its own incident triangles
 * (not averaged across a weld group) -- reproducing the authored normals exactly on an undented
 * mesh, so a hard edge stays hard and a smooth patch stays smooth whether or not it was touched.
 *
 * @param world     The object's world matrix.
 * @param inv_world Its inverse -- the caller already has both to place the object, so this
 *                  avoids inverting it again per blow.
 * @return Unique positions the footprint touched; 0 means the blow missed the mesh entirely.
 */
std::size_t apply_dent(DentState& state, std::vector<Vertex>& vertices,
                       const std::vector<std::uint32_t>& indices, const DentBlow& blow,
                       const glm::mat4& world, const glm::mat4& inv_world);

/**
 * @brief Uniform 1-to-4 triangle subdivision, repeated until the mesh's mean edge length in
 *        WORLD units is at or below `target_edge_world`, or a cap is hit.
 *
 * Only called once, when a dented object's private mesh clone is first built -- topology is
 * fixed for the rest of the mesh's life, so this is the only point subdivision can happen at.
 * Edge midpoints are shared through an (min index, max index) -> new-index map, so interior
 * edges share one vertex and the surface stays closed; midpoints lie on chords, so the result is
 * visually identical to the source mesh, which is what makes doing this silently on first
 * impact safe.
 *
 * @param world_scale  The object's world scale along its longest axis -- converts the mesh's own
 *                     (object-space) mean edge length to world units without needing the full
 *                     world matrix.
 * @param max_levels   Hard cap on passes, regardless of how coarse the input is.
 * @param max_vertices Refuses a level that would push the vertex count past this.
 * @return Levels actually applied; 0 when the mesh was already fine enough, or neither cap left
 *         room for a single level.
 */
int subdivide_for_dent(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices,
                       float target_edge_world, float world_scale, int max_levels = 4,
                       std::size_t max_vertices = 200000);

} // namespace destruct
} // namespace toy

#endif // TOYENGINE_DESTRUCT_DENT_H

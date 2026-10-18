/**
 * @file fracture.h
 * @brief Breaking a mesh into solid chunks: Voronoi fracture over its triangles, on the CPU.
 *
 * The classic cell-fracture construction (Blender's Cell Fracture, every "RayFire"-style tool):
 * scatter `pieces` sites through the mesh, and give each site everything closer to it than to
 * any other -- its Voronoi cell. A cell is an intersection of half-spaces, so a piece is the
 * source mesh clipped by one plane per neighbouring site, and the clip is the only geometry
 * operation needed. Each cut leaves an open mouth, which is capped with a fan over the cut
 * loop, so a piece is a closed solid with a flat new face wherever it was broken.
 *
 * Pure: vertices and indices in, pieces out. No Vulkan, no Scene, no physics -- which is what
 * lets it run on a worker thread (toy::scene::DestructionSystem fractures off the main thread)
 * and be tested with nothing but two arrays.
 *
 * **What it is good at**: solid, roughly convex shapes -- crates, barrels, rocks, statues,
 * walls. Those are what gets broken in practice, and their cut loops are single rings a fan
 * closes exactly.
 *
 * **What it is not**: a boolean engine. A cut across a concave mesh can leave several rings
 * in one plane, and the cap then fills each ring it finds separately -- correct for a tube or
 * a torus cut in two, wrong for a cut that produces a ring inside a ring (the inner hole is
 * filled too). Meshes with holes through the cut plane are the case to watch, and a thin
 * shell (an open surface, a plane, a cloth) has no inside to fill at all: it fractures into
 * open shells, which look right but enclose no volume.
 */

#ifndef TOYENGINE_DESTRUCT_FRACTURE_H
#define TOYENGINE_DESTRUCT_FRACTURE_H

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gfxcoopa/engine/data/mesh.h>

namespace toy {
namespace destruct {

using Vertex = coopa::gfx::engine::data::Vertex;

/**
 * @struct FractureSettings
 * @brief How to break a mesh up.
 */
struct FractureSettings {
    /** @brief Cells to cut the mesh into. Pieces that come out empty are dropped, so the count
     *         returned can be lower -- sites land outside a concave mesh, or inside a
     *         neighbour's cell entirely. */
    int pieces = 12;

    /** @brief Seeds the site scatter: the same mesh and seed always break the same way. */
    std::uint32_t seed = 1u;

    /**
     * @brief How unevenly the sites are scattered, 0..1.
     *
     * 0 puts them on a jittered grid -- pieces of similar size, the look of a pane of glass or
     * a cast slab. 1 is uniform random: a few large pieces and a scatter of small ones, which
     * reads as a rock or a crate. The default leans that way because even debris looks
     * manufactured.
     */
    float irregularity = 0.7f;

    /**
     * @brief Sites nearest a cell that are considered when clipping it; 0 uses every site.
     *
     * The cell only needs its Voronoi NEIGHBOURS, and past a couple of dozen sites most pairs
     * are not neighbours at all -- clipping against them costs a full mesh pass and removes
     * nothing. 16 is well past what a cell can have in practice.
     */
    int neighbours = 16;

    /** @brief Give the new interior faces this material slot's look, if the caller has one. */
    bool cap_interiors = true;

    /**
     * @brief How much of its box a piece must fill before it is left as one box, 0..1.
     *
     * A Voronoi shard is a wedge as often as it is a brick, and a wedge fills barely half of
     * any box drawn round it -- as collision that reads as debris resting on thin air at the
     * corners. Below this ratio the piece is cut along its longest axis and each part gets its
     * own box (up to `max_boxes`), which costs shapes and buys a silhouette. 1 always splits,
     * 0 never does.
     */
    float collider_fill = 0.6f;

    /** @brief Most boxes one piece's collision shape may use. 1 disables the split entirely. */
    int max_boxes = 4;
};

/**
 * @struct FractureBox
 * @brief One box of a piece's collision shape, in the piece's own (principal-axis) space.
 */
struct FractureBox {
    glm::vec3 centre{0.0f};
    glm::vec3 half_extent{0.0f};
};

/**
 * @struct FracturePiece
 * @brief One chunk: a closed mesh in ITS OWN space, plus where that space sits in the original's.
 *
 * Centring each piece on its own centroid is what lets a chunk become a rigid body: a body
 * spins about its origin, and geometry heaped off to one side would orbit instead of tumble.
 * `centre` and `orientation` are where to put that space to leave the mesh exactly where it was.
 *
 * The piece's own space is its PRINCIPAL frame: the vertices are rotated so the shard's
 * longest direction is x, its next y, its thinnest z. That is what makes `boxes` worth
 * having -- a world-axis box around a diagonal splinter is mostly empty, while a box around
 * the same splinter in its own frame is the splinter.
 */
struct FracturePiece {
    std::vector<Vertex>        vertices;
    std::vector<std::uint32_t> indices;
    glm::vec3 centre{0.0f};       /**< Where the piece's space sits in the source mesh's. */
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f}; /**< ...and how it is turned (the principal frame). */
    glm::vec3 half_extent{0.0f};  /**< Half-size of the piece's box in its own frame. */
    float     volume = 0.0f;      /**< Volume of the closed piece, for mass. */

    /**
     * @brief The piece's collision shape: one box for a blocky shard, a few for an awkward one.
     *
     * Always at least one (the whole bounding box). A caller that wants a single shape can use
     * `half_extent` and ignore this; a caller that can build a compound body gets a closer fit
     * -- see FractureSettings::collider_fill and ::max_boxes.
     */
    std::vector<FractureBox> boxes;
};

/**
 * @brief Breaks a triangle mesh into Voronoi chunks.
 *
 * @param vertices Source vertices (object space).
 * @param indices  Source triangle indices.
 * @param settings How many pieces, how they scatter.
 * @return The pieces, largest first; empty if the mesh has no triangles.
 */
std::vector<FracturePiece> fracture_mesh(const std::vector<Vertex>& vertices,
                                         const std::vector<std::uint32_t>& indices,
                                         const FractureSettings& settings);

/**
 * @brief Clips a triangle mesh by a plane, keeping the half `dot(normal, p) <= distance`, and
 *        caps the opening so the result is closed again.
 *
 * fracture_mesh()'s one primitive, exposed because it is the whole of the geometry here and so
 * the thing worth testing on its own: a cube clipped by a plane through its middle is half a
 * cube, closed, with a square cap.
 *
 * @return False when the plane removes everything (the output is then empty).
 */
bool clip_mesh(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices,
               const glm::vec3& normal, float distance, bool cap,
               std::vector<Vertex>& out_vertices, std::vector<std::uint32_t>& out_indices);

} // namespace destruct
} // namespace toy

#endif // TOYENGINE_DESTRUCT_FRACTURE_H

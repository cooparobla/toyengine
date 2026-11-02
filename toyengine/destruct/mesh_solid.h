/**
 * @file mesh_solid.h
 * @brief Measuring and framing a closed triangle mesh: volume, principal axes, a tight box fit.
 *
 * Pure geometry, no Scene and no physics -- shared by fracture_mesh() and slice_mesh(), both of
 * which turn a cut-off chunk of mesh into a rigid piece and need the same three things: a
 * volume for mass, a local frame with no off-diagonal inertia terms, and a collision box (or
 * boxes) that is not mostly empty space.
 */

#ifndef TOYENGINE_DESTRUCT_MESH_SOLID_H
#define TOYENGINE_DESTRUCT_MESH_SOLID_H

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/data/mesh.h>

namespace toy {
namespace destruct {

using Vertex = coopa::gfx::engine::data::Vertex;

/**
 * @struct FractureBox
 * @brief One box fit to some of a piece's vertices, in the piece's own (principal-axis) space.
 */
struct FractureBox {
    glm::vec3 centre{0.0f};
    glm::vec3 half_extent{0.0f};
};

/** @brief Signed volume of a closed triangle mesh (the divergence theorem over its faces). */
float mesh_volume(const std::vector<Vertex>& v, const std::vector<std::uint32_t>& idx);

/**
 * @brief Eigenvectors of a symmetric 3x3, by cyclic Jacobi rotation -- a mesh's principal axes,
 *        as the columns of the returned matrix.
 *
 * Jacobi rather than a closed-form cubic: it is a dozen lines, it never needs a special case
 * for repeated eigenvalues (a cube's covariance has three of them), and the vectors come out
 * orthonormal to machine precision, which is what a rotation has to be.
 */
glm::mat3 jacobi_eigenvectors(glm::mat3 a);

/**
 * @brief A mesh's principal axes, from the covariance of its vertices -- one of the frames
 *        best_frame() tries.
 */
glm::mat3 principal_axes(const std::vector<Vertex>& vertices);

/**
 * @brief The frame to box a mesh piece in: the smallest box among a handful of candidates.
 *
 * Not PCA alone, which is the usual first guess and is measurably wrong here: PCA minimises
 * variance, not box volume, and on a fracture shard it loses to the plain world axes. What a
 * piece does have is large flat faces, because every one of them is a cut plane or a face of
 * the source mesh, and a box that lies along the biggest face is usually the tight one. So: try
 * the world axes, try PCA, try a frame per large face, and keep whichever boxes smallest.
 *
 * Axes come back ordered longest first, which is what fit_boxes() splits along.
 */
glm::mat3 best_frame(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices);

/**
 * @brief Splits a piece's box into up to `max_boxes` boxes that hug its geometry.
 *
 * Halves along the longest axis while the part fills less of its box than `fill` asks, fitting
 * each half to the vertices that actually fall in it -- so a wedge becomes a staircase of two
 * or three boxes instead of one box with a hollow corner. The fit is per PART, not per plane,
 * which is what makes two boxes worth more than one cut.
 */
std::vector<FractureBox> fit_boxes(const std::vector<Vertex>& vertices, float volume,
                                   const glm::vec3& half_extent, float fill, int max_boxes);

/**
 * @brief Turns `vertices` into `frame`'s axes and centres them on their own bounds, in place --
 *        the finishing step every fracture or slice piece needs before it is a rigid body.
 *
 * Positions, normals and tangent directions are all rotated (by `frame`'s transpose, since the
 * piece moves from world axes INTO the frame); only positions are then re-centred. A body spins
 * about its own origin, so geometry left off-centre would orbit rather than tumble, and a
 * non-principal frame leaves off-diagonal inertia terms that `Body::inv_inertia_local` (a plain
 * vec3 diagonal) cannot represent -- which is why `frame` should come from best_frame() or
 * principal_axes(), never an arbitrary rotation.
 *
 * @param frame The piece's own axes, as columns -- what best_frame() or principal_axes() return.
 * @param out_half_extent Half-size of the now-centred piece, in its own frame.
 * @param out_centre Where the piece's new local origin sits in the space `vertices` were given
 *                   in (e.g. the source mesh's space) -- what a caller places a rigid body at.
 */
void centre_in_frame(std::vector<Vertex>& vertices, const glm::mat3& frame,
                     glm::vec3& out_half_extent, glm::vec3& out_centre);

} // namespace destruct
} // namespace toy

#endif // TOYENGINE_DESTRUCT_MESH_SOLID_H

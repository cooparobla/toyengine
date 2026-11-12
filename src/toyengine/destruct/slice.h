/**
 * @file slice.h
 * @brief Cutting a closed triangle mesh along a plane into two closed, independently simulated
 *        solids.
 *
 * Built on fracture.h's clip_mesh(): one call for each side of the plane, then the same
 * principal-frame finishing step fracture_mesh() gives every chunk (mesh_solid.h's
 * best_frame() + centre_in_frame()), so a sliced half is a drop-in rigid piece exactly like a
 * fracture piece is -- same closed-mesh contract, same diagonal-inertia-friendly frame.
 *
 * Pure: vertices and a plane in, two pieces out. No Vulkan, no Scene, no physics.
 *
 * Inherited from clip_mesh(), stated here so a caller does not discover them by surprise: a
 * plane crossing a concave mesh in a ring-inside-a-ring fills the inner hole along with the
 * outer one, and an open shell (a plane, a cloth, a mesh with a hole through the cut) slices
 * into open shells that enclose no volume -- caught by `min_volume_fraction` and dropped.
 */

#ifndef TOYENGINE_DESTRUCT_SLICE_H
#define TOYENGINE_DESTRUCT_SLICE_H

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <toyengine/destruct/mesh_solid.h>

namespace toy {
namespace destruct {

/**
 * @struct SlicePiece
 * @brief One side of a cut: a closed mesh in its own principal frame, plus where that frame
 *        sits in the space the source mesh was given in.
 */
struct SlicePiece {
    std::vector<Vertex>        vertices;
    std::vector<std::uint32_t> indices;
    /** @brief Triangles from here on (into `indices`) are the new cut face, not the source
     *         mesh's own surface -- what gives the cut an interior material (see slice.h's
     *         file doc). Equal to indices.size() when the cut was not capped. */
    std::uint32_t cap_first_index = 0;
    glm::vec3 centre{0.0f};             /**< Where this piece's frame sits in the source's space. */
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f}; /**< Its principal frame. */
    glm::vec3 half_extent{0.0f};
    float     volume = 0.0f;
};

/**
 * @struct SliceSettings
 * @brief How to cut a mesh.
 */
struct SliceSettings {
    /** @brief Close the cut with a fan over the new opening. False leaves two open shells --
     *         rarely useful on its own, but what a caller doing its own capping wants. */
    bool  cap = true;

    /** @brief A half below this fraction of the source mesh's volume is dropped, and so is its
     *         sibling -- a plane that barely clips a corner should not spawn a sliver piece
     *         with no meaningful mass. */
    float min_volume_fraction = 1e-3f;
};

/**
 * @brief Cuts a closed triangle mesh by the plane `dot(normal, p) == distance`.
 *
 * @param vertices Source vertices (object space).
 * @param indices  Source triangle indices.
 * @param normal   Unit plane normal. `out_negative` is the side with `dot(normal, p) < distance`.
 * @param distance Plane offset along `normal`.
 * @return False when the plane misses the mesh entirely, or either half comes back empty or
 *         under `min_volume_fraction` of the source -- the caller should then leave the source
 *         object alone rather than replacing it with a copy of itself.
 */
bool slice_mesh(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices,
               const glm::vec3& normal, float distance, const SliceSettings& settings,
               SlicePiece& out_negative, SlicePiece& out_positive);

} // namespace destruct
} // namespace toy

#endif // TOYENGINE_DESTRUCT_SLICE_H

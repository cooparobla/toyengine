/**
 * @file shader_ball.h
 * @brief The material viewer's shader ball: curved and hard-edged surfaces in one shape.
 *
 * A sphere alone only shows a material on smooth curvature. The shader ball adds what it
 * can't: a thick outer shell with a quarter of its upper half cut away (flat, square-edged
 * cut walls, and the shell's inside visible through the opening), a smaller core ball inside
 * the cut, a neck, and a square stepped plinth with a chamfered top edge -- so a material is
 * seen on convex and concave curves, flat planes, sharp 90-degree and 45-degree edges, and in
 * the shadowed cavity at once. Z-up, counter-clockwise front faces, every part closed.
 */

#ifndef TOYEDITOR_MESH_SHADER_BALL_H
#define TOYEDITOR_MESH_SHADER_BALL_H

#include "primitives.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace toy::editor {

namespace detail {
/**
 * @brief Adds a polygon over existing vertices, wound so its normal faces `outward` (the
 *        builders below give an intended facing instead of tracking winding by hand).
 */
void add_facing(EditMesh& m, std::vector<Corner> corners, const glm::vec3& outward, bool smooth);

/** @brief Appends `src` moved by `offset` as separate geometry. */
void append_offset(EditMesh& dst, const EditMesh& src, const glm::vec3& offset);

/**
 * @brief A square slab: vertical sides from `z0` to `z1 - chamfer`, then a 45-degree chamfer
 *        band up to a top of half-width `half - chamfer` at `z1`. Flat-shaded.
 */
void add_chamfered_slab(EditMesh& m, float half, float z0, float z1, float chamfer);
}  // namespace detail

/**
 * @brief The shader ball (see the file comment). The cut faces `cut_azimuth_deg` (0 = +X,
 *        counter-clockwise; the default faces the editor camera's default view).
 */
EditMesh make_shader_ball(float cut_azimuth_deg = -55.0f);

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_SHADER_BALL_H

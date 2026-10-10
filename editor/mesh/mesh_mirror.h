/**
 * @file mesh_mirror.h
 * @brief Mirroring for Edit and Sculpt Mode, as pure functions over an EditMesh (testable).
 *
 * Two features share this:
 *   - Symmetry (the X / Y / Z toggles in the Edit and Sculpt headers): an edit on one side is
 *     repeated on the other. Edit Mode moves each moved vertex's mirror partner to the
 *     mirrored position (and keeps vertices on the mirror plane on it); Sculpt Mode repeats
 *     every dab at its mirror images.
 *   - Mirror (Mesh > Mirror, Ctrl M): reflects the selection across a plane through the pivot.
 *
 * Both work in **Local** space (the mesh's own axes, through its origin, as Blender does) or
 * **Global** space (the world axes, through the world origin). Every mirror is expressed as a
 * mesh-local affine matrix, so a Global mirror is just W^-1 * R * W for the mesh's world W.
 */

#ifndef TOYEDITOR_MESH_MESH_MIRROR_H
#define TOYEDITOR_MESH_MESH_MIRROR_H

#include "edit_mesh.h"
#include "mesh_ops.h"   // MeshSelection

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <vector>

namespace toy::editor {

/** @brief Which axes edits are mirrored across, and in which space. */
struct MirrorSettings {
    bool axis[3] = {false, false, false};
    bool global = false;   ///< World axes through the world origin; else the mesh's local axes.
    bool any() const { return axis[0] || axis[1] || axis[2]; }
};

/** @brief The reflection across world/local plane `a` (the plane whose normal is axis a). */
glm::mat4 axis_reflection(int a);

/** @brief One mirror image: the mesh-local matrix, and the axes it flips (bit mask). */
struct MirrorImage {
    glm::mat4 local{1.0f};
    int mask = 0;
};

/**
 * @brief Every mirror image the settings ask for (each non-empty combination of the enabled
 *        axes: X, Y, XY, Z, ...), as mesh-local matrices for a mesh at world matrix `world`.
 */
std::vector<MirrorImage> mirror_images(const MirrorSettings& s, const glm::mat4& world);

/** @brief A matching tolerance for "the same position" on this mesh (scale-relative). */
float mirror_tolerance(const EditMesh& m);

/**
 * @brief Which vertex mirrors which, for the vertices a transform moves -- captured once at
 *        the start of the transform (positions before any movement).
 */
struct MirrorMap {
    struct Pair { uint32_t src, dst; glm::mat4 m; };
    std::vector<Pair> pairs;   ///< dst follows src through m.
    struct OnPlane { uint32_t v; glm::mat4 m; };
    std::vector<OnPlane> on_plane;   ///< Moved vertices on a single-axis mirror plane: kept on it.
    bool empty() const { return pairs.empty() && on_plane.empty(); }
};

/**
 * @brief Pairs every vertex in `moved` with its mirror partner (the vertex at its mirrored
 *        position, within a tolerance) for each mirror image. Partners that are themselves
 *        moved are left alone (they're transformed directly); unmatched vertices just don't
 *        mirror, as in Blender with asymmetric topology.
 */
MirrorMap build_mirror_map(const EditMesh& m, const std::set<uint32_t>& moved, const MirrorSettings& s,
                                  const glm::mat4& world);

/** @brief After the moved vertices have moved: keep plane vertices on their plane, move partners. */
void apply_mirror_map(EditMesh& m, const MirrorMap& map);

/**
 * @brief Mesh > Mirror: reflects the selected vertices by the mesh-local matrix `reflect`.
 *        Faces that are wholly selected get their winding reversed so they keep facing out
 *        (a reflection turns a surface inside out).
 */
void mirror_selection(EditMesh& m, const MeshSelection& sel, const glm::mat4& reflect);

/**
 * @brief The mesh-local matrix that reflects across the plane through `pivot_local` whose
 *        normal is axis `a` -- of the world (`global`) or of the mesh itself.
 */
glm::mat4 mirror_plane_matrix(int a, bool global, const glm::vec3& pivot_local, const glm::mat4& world);

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_MESH_MIRROR_H

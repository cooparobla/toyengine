/**
 * @file mesh_ops.h
 * @brief Selection and modelling operations on EditMesh.
 *
 * Every operation is a function of (mesh, selection, parameters) that edits the mesh in
 * place and updates the selection to what the user expects to keep working on (Blender's
 * convention: after an extrude, the new cap is selected). Undo is the caller's snapshot of
 * the whole EditMesh, so none of these needs an inverse.
 */

#ifndef TOYEDITOR_MESH_MESH_OPS_H
#define TOYEDITOR_MESH_MESH_OPS_H

#include "edit_mesh.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace toy::editor {

enum class SelectMode { Vertex, Edge, Face };

struct MeshSelection {
    SelectMode mode = SelectMode::Face;
    std::set<uint32_t> verts;
    std::set<Edge> edges;
    std::set<uint32_t> faces;

    bool empty() const { return verts.empty() && edges.empty() && faces.empty(); }
    void clear();

    /**
     * @brief The vertices the selection touches, whatever the mode -- what a transform moves.
     */
    std::set<uint32_t> affected_vertices(const EditMesh& m) const;

    /** @brief Faces whose every vertex is selected (vertex/edge modes) or the face set. */
    std::set<uint32_t> affected_faces(const EditMesh& m) const;

    void select_all(const EditMesh& m);

    /** @brief Drops indices that no longer exist after a topology change. */
    void validate(const EditMesh& m);
};

/** @brief Centroid of the selection's vertices (the gizmo pivot). */
glm::vec3 selection_center(const EditMesh& m, const MeshSelection& sel);

/**
 * @brief Blender's Normal transform orientation (mesh-local axes, columns): Z is the
 *        area-weighted normal of the selected faces (vertex / edge modes: the faces around
 *        the selection), X follows the longest selected edge with Z projected out.
 */
glm::mat3 normal_basis(const EditMesh& m, const MeshSelection& sel);

/** @brief Applies `xf` (about the origin) to every selected vertex. */
void transform_selection(EditMesh& m, const MeshSelection& sel, const glm::mat4& xf);

/** @brief Moves every selected vertex by `d`. */
inline void translate_selection(EditMesh& m, const MeshSelection& sel, const glm::vec3& d) {
    transform_selection(m, sel, glm::translate(glm::mat4(1.0f), d));
}

/**
 * @brief Extrudes the selected faces as one region `distance` along their average normal:
 *        the region's vertices are duplicated, the faces move to the copies, and a quad is
 *        added along every region-boundary edge. The new cap stays selected.
 */
void extrude_faces(EditMesh& m, MeshSelection& sel, float distance);

/**
 * @brief Extrudes selected (open/boundary) edges into new quads `offset` away.
 *        The new outer edges become the selection.
 */
void extrude_edges(EditMesh& m, MeshSelection& sel, const glm::vec3& offset);

/**
 * @brief Insets each selected face individually: an inner copy scaled toward its centre by
 *        `amount` (0..1 of the way), joined to the original rim by quads. Inner faces stay selected.
 */
void inset_faces(EditMesh& m, MeshSelection& sel, float amount);

/**
 * @brief Chamfers each selected manifold edge into a strip `width` wide (one segment).
 *
 * Each endpoint is split into two new vertices slid `width` along the adjacent faces' other
 * edges; neighbouring faces that share those edges gain the new vertices, and where the
 * old corner vertex is still needed (valence > 3) a triangle closes the gap. The bevel
 * strips become the selection.
 */
void bevel_edges(EditMesh& m, MeshSelection& sel, float width);

/** @brief Deletes the selection: faces in face mode, faces touching selected edges/vertices otherwise. */
void delete_selection(EditMesh& m, MeshSelection& sel);

/** @brief Welds every selected vertex into one at their centroid. */
void merge_at_center(EditMesh& m, MeshSelection& sel);

/**
 * @brief Welds vertices closer than `dist` (all vertices, or only the selection's).
 * @return How many vertices were removed.
 */
size_t merge_by_distance(EditMesh& m, MeshSelection& sel, float dist);

/** @brief Reverses the winding (and so the normal) of the selected faces, or of all faces. */
void flip_normals(EditMesh& m, const MeshSelection& sel);

/** @brief Sets smooth/flat shading on the selected faces (or all). */
void set_smooth(EditMesh& m, const MeshSelection& sel, bool smooth);

/**
 * @brief Box-projects UVs: each face takes the planar projection of the axis its normal is
 *        most aligned with, `scale` UV units per world unit.
 */
void uv_box_project(EditMesh& m, const MeshSelection& sel, float scale = 1.0f);

/** @brief Planar UVs along `axis` (0 = X, 1 = Y, 2 = Z), normalized to the selection bounds. */
void uv_planar_project(EditMesh& m, const MeshSelection& sel, int axis);

/**
 * @brief Blender's F: makes one face from the selected vertices (three or more), ordered
 *        by angle around their centroid and wound to face away from the mesh's centre.
 * @return False if fewer than three vertices are selected.
 */
bool fill_face(EditMesh& m, MeshSelection& sel);

/** @brief Shift+D in edit mode: copies the selected faces onto new vertices; the copy is selected. */
void duplicate_faces(EditMesh& m, MeshSelection& sel);

/** @brief Selects every element connected to the current selection. */
void select_linked(const EditMesh& m, MeshSelection& sel);

/** @brief Converts the selection to another mode, keeping what it covers. */
void convert_selection(const EditMesh& m, MeshSelection& sel, SelectMode to);

} // namespace toy::editor

#endif // TOYEDITOR_MESH_MESH_OPS_H

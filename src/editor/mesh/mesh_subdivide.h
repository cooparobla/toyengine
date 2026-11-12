/**
 * @file mesh_subdivide.h
 * @brief Topology operators that keep a quad-first workflow: Subdivide, Catmull-Clark,
 *        Triangulate, Tris to Quads, Dissolve and Bridge Edge Loops (Blender's names).
 *
 * All edit an EditMesh in place, interpolate per-face corner UVs, and leave the selection on
 * the result. Shared edge vertices are keyed by the undirected edge and stored in min -> max
 * order, so neighbouring faces agree on them.
 */

#ifndef TOYEDITOR_MESH_MESH_SUBDIVIDE_H
#define TOYEDITOR_MESH_MESH_SUBDIVIDE_H

#include "mesh_loops.h"
#include "mesh_ops.h"
#include "mesh_topology.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>

namespace toy::editor {

namespace subdiv_detail {

glm::vec2 corner_uv(const Face& f, uint32_t v);

/** @brief A corner part-way between two (UV and colour interpolated). */
inline Corner lerp_corner(uint32_t v, const Corner& a, const Corner& b, float t) {
    return {v, glm::mix(a.uv, b.uv, t), glm::mix(a.color, b.color, t)};
}

/** @brief Cut vertices on undirected edges, created on demand in min -> max order. */
struct EdgeCuts {
    EditMesh* m;
    int n;
    std::map<Edge, std::vector<uint32_t>> cuts;
    const std::vector<uint32_t>& get(uint32_t x, uint32_t y);
    /** @brief The j-th point (0..n+1) walking x -> y. */
    uint32_t at(uint32_t x, uint32_t y, int j);
    bool has(uint32_t x, uint32_t y) const { return cuts.count(make_edge(x, y)) > 0; }
};

}  // namespace subdiv_detail

/**
 * @brief Subdivide: every edge of the selected faces (Edge mode: faces whose edges are all
 *        selected) gets `cuts` vertices; quads become a grid, triangles a triangle lattice,
 *        other n-gons fan around a centre point. Unselected neighbours keep the new edge
 *        vertices in their outline, so the mesh stays closed. The new faces are selected.
 */
void subdivide(EditMesh& m, MeshSelection& sel, int cuts);

/**
 * @brief Catmull-Clark subdivision of the whole mesh (`levels` times): every k-gon becomes k
 *        quads and the surface smooths toward its limit. Boundaries use the crease rules.
 *        UVs are interpolated linearly (no UV smoothing). Selection is cleared.
 */
void catmull_clark(EditMesh& m, MeshSelection& sel, int levels = 1);

/** @brief Ear-clips one face into triangles (indices into its corner list). */
std::vector<std::array<size_t, 3>> triangulate_face(const EditMesh& m, size_t f);

/** @brief Ctrl+T: triangulates the selected faces (quads on their shorter diagonal). */
void triangulate(EditMesh& m, MeshSelection& sel);

/**
 * @brief Alt+J: joins pairs of selected triangles sharing an edge into quads, best pairs
 *        first, within `max_face_deg` (angle between the two normals) and `max_shape_deg`
 *        (how far the quad's corners stray from 90 degrees).
 */
void tris_to_quads(EditMesh& m, MeshSelection& sel, float max_face_deg = 40.0f, float max_shape_deg = 40.0f);

/**
 * @brief Removes vertices that only join two edges (left behind on a straight run by a
 *        dissolve), when they are in `candidates`. Returns how many were removed.
 */
size_t dissolve_valence2(EditMesh& m, const std::set<uint32_t>& candidates);

/**
 * @brief Merges the faces on either side of each edge in `edges` into single faces (Blender's
 *        Dissolve Edges; with `cleanup_verts`, straight-run vertices left with two edges go
 *        too). Groups that would produce a face with a hole are skipped; `skipped` counts them.
 */
void dissolve_edges(EditMesh& m, MeshSelection& sel, const std::set<Edge>& edges, bool cleanup_verts = true,
                           size_t* skipped = nullptr, const std::set<uint32_t>* cleanup_only = nullptr);

/** @brief Dissolve Vertices: merges the faces around each selected vertex and removes it. */
void dissolve_verts(EditMesh& m, MeshSelection& sel);

/**
 * @brief Ctrl+E Bridge Edge Loops: the selected edges must form two closed boundary loops
 *        of equal length; quads join them, matched at the rotation that minimises distance.
 */
bool bridge_edge_loops(EditMesh& m, MeshSelection& sel, std::string* err = nullptr);

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_MESH_SUBDIVIDE_H

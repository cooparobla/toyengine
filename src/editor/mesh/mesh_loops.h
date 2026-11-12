/**
 * @file mesh_loops.h
 * @brief Quad topology tools, as in Blender: edge loops and rings, Loop Cut, Edge Slide.
 *
 *   - An edge RING is the strip of edges facing each other across a run of quads: in a quad
 *     the opposite of the edge at corner k is the edge at corner k + 2. It stops at a
 *     non-quad (that face is a "terminal" -- a loop cut still splits its edge), at a
 *     boundary, or at a non-manifold edge, and is closed when it comes back to its seed.
 *   - An edge LOOP continues straight through valence-4 vertices: the next edge is the one
 *     at the vertex sharing no face with the current edge. It stops at poles (valence != 4)
 *     and follows open boundaries.
 *   - Loop Cut inserts `cuts` parallel loops across a ring: every ring quad becomes cuts + 1
 *     quads; a terminal triangle / n-gon just gains the new vertices on its edge (a triangle
 *     becomes a quad). Per-face corner UVs are interpolated, so UV seams survive.
 */

#ifndef TOYEDITOR_MESH_MESH_LOOPS_H
#define TOYEDITOR_MESH_MESH_LOOPS_H

#include "mesh_ops.h"
#include "mesh_topology.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace toy::editor {

/** @brief An ordered run of edges; for rings, faces[i] lies between edges[i] and edges[i+1]. */
struct EdgePath {
    std::vector<uint32_t> edges;           ///< Edge ids (MeshTopology).
    std::vector<uint32_t> faces;           ///< Ring only: the quads crossed.
    bool closed = false;
    uint32_t front_terminal = MeshTopology::kNone;   ///< Ring: non-quad face at the front end.
    uint32_t back_terminal = MeshTopology::kNone;    ///< Ring: non-quad face at the back end.
};

/** @brief The edge ring through `seed` (an edge id). */
EdgePath edge_ring(const MeshTopology& t, uint32_t seed);

/** @brief The edge loop through `seed` (an edge id). */
EdgePath edge_loop(const MeshTopology& t, uint32_t seed);

// =====================================================================================
// Selection
// =====================================================================================

namespace loops_detail {
void apply_edges(const MeshTopology& t, const std::vector<uint32_t>& ids, MeshSelection& sel, bool additive);
void apply_faces(const std::vector<uint32_t>& fs, MeshSelection& sel, bool additive);
}  // namespace loops_detail

/**
 * @brief Alt+click: the loop through `e`. Face mode selects the face loop (the quads of the
 *        ring across `e`), as Blender does. `additive` (Shift) toggles the loop in/out.
 */
bool select_edge_loop(const EditMesh& m, MeshSelection& sel, Edge e, bool additive);

/** @brief Ctrl+Alt+click: the ring through `e` (its quads in Face mode). */
bool select_edge_ring(const EditMesh& m, MeshSelection& sel, Edge e, bool additive);

// =====================================================================================
// Loop Cut and Edge Slide
// =====================================================================================

/** @brief One vertex's slide: t in [0, 1] moves toward `a`, t in [-1, 0] toward `b`. */
struct SlideRail {
    uint32_t v = 0;
    glm::vec3 origin{0.0f}, a{0.0f}, b{0.0f};
};

void apply_edge_slide(EditMesh& m, const std::vector<SlideRail>& rails, float t);

struct LoopCutResult {
    bool ok = false;
    std::vector<SlideRail> rails;   ///< For cuts == 1: each new vertex slides along its ring edge.
    std::string error;
};

/**
 * @brief Orients a ring: a[i] / b[i] are ring edge i's endpoints such that a[i] and a[i+1]
 *        are joined by a side of the quad between them. False for a twisted (Moebius) ring.
 */
bool orient_ring(const EditMesh& m, const MeshTopology& t, const EdgePath& ring,
                        std::vector<uint32_t>& a, std::vector<uint32_t>& b);

/**
 * @brief The edge ring a loop cut through `seed` would split, as world-independent segments
 *        (for the hover preview): for every cut j, the polyline through the ring edges at
 *        fraction s_j. Empty if no cut is possible there.
 */
std::vector<std::vector<glm::vec3>> loop_cut_preview(const EditMesh& m, const MeshTopology& t, uint32_t seed, int cuts);

/**
 * @brief Ctrl+R: cuts `cuts` loops across the ring through edge `seed_edge`. `offset` in
 *        [-1, 1] slides a single cut toward the ring's first / second endpoints. Afterwards
 *        the new edges are selected (Edge mode).
 */
LoopCutResult loop_cut(EditMesh& m, MeshSelection& sel, Edge seed_edge, int cuts, float offset = 0.0f);

/**
 * @brief Edge Slide (G G) rails for the selected edges: each selected vertex slides along
 *        the unselected edges leaving it, one per side of the selected chain. Fails on a
 *        branching selection (a vertex with more than two selected edges).
 */
std::optional<std::vector<SlideRail>> edge_slide_rails(const EditMesh& m, const std::set<Edge>& selected, std::string* err = nullptr);

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_MESH_LOOPS_H

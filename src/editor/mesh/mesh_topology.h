/**
 * @file mesh_topology.h
 * @brief Flat adjacency for an EditMesh, built once per operation.
 *
 * EditMesh is a plain face list (see edit_mesh.h for why); operations that walk the surface
 * -- edge loops and rings, loop cuts, subdivision, sculpt neighbourhoods -- need adjacency
 * many times over, so they build one MeshTopology up front instead of the std::map that
 * EditMesh::edge_faces() rebuilds on every call. Everything is CSR (offset + flat list) and
 * indexed by dense ids, O(corners) to build. It is a snapshot: rebuild after topology edits.
 */

#ifndef TOYEDITOR_MESH_MESH_TOPOLOGY_H
#define TOYEDITOR_MESH_MESH_TOPOLOGY_H

#include "edit_mesh.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace toy::editor {

struct MeshTopology {
    static constexpr uint32_t kNone = ~0u;

    std::vector<Edge> edges;                           ///< Edge id -> (min, max) vertices.
    std::unordered_map<uint64_t, uint32_t> edge_id;    ///< key(a, b) -> edge id.
    std::vector<uint32_t> ef_off, ef;                  ///< Edge -> faces (CSR).
    std::vector<uint32_t> ve_off, ve;                  ///< Vertex -> edges (CSR).
    std::vector<uint32_t> vf_off, vf;                  ///< Vertex -> faces (CSR).
    std::vector<uint32_t> corner_off;                  ///< Face -> first corner slot.
    std::vector<uint32_t> corner_edge;                 ///< Corner slot i of face f: edge (c[i], c[i+1]).

    struct Span {
        const uint32_t* b;
        const uint32_t* e;
        const uint32_t* begin() const { return b; }
        const uint32_t* end() const { return e; }
        size_t size() const { return static_cast<size_t>(e - b); }
        uint32_t operator[](size_t i) const { return b[i]; }
    };

    static uint64_t key(uint32_t a, uint32_t b);

    explicit MeshTopology(const EditMesh& m) { build(m); }
    MeshTopology() = default;

    void build(const EditMesh& m);

    /** @brief Edge id of (a, b), or kNone. */
    uint32_t find_edge(uint32_t a, uint32_t b) const;
    uint32_t find_edge(const Edge& e) const { return find_edge(e.first, e.second); }

    Span faces_of(uint32_t e) const { return {ef.data() + ef_off[e], ef.data() + ef_off[e + 1]}; }
    Span edges_of(uint32_t v) const { return {ve.data() + ve_off[v], ve.data() + ve_off[v + 1]}; }
    Span vertex_faces(uint32_t v) const { return {vf.data() + vf_off[v], vf.data() + vf_off[v + 1]}; }
    uint32_t valence(uint32_t v) const { return ve_off[v + 1] - ve_off[v]; }
    bool boundary(uint32_t e) const { return ef_off[e + 1] - ef_off[e] == 1; }
    bool manifold(uint32_t e) const { return ef_off[e + 1] - ef_off[e] <= 2; }

    /** @brief The other face across edge `e` from `f`, or kNone (boundary / non-manifold). */
    uint32_t other_face(uint32_t e, uint32_t f) const;

    /** @brief Corner index of vertex `v` in face `f`, or -1. */
    static int corner_of(const EditMesh& m, uint32_t f, uint32_t v);

    /** @brief Corner index i such that face f's edge (c[i], c[i+1]) is `e`, or -1. */
    int corner_of_edge(uint32_t f, uint32_t e) const;

    /** @brief Edge id of face f's i-th side (wrapping). */
    uint32_t face_edge(uint32_t f, int i) const;
    uint32_t face_size(uint32_t f) const { return corner_off[f + 1] - corner_off[f]; }

    /** @brief The other endpoint of edge `e` from `v`. */
    uint32_t other_vertex(uint32_t e, uint32_t v) const { return edges[e].first == v ? edges[e].second : edges[e].first; }
};

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_MESH_TOPOLOGY_H

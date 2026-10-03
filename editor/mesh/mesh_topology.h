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

    static uint64_t key(uint32_t a, uint32_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<uint64_t>(a) << 32) | b;
    }

    explicit MeshTopology(const EditMesh& m) { build(m); }
    MeshTopology() = default;

    void build(const EditMesh& m) {
        const size_t nv = m.positions.size(), nf = m.faces.size();
        edges.clear();
        edge_id.clear();
        corner_off.assign(nf + 1, 0);
        for (size_t f = 0; f < nf; ++f) corner_off[f + 1] = corner_off[f] + static_cast<uint32_t>(m.faces[f].corners.size());
        corner_edge.assign(corner_off[nf], kNone);
        edge_id.reserve(corner_off[nf]);
        std::vector<uint32_t> ef_count;
        for (size_t f = 0; f < nf; ++f) {
            const auto& c = m.faces[f].corners;
            for (size_t i = 0; i < c.size(); ++i) {
                const uint32_t a = c[i].v, b = c[(i + 1) % c.size()].v;
                auto [it, fresh] = edge_id.emplace(key(a, b), static_cast<uint32_t>(edges.size()));
                if (fresh) { edges.push_back(make_edge(a, b)); ef_count.push_back(0); }
                corner_edge[corner_off[f] + i] = it->second;
                ++ef_count[it->second];
            }
        }
        // Edge -> faces.
        ef_off.assign(edges.size() + 1, 0);
        for (size_t e = 0; e < edges.size(); ++e) ef_off[e + 1] = ef_off[e] + ef_count[e];
        ef.assign(ef_off.back(), 0);
        std::vector<uint32_t> fill(ef_off.begin(), ef_off.end() - 1);
        for (size_t f = 0; f < nf; ++f) {
            for (uint32_t s = corner_off[f]; s < corner_off[f + 1]; ++s) ef[fill[corner_edge[s]]++] = static_cast<uint32_t>(f);
        }
        // Vertex -> edges.
        ve_off.assign(nv + 1, 0);
        for (const auto& e : edges) { ++ve_off[e.first + 1]; ++ve_off[e.second + 1]; }
        for (size_t v = 0; v < nv; ++v) ve_off[v + 1] += ve_off[v];
        ve.assign(ve_off.back(), 0);
        fill.assign(ve_off.begin(), ve_off.end() - 1);
        for (uint32_t e = 0; e < edges.size(); ++e) { ve[fill[edges[e].first]++] = e; ve[fill[edges[e].second]++] = e; }
        // Vertex -> faces.
        vf_off.assign(nv + 1, 0);
        for (size_t f = 0; f < nf; ++f) for (const auto& c : m.faces[f].corners) ++vf_off[c.v + 1];
        for (size_t v = 0; v < nv; ++v) vf_off[v + 1] += vf_off[v];
        vf.assign(vf_off.back(), 0);
        fill.assign(vf_off.begin(), vf_off.end() - 1);
        for (size_t f = 0; f < nf; ++f) for (const auto& c : m.faces[f].corners) vf[fill[c.v]++] = static_cast<uint32_t>(f);
    }

    /** @brief Edge id of (a, b), or kNone. */
    uint32_t find_edge(uint32_t a, uint32_t b) const {
        auto it = edge_id.find(key(a, b));
        return it == edge_id.end() ? kNone : it->second;
    }
    uint32_t find_edge(const Edge& e) const { return find_edge(e.first, e.second); }

    Span faces_of(uint32_t e) const { return {ef.data() + ef_off[e], ef.data() + ef_off[e + 1]}; }
    Span edges_of(uint32_t v) const { return {ve.data() + ve_off[v], ve.data() + ve_off[v + 1]}; }
    Span vertex_faces(uint32_t v) const { return {vf.data() + vf_off[v], vf.data() + vf_off[v + 1]}; }
    uint32_t valence(uint32_t v) const { return ve_off[v + 1] - ve_off[v]; }
    bool boundary(uint32_t e) const { return ef_off[e + 1] - ef_off[e] == 1; }
    bool manifold(uint32_t e) const { return ef_off[e + 1] - ef_off[e] <= 2; }

    /** @brief The other face across edge `e` from `f`, or kNone (boundary / non-manifold). */
    uint32_t other_face(uint32_t e, uint32_t f) const {
        const Span fs = faces_of(e);
        if (fs.size() != 2) return kNone;
        return fs[0] == f ? fs[1] : fs[1] == f ? fs[0] : kNone;
    }

    /** @brief Corner index of vertex `v` in face `f`, or -1. */
    static int corner_of(const EditMesh& m, uint32_t f, uint32_t v) {
        const auto& c = m.faces[f].corners;
        for (size_t i = 0; i < c.size(); ++i) if (c[i].v == v) return static_cast<int>(i);
        return -1;
    }

    /** @brief Corner index i such that face f's edge (c[i], c[i+1]) is `e`, or -1. */
    int corner_of_edge(uint32_t f, uint32_t e) const {
        for (uint32_t s = corner_off[f]; s < corner_off[f + 1]; ++s) if (corner_edge[s] == e) return static_cast<int>(s - corner_off[f]);
        return -1;
    }

    /** @brief Edge id of face f's i-th side (wrapping). */
    uint32_t face_edge(uint32_t f, int i) const {
        const int n = static_cast<int>(corner_off[f + 1] - corner_off[f]);
        return corner_edge[corner_off[f] + static_cast<uint32_t>(((i % n) + n) % n)];
    }
    uint32_t face_size(uint32_t f) const { return corner_off[f + 1] - corner_off[f]; }

    /** @brief The other endpoint of edge `e` from `v`. */
    uint32_t other_vertex(uint32_t e, uint32_t v) const { return edges[e].first == v ? edges[e].second : edges[e].first; }
};

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_MESH_TOPOLOGY_H

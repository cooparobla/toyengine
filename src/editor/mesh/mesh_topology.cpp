#include "editor/mesh/mesh_topology.h"

namespace toy {
namespace editor {

uint64_t MeshTopology::key(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

void MeshTopology::build(const EditMesh& m) {
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

uint32_t MeshTopology::find_edge(uint32_t a, uint32_t b) const {
    auto it = edge_id.find(key(a, b));
    return it == edge_id.end() ? kNone : it->second;
}

uint32_t MeshTopology::other_face(uint32_t e, uint32_t f) const {
    const Span fs = faces_of(e);
    if (fs.size() != 2) return kNone;
    return fs[0] == f ? fs[1] : fs[1] == f ? fs[0] : kNone;
}

int MeshTopology::corner_of(const EditMesh& m, uint32_t f, uint32_t v) {
    const auto& c = m.faces[f].corners;
    for (size_t i = 0; i < c.size(); ++i) if (c[i].v == v) return static_cast<int>(i);
    return -1;
}

int MeshTopology::corner_of_edge(uint32_t f, uint32_t e) const {
    for (uint32_t s = corner_off[f]; s < corner_off[f + 1]; ++s) if (corner_edge[s] == e) return static_cast<int>(s - corner_off[f]);
    return -1;
}

uint32_t MeshTopology::face_edge(uint32_t f, int i) const {
    const int n = static_cast<int>(corner_off[f + 1] - corner_off[f]);
    return corner_edge[corner_off[f] + static_cast<uint32_t>(((i % n) + n) % n)];
}

} // namespace editor
} // namespace toy

#include "editor/mesh/mesh_loops.h"

namespace toy {
namespace editor {

EdgePath edge_ring(const MeshTopology& t, uint32_t seed) {
    EdgePath out;
    if (seed >= t.edges.size()) return out;
    struct Walk { std::vector<uint32_t> faces, edges; uint32_t terminal = MeshTopology::kNone; bool closed = false; };
    std::vector<char> seen_face;
    auto walk = [&](uint32_t start_face) {
        Walk w;
        uint32_t e = seed, f = start_face;
        while (f != MeshTopology::kNone) {
            if (t.face_size(f) != 4) { w.terminal = f; break; }
            if (std::find(w.faces.begin(), w.faces.end(), f) != w.faces.end()) break;
            const int k = t.corner_of_edge(f, e);
            if (k < 0) break;
            const uint32_t opp = t.face_edge(f, k + 2);
            w.faces.push_back(f);
            if (opp == seed) { w.closed = true; break; }
            w.edges.push_back(opp);
            f = t.other_face(opp, f);
            e = opp;
            if (!t.manifold(opp)) break;
        }
        return w;
    };
    const auto fs = t.faces_of(seed);
    Walk a = fs.size() >= 1 && fs.size() <= 2 ? walk(fs[0]) : Walk{};
    if (a.closed) {
        out.edges.push_back(seed);
        out.edges.insert(out.edges.end(), a.edges.begin(), a.edges.end());
        out.faces = a.faces;
        out.closed = true;
        return out;
    }
    Walk b = fs.size() == 2 ? walk(fs[1]) : Walk{};
    out.edges.assign(b.edges.rbegin(), b.edges.rend());
    out.edges.push_back(seed);
    out.edges.insert(out.edges.end(), a.edges.begin(), a.edges.end());
    out.faces.assign(b.faces.rbegin(), b.faces.rend());
    out.faces.insert(out.faces.end(), a.faces.begin(), a.faces.end());
    out.front_terminal = b.terminal;
    out.back_terminal = a.terminal;
    return out;
}

EdgePath edge_loop(const MeshTopology& t, uint32_t seed) {
    EdgePath out;
    if (seed >= t.edges.size()) return out;
    auto shares_face = [&](uint32_t e1, uint32_t e2) {
        for (uint32_t f : t.faces_of(e1)) if (t.corner_of_edge(f, e2) >= 0) return true;
        return false;
    };
    auto step = [&](uint32_t e_in, uint32_t v) -> uint32_t {
        const auto fs = t.faces_of(e_in);
        if (fs.size() == 2) {
            if (t.valence(v) != 4) return MeshTopology::kNone;
            uint32_t next = MeshTopology::kNone;
            for (uint32_t e : t.edges_of(v)) {
                if (e == e_in || shares_face(e_in, e)) continue;
                if (next != MeshTopology::kNone) return MeshTopology::kNone;
                next = e;
            }
            return next;
        }
        if (fs.size() == 1) {
            if (t.valence(v) > 3) return MeshTopology::kNone;
            uint32_t next = MeshTopology::kNone;
            for (uint32_t e : t.edges_of(v)) {
                if (e == e_in || !t.boundary(e)) continue;
                if (next != MeshTopology::kNone) return MeshTopology::kNone;
                next = e;
            }
            return next;
        }
        return MeshTopology::kNone;
    };
    auto walk = [&](uint32_t v, std::vector<uint32_t>& list) {
        uint32_t e = seed;
        for (size_t guard = 0; guard < t.edges.size(); ++guard) {
            const uint32_t n = step(e, v);
            if (n == MeshTopology::kNone) return false;
            if (n == seed) return true;
            if (std::find(list.begin(), list.end(), n) != list.end()) return false;
            list.push_back(n);
            v = t.other_vertex(n, v);
            e = n;
        }
        return false;
    };
    std::vector<uint32_t> fwd, back;
    out.closed = walk(t.edges[seed].second, fwd);
    if (!out.closed) walk(t.edges[seed].first, back);
    out.edges.assign(back.rbegin(), back.rend());
    out.edges.push_back(seed);
    out.edges.insert(out.edges.end(), fwd.begin(), fwd.end());
    return out;
}

} // namespace editor
} // namespace toy

namespace toy {
namespace editor {
namespace loops_detail {

void apply_edges(const MeshTopology& t, const std::vector<uint32_t>& ids, MeshSelection& sel, bool additive) {
    std::set<Edge> es;
    for (uint32_t e : ids) es.insert(t.edges[e]);
    std::set<uint32_t> vs;
    for (const auto& e : es) { vs.insert(e.first); vs.insert(e.second); }
    bool all_in = additive;
    if (additive) {
        for (const auto& e : es) all_in &= sel.mode == SelectMode::Edge ? sel.edges.count(e) > 0 : true;
        for (uint32_t v : vs) all_in &= sel.mode == SelectMode::Vertex ? sel.verts.count(v) > 0 : true;
    }
    if (!additive) { sel.edges.clear(); sel.verts.clear(); sel.faces.clear(); }
    for (const auto& e : es) all_in ? (void)sel.edges.erase(e) : (void)sel.edges.insert(e);
    for (uint32_t v : vs) all_in ? (void)sel.verts.erase(v) : (void)sel.verts.insert(v);
}

void apply_faces(const std::vector<uint32_t>& fs, MeshSelection& sel, bool additive) {
    bool all_in = additive && !fs.empty();
    for (uint32_t f : fs) all_in &= sel.faces.count(f) > 0;
    if (!additive) sel.faces.clear();
    for (uint32_t f : fs) all_in ? (void)sel.faces.erase(f) : (void)sel.faces.insert(f);
}

} // namespace loops_detail
} // namespace editor
} // namespace toy

namespace toy {
namespace editor {

bool select_edge_loop(const EditMesh& m, MeshSelection& sel, Edge e, bool additive) {
    const MeshTopology t(m);
    const uint32_t id = t.find_edge(e);
    if (id == MeshTopology::kNone) return false;
    if (sel.mode == SelectMode::Face) {
        loops_detail::apply_faces(edge_ring(t, id).faces, sel, additive);
        return true;
    }
    loops_detail::apply_edges(t, edge_loop(t, id).edges, sel, additive);
    return true;
}

bool select_edge_ring(const EditMesh& m, MeshSelection& sel, Edge e, bool additive) {
    const MeshTopology t(m);
    const uint32_t id = t.find_edge(e);
    if (id == MeshTopology::kNone) return false;
    const EdgePath ring = edge_ring(t, id);
    if (sel.mode == SelectMode::Face) loops_detail::apply_faces(ring.faces, sel, additive);
    else loops_detail::apply_edges(t, ring.edges, sel, additive);
    return true;
}

void apply_edge_slide(EditMesh& m, const std::vector<SlideRail>& rails, float t) {
    t = std::clamp(t, -1.0f, 1.0f);
    for (const auto& r : rails) m.positions[r.v] = t >= 0.0f ? glm::mix(r.origin, r.a, t) : glm::mix(r.origin, r.b, -t);
}

bool orient_ring(const EditMesh& m, const MeshTopology& t, const EdgePath& ring,
                        std::vector<uint32_t>& a, std::vector<uint32_t>& b) {
    const size_t ne = ring.edges.size();
    a.assign(ne, 0);
    b.assign(ne, 0);
    if (ne == 0) return false;
    a[0] = t.edges[ring.edges[0]].first;
    b[0] = t.edges[ring.edges[0]].second;
    for (size_t i = 0; i < ring.faces.size(); ++i) {
        const uint32_t q = ring.faces[i];
        const int k = t.corner_of_edge(q, ring.edges[i]);
        if (k < 0) return false;
        const auto& c = m.faces[q].corners;
        uint32_t na, nb;
        if (c[static_cast<size_t>(k)].v == a[i]) { na = c[static_cast<size_t>((k + 3) % 4)].v; nb = c[static_cast<size_t>((k + 2) % 4)].v; }
        else { na = c[static_cast<size_t>((k + 2) % 4)].v; nb = c[static_cast<size_t>((k + 3) % 4)].v; }
        if (i + 1 < ne) { a[i + 1] = na; b[i + 1] = nb; }
        else if (na != a[0]) return false;
    }
    return true;
}

std::vector<std::vector<glm::vec3>> loop_cut_preview(const EditMesh& m, const MeshTopology& t, uint32_t seed, int cuts) {
    std::vector<std::vector<glm::vec3>> lines;
    const EdgePath ring = edge_ring(t, seed);
    if (ring.faces.empty() && ring.front_terminal == MeshTopology::kNone && ring.back_terminal == MeshTopology::kNone) return lines;
    const size_t ne = ring.edges.size();
    std::vector<uint32_t> a, b;
    if (!orient_ring(m, t, ring, a, b)) return lines;
    cuts = std::max(1, cuts);
    for (int j = 1; j <= cuts; ++j) {
        const float s = static_cast<float>(j) / static_cast<float>(cuts + 1);
        std::vector<glm::vec3> line;
        for (size_t i = 0; i < ne; ++i) line.push_back(glm::mix(m.positions[a[i]], m.positions[b[i]], s));
        if (ring.closed && !line.empty()) line.push_back(line.front());
        lines.push_back(std::move(line));
    }
    return lines;
}

LoopCutResult loop_cut(EditMesh& m, MeshSelection& sel, Edge seed_edge, int cuts, float offset) {
    LoopCutResult res;
    cuts = std::clamp(cuts, 1, 100);
    const MeshTopology t(m);
    const uint32_t seed = t.find_edge(seed_edge);
    if (seed == MeshTopology::kNone) { res.error = "no such edge"; return res; }
    const EdgePath ring = edge_ring(t, seed);
    if (ring.faces.empty()) { res.error = "Loop Cut needs a quad next to the edge"; return res; }

    const size_t ne = ring.edges.size();
    std::vector<uint32_t> a, b;
    if (!orient_ring(m, t, ring, a, b)) { res.error = "the ring twists (a Moebius strip) -- can't cut it"; return res; }

    // Shared cut vertices, per ring edge.
    auto frac = [&](int j) {
        if (cuts == 1 && j == 1) return 0.5f * (1.0f + std::clamp(offset, -1.0f, 1.0f));
        return static_cast<float>(j) / static_cast<float>(cuts + 1);
    };
    std::vector<std::vector<uint32_t>> cutv(ne);
    for (size_t i = 0; i < ne; ++i) {
        for (int j = 1; j <= cuts; ++j) {
            cutv[i].push_back(m.add_vertex_lerp(glm::mix(m.positions[a[i]], m.positions[b[i]], frac(j)), a[i], b[i], frac(j)));
        }
    }
    auto P = [&](size_t i, int j) -> uint32_t { return j == 0 ? a[i] : j == cuts + 1 ? b[i] : cutv[i][static_cast<size_t>(j - 1)]; };
    auto uv_in = [&](const Face& f, uint32_t v) {
        for (const auto& c : f.corners) if (c.v == v) return c.uv;
        return glm::vec2(0.0f);
    };
    auto col_in = [&](const Face& f, uint32_t v) {
        for (const auto& c : f.corners) if (c.v == v) return c.color;
        return glm::vec4(1.0f);
    };

    // Split each ring quad into cuts + 1 quads (first piece reuses the slot).
    std::set<Edge> new_edges;
    const size_t nfaces = ring.faces.size();
    std::vector<Face> appended;
    for (size_t i = 0; i < nfaces; ++i) {
        const size_t i1 = (i + 1) % ne;
        const Face q = m.faces[ring.faces[i]];
        const int k = t.corner_of_edge(ring.faces[i], ring.edges[i]);
        const bool forward = q.corners[static_cast<size_t>(k)].v == a[i];   // walks a_i -> b_i
        const glm::vec2 ua = uv_in(q, a[i]), ub = uv_in(q, b[i]), ua1 = uv_in(q, a[i1]), ub1 = uv_in(q, b[i1]);
        const glm::vec4 ca = col_in(q, a[i]), cb = col_in(q, b[i]), ca1 = col_in(q, a[i1]), cb1 = col_in(q, b[i1]);
        for (int j = 0; j <= cuts; ++j) {
            const float s0 = j == 0 ? 0.0f : frac(j), s1 = j + 1 == cuts + 1 ? 1.0f : frac(j + 1);
            Face piece;
            piece.smooth = q.smooth;
            piece.slot = q.slot;
            Corner c0{P(i, j), glm::mix(ua, ub, s0), glm::mix(ca, cb, s0)}, c1{P(i, j + 1), glm::mix(ua, ub, s1), glm::mix(ca, cb, s1)};
            Corner c2{P(i1, j + 1), glm::mix(ua1, ub1, s1), glm::mix(ca1, cb1, s1)}, c3{P(i1, j), glm::mix(ua1, ub1, s0), glm::mix(ca1, cb1, s0)};
            piece.corners = forward ? std::vector<Corner>{c0, c1, c2, c3} : std::vector<Corner>{c3, c2, c1, c0};
            if (j == 0) m.faces[ring.faces[i]] = piece;
            else appended.push_back(piece);
            if (j >= 1) new_edges.insert(make_edge(P(i, j), P(i1, j)));
        }
    }
    // Terminal faces gain the cut vertices on their ring edge.
    auto insert_into = [&](uint32_t f, size_t ring_index) {
        if (f == MeshTopology::kNone) return;
        Face& face = m.faces[f];
        const uint32_t va = a[ring_index], vb = b[ring_index];
        const size_t n = face.corners.size();
        for (size_t c = 0; c < n; ++c) {
            const uint32_t x = face.corners[c].v, y = face.corners[(c + 1) % n].v;
            if (!((x == va && y == vb) || (x == vb && y == va))) continue;
            const glm::vec2 ux = face.corners[c].uv, uy = face.corners[(c + 1) % n].uv;
            const glm::vec4 cx = face.corners[c].color, cy = face.corners[(c + 1) % n].color;
            std::vector<Corner> ins;
            for (int j = 1; j <= cuts; ++j) {
                // Walking x -> y: from a toward b when x == a.
                const int jj = x == va ? j : cuts + 1 - j;
                const float s = frac(jj);
                const float along = x == va ? s : 1.0f - s;
                ins.push_back({P(ring_index, jj), glm::mix(ux, uy, along), glm::mix(cx, cy, along)});
            }
            face.corners.insert(face.corners.begin() + static_cast<std::ptrdiff_t>(c + 1), ins.begin(), ins.end());
            return;
        }
    };
    if (!ring.closed) {
        insert_into(ring.front_terminal, 0);
        insert_into(ring.back_terminal, ne - 1);
    }
    for (auto& f : appended) m.faces.push_back(std::move(f));

    sel.clear();
    sel.mode = SelectMode::Edge;
    sel.edges = new_edges;
    for (const auto& e : new_edges) { sel.verts.insert(e.first); sel.verts.insert(e.second); }
    if (cuts == 1) {
        for (size_t i = 0; i < ne; ++i) res.rails.push_back({cutv[i][0], m.positions[cutv[i][0]], m.positions[a[i]], m.positions[b[i]]});
    }
    res.ok = true;
    return res;
}

std::optional<std::vector<SlideRail>> edge_slide_rails(const EditMesh& m, const std::set<Edge>& selected, std::string* err) {
    if (selected.empty()) { if (err) *err = "select edges to slide"; return std::nullopt; }
    const MeshTopology t(m);
    std::map<uint32_t, std::vector<Edge>> at;
    for (const auto& e : selected) { at[e.first].push_back(e); at[e.second].push_back(e); }
    for (const auto& [v, es] : at) {
        if (es.size() > 2) { if (err) *err = "Edge Slide needs a non-branching edge selection"; return std::nullopt; }
    }
    // Orient chains so "left" means the same side all along a chain.
    std::set<Edge> done;
    std::map<uint32_t, std::pair<std::optional<uint32_t>, std::optional<uint32_t>>> side;   // v -> (left, right) targets
    for (const auto& start : selected) {
        if (done.count(start)) continue;
        // Walk to one end of this chain.
        uint32_t v = start.first;
        Edge e = start;
        for (size_t guard = 0; guard < selected.size(); ++guard) {
            const auto& es = at[v];
            if (es.size() < 2) break;
            const Edge nxt = es[0] == e ? es[1] : es[0];
            if (nxt == start) break;
            e = nxt;
            v = e.first == v ? e.second : e.first;
        }
        // Now walk forward from v along the chain, orienting each edge v -> w.
        for (size_t guard = 0; guard <= selected.size(); ++guard) {
            if (done.count(e)) break;
            done.insert(e);
            const uint32_t w = e.first == v ? e.second : e.first;
            const uint32_t id = t.find_edge(e);
            if (id != MeshTopology::kNone) {
                for (uint32_t f : t.faces_of(id)) {
                    const auto& c = m.faces[f].corners;
                    const size_t n = c.size();
                    const int kv = MeshTopology::corner_of(m, f, v);
                    if (kv < 0) continue;
                    const bool left = c[(static_cast<size_t>(kv) + 1) % n].v == w;
                    // The face's other edge at v (and at w): its far endpoint is the rail target.
                    const uint32_t from_v = left ? c[(static_cast<size_t>(kv) + n - 1) % n].v : c[(static_cast<size_t>(kv) + 1) % n].v;
                    const int kw = MeshTopology::corner_of(m, f, w);
                    const uint32_t from_w = left ? c[(static_cast<size_t>(kw) + 1) % n].v : c[(static_cast<size_t>(kw) + n - 1) % n].v;
                    auto& sv = side[v];
                    auto& sw = side[w];
                    if (left) { if (!sv.first) sv.first = from_v; if (!sw.first) sw.first = from_w; }
                    else { if (!sv.second) sv.second = from_v; if (!sw.second) sw.second = from_w; }
                }
            }
            const auto& es = at[w];
            if (es.size() < 2) break;
            const Edge nxt = es[0] == e ? es[1] : es[0];
            v = w;
            e = nxt;
        }
    }
    std::vector<SlideRail> rails;
    for (const auto& [v, s] : side) {
        SlideRail r;
        r.v = v;
        r.origin = m.positions[v];
        r.a = s.first ? m.positions[*s.first] : r.origin;
        r.b = s.second ? m.positions[*s.second] : r.origin;
        rails.push_back(r);
    }
    return rails;
}

} // namespace editor
} // namespace toy

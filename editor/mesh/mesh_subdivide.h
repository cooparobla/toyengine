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

inline glm::vec2 corner_uv(const Face& f, uint32_t v) {
    for (const auto& c : f.corners) if (c.v == v) return c.uv;
    return glm::vec2(0.0f);
}

/** @brief Cut vertices on undirected edges, created on demand in min -> max order. */
struct EdgeCuts {
    EditMesh* m;
    int n;
    std::map<Edge, std::vector<uint32_t>> cuts;
    const std::vector<uint32_t>& get(uint32_t x, uint32_t y) {
        const Edge e = make_edge(x, y);
        auto it = cuts.find(e);
        if (it != cuts.end()) return it->second;
        std::vector<uint32_t> ids;
        for (int j = 1; j <= n; ++j) {
            ids.push_back(static_cast<uint32_t>(m->positions.size()));
            const float s = static_cast<float>(j) / static_cast<float>(n + 1);
            m->positions.push_back(glm::mix(m->positions[e.first], m->positions[e.second], s));
        }
        return cuts.emplace(e, std::move(ids)).first->second;
    }
    /** @brief The j-th point (0..n+1) walking x -> y. */
    uint32_t at(uint32_t x, uint32_t y, int j) {
        if (j == 0) return x;
        if (j == n + 1) return y;
        const auto& ids = get(x, y);
        return x < y ? ids[static_cast<size_t>(j - 1)] : ids[static_cast<size_t>(n - j)];
    }
    bool has(uint32_t x, uint32_t y) const { return cuts.count(make_edge(x, y)) > 0; }
};

}  // namespace subdiv_detail

/**
 * @brief Subdivide: every edge of the selected faces (Edge mode: faces whose edges are all
 *        selected) gets `cuts` vertices; quads become a grid, triangles a triangle lattice,
 *        other n-gons fan around a centre point. Unselected neighbours keep the new edge
 *        vertices in their outline, so the mesh stays closed. The new faces are selected.
 */
inline void subdivide(EditMesh& m, MeshSelection& sel, int cuts) {
    using namespace subdiv_detail;
    cuts = std::clamp(cuts, 1, 32);
    std::set<uint32_t> scope = sel.affected_faces(m);
    if (scope.empty()) return;
    const int N = cuts + 1;
    EdgeCuts ec{&m, cuts, {}};
    const size_t old_faces = m.faces.size();
    std::vector<Face> out_faces;
    std::vector<char> replaced(old_faces, 0);
    std::set<uint32_t> new_sel;

    for (uint32_t fi : scope) {
        const Face f = m.faces[fi];
        const auto& c = f.corners;
        const size_t k = c.size();
        replaced[fi] = 1;
        if (k == 4) {
            // Grid point (i along c0->c1, j along c0->c3).
            std::vector<uint32_t> grid(static_cast<size_t>((N + 1) * (N + 1)));
            std::vector<glm::vec2> guv(grid.size());
            auto G = [&](int i, int j) -> uint32_t& { return grid[static_cast<size_t>(j * (N + 1) + i)]; };
            auto UV = [&](int i, int j) -> glm::vec2& { return guv[static_cast<size_t>(j * (N + 1) + i)]; };
            for (int j = 0; j <= N; ++j) {
                for (int i = 0; i <= N; ++i) {
                    const float u = static_cast<float>(i) / N, v = static_cast<float>(j) / N;
                    UV(i, j) = glm::mix(glm::mix(c[0].uv, c[1].uv, u), glm::mix(c[3].uv, c[2].uv, u), v);
                    uint32_t id;
                    if (j == 0) id = ec.at(c[0].v, c[1].v, i);
                    else if (j == N) id = ec.at(c[3].v, c[2].v, i);
                    else if (i == 0) id = ec.at(c[0].v, c[3].v, j);
                    else if (i == N) id = ec.at(c[1].v, c[2].v, j);
                    else {
                        id = static_cast<uint32_t>(m.positions.size());
                        const glm::vec3 p = glm::mix(glm::mix(m.positions[c[0].v], m.positions[c[1].v], u),
                                                     glm::mix(m.positions[c[3].v], m.positions[c[2].v], u), v);
                        m.positions.push_back(p);
                    }
                    G(i, j) = id;
                }
            }
            for (int j = 0; j < N; ++j) {
                for (int i = 0; i < N; ++i) {
                    Face q;
                    q.smooth = f.smooth;
                    q.slot = f.slot;
                    q.corners = {{G(i, j), UV(i, j)}, {G(i + 1, j), UV(i + 1, j)}, {G(i + 1, j + 1), UV(i + 1, j + 1)}, {G(i, j + 1), UV(i, j + 1)}};
                    out_faces.push_back(q);
                }
            }
        } else if (k == 3) {
            // Lattice point (i along c0->c1, j along c0->c2), i + j <= N.
            std::map<std::pair<int, int>, std::pair<uint32_t, glm::vec2>> pts;
            auto P = [&](int i, int j) -> std::pair<uint32_t, glm::vec2> {
                auto it = pts.find({i, j});
                if (it != pts.end()) return it->second;
                const float u = static_cast<float>(i) / N, v = static_cast<float>(j) / N;
                const glm::vec2 uv = c[0].uv + (c[1].uv - c[0].uv) * u + (c[2].uv - c[0].uv) * v;
                uint32_t id;
                if (j == 0) id = ec.at(c[0].v, c[1].v, i);
                else if (i == 0) id = ec.at(c[0].v, c[2].v, j);
                else if (i + j == N) id = ec.at(c[1].v, c[2].v, j);
                else {
                    id = static_cast<uint32_t>(m.positions.size());
                    const glm::vec3 a = m.positions[c[0].v], b = m.positions[c[1].v], d = m.positions[c[2].v];
                    m.positions.push_back(a + (b - a) * u + (d - a) * v);
                }
                return pts[{i, j}] = {id, uv};
            };
            auto tri = [&](std::pair<int, int> x, std::pair<int, int> y, std::pair<int, int> z) {
                const auto px = P(x.first, x.second), py = P(y.first, y.second), pz = P(z.first, z.second);
                Face t;
                t.smooth = f.smooth;
                t.slot = f.slot;
                t.corners = {{px.first, px.second}, {py.first, py.second}, {pz.first, pz.second}};
                out_faces.push_back(t);
            };
            for (int j = 0; j < N; ++j) {
                for (int i = 0; i + j < N; ++i) {
                    tri({i, j}, {i + 1, j}, {i, j + 1});
                    if (i + j < N - 1) tri({i + 1, j}, {i + 1, j + 1}, {i, j + 1});
                }
            }
        } else {
            // N-gon: a centre point; each corner becomes one face.
            glm::vec3 centre(0.0f);
            glm::vec2 cuv(0.0f);
            for (const auto& cc : c) { centre += m.positions[cc.v]; cuv += cc.uv; }
            centre /= static_cast<float>(k);
            cuv /= static_cast<float>(k);
            const uint32_t cid = static_cast<uint32_t>(m.positions.size());
            m.positions.push_back(centre);
            for (size_t i = 0; i < k; ++i) {
                const Corner& cur = c[i];
                const Corner& nxt = c[(i + 1) % k];
                const Corner& prv = c[(i + k - 1) % k];
                Face q;
                q.smooth = f.smooth;
                q.slot = f.slot;
                q.corners.push_back(cur);
                // Forward edge up to ceil(N/2), backward edge up to floor(N/2): with an odd N
                // the middle segment belongs to exactly one of the two corner faces.
                for (int j = 1; j <= (N + 1) / 2 && j <= cuts; ++j) {
                    const float s = static_cast<float>(j) / N;
                    q.corners.push_back({ec.at(cur.v, nxt.v, j), glm::mix(cur.uv, nxt.uv, s)});
                }
                q.corners.push_back({cid, cuv});
                std::vector<Corner> back;
                for (int j = 1; j <= N / 2 && j <= cuts; ++j) {
                    const float s = static_cast<float>(j) / N;
                    back.push_back({ec.at(cur.v, prv.v, j), glm::mix(cur.uv, prv.uv, s)});
                }
                for (auto it = back.rbegin(); it != back.rend(); ++it) q.corners.push_back(*it);
                out_faces.push_back(q);
            }
        }
    }
    // Neighbours outside the scope: insert cut vertices on their cut edges.
    for (uint32_t fi = 0; fi < old_faces; ++fi) {
        if (replaced[fi]) continue;
        Face& f = m.faces[fi];
        std::vector<Corner> nc;
        const size_t k = f.corners.size();
        for (size_t i = 0; i < k; ++i) {
            const Corner& x = f.corners[i];
            const Corner& y = f.corners[(i + 1) % k];
            nc.push_back(x);
            if (!ec.has(x.v, y.v)) continue;
            for (int j = 1; j <= cuts; ++j) nc.push_back({ec.at(x.v, y.v, j), glm::mix(x.uv, y.uv, static_cast<float>(j) / N)});
        }
        f.corners = std::move(nc);
    }
    // Replace scope faces: keep unaffected faces in place, then append the new ones.
    std::vector<Face> kept;
    for (uint32_t fi = 0; fi < old_faces; ++fi) if (!replaced[fi]) kept.push_back(std::move(m.faces[fi]));
    const uint32_t first_new = static_cast<uint32_t>(kept.size());
    for (auto& f : out_faces) kept.push_back(std::move(f));
    m.faces = std::move(kept);
    sel.clear();
    sel.mode = SelectMode::Face;
    for (uint32_t f = first_new; f < m.faces.size(); ++f) sel.faces.insert(f);
}

/**
 * @brief Catmull-Clark subdivision of the whole mesh (`levels` times): every k-gon becomes k
 *        quads and the surface smooths toward its limit. Boundaries use the crease rules.
 *        UVs are interpolated linearly (no UV smoothing). Selection is cleared.
 */
inline void catmull_clark(EditMesh& m, MeshSelection& sel, int levels = 1) {
    levels = std::clamp(levels, 1, 4);
    for (int level = 0; level < levels; ++level) {
        const MeshTopology t(m);
        const size_t nv = m.positions.size(), nf = m.faces.size(), ne = t.edges.size();
        std::vector<glm::vec3> fp(nf);
        for (size_t f = 0; f < nf; ++f) fp[f] = m.face_center(f);
        std::vector<glm::vec3> ep(ne);
        for (size_t e = 0; e < ne; ++e) {
            const auto fs = t.faces_of(static_cast<uint32_t>(e));
            const glm::vec3 a = m.positions[t.edges[e].first], b = m.positions[t.edges[e].second];
            ep[e] = fs.size() == 2 ? (a + b + fp[fs[0]] + fp[fs[1]]) * 0.25f : (a + b) * 0.5f;
        }
        std::vector<glm::vec3> vp(nv);
        for (uint32_t v = 0; v < nv; ++v) {
            const auto es = t.edges_of(v);
            std::vector<uint32_t> bnd;
            bool nonmanifold = false;
            for (uint32_t e : es) {
                const size_t c = t.faces_of(e).size();
                if (c == 1) bnd.push_back(e);
                if (c > 2) nonmanifold = true;
            }
            const glm::vec3 S = m.positions[v];
            if (nonmanifold || es.size() == 0) { vp[v] = S; continue; }
            if (!bnd.empty()) {
                if (bnd.size() == 2) {
                    vp[v] = (S * 6.0f + m.positions[t.other_vertex(bnd[0], v)] + m.positions[t.other_vertex(bnd[1], v)]) / 8.0f;
                } else {
                    vp[v] = S;   // corner of something odd: keep it
                }
                continue;
            }
            const auto fs = t.vertex_faces(v);
            glm::vec3 Q(0.0f), R(0.0f);
            for (uint32_t f : fs) Q += fp[f];
            Q /= static_cast<float>(fs.size());
            for (uint32_t e : es) R += (m.positions[t.edges[e].first] + m.positions[t.edges[e].second]) * 0.5f;
            R /= static_cast<float>(es.size());
            const float n = static_cast<float>(es.size());
            vp[v] = (Q + 2.0f * R + (n - 3.0f) * S) / n;
        }
        EditMesh out;
        out.passthrough = m.passthrough;
        out.positions = vp;
        const uint32_t ebase = static_cast<uint32_t>(nv);
        for (const auto& p : ep) out.positions.push_back(p);
        const uint32_t fbase = static_cast<uint32_t>(out.positions.size());
        for (const auto& p : fp) out.positions.push_back(p);
        for (uint32_t f = 0; f < nf; ++f) {
            const auto& c = m.faces[f].corners;
            const size_t k = c.size();
            glm::vec2 fuv(0.0f);
            for (const auto& cc : c) fuv += cc.uv;
            fuv /= static_cast<float>(k);
            for (size_t i = 0; i < k; ++i) {
                const Corner& cur = c[i];
                const Corner& nxt = c[(i + 1) % k];
                const Corner& prv = c[(i + k - 1) % k];
                Face q;
                q.smooth = m.faces[f].smooth;
                q.slot = m.faces[f].slot;
                q.corners = {{cur.v, cur.uv},
                             {ebase + t.face_edge(f, static_cast<int>(i)), (cur.uv + nxt.uv) * 0.5f},
                             {fbase + f, fuv},
                             {ebase + t.face_edge(f, static_cast<int>(i) - 1), (cur.uv + prv.uv) * 0.5f}};
                out.faces.push_back(q);
            }
        }
        m = std::move(out);
    }
    sel.clear();
}

/** @brief Ear-clips one face into triangles (indices into its corner list). */
inline std::vector<std::array<size_t, 3>> triangulate_face(const EditMesh& m, size_t f) {
    const auto& c = m.faces[f].corners;
    std::vector<std::array<size_t, 3>> tris;
    const size_t k = c.size();
    if (k < 3) return tris;
    if (k == 3) return {{0, 1, 2}};
    if (k == 4) {
        // The shorter diagonal.
        const float d02 = glm::length(m.positions[c[0].v] - m.positions[c[2].v]);
        const float d13 = glm::length(m.positions[c[1].v] - m.positions[c[3].v]);
        if (d02 <= d13) return {{0, 1, 2}, {0, 2, 3}};
        return {{0, 1, 3}, {1, 2, 3}};
    }
    // Project onto the face plane.
    const glm::vec3 n = m.face_normal(f);
    const glm::vec3 ax = std::abs(n.x) < 0.9f ? glm::normalize(glm::cross(n, glm::vec3(1, 0, 0))) : glm::normalize(glm::cross(n, glm::vec3(0, 1, 0)));
    const glm::vec3 ay = glm::cross(n, ax);
    std::vector<glm::vec2> p(k);
    for (size_t i = 0; i < k; ++i) p[i] = {glm::dot(m.positions[c[i].v], ax), glm::dot(m.positions[c[i].v], ay)};
    std::vector<size_t> idx(k);
    std::iota(idx.begin(), idx.end(), 0);
    auto cross2 = [](glm::vec2 a, glm::vec2 b, glm::vec2 q) { return (b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x); };
    auto inside = [&](glm::vec2 a, glm::vec2 b, glm::vec2 d, glm::vec2 q) {
        return cross2(a, b, q) > 0 && cross2(b, d, q) > 0 && cross2(d, a, q) > 0;
    };
    size_t guard = 0;
    while (idx.size() > 3 && guard++ < k * k) {
        bool clipped = false;
        for (size_t i = 0; i < idx.size(); ++i) {
            const size_t ia = idx[(i + idx.size() - 1) % idx.size()], ib = idx[i], ic = idx[(i + 1) % idx.size()];
            if (cross2(p[ia], p[ib], p[ic]) <= 1e-12f) continue;   // reflex
            bool ear = true;
            for (size_t j : idx) if (j != ia && j != ib && j != ic && inside(p[ia], p[ib], p[ic], p[j])) { ear = false; break; }
            if (!ear) continue;
            tris.push_back({ia, ib, ic});
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    if (idx.size() == 3) tris.push_back({idx[0], idx[1], idx[2]});
    else for (size_t i = 1; i + 1 < idx.size(); ++i) tris.push_back({idx[0], idx[i], idx[i + 1]});   // fallback fan
    return tris;
}

/** @brief Ctrl+T: triangulates the selected faces (quads on their shorter diagonal). */
inline void triangulate(EditMesh& m, MeshSelection& sel) {
    const std::set<uint32_t> scope = sel.affected_faces(m);
    if (scope.empty()) return;
    std::vector<Face> out;
    std::set<uint32_t> new_sel;
    for (uint32_t f = 0; f < m.faces.size(); ++f) {
        if (!scope.count(f) || m.faces[f].corners.size() <= 3) {
            if (scope.count(f)) new_sel.insert(static_cast<uint32_t>(out.size()));
            out.push_back(m.faces[f]);
            continue;
        }
        for (const auto& t : triangulate_face(m, f)) {
            Face tf;
            tf.smooth = m.faces[f].smooth;
            tf.slot = m.faces[f].slot;
            for (size_t i : t) tf.corners.push_back(m.faces[f].corners[i]);
            new_sel.insert(static_cast<uint32_t>(out.size()));
            out.push_back(tf);
        }
    }
    m.faces = std::move(out);
    sel.clear();
    sel.mode = SelectMode::Face;
    sel.faces = new_sel;
}

/**
 * @brief Alt+J: joins pairs of selected triangles sharing an edge into quads, best pairs
 *        first, within `max_face_deg` (angle between the two normals) and `max_shape_deg`
 *        (how far the quad's corners stray from 90 degrees).
 */
inline void tris_to_quads(EditMesh& m, MeshSelection& sel, float max_face_deg = 40.0f, float max_shape_deg = 40.0f) {
    const std::set<uint32_t> scope = sel.affected_faces(m);
    const MeshTopology t(m);
    struct Cand { float score; uint32_t f1, f2; uint32_t e; };
    std::vector<Cand> cands;
    for (uint32_t e = 0; e < t.edges.size(); ++e) {
        const auto fs = t.faces_of(e);
        if (fs.size() != 2) continue;
        const uint32_t f1 = fs[0], f2 = fs[1];
        if (!scope.count(f1) || !scope.count(f2)) continue;
        if (m.faces[f1].corners.size() != 3 || m.faces[f2].corners.size() != 3) continue;
        const float fa = glm::degrees(std::acos(std::clamp(glm::dot(m.face_normal(f1), m.face_normal(f2)), -1.0f, 1.0f)));
        if (fa > max_face_deg) continue;
        // The would-be quad: f1 = (a, b, c) with the shared edge a -> b; f2's opposite vertex d.
        const auto& c1 = m.faces[f1].corners;
        int k = t.corner_of_edge(f1, e);
        const uint32_t a = c1[static_cast<size_t>(k)].v, b = c1[static_cast<size_t>((k + 1) % 3)].v, c = c1[static_cast<size_t>((k + 2) % 3)].v;
        uint32_t d = 0;
        for (const auto& cc : m.faces[f2].corners) if (cc.v != a && cc.v != b) d = cc.v;
        const glm::vec3 q[4] = {m.positions[a], m.positions[d], m.positions[b], m.positions[c]};
        float worst = 0.0f;
        bool convex = true;
        const glm::vec3 n = glm::normalize(m.face_normal(f1) + m.face_normal(f2));
        for (int i = 0; i < 4; ++i) {
            const glm::vec3 u = q[(i + 3) % 4] - q[i], v = q[(i + 1) % 4] - q[i];
            const float lu = glm::length(u), lv = glm::length(v);
            if (lu < 1e-9f || lv < 1e-9f) { convex = false; break; }
            const float ang = glm::degrees(std::acos(std::clamp(glm::dot(u, v) / (lu * lv), -1.0f, 1.0f)));
            if (glm::dot(glm::cross(v, u), n) < 0.0f) convex = false;   // reflex corner
            worst = std::max(worst, std::abs(ang - 90.0f));
        }
        if (!convex || worst > max_shape_deg) continue;
        cands.push_back({fa + worst, f1, f2, e});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.score < y.score; });
    std::vector<char> used(m.faces.size(), 0), dead(m.faces.size(), 0);
    std::set<uint32_t> merged;
    for (const auto& cd : cands) {
        if (used[cd.f1] || used[cd.f2]) continue;
        used[cd.f1] = used[cd.f2] = 1;
        const Face f1 = m.faces[cd.f1], f2 = m.faces[cd.f2];
        const int k = t.corner_of_edge(cd.f1, cd.e);
        const Corner A = f1.corners[static_cast<size_t>(k)], B = f1.corners[static_cast<size_t>((k + 1) % 3)], C = f1.corners[static_cast<size_t>((k + 2) % 3)];
        Corner D{};
        for (const auto& cc : f2.corners) if (cc.v != A.v && cc.v != B.v) D = cc;
        Face q;
        q.smooth = f1.smooth;
        q.slot = f1.slot;
        q.corners = {A, D, B, C};
        m.faces[cd.f1] = q;
        dead[cd.f2] = 1;
        merged.insert(cd.f1);
    }
    std::vector<Face> out;
    std::set<uint32_t> new_sel;
    for (uint32_t f = 0; f < m.faces.size(); ++f) {
        if (dead[f]) continue;
        if (scope.count(f)) new_sel.insert(static_cast<uint32_t>(out.size()));
        out.push_back(std::move(m.faces[f]));
    }
    m.faces = std::move(out);
    sel.clear();
    sel.mode = SelectMode::Face;
    sel.faces = new_sel;
}

/**
 * @brief Removes vertices that only join two edges (left behind on a straight run by a
 *        dissolve), when they are in `candidates`. Returns how many were removed.
 */
inline size_t dissolve_valence2(EditMesh& m, const std::set<uint32_t>& candidates) {
    const MeshTopology t(m);
    std::set<uint32_t> drop;
    for (uint32_t v : candidates) if (v < m.positions.size() && t.valence(v) == 2) drop.insert(v);
    if (drop.empty()) return 0;
    for (auto& f : m.faces) {
        f.corners.erase(std::remove_if(f.corners.begin(), f.corners.end(), [&](const Corner& c) { return drop.count(c.v) > 0; }),
                        f.corners.end());
    }
    m.cleanup_faces();
    m.compact();
    return drop.size();
}

/**
 * @brief Merges the faces on either side of each edge in `edges` into single faces (Blender's
 *        Dissolve Edges; with `cleanup_verts`, straight-run vertices left with two edges go
 *        too). Groups that would produce a face with a hole are skipped; `skipped` counts them.
 */
inline void dissolve_edges(EditMesh& m, MeshSelection& sel, const std::set<Edge>& edges, bool cleanup_verts = true,
                           size_t* skipped = nullptr, const std::set<uint32_t>* cleanup_only = nullptr) {
    const MeshTopology t(m);
    const size_t nf = m.faces.size();
    std::vector<uint32_t> parent(nf);
    std::iota(parent.begin(), parent.end(), 0u);
    std::function<uint32_t(uint32_t)> find = [&](uint32_t x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
    std::set<uint32_t> touched_verts;
    for (const auto& e : edges) {
        const uint32_t id = t.find_edge(e);
        if (id == MeshTopology::kNone) continue;
        const auto fs = t.faces_of(id);
        if (fs.size() != 2) continue;
        parent[find(fs[0])] = find(fs[1]);
        touched_verts.insert(e.first);
        touched_verts.insert(e.second);
    }
    std::map<uint32_t, std::vector<uint32_t>> groups;
    for (uint32_t f = 0; f < nf; ++f) groups[find(f)].push_back(f);
    std::vector<char> dead(nf, 0);
    std::set<uint32_t> result_faces;
    size_t skip = 0;
    for (auto& [root, fs] : groups) {
        if (fs.size() < 2) continue;
        std::set<uint32_t> in(fs.begin(), fs.end());
        // Boundary half-edges: sides whose edge has no other face in the group.
        std::map<uint32_t, std::vector<std::pair<Corner, uint32_t>>> out_from;   // start v -> (corner, end v)
        size_t count = 0;
        for (uint32_t f : fs) {
            const auto& c = m.faces[f].corners;
            for (size_t i = 0; i < c.size(); ++i) {
                const uint32_t e = t.face_edge(f, static_cast<int>(i));
                bool internal = false;
                for (uint32_t g : t.faces_of(e)) if (g != f && in.count(g)) internal = true;
                if (internal) continue;
                out_from[c[i].v].push_back({c[i], c[(i + 1) % c.size()].v});
                ++count;
            }
        }
        bool ok = count >= 3;
        for (const auto& [v, list] : out_from) if (list.size() != 1) ok = false;
        Face merged;
        merged.smooth = m.faces[fs[0]].smooth;
        merged.slot = m.faces[fs[0]].slot;
        if (ok) {
            uint32_t v = out_from.begin()->first;
            const uint32_t start = v;
            for (size_t guard = 0; guard <= count; ++guard) {
                const auto& [corner, next] = out_from[v][0];
                merged.corners.push_back(corner);
                v = next;
                if (v == start) break;
            }
            ok = merged.corners.size() == count;
        }
        if (!ok) { ++skip; continue; }
        m.faces[fs[0]] = merged;
        result_faces.insert(fs[0]);
        for (size_t i = 1; i < fs.size(); ++i) dead[fs[i]] = 1;
    }
    std::vector<Face> out;
    std::set<uint32_t> new_sel;
    for (uint32_t f = 0; f < nf; ++f) {
        if (dead[f]) continue;
        if (result_faces.count(f)) new_sel.insert(static_cast<uint32_t>(out.size()));
        out.push_back(std::move(m.faces[f]));
    }
    m.faces = std::move(out);
    m.cleanup_faces();
    if (cleanup_verts) {
        // Vertex ids are still the originals here (no compact yet), so `touched_verts` holds.
        const MeshTopology t2(m);
        std::set<uint32_t> drop;
        for (uint32_t v : cleanup_only ? *cleanup_only : touched_verts) {
            if (v < m.positions.size() && t2.valence(v) == 2) drop.insert(v);
        }
        // Unused vertices (fully interior to a merged face) are dropped by compact().
        for (auto& f : m.faces) {
            f.corners.erase(std::remove_if(f.corners.begin(), f.corners.end(), [&](const Corner& c) { return drop.count(c.v) > 0; }),
                            f.corners.end());
        }
        m.cleanup_faces();
    }
    m.compact();
    if (skipped) *skipped = skip;
    sel.clear();
    sel.mode = SelectMode::Face;
    for (uint32_t f : new_sel) if (f < m.faces.size()) sel.faces.insert(f);
}

/** @brief Dissolve Vertices: merges the faces around each selected vertex and removes it. */
inline void dissolve_verts(EditMesh& m, MeshSelection& sel) {
    const std::set<uint32_t> vs = sel.affected_vertices(m);
    if (vs.empty()) return;
    const MeshTopology t(m);
    std::set<Edge> es;
    for (uint32_t v : vs) for (uint32_t e : t.edges_of(v)) es.insert(t.edges[e]);
    // Only the dissolved vertices themselves are cleaned up (a boundary one is left joining
    // two edges); their neighbours keep their place in the merged outline.
    dissolve_edges(m, sel, es, true, nullptr, &vs);
}

/**
 * @brief Ctrl+E Bridge Edge Loops: the selected edges must form two closed boundary loops
 *        of equal length; quads join them, matched at the rotation that minimises distance.
 */
inline bool bridge_edge_loops(EditMesh& m, MeshSelection& sel, std::string* err = nullptr) {
    const MeshTopology t(m);
    std::set<Edge> chosen = sel.mode == SelectMode::Edge ? sel.edges : std::set<Edge>{};
    if (sel.mode != SelectMode::Edge) {
        const auto vs = sel.affected_vertices(m);
        for (const auto& e : t.edges) if (vs.count(e.first) && vs.count(e.second)) chosen.insert(e);
    }
    // Boundary half-edges as their single face walks them.
    std::map<uint32_t, uint32_t> next;   // x -> y for the face's walk x -> y
    for (const auto& e : chosen) {
        const uint32_t id = t.find_edge(e);
        if (id == MeshTopology::kNone || !t.boundary(id)) { if (err) *err = "Bridge needs two boundary loops"; return false; }
        const uint32_t f = t.faces_of(id)[0];
        const int k = t.corner_of_edge(f, id);
        const auto& c = m.faces[f].corners;
        next[c[static_cast<size_t>(k)].v] = c[(static_cast<size_t>(k) + 1) % c.size()].v;
    }
    std::vector<std::vector<uint32_t>> loops;
    std::set<uint32_t> seen;
    for (const auto& [start, _] : next) {
        if (seen.count(start)) continue;
        std::vector<uint32_t> loop;
        uint32_t v = start;
        for (size_t guard = 0; guard <= next.size(); ++guard) {
            loop.push_back(v);
            seen.insert(v);
            auto it = next.find(v);
            if (it == next.end()) { loop.clear(); break; }
            v = it->second;
            if (v == start) break;
        }
        if (loop.empty() || v != start) { if (err) *err = "Bridge needs closed loops"; return false; }
        loops.push_back(loop);
    }
    if (loops.size() != 2 || loops[0].size() != loops[1].size()) {
        if (err) *err = "Bridge needs exactly two loops with the same number of edges";
        return false;
    }
    // Loop 1 reversed from its face's walk; loop 2 as its face walks it (matched in step).
    std::vector<uint32_t> L1(loops[0].rbegin(), loops[0].rend());
    const std::vector<uint32_t>& L2 = loops[1];
    const size_t n = L1.size();
    size_t best_o = 0;
    float best = 1e30f;
    for (size_t o = 0; o < n; ++o) {
        float d = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const glm::vec3 x = m.positions[L1[i]] - m.positions[L2[(o + i) % n]];
            d += glm::dot(x, x);
        }
        if (d < best) { best = d; best_o = o; }
    }
    sel.clear();
    sel.mode = SelectMode::Face;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t a = L1[i], b = L1[(i + 1) % n];
        // The bridge walks loop 1 against its face (a -> b) and loop 2 against its face (c -> d).
        const uint32_t c = L2[(best_o + i + 1) % n], d = L2[(best_o + i) % n];
        Face q;
        const float u0 = static_cast<float>(i) / n, u1 = static_cast<float>(i + 1) / n;
        q.corners = {{a, {u0, 0}}, {b, {u1, 0}}, {c, {u1, 1}}, {d, {u0, 1}}};
        sel.faces.insert(static_cast<uint32_t>(m.faces.size()));
        m.faces.push_back(q);
    }
    return true;
}

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_MESH_SUBDIVIDE_H

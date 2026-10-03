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
    void clear() { verts.clear(); edges.clear(); faces.clear(); }

    /**
     * @brief The vertices the selection touches, whatever the mode -- what a transform moves.
     */
    std::set<uint32_t> affected_vertices(const EditMesh& m) const {
        std::set<uint32_t> out;
        switch (mode) {
            case SelectMode::Vertex: out = verts; break;
            case SelectMode::Edge: for (const auto& e : edges) { out.insert(e.first); out.insert(e.second); } break;
            case SelectMode::Face:
                for (uint32_t f : faces) if (f < m.faces.size()) for (const auto& c : m.faces[f].corners) out.insert(c.v);
                break;
        }
        return out;
    }

    /** @brief Faces whose every vertex is selected (vertex/edge modes) or the face set. */
    std::set<uint32_t> affected_faces(const EditMesh& m) const {
        if (mode == SelectMode::Face) return faces;
        const auto vs = affected_vertices(m);
        std::set<uint32_t> out;
        for (uint32_t f = 0; f < m.faces.size(); ++f) {
            bool all = !m.faces[f].corners.empty();
            for (const auto& c : m.faces[f].corners) all &= vs.count(c.v) > 0;
            if (all) out.insert(f);
        }
        return out;
    }

    void select_all(const EditMesh& m) {
        clear();
        for (uint32_t v = 0; v < m.positions.size(); ++v) verts.insert(v);
        for (const auto& e : m.edges()) edges.insert(e);
        for (uint32_t f = 0; f < m.faces.size(); ++f) faces.insert(f);
    }

    /** @brief Drops indices that no longer exist after a topology change. */
    void validate(const EditMesh& m) {
        for (auto it = verts.begin(); it != verts.end();) it = *it >= m.positions.size() ? verts.erase(it) : std::next(it);
        for (auto it = faces.begin(); it != faces.end();) it = *it >= m.faces.size() ? faces.erase(it) : std::next(it);
        const auto all = m.edge_faces();
        for (auto it = edges.begin(); it != edges.end();) it = all.count(*it) ? std::next(it) : edges.erase(it);
    }
};

/** @brief Centroid of the selection's vertices (the gizmo pivot). */
inline glm::vec3 selection_center(const EditMesh& m, const MeshSelection& sel) {
    const auto vs = sel.affected_vertices(m);
    glm::vec3 c(0.0f);
    for (uint32_t v : vs) c += m.positions[v];
    return vs.empty() ? c : c / static_cast<float>(vs.size());
}

/**
 * @brief Blender's Normal transform orientation (mesh-local axes, columns): Z is the
 *        area-weighted normal of the selected faces (vertex / edge modes: the faces around
 *        the selection), X follows the longest selected edge with Z projected out.
 */
inline glm::mat3 normal_basis(const EditMesh& m, const MeshSelection& sel) {
    std::set<uint32_t> fs = sel.affected_faces(m);
    const auto vs = sel.affected_vertices(m);
    if (fs.empty()) {
        for (uint32_t f = 0; f < m.faces.size(); ++f) {
            for (const auto& c : m.faces[f].corners) if (vs.count(c.v)) { fs.insert(f); break; }
        }
    }
    glm::vec3 z(0.0f);
    for (uint32_t f : fs) {
        // Newell's vector is area-weighted before normalization.
        const auto& c = m.faces[f].corners;
        glm::vec3 n(0.0f);
        for (size_t i = 0; i < c.size(); ++i) {
            const glm::vec3& a = m.positions[c[i].v];
            const glm::vec3& b = m.positions[c[(i + 1) % c.size()].v];
            n += glm::cross(a, b);
        }
        z += n;
    }
    z = glm::length(z) > 1e-9f ? glm::normalize(z) : glm::vec3(0, 0, 1);
    glm::vec3 x(0.0f);
    float best = 0.0f;
    auto consider = [&](uint32_t a, uint32_t b) {
        glm::vec3 d = m.positions[b] - m.positions[a];
        d -= z * glm::dot(d, z);
        const float l = glm::length(d);
        if (l > best) { best = l; x = d / l; }
    };
    for (uint32_t f : fs) {
        const auto& c = m.faces[f].corners;
        for (size_t i = 0; i < c.size(); ++i) {
            const uint32_t a = c[i].v, b = c[(i + 1) % c.size()].v;
            if (vs.count(a) && vs.count(b)) consider(a, b);
        }
    }
    if (best < 1e-9f) {
        x = std::abs(z.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        x = glm::normalize(x - z * glm::dot(x, z));
    }
    return glm::mat3(x, glm::cross(z, x), z);
}

/** @brief Applies `xf` (about the origin) to every selected vertex. */
inline void transform_selection(EditMesh& m, const MeshSelection& sel, const glm::mat4& xf) {
    for (uint32_t v : sel.affected_vertices(m)) m.positions[v] = glm::vec3(xf * glm::vec4(m.positions[v], 1.0f));
}

/** @brief Moves every selected vertex by `d`. */
inline void translate_selection(EditMesh& m, const MeshSelection& sel, const glm::vec3& d) {
    transform_selection(m, sel, glm::translate(glm::mat4(1.0f), d));
}

/**
 * @brief Extrudes the selected faces as one region `distance` along their average normal:
 *        the region's vertices are duplicated, the faces move to the copies, and a quad is
 *        added along every region-boundary edge. The new cap stays selected.
 */
inline void extrude_faces(EditMesh& m, MeshSelection& sel, float distance) {
    const std::set<uint32_t> region = sel.affected_faces(m);
    if (region.empty()) return;
    glm::vec3 n(0.0f);
    for (uint32_t f : region) n += m.face_normal(f);
    n = glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0, 0, 1);

    // Count region-face uses of each edge: count 1 = boundary.
    std::map<Edge, int> use;
    for (uint32_t f : region) {
        const auto& c = m.faces[f].corners;
        for (size_t i = 0; i < c.size(); ++i) ++use[make_edge(c[i].v, c[(i + 1) % c.size()].v)];
    }
    std::map<uint32_t, uint32_t> dup;
    for (uint32_t f : region) {
        for (const auto& c : m.faces[f].corners) {
            if (!dup.count(c.v)) {
                dup[c.v] = static_cast<uint32_t>(m.positions.size());
                m.positions.push_back(m.positions[c.v] + n * distance);
            }
        }
    }
    std::vector<Face> sides;
    for (uint32_t f : region) {
        auto& c = m.faces[f].corners;
        for (size_t i = 0; i < c.size(); ++i) {
            const Corner a = c[i], b = c[(i + 1) % c.size()];
            if (use[make_edge(a.v, b.v)] != 1) continue;
            const float u0 = 0, u1 = 1;
            Face side;
            side.smooth = m.faces[f].smooth;
            side.corners = {{a.v, {u0, 0}}, {b.v, {u1, 0}}, {dup[b.v], {u1, 1}}, {dup[a.v], {u0, 1}}};
            sides.push_back(side);
        }
        for (auto& corner : c) corner.v = dup[corner.v];
    }
    for (auto& s : sides) m.faces.push_back(std::move(s));
    sel.mode = SelectMode::Face;
    sel.faces = region;
    sel.verts.clear();
    sel.edges.clear();
}

/**
 * @brief Extrudes selected (open/boundary) edges into new quads `offset` away.
 *        The new outer edges become the selection.
 */
inline void extrude_edges(EditMesh& m, MeshSelection& sel, const glm::vec3& offset) {
    if (sel.edges.empty()) return;
    const auto ef = m.edge_faces();
    std::map<uint32_t, uint32_t> dup;
    std::set<Edge> new_edges;
    auto copy = [&](uint32_t v) {
        if (!dup.count(v)) { dup[v] = static_cast<uint32_t>(m.positions.size()); m.positions.push_back(m.positions[v] + offset); }
        return dup[v];
    };
    for (const auto& e : sel.edges) {
        // Orient the new quad opposite to how the edge's (first) face uses it.
        uint32_t a = e.first, b = e.second;
        auto it = ef.find(e);
        if (it != ef.end() && !it->second.empty()) {
            const auto& c = m.faces[it->second.front()].corners;
            for (size_t i = 0; i < c.size(); ++i) {
                if (c[i].v == e.first && c[(i + 1) % c.size()].v == e.second) { a = e.second; b = e.first; break; }
            }
        }
        const uint32_t a2 = copy(a), b2 = copy(b);
        Face f;
        f.corners = {{a, {0, 0}}, {b, {1, 0}}, {b2, {1, 1}}, {a2, {0, 1}}};
        m.faces.push_back(f);
        new_edges.insert(make_edge(a2, b2));
    }
    sel.mode = SelectMode::Edge;
    sel.edges = new_edges;
}

/**
 * @brief Insets each selected face individually: an inner copy scaled toward its centre by
 *        `amount` (0..1 of the way), joined to the original rim by quads. Inner faces stay selected.
 */
inline void inset_faces(EditMesh& m, MeshSelection& sel, float amount) {
    const std::set<uint32_t> region = sel.affected_faces(m);
    amount = std::clamp(amount, 0.0f, 0.99f);
    std::vector<Face> rims;
    for (uint32_t f : region) {
        auto& c = m.faces[f].corners;
        const glm::vec3 ctr = m.face_center(f);
        glm::vec2 uv_ctr(0.0f);
        for (const auto& k : c) uv_ctr += k.uv;
        uv_ctr /= static_cast<float>(c.size());
        std::vector<Corner> inner;
        for (const auto& k : c) {
            const uint32_t nv = static_cast<uint32_t>(m.positions.size());
            m.positions.push_back(glm::mix(m.positions[k.v], ctr, amount));
            inner.push_back({nv, glm::mix(k.uv, uv_ctr, amount)});
        }
        for (size_t i = 0; i < c.size(); ++i) {
            const size_t j = (i + 1) % c.size();
            Face rim;
            rim.smooth = m.faces[f].smooth;
            rim.corners = {c[i], c[j], inner[j], inner[i]};
            rims.push_back(rim);
        }
        c = inner;
    }
    for (auto& r : rims) m.faces.push_back(std::move(r));
    sel.mode = SelectMode::Face;
    sel.faces = region;
}

/**
 * @brief Chamfers each selected manifold edge into a strip `width` wide (one segment).
 *
 * Each endpoint is split into two new vertices slid `width` along the adjacent faces' other
 * edges; neighbouring faces that share those edges gain the new vertices, and where the
 * old corner vertex is still needed (valence > 3) a triangle closes the gap. The bevel
 * strips become the selection.
 */
inline void bevel_edges(EditMesh& m, MeshSelection& sel, float width) {
    std::set<Edge> todo = sel.edges;
    if (sel.mode == SelectMode::Face) {
        // Face mode: bevel the region's boundary edges.
        std::map<Edge, int> use;
        for (uint32_t f : sel.faces) {
            const auto& c = m.faces[f].corners;
            for (size_t i = 0; i < c.size(); ++i) ++use[make_edge(c[i].v, c[(i + 1) % c.size()].v)];
        }
        for (const auto& kv : use) if (kv.second == 1) todo.insert(kv.first);
    }
    std::set<uint32_t> new_faces;
    for (const Edge& e : todo) {
        const auto ef = m.edge_faces();
        auto it = ef.find(e);
        if (it == ef.end() || it->second.size() != 2) continue;   // manifold edges only
        const uint32_t F1 = it->second[0], F2 = it->second[1];
        auto pos_in = [&](uint32_t f, uint32_t v) -> int {
            const auto& c = m.faces[f].corners;
            for (size_t i = 0; i < c.size(); ++i) if (c[i].v == v) return static_cast<int>(i);
            return -1;
        };
        // Direction of e in F1: true if F1 walks first -> second.
        bool f1_forward = false;
        {
            const auto& c = m.faces[F1].corners;
            const int i = pos_in(F1, e.first);
            f1_forward = c[(static_cast<size_t>(i) + 1) % c.size()].v == e.second;
        }
        const float len = glm::distance(m.positions[e.first], m.positions[e.second]);
        const float w = std::min(width, len * 0.45f);

        // For endpoint v (other endpoint o): split in face F into v_F, sliding along F's other edge at v.
        auto split = [&](uint32_t F, uint32_t v, uint32_t o) -> std::pair<uint32_t, uint32_t> {
            const auto& c = m.faces[F].corners;
            const int i = pos_in(F, v);
            const uint32_t prev = c[(static_cast<size_t>(i) + c.size() - 1) % c.size()].v;
            const uint32_t next = c[(static_cast<size_t>(i) + 1) % c.size()].v;
            const uint32_t other = prev == o ? next : prev;
            const glm::vec3 dir = m.positions[other] - m.positions[v];
            const float dl = glm::length(dir);
            const glm::vec3 p = m.positions[v] + (dl > 1e-8f ? dir / dl * std::min(w, dl * 0.45f) : glm::vec3(0.0f));
            const uint32_t nv = static_cast<uint32_t>(m.positions.size());
            m.positions.push_back(p);
            return {nv, other};
        };
        const auto [a1, an1] = split(F1, e.first, e.second);
        const auto [b1, bn1] = split(F1, e.second, e.first);
        const auto [a2, an2] = split(F2, e.first, e.second);
        const auto [b2, bn2] = split(F2, e.second, e.first);

        auto replace_in = [&](uint32_t F, uint32_t from, uint32_t to) {
            for (auto& c : m.faces[F].corners) if (c.v == from) c.v = to;
        };
        replace_in(F1, e.first, a1); replace_in(F1, e.second, b1);
        replace_in(F2, e.first, a2); replace_in(F2, e.second, b2);

        // Neighbours sharing the slid-along edges gain the new vertex next to the old corner.
        auto insert_between = [&](uint32_t skip1, uint32_t skip2, uint32_t v, uint32_t nbr, uint32_t nv) -> int {
            int touched = -1;
            for (uint32_t f = 0; f < m.faces.size(); ++f) {
                if (f == skip1 || f == skip2) continue;
                auto& c = m.faces[f].corners;
                for (size_t i = 0; i < c.size(); ++i) {
                    const size_t j = (i + 1) % c.size();
                    if ((c[i].v == v && c[j].v == nbr)) { c.insert(c.begin() + static_cast<long>(j), Corner{nv, c[i].uv}); touched = f; break; }
                    if ((c[i].v == nbr && c[j].v == v)) { c.insert(c.begin() + static_cast<long>(j), Corner{nv, c[j].uv}); touched = f; break; }
                }
            }
            return touched;
        };
        auto close_corner = [&](uint32_t v, uint32_t n1, uint32_t v1, uint32_t n2, uint32_t v2) {
            const int g = insert_between(F1, F2, v, n1, v1);
            const int h = insert_between(F1, F2, v, n2, v2);
            if (g >= 0 && g == h) {
                // Valence 3: the one remaining face held both slides -- the old corner just goes.
                auto& c = m.faces[static_cast<size_t>(g)].corners;
                c.erase(std::remove_if(c.begin(), c.end(), [v](const Corner& k) { return k.v == v; }), c.end());
                return;
            }
            // Higher valence: fill the gap v1 - v - v2 with a triangle, wound against the strip.
            bool still_used = false;
            for (const auto& f : m.faces) for (const auto& c : f.corners) still_used |= c.v == v;
            if (!still_used) return;
            Face tri;
            tri.corners = {{v1, {0, 0}}, {v, {0.5f, 1}}, {v2, {1, 0}}};
            m.faces.push_back(tri);
        };
        close_corner(e.first, an1, a1, an2, a2);
        close_corner(e.second, bn1, b1, bn2, b2);

        // The strip: shares a1-b1 with F1 (walked the opposite way) and a2-b2 with F2.
        Face strip;
        if (f1_forward) strip.corners = {{b1, {0, 0}}, {a1, {1, 0}}, {a2, {1, 1}}, {b2, {0, 1}}};
        else            strip.corners = {{a1, {0, 0}}, {b1, {1, 0}}, {b2, {1, 1}}, {a2, {0, 1}}};
        m.faces.push_back(strip);
        new_faces.insert(static_cast<uint32_t>(m.faces.size() - 1));
    }
    // Triangles added at high-valence corners may be wound against their neighbours; make
    // every face agree with an adjacent face across a shared edge.
    for (uint32_t f = 0; f < m.faces.size(); ++f) {
        if (m.faces[f].corners.size() != 3) continue;
        const auto ef2 = m.edge_faces();
        const auto& c = m.faces[f].corners;
        for (size_t i = 0; i < c.size(); ++i) {
            const uint32_t a = c[i].v, b = c[(i + 1) % c.size()].v;
            auto jt = ef2.find(make_edge(a, b));
            if (jt == ef2.end()) continue;
            for (uint32_t g : jt->second) {
                if (g == f) continue;
                const auto& d = m.faces[g].corners;
                for (size_t k = 0; k < d.size(); ++k) {
                    if (d[k].v == a && d[(k + 1) % d.size()].v == b) {   // same direction = inconsistent
                        std::reverse(m.faces[f].corners.begin(), m.faces[f].corners.end());
                    }
                }
                i = c.size();
                break;
            }
        }
    }
    m.compact();
    sel.mode = SelectMode::Face;
    sel.faces = new_faces;
    sel.edges.clear();
    sel.verts.clear();
    sel.validate(m);
}

/** @brief Deletes the selection: faces in face mode, faces touching selected edges/vertices otherwise. */
inline void delete_selection(EditMesh& m, MeshSelection& sel) {
    std::set<uint32_t> kill;
    if (sel.mode == SelectMode::Face) kill = sel.faces;
    else if (sel.mode == SelectMode::Edge) {
        for (uint32_t f = 0; f < m.faces.size(); ++f) {
            const auto& c = m.faces[f].corners;
            for (size_t i = 0; i < c.size(); ++i) if (sel.edges.count(make_edge(c[i].v, c[(i + 1) % c.size()].v))) kill.insert(f);
        }
    } else {
        for (uint32_t f = 0; f < m.faces.size(); ++f) for (const auto& c : m.faces[f].corners) if (sel.verts.count(c.v)) kill.insert(f);
    }
    std::vector<Face> kept;
    for (uint32_t f = 0; f < m.faces.size(); ++f) if (!kill.count(f)) kept.push_back(m.faces[f]);
    m.faces = std::move(kept);
    m.compact();
    sel.clear();
}

/** @brief Welds every selected vertex into one at their centroid. */
inline void merge_at_center(EditMesh& m, MeshSelection& sel) {
    const auto vs = sel.affected_vertices(m);
    if (vs.size() < 2) return;
    const glm::vec3 c = selection_center(m, sel);
    const uint32_t keep = *vs.begin();
    m.positions[keep] = c;
    for (auto& f : m.faces) for (auto& k : f.corners) if (vs.count(k.v)) k.v = keep;
    m.cleanup_faces();
    m.compact();
    sel.clear();
}

/**
 * @brief Welds vertices closer than `dist` (all vertices, or only the selection's).
 * @return How many vertices were removed.
 */
inline size_t merge_by_distance(EditMesh& m, MeshSelection& sel, float dist) {
    std::set<uint32_t> scope = sel.affected_vertices(m);
    if (scope.empty()) for (uint32_t v = 0; v < m.positions.size(); ++v) scope.insert(v);
    std::vector<uint32_t> target(m.positions.size());
    for (uint32_t v = 0; v < target.size(); ++v) target[v] = v;
    std::vector<uint32_t> list(scope.begin(), scope.end());
    for (size_t i = 0; i < list.size(); ++i) {
        if (target[list[i]] != list[i]) continue;
        for (size_t j = i + 1; j < list.size(); ++j) {
            if (target[list[j]] != list[j]) continue;
            if (glm::distance(m.positions[list[i]], m.positions[list[j]]) <= dist) target[list[j]] = list[i];
        }
    }
    const size_t before = m.positions.size();
    for (auto& f : m.faces) for (auto& k : f.corners) k.v = target[k.v];
    m.cleanup_faces();
    m.compact();
    sel.clear();
    return before - m.positions.size();
}

/** @brief Reverses the winding (and so the normal) of the selected faces, or of all faces. */
inline void flip_normals(EditMesh& m, const MeshSelection& sel) {
    std::set<uint32_t> fs = sel.affected_faces(m);
    if (fs.empty()) for (uint32_t f = 0; f < m.faces.size(); ++f) fs.insert(f);
    for (uint32_t f : fs) std::reverse(m.faces[f].corners.begin(), m.faces[f].corners.end());
}

/** @brief Sets smooth/flat shading on the selected faces (or all). */
inline void set_smooth(EditMesh& m, const MeshSelection& sel, bool smooth) {
    std::set<uint32_t> fs = sel.affected_faces(m);
    if (fs.empty()) for (uint32_t f = 0; f < m.faces.size(); ++f) fs.insert(f);
    for (uint32_t f : fs) m.faces[f].smooth = smooth;
}

/**
 * @brief Box-projects UVs: each face takes the planar projection of the axis its normal is
 *        most aligned with, `scale` UV units per world unit.
 */
inline void uv_box_project(EditMesh& m, const MeshSelection& sel, float scale = 1.0f) {
    std::set<uint32_t> fs = sel.affected_faces(m);
    if (fs.empty()) for (uint32_t f = 0; f < m.faces.size(); ++f) fs.insert(f);
    for (uint32_t f : fs) {
        const glm::vec3 n = m.face_normal(f);
        const glm::vec3 a = glm::abs(n);
        for (auto& c : m.faces[f].corners) {
            const glm::vec3& p = m.positions[c.v];
            if (a.z >= a.x && a.z >= a.y)      c.uv = glm::vec2(n.z >= 0 ? p.x : -p.x, p.y) * scale;
            else if (a.x >= a.y)               c.uv = glm::vec2(n.x >= 0 ? p.y : -p.y, p.z) * scale;
            else                               c.uv = glm::vec2(n.y >= 0 ? -p.x : p.x, p.z) * scale;
        }
    }
}

/** @brief Planar UVs along `axis` (0 = X, 1 = Y, 2 = Z), normalized to the selection bounds. */
inline void uv_planar_project(EditMesh& m, const MeshSelection& sel, int axis) {
    std::set<uint32_t> fs = sel.affected_faces(m);
    if (fs.empty()) for (uint32_t f = 0; f < m.faces.size(); ++f) fs.insert(f);
    const int u = axis == 0 ? 1 : 0, v = axis == 2 ? 1 : 2;
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (uint32_t f : fs) for (const auto& c : m.faces[f].corners) {
        const glm::vec2 q(m.positions[c.v][u], m.positions[c.v][v]);
        lo = glm::min(lo, q); hi = glm::max(hi, q);
    }
    const glm::vec2 ext = glm::max(hi - lo, glm::vec2(1e-6f));
    for (uint32_t f : fs) for (auto& c : m.faces[f].corners) {
        c.uv = (glm::vec2(m.positions[c.v][u], m.positions[c.v][v]) - lo) / ext;
    }
}

/**
 * @brief Blender's F: makes one face from the selected vertices (three or more), ordered
 *        by angle around their centroid and wound to face away from the mesh's centre.
 * @return False if fewer than three vertices are selected.
 */
inline bool fill_face(EditMesh& m, MeshSelection& sel) {
    const auto vs = sel.affected_vertices(m);
    if (vs.size() < 3) return false;
    std::vector<uint32_t> ids(vs.begin(), vs.end());
    glm::vec3 c(0.0f);
    for (uint32_t v : ids) c += m.positions[v];
    c /= static_cast<float>(ids.size());
    // Plane normal: the largest cross product of centroid spokes.
    glm::vec3 n(0.0f);
    for (size_t i = 0; i < ids.size(); ++i)
        for (size_t j = i + 1; j < ids.size(); ++j) {
            const glm::vec3 k = glm::cross(m.positions[ids[i]] - c, m.positions[ids[j]] - c);
            if (glm::length(k) > glm::length(n)) n = k;
        }
    if (glm::length(n) < 1e-9f) return false;
    n = glm::normalize(n);
    const glm::vec3 u = glm::normalize(m.positions[ids[0]] - c - n * glm::dot(m.positions[ids[0]] - c, n));
    const glm::vec3 w = glm::cross(n, u);
    std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
        const glm::vec3 pa = m.positions[a] - c, pb = m.positions[b] - c;
        return std::atan2(glm::dot(pa, w), glm::dot(pa, u)) < std::atan2(glm::dot(pb, w), glm::dot(pb, u));
    });
    // Wind away from the mesh centre (outward for a closed shape).
    glm::vec3 lo, hi;
    m.bounds(lo, hi);
    if (glm::dot(n, c - (lo + hi) * 0.5f) < 0.0f) std::reverse(ids.begin(), ids.end());
    Face f;
    for (uint32_t v : ids) f.corners.push_back({v, glm::vec2(0.0f)});
    m.faces.push_back(f);
    sel.mode = SelectMode::Face;
    sel.faces = {static_cast<uint32_t>(m.faces.size() - 1)};
    return true;
}

/** @brief Shift+D in edit mode: copies the selected faces onto new vertices; the copy is selected. */
inline void duplicate_faces(EditMesh& m, MeshSelection& sel) {
    const std::set<uint32_t> region = sel.affected_faces(m);
    if (region.empty()) return;
    std::map<uint32_t, uint32_t> dup;
    std::set<uint32_t> copies;
    for (uint32_t f : region) {
        Face nf = m.faces[f];
        for (auto& c : nf.corners) {
            if (!dup.count(c.v)) { dup[c.v] = static_cast<uint32_t>(m.positions.size()); m.positions.push_back(m.positions[c.v]); }
            c.v = dup[c.v];
        }
        m.faces.push_back(nf);
        copies.insert(static_cast<uint32_t>(m.faces.size() - 1));
    }
    sel.mode = SelectMode::Face;
    sel.faces = copies;
    sel.verts.clear();
    sel.edges.clear();
}

/** @brief Selects every element connected to the current selection. */
inline void select_linked(const EditMesh& m, MeshSelection& sel) {
    std::set<uint32_t> verts = sel.affected_vertices(m);
    const auto vf = m.vertex_faces();
    std::vector<uint32_t> stack(verts.begin(), verts.end());
    std::set<uint32_t> faces;
    while (!stack.empty()) {
        const uint32_t v = stack.back();
        stack.pop_back();
        for (uint32_t f : vf[v]) {
            if (!faces.insert(f).second) continue;
            for (const auto& c : m.faces[f].corners) if (verts.insert(c.v).second) stack.push_back(c.v);
        }
    }
    sel.verts = verts;
    sel.faces = faces;
    sel.edges.clear();
    for (uint32_t f : faces) {
        const auto& c = m.faces[f].corners;
        for (size_t i = 0; i < c.size(); ++i) sel.edges.insert(make_edge(c[i].v, c[(i + 1) % c.size()].v));
    }
}

/** @brief Converts the selection to another mode, keeping what it covers. */
inline void convert_selection(const EditMesh& m, MeshSelection& sel, SelectMode to) {
    const auto vs = sel.affected_vertices(m);
    const auto fs = sel.affected_faces(m);
    sel.verts = vs;
    sel.faces = fs;
    sel.edges.clear();
    for (const auto& e : m.edges()) if (vs.count(e.first) && vs.count(e.second)) sel.edges.insert(e);
    sel.mode = to;
}

} // namespace toy::editor

#endif // TOYEDITOR_MESH_MESH_OPS_H

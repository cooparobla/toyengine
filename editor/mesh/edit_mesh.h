/**
 * @file edit_mesh.h
 * @brief The editor's editable polygon mesh, and its round trip to the engine's mesh YAML.
 *
 * The engine's mesh files are Blender's per-corner export: `vertices`, `normals` and `uvs`
 * are parallel arrays indexed by `faces` (N-gons), with a vertex duplicated wherever two
 * corners differ in normal or UV. Gfxcoopa welds those duplicates at load. EditMesh keeps
 * the opposite split, the one modelling needs:
 *
 *   - `positions` are WELDED: one entry per topological vertex, so moving a vertex moves
 *     every face that uses it;
 *   - each face corner carries its own UV; normals are not stored at all -- they are
 *     derived on export from the geometry (flat, or averaged across a face's `smooth`
 *     neighbours), so they can never go stale after an edit.
 *
 * A face list rather than a half-edge structure: the file format is already N-gons with
 * per-corner attributes, the operations at authoring scale are cheap with a rebuilt edge
 * map, and non-manifold input (imported files) needs no special casing.
 */

#ifndef TOYEDITOR_MESH_EDIT_MESH_H
#define TOYEDITOR_MESH_EDIT_MESH_H

#include "../core/yaml_util.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toy::editor {

struct Corner {
    uint32_t  v = 0;
    glm::vec2 uv{0.0f};
};

struct Face {
    std::vector<Corner> corners;
    bool smooth = false;
};

using Edge = std::pair<uint32_t, uint32_t>;   ///< Always (min, max).
inline Edge make_edge(uint32_t a, uint32_t b) { return a < b ? Edge{a, b} : Edge{b, a}; }

struct EditMesh {
    std::vector<glm::vec3> positions;
    std::vector<Face> faces;
    /// Keys carried through import -> export untouched (lods, cull_screen_size, ...).
    Node passthrough = Node::mapping();

    bool operator==(const EditMesh& o) const {
        if (positions != o.positions || faces.size() != o.faces.size()) return false;
        for (size_t i = 0; i < faces.size(); ++i) {
            const auto& a = faces[i];
            const auto& b = o.faces[i];
            if (a.smooth != b.smooth || a.corners.size() != b.corners.size()) return false;
            for (size_t k = 0; k < a.corners.size(); ++k) {
                if (a.corners[k].v != b.corners[k].v || a.corners[k].uv != b.corners[k].uv) return false;
            }
        }
        return true;
    }

    // --- geometry queries ---

    glm::vec3 face_normal(size_t f) const {
        // Newell's method: robust for non-planar and concave N-gons.
        glm::vec3 n(0.0f);
        const auto& c = faces[f].corners;
        for (size_t i = 0; i < c.size(); ++i) {
            const glm::vec3& a = positions[c[i].v];
            const glm::vec3& b = positions[c[(i + 1) % c.size()].v];
            n.x += (a.y - b.y) * (a.z + b.z);
            n.y += (a.z - b.z) * (a.x + b.x);
            n.z += (a.x - b.x) * (a.y + b.y);
        }
        const float len = glm::length(n);
        return len > 1e-12f ? n / len : glm::vec3(0, 0, 1);
    }

    glm::vec3 face_center(size_t f) const {
        glm::vec3 s(0.0f);
        for (const auto& c : faces[f].corners) s += positions[c.v];
        return faces[f].corners.empty() ? s : s / static_cast<float>(faces[f].corners.size());
    }

    /** @brief Every edge with the faces using it (indices into `faces`). */
    std::map<Edge, std::vector<uint32_t>> edge_faces() const {
        std::map<Edge, std::vector<uint32_t>> m;
        for (uint32_t f = 0; f < faces.size(); ++f) {
            const auto& c = faces[f].corners;
            for (size_t i = 0; i < c.size(); ++i) m[make_edge(c[i].v, c[(i + 1) % c.size()].v)].push_back(f);
        }
        return m;
    }

    std::vector<Edge> edges() const {
        std::vector<Edge> out;
        for (const auto& kv : edge_faces()) out.push_back(kv.first);
        return out;
    }

    /** @brief Faces using each vertex. */
    std::vector<std::vector<uint32_t>> vertex_faces() const {
        std::vector<std::vector<uint32_t>> vf(positions.size());
        for (uint32_t f = 0; f < faces.size(); ++f) for (const auto& c : faces[f].corners) vf[c.v].push_back(f);
        return vf;
    }

    size_t triangle_count() const {
        size_t n = 0;
        for (const auto& f : faces) if (f.corners.size() >= 3) n += f.corners.size() - 2;
        return n;
    }

    void bounds(glm::vec3& lo, glm::vec3& hi) const {
        lo = glm::vec3(1e30f); hi = glm::vec3(-1e30f);
        for (const auto& p : positions) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
        if (positions.empty()) lo = hi = glm::vec3(0.0f);
    }

    /** @brief Drops vertices no face uses, remapping indices. */
    void compact() {
        std::vector<int64_t> remap(positions.size(), -1);
        std::vector<glm::vec3> kept;
        for (auto& f : faces) {
            for (auto& c : f.corners) {
                if (remap[c.v] < 0) { remap[c.v] = static_cast<int64_t>(kept.size()); kept.push_back(positions[c.v]); }
                c.v = static_cast<uint32_t>(remap[c.v]);
            }
        }
        positions = std::move(kept);
    }

    /** @brief Removes consecutive duplicate corners and faces left with fewer than 3. */
    void cleanup_faces() {
        for (auto& f : faces) {
            auto& c = f.corners;
            std::vector<Corner> out;
            for (size_t i = 0; i < c.size(); ++i) {
                if (!out.empty() && out.back().v == c[i].v) continue;
                out.push_back(c[i]);
            }
            while (out.size() > 1 && out.front().v == out.back().v) out.pop_back();
            c = std::move(out);
        }
        faces.erase(std::remove_if(faces.begin(), faces.end(), [](const Face& f) { return f.corners.size() < 3; }), faces.end());
    }

    /** @brief Per-face-corner export normals (flat, or smooth-averaged). */
    std::vector<std::vector<glm::vec3>> corner_normals() const {
        std::vector<glm::vec3> fnorm(faces.size());
        for (size_t f = 0; f < faces.size(); ++f) fnorm[f] = face_normal(f);
        std::vector<glm::vec3> vsmooth(positions.size(), glm::vec3(0.0f));
        for (size_t f = 0; f < faces.size(); ++f) {
            if (!faces[f].smooth) continue;
            for (const auto& c : faces[f].corners) vsmooth[c.v] += fnorm[f];
        }
        std::vector<std::vector<glm::vec3>> out(faces.size());
        for (size_t f = 0; f < faces.size(); ++f) {
            for (const auto& c : faces[f].corners) {
                glm::vec3 n = fnorm[f];
                if (faces[f].smooth && glm::length(vsmooth[c.v]) > 1e-8f) n = glm::normalize(vsmooth[c.v]);
                out[f].push_back(n);
            }
        }
        return out;
    }
};

// =====================================================================================
// YAML round trip
// =====================================================================================

/**
 * @brief Builds an EditMesh from a mesh document. Corner positions equal within `weld_eps`
 *        become one vertex; a face whose stored normals differ from its flat normal is
 *        marked smooth.
 */
inline EditMesh mesh_from_node(const Node& node, float weld_eps = 1e-5f) {
    EditMesh m;
    std::vector<glm::vec3> raw_pos, raw_nrm;
    std::vector<glm::vec2> raw_uv;
    if (node.contains("vertices")) for (const auto& v : node.at("vertices").as_seq()) raw_pos.push_back(as_vec3(v));
    if (node.contains("normals"))  for (const auto& v : node.at("normals").as_seq()) raw_nrm.push_back(as_vec3(v));
    if (node.contains("uvs")) {
        for (const auto& v : node.at("uvs").as_seq()) {
            const auto& s = v.as_seq();
            raw_uv.push_back({s.size() > 0 ? as_float(s[0]) : 0.0f, s.size() > 1 ? as_float(s[1]) : 0.0f});
        }
    }
    // Weld by quantized position.
    const float q = 1.0f / std::max(weld_eps, 1e-9f);
    std::map<std::tuple<long long, long long, long long>, uint32_t> weld;
    std::vector<uint32_t> raw_to_weld(raw_pos.size());
    for (size_t i = 0; i < raw_pos.size(); ++i) {
        const auto key = std::make_tuple(std::llround(raw_pos[i].x * q), std::llround(raw_pos[i].y * q), std::llround(raw_pos[i].z * q));
        auto it = weld.find(key);
        if (it == weld.end()) {
            it = weld.emplace(key, static_cast<uint32_t>(m.positions.size())).first;
            m.positions.push_back(raw_pos[i]);
        }
        raw_to_weld[i] = it->second;
    }
    if (node.contains("faces")) {
        for (const auto& fn : node.at("faces").as_seq()) {
            Face f;
            std::vector<uint32_t> raw_idx;
            for (const auto& idx : fn.as_seq()) {
                const auto r = static_cast<uint32_t>(idx.get_value<int64_t>());
                if (r >= raw_pos.size()) continue;
                raw_idx.push_back(r);
                Corner c;
                c.v = raw_to_weld[r];
                if (r < raw_uv.size()) c.uv = raw_uv[r];
                f.corners.push_back(c);
            }
            if (f.corners.size() < 3) continue;
            m.faces.push_back(std::move(f));
            const glm::vec3 flat = m.face_normal(m.faces.size() - 1);
            for (uint32_t r : raw_idx) {
                if (r < raw_nrm.size() && glm::length(raw_nrm[r]) > 0.5f && glm::dot(glm::normalize(raw_nrm[r]), flat) < 0.999f) {
                    m.faces.back().smooth = true;
                    break;
                }
            }
        }
    }
    m.cleanup_faces();
    for (const char* k : {"lods", "cull_screen_size"}) {
        if (node.contains(k)) m.passthrough[k] = node.at(k);
    }
    return m;
}

/** @brief The engine mesh document for `m` (per-corner arrays, N-gon faces). */
inline Node mesh_to_node(const EditMesh& m) {
    Node out = Node::mapping();
    Node verts = Node::sequence(), norms = Node::sequence(), uvs = Node::sequence(), faces = Node::sequence();
    const auto cn = m.corner_normals();
    // Dedup identical (vertex, normal, uv) corners so files stay small; the engine welds anyway.
    std::map<std::tuple<uint32_t, int, int, int, int, int>, int64_t> seen;
    int64_t next = 0;
    auto qz = [](float v) { return static_cast<int>(std::lround(v * 10000.0f)); };
    for (size_t f = 0; f < m.faces.size(); ++f) {
        Node face = Node::sequence();
        for (size_t k = 0; k < m.faces[f].corners.size(); ++k) {
            const Corner& c = m.faces[f].corners[k];
            const glm::vec3 n = cn[f][k];
            const auto key = std::make_tuple(c.v, qz(n.x), qz(n.y), qz(n.z), qz(c.uv.x), qz(c.uv.y));
            auto it = seen.find(key);
            if (it == seen.end()) {
                it = seen.emplace(key, next++).first;
                const glm::vec3& p = m.positions[c.v];
                float pv[3] = {p.x, p.y, p.z};
                float nv[3] = {n.x, n.y, n.z};
                float uv[2] = {c.uv.x, c.uv.y};
                verts.as_seq().push_back(make_float_seq(pv, 3));
                norms.as_seq().push_back(make_float_seq(nv, 3));
                uvs.as_seq().push_back(make_float_seq(uv, 2));
            }
            face.as_seq().push_back(Node(it->second));
        }
        faces.as_seq().push_back(face);
    }
    out["vertices"] = verts;
    out["normals"] = norms;
    out["uvs"] = uvs;
    out["faces"] = faces;
    if (m.passthrough.is_mapping()) for (const auto& kv : m.passthrough.as_map()) out[kv.first.get_value<std::string>()] = kv.second;
    return out;
}

} // namespace toy::editor

#endif // TOYEDITOR_MESH_EDIT_MESH_H

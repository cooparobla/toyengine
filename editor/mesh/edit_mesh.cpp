#include "editor/mesh/edit_mesh.h"

namespace toy {
namespace editor {

bool Corner::authored_valid(const glm::vec3& computed) const {
    return glm::dot(normal, normal) > 0.25f && glm::dot(computed, ref_normal) > 0.99999f * glm::length(ref_normal) * glm::length(computed);
}

bool EditMesh::operator==(const EditMesh& o) const {
    if (positions != o.positions || faces.size() != o.faces.size() || slots != o.slots) return false;
    if (has_colors != o.has_colors || groups != o.groups || weights != o.weights) return false;
    if (has_normals != o.has_normals || has_tangents != o.has_tangents) return false;
    if (joints != o.joints || joint_weights != o.joint_weights) return false;
    for (size_t i = 0; i < faces.size(); ++i) {
        const auto& a = faces[i];
        const auto& b = o.faces[i];
        if (a.smooth != b.smooth || a.slot != b.slot || a.corners.size() != b.corners.size()) return false;
        for (size_t k = 0; k < a.corners.size(); ++k) {
            if (a.corners[k].v != b.corners[k].v || a.corners[k].uv != b.corners[k].uv) return false;
            if (has_colors && a.corners[k].color != b.corners[k].color) return false;
        }
    }
    return true;
}

void EditMesh::sync_vertex_data() {
    if (!weights.empty() || !groups.empty()) weights.resize(positions.size());
    if (!joints.empty()) joints.resize(positions.size(), glm::ivec4(-1));
    if (!joint_weights.empty()) joint_weights.resize(positions.size(), glm::vec4(0.0f));
}

uint32_t EditMesh::add_vertex_mix(const glm::vec3& p, const std::vector<std::pair<uint32_t, float>>& sources) {
    sync_vertex_data();
    const uint32_t nv = static_cast<uint32_t>(positions.size());
    positions.push_back(p);
    float total = 0.0f;
    for (const auto& s : sources) total += std::max(s.second, 0.0f);
    if (!weights.empty()) {
        std::vector<VertexWeight> mixed;
        for (const auto& [v, k] : sources) {
            if (v >= nv || k <= 0.0f || total <= 0.0f) continue;
            for (const auto& w : weights[v]) {
                auto it = std::find_if(mixed.begin(), mixed.end(), [&](const VertexWeight& x) { return x.group == w.group; });
                if (it == mixed.end()) mixed.push_back({w.group, w.weight * k / total});
                else it->weight += w.weight * k / total;
            }
        }
        weights.push_back(std::move(mixed));
    }
    // The palette can't be blended index-wise: take the strongest source's.
    uint32_t best = sources.empty() ? 0u : sources.front().first;
    float best_k = -1.0f;
    for (const auto& [v, k] : sources) if (k > best_k && v < nv) { best = v; best_k = k; }
    if (!joints.empty()) joints.push_back(best < nv && best_k >= 0.0f ? joints[best] : glm::ivec4(-1));
    if (!joint_weights.empty()) joint_weights.push_back(best < nv && best_k >= 0.0f ? joint_weights[best] : glm::vec4(0.0f));
    return nv;
}

glm::vec4 EditMesh::corner_color(uint32_t f, uint32_t v) const {
    if (f < faces.size()) for (const auto& c : faces[f].corners) if (c.v == v) return c.color;
    return glm::vec4(1.0f);
}

float EditMesh::weight(uint32_t v, uint32_t group) const {
    if (v >= weights.size()) return 0.0f;
    for (const auto& w : weights[v]) if (w.group == group) return w.weight;
    return 0.0f;
}

void EditMesh::set_weight(uint32_t v, uint32_t group, float w) {
    if (v >= positions.size()) return;
    if (weights.size() < positions.size()) weights.resize(positions.size());
    auto& list = weights[v];
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->group != group) continue;
        if (w <= 0.0f) list.erase(it);
        else it->weight = w;
        return;
    }
    if (w > 0.0f) list.push_back({group, w});
}

uint32_t EditMesh::group_index(const std::string& name) {
    for (uint32_t i = 0; i < groups.size(); ++i) if (groups[i] == name) return i;
    groups.push_back(name);
    sync_vertex_data();
    return static_cast<uint32_t>(groups.size() - 1);
}

void EditMesh::remove_group(uint32_t g) {
    if (g >= groups.size()) return;
    groups.erase(groups.begin() + g);
    for (auto& list : weights) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const VertexWeight& w) { return w.group == g; }), list.end());
        for (auto& w : list) if (w.group > g) --w.group;
    }
}

glm::vec4 EditMesh::vertex_color(uint32_t v) const {
    glm::vec4 sum(0.0f);
    float n = 0.0f;
    for (const auto& f : faces) for (const auto& c : f.corners) if (c.v == v) { sum += c.color; n += 1.0f; }
    return n > 0.0f ? sum / n : glm::vec4(1.0f);
}

glm::vec3 EditMesh::face_normal(size_t f) const {
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

glm::vec3 EditMesh::face_center(size_t f) const {
    glm::vec3 s(0.0f);
    for (const auto& c : faces[f].corners) s += positions[c.v];
    return faces[f].corners.empty() ? s : s / static_cast<float>(faces[f].corners.size());
}

std::map<Edge, std::vector<uint32_t>> EditMesh::edge_faces() const {
    std::map<Edge, std::vector<uint32_t>> m;
    for (uint32_t f = 0; f < faces.size(); ++f) {
        const auto& c = faces[f].corners;
        for (size_t i = 0; i < c.size(); ++i) m[make_edge(c[i].v, c[(i + 1) % c.size()].v)].push_back(f);
    }
    return m;
}

std::vector<Edge> EditMesh::edges() const {
    std::vector<Edge> out;
    for (const auto& kv : edge_faces()) out.push_back(kv.first);
    return out;
}

std::vector<std::vector<uint32_t>> EditMesh::vertex_faces() const {
    std::vector<std::vector<uint32_t>> vf(positions.size());
    for (uint32_t f = 0; f < faces.size(); ++f) for (const auto& c : faces[f].corners) vf[c.v].push_back(f);
    return vf;
}

size_t EditMesh::triangle_count() const {
    size_t n = 0;
    for (const auto& f : faces) if (f.corners.size() >= 3) n += f.corners.size() - 2;
    return n;
}

void EditMesh::bounds(glm::vec3& lo, glm::vec3& hi) const {
    lo = glm::vec3(1e30f); hi = glm::vec3(-1e30f);
    for (const auto& p : positions) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    if (positions.empty()) lo = hi = glm::vec3(0.0f);
}

void EditMesh::compact() {
    sync_vertex_data();
    std::vector<int64_t> remap(positions.size(), -1);
    std::vector<glm::vec3> kept;
    std::vector<std::vector<VertexWeight>> kept_w;
    std::vector<glm::ivec4> kept_j;
    std::vector<glm::vec4> kept_jw;
    for (auto& f : faces) {
        for (auto& c : f.corners) {
            if (remap[c.v] < 0) {
                remap[c.v] = static_cast<int64_t>(kept.size());
                kept.push_back(positions[c.v]);
                if (!weights.empty()) kept_w.push_back(weights[c.v]);
                if (!joints.empty()) kept_j.push_back(joints[c.v]);
                if (!joint_weights.empty()) kept_jw.push_back(joint_weights[c.v]);
            }
            c.v = static_cast<uint32_t>(remap[c.v]);
        }
    }
    positions = std::move(kept);
    if (!weights.empty()) weights = std::move(kept_w);
    if (!joints.empty()) joints = std::move(kept_j);
    if (!joint_weights.empty()) joint_weights = std::move(kept_jw);
}

void EditMesh::cleanup_faces() {
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

std::vector<std::vector<glm::vec3>> EditMesh::corner_normals() const {
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

void resolve_welded_duplicates(const EditMesh& m, Face& f) {
    const auto& c = f.corners;
    std::vector<size_t> dup;   // index i where c[i] repeats c[i-1] (cyclically)
    for (size_t i = 0; i < c.size(); ++i) if (c[i].v == c[(i + c.size() - 1) % c.size()].v) dup.push_back(i);
    if (dup.empty()) return;
    auto fan = [&](const std::vector<Corner>& cs) {
        std::vector<std::array<const Corner*, 3>> tris;
        for (size_t i = 1; i + 1 < cs.size(); ++i) {
            const glm::vec3 a = m.positions[cs[0].v], b = m.positions[cs[i].v], d = m.positions[cs[i + 1].v];
            if (glm::length(glm::cross(b - a, d - a)) < 1e-12f) continue;
            tris.push_back({&cs[0], &cs[i], &cs[i + 1]});
        }
        return tris;
    };
    auto same = [](const Corner& a, const Corner& b) { return a.v == b.v && a.uv == b.uv && a.color == b.color && a.normal == b.normal && a.tangent == b.tangent; };
    const auto want = fan(c);
    std::vector<Corner> best;
    const size_t k = std::min<size_t>(dup.size(), 4);
    for (uint32_t mask = 0; mask < (1u << k); ++mask) {
        // Bit set: drop the earlier corner of the pair (keep the later); clear: drop the later.
        std::vector<bool> drop(c.size(), false);
        for (size_t j = 0; j < dup.size(); ++j) {
            const size_t i = dup[j];
            const bool keep_later = j < k && ((mask >> j) & 1u);
            drop[keep_later ? (i + c.size() - 1) % c.size() : i] = true;
        }
        std::vector<Corner> cand;
        for (size_t i = 0; i < c.size(); ++i) if (!drop[i]) cand.push_back(c[i]);
        if (cand.size() < 3) continue;
        const auto got = fan(cand);
        bool match = got.size() == want.size();
        for (size_t t = 0; match && t < got.size(); ++t) {
            // Same triangle (any rotation of its corners: the fan may start elsewhere).
            bool rot = false;
            for (int r = 0; r < 3 && !rot; ++r) {
                rot = same(*got[t][0], *want[t][r]) && same(*got[t][1], *want[t][(r + 1) % 3]) && same(*got[t][2], *want[t][(r + 2) % 3]);
            }
            match = rot;
        }
        if (match) { f.corners = std::move(cand); return; }
        if (best.empty()) best = cand;
    }
    if (!best.empty()) f.corners = std::move(best);
}

EditMesh mesh_from_node(const Node& node, float weld_eps) {
    EditMesh m;
    std::vector<glm::vec3> raw_pos, raw_nrm;
    std::vector<glm::vec2> raw_uv;
    std::vector<glm::vec4> raw_col, raw_tan;
    std::vector<glm::ivec4> raw_joints;
    std::vector<glm::vec4> raw_jw;
    if (node.contains("vertices")) for (const auto& v : node.at("vertices").as_seq()) raw_pos.push_back(as_vec3(v));
    if (node.contains("normals"))  for (const auto& v : node.at("normals").as_seq()) raw_nrm.push_back(as_vec3(v));
    if (node.contains("uvs")) {
        for (const auto& v : node.at("uvs").as_seq()) {
            const auto& s = v.as_seq();
            raw_uv.push_back({s.size() > 0 ? as_float(s[0]) : 0.0f, s.size() > 1 ? as_float(s[1]) : 0.0f});
        }
    }
    if (node.contains("colors")) {
        for (const auto& v : node.at("colors").as_seq()) {
            const auto& s = v.as_seq();
            raw_col.push_back({s.size() > 0 ? as_float(s[0]) : 1.0f, s.size() > 1 ? as_float(s[1]) : 1.0f,
                               s.size() > 2 ? as_float(s[2]) : 1.0f, s.size() > 3 ? as_float(s[3]) : 1.0f});
        }
    }
    m.has_colors = !raw_col.empty();
    m.has_normals = !raw_nrm.empty();
    m.has_tangents = node.contains("tangents") && node.at("tangents").is_sequence() && !node.at("tangents").as_seq().empty();
    if (node.contains("tangents")) {
        for (const auto& v : node.at("tangents").as_seq()) {
            const auto& s = v.as_seq();
            raw_tan.push_back({s.size() > 0 ? as_float(s[0]) : 0.0f, s.size() > 1 ? as_float(s[1]) : 0.0f,
                               s.size() > 2 ? as_float(s[2]) : 0.0f, s.size() > 3 ? as_float(s[3]) : 1.0f});
        }
    }
    auto vec4_seq = [&](const char* key, auto& out, auto conv) {
        if (!node.contains(key)) return;
        for (const auto& v : node.at(key).as_seq()) {
            const auto& s = v.as_seq();
            typename std::decay_t<decltype(out)>::value_type x{};
            for (int i = 0; i < 4; ++i) x[i] = conv(i < static_cast<int>(s.size()) ? &s[static_cast<size_t>(i)] : nullptr);
            out.push_back(x);
        }
    };
    vec4_seq("joints", raw_joints, [](const Node* n) { return n ? static_cast<int>(n->get_value<int64_t>()) : -1; });
    vec4_seq("joint_weights", raw_jw, [](const Node* n) { return n ? as_float(*n) : 0.0f; });
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
    // Per-vertex data from the raw arrays: the first raw corner of each welded vertex wins
    // (the exporter writes the same groups/palette on every corner of a vertex).
    std::vector<bool> seen_vertex(m.positions.size(), false);
    if (node.contains("weights")) {
        const auto& ws = node.at("weights").as_seq();
        m.weights.resize(m.positions.size());
        for (size_t i = 0; i < ws.size() && i < raw_pos.size(); ++i) {
            const uint32_t v = raw_to_weld[i];
            if (seen_vertex[v] || !ws[i].is_mapping()) continue;
            seen_vertex[v] = true;
            for (const auto& kv : ws[i].as_map()) {
                const float w = as_float(kv.second);
                if (w > 0.0f) m.set_weight(v, m.group_index(kv.first.get_value<std::string>()), w);
            }
        }
        bool any = !m.groups.empty();
        if (!any) m.weights.clear();
    }
    if (!raw_joints.empty() || !raw_jw.empty()) {
        if (!raw_joints.empty()) m.joints.assign(m.positions.size(), glm::ivec4(-1));
        if (!raw_jw.empty()) m.joint_weights.assign(m.positions.size(), glm::vec4(0.0f));
        for (size_t i = raw_pos.size(); i-- > 0;) {   // backwards: the first corner wins
            const uint32_t v = raw_to_weld[i];
            if (i < raw_joints.size()) m.joints[v] = raw_joints[i];
            if (i < raw_jw.size()) m.joint_weights[v] = raw_jw[i];
        }
    }
    if (node.contains("material_slots")) {
        for (const auto& sn : node.at("material_slots").as_seq()) m.slots.push_back(sn.get_value<std::string>());
    }
    std::vector<uint32_t> face_slots;
    if (node.contains("face_materials")) {
        for (const auto& fm : node.at("face_materials").as_seq()) face_slots.push_back(static_cast<uint32_t>(fm.get_value<int64_t>()));
    }
    size_t face_index = 0;
    if (node.contains("faces")) {
        for (const auto& fn : node.at("faces").as_seq()) {
            Face f;
            f.slot = face_index < face_slots.size() ? face_slots[face_index] : 0u;
            ++face_index;
            std::vector<uint32_t> raw_idx;
            for (const auto& idx : fn.as_seq()) {
                const auto r = static_cast<uint32_t>(idx.get_value<int64_t>());
                if (r >= raw_pos.size()) continue;
                raw_idx.push_back(r);
                Corner c;
                c.v = raw_to_weld[r];
                if (r < raw_uv.size()) c.uv = raw_uv[r];
                if (r < raw_col.size()) c.color = raw_col[r];
                if (r < raw_nrm.size() && glm::length(raw_nrm[r]) > 0.5f) c.normal = raw_nrm[r];   // verbatim
                if (r < raw_tan.size()) c.tangent = raw_tan[r];
                f.corners.push_back(c);
            }
            if (f.corners.size() < 3) continue;
            resolve_welded_duplicates(m, f);
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
    // What the editor computes for each corner now: the reference an export compares against.
    {
        const auto cn = m.corner_normals();
        for (size_t f = 0; f < m.faces.size(); ++f)
            for (size_t k = 0; k < m.faces[f].corners.size(); ++k) m.faces[f].corners[k].ref_normal = cn[f][k];
    }
    // Everything the editor does not rebuild rides along to export unchanged.
    if (node.is_mapping()) {
        for (const auto& kv : node.as_map()) {
            const std::string key = kv.first.get_value<std::string>();
            if (std::find(k_regenerated_keys.begin(), k_regenerated_keys.end(), key) == k_regenerated_keys.end()) {
                m.passthrough[key] = kv.second;
            }
        }
    }
    return m;
}

std::vector<glm::vec4> export_tangents(const EditMesh& m, const std::vector<std::vector<int64_t>>& corner_ids,
                                              const std::vector<glm::vec3>& normals, size_t count,
                                              const std::vector<std::vector<char>>& keep_authored) {
    std::vector<glm::vec3> t_sum(count, glm::vec3(0.0f)), b_sum(count, glm::vec3(0.0f));
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& c = m.faces[f].corners;
        for (size_t i = 1; i + 1 < c.size(); ++i) {
            const size_t k[3] = {0, i, i + 1};
            const glm::vec3 e1 = m.positions[c[k[1]].v] - m.positions[c[k[0]].v];
            const glm::vec3 e2 = m.positions[c[k[2]].v] - m.positions[c[k[0]].v];
            const glm::vec2 d1 = c[k[1]].uv - c[k[0]].uv;
            const glm::vec2 d2 = c[k[2]].uv - c[k[0]].uv;
            const float det = d1.x * d2.y - d2.x * d1.y;
            if (std::abs(det) < 1e-12f) continue;
            const glm::vec3 t = (e1 * d2.y - e2 * d1.y) / det;
            const glm::vec3 b = (e2 * d1.x - e1 * d2.x) / det;
            for (size_t j : k) {
                t_sum[static_cast<size_t>(corner_ids[f][j])] += t;
                b_sum[static_cast<size_t>(corner_ids[f][j])] += b;
            }
        }
    }
    // Authored tangents still valid for their exported vertex.
    std::vector<glm::vec4> authored(count, glm::vec4(0.0f));
    for (size_t f = 0; f < m.faces.size(); ++f) {
        for (size_t k = 0; k < m.faces[f].corners.size(); ++k) {
            const Corner& c = m.faces[f].corners[k];
            const size_t id = static_cast<size_t>(corner_ids[f][k]);
            if (authored[id] != glm::vec4(0.0f) || glm::length(glm::vec3(c.tangent)) < 1e-6f) continue;
            if (keep_authored[f][k]) authored[id] = c.tangent;
        }
    }
    std::vector<glm::vec4> out(count);
    for (size_t i = 0; i < count; ++i) {
        if (authored[i] != glm::vec4(0.0f)) {
            out[i] = authored[i];
            continue;
        }
        const glm::vec3 n = normals[i];
        glm::vec3 t = t_sum[i] - n * glm::dot(n, t_sum[i]);
        if (glm::length(t) < 1e-8f) {   // no usable UVs here: any tangent perpendicular to n
            t = glm::cross(n, std::abs(n.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
        }
        t = glm::normalize(t);
        const float w = glm::dot(glm::cross(n, t), b_sum[i]) < 0.0f ? -1.0f : 1.0f;
        out[i] = glm::vec4(t, w);
    }
    return out;
}

Node mesh_to_node(const EditMesh& m) {
    Node out = Node::mapping();
    Node verts = Node::sequence(), norms = Node::sequence(), uvs = Node::sequence(), faces = Node::sequence();
    Node colors = Node::sequence(), weights = Node::sequence(), joints = Node::sequence(), jweights = Node::sequence();
    const auto cn = m.corner_normals();
    // Dedup identical (vertex, normal, uv, colour) corners so files stay small; the engine welds anyway.
    std::map<std::tuple<uint32_t, int, int, int, int, int, std::tuple<int, int, int, int>>, int64_t> seen;
    std::vector<std::vector<int64_t>> corner_ids(m.faces.size());
    std::vector<std::vector<char>> keep_authored(m.faces.size());
    std::vector<glm::vec3> out_normals;
    int64_t next = 0;
    auto qz = [](float v) { return static_cast<int>(std::lround(v * 10000.0f)); };
    for (size_t f = 0; f < m.faces.size(); ++f) {
        Node face = Node::sequence();
        for (size_t k = 0; k < m.faces[f].corners.size(); ++k) {
            const Corner& c = m.faces[f].corners[k];
            const bool keep = c.authored_valid(cn[f][k]);
            keep_authored[f].push_back(keep ? 1 : 0);
            const glm::vec3 n = keep ? c.normal : cn[f][k];
            const glm::vec4 col = m.has_colors ? c.color : glm::vec4(1.0f);
            const auto key = std::make_tuple(c.v, qz(n.x), qz(n.y), qz(n.z), qz(c.uv.x), qz(c.uv.y),
                                             std::make_tuple(qz(col.r), qz(col.g), qz(col.b), qz(col.a)));
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
                out_normals.push_back(n);
                if (m.has_colors) {
                    float cv[4] = {col.r, col.g, col.b, col.a};
                    colors.as_seq().push_back(make_float_seq(cv, 4));
                }
                Node w = Node::mapping();
                if (c.v < m.weights.size()) {
                    for (const auto& vw : m.weights[c.v]) {
                        if (vw.group < m.groups.size()) w[m.groups[vw.group]] = make_float(vw.weight);
                    }
                }
                weights.as_seq().push_back(w);
                if (!m.joints.empty()) {
                    const glm::ivec4 j = c.v < m.joints.size() ? m.joints[c.v] : glm::ivec4(-1);
                    Node js = Node::sequence();
                    for (int i = 0; i < 4; ++i) js.as_seq().push_back(Node(static_cast<int64_t>(j[i])));
                    joints.as_seq().push_back(js);
                }
                if (!m.joint_weights.empty()) {
                    const glm::vec4 jw = c.v < m.joint_weights.size() ? m.joint_weights[c.v] : glm::vec4(0.0f);
                    float jv[4] = {jw.x, jw.y, jw.z, jw.w};
                    jweights.as_seq().push_back(make_float_seq(jv, 4));
                }
            }
            corner_ids[f].push_back(it->second);
            face.as_seq().push_back(Node(it->second));
        }
        faces.as_seq().push_back(face);
    }
    Node tangents = Node::sequence();
    for (const glm::vec4& t : export_tangents(m, corner_ids, out_normals, static_cast<size_t>(next), keep_authored)) {
        float tv[4] = {t.x, t.y, t.z, t.w};
        tangents.as_seq().push_back(make_float_seq(tv, 4));
    }
    out["vertices"] = verts;
    if (m.has_normals) out["normals"] = norms;
    out["uvs"] = uvs;
    out["faces"] = faces;
    // The rest of Blender's export layout: colours (empty without a colour attribute), one
    // vertex-group map per vertex, and tangents (when the source had them -- see has_tangents).
    out["colors"] = colors;
    out["weights"] = weights;
    if (m.has_tangents) out["tangents"] = tangents;
    if (!m.joints.empty()) out["joints"] = joints;
    if (!m.joint_weights.empty()) out["joint_weights"] = jweights;
    // Material slots (submeshes): written when named, or when any face uses a slot past 0.
    bool any_slot = false;
    for (const auto& f : m.faces) any_slot |= f.slot != 0;
    if (!m.slots.empty() || any_slot) {
        Node names = Node::sequence();
        uint32_t count = static_cast<uint32_t>(m.slots.size());
        for (const auto& f : m.faces) count = std::max(count, f.slot + 1);
        for (uint32_t i = 0; i < count; ++i) names.as_seq().push_back(Node(i < m.slots.size() ? m.slots[i] : "slot" + std::to_string(i)));
        out["material_slots"] = names;
        if (any_slot) {
            Node fm = Node::sequence();
            for (const auto& f : m.faces) fm.as_seq().push_back(Node(static_cast<int64_t>(f.slot)));
            out["face_materials"] = fm;
        }
    }
    if (m.passthrough.is_mapping()) for (const auto& kv : m.passthrough.as_map()) out[kv.first.get_value<std::string>()] = kv.second;
    return out;
}

} // namespace editor
} // namespace toy

#include "editor/mesh/mesh_bvh.h"

namespace toy {
namespace editor {

void TriangleBVH::build(const EditMesh& m) {
    tris_.clear();
    for (uint32_t f = 0; f < m.faces.size(); ++f) {
        const auto& c = m.faces[f].corners;
        for (size_t i = 1; i + 1 < c.size(); ++i) tris_.push_back({{c[0].v, c[i].v, c[i + 1].v}, f});
    }
    nodes_.clear();
    order_.resize(tris_.size());
    for (uint32_t i = 0; i < order_.size(); ++i) order_[i] = i;
    centroids_.resize(tris_.size());
    for (size_t i = 0; i < tris_.size(); ++i) {
        const auto& t = tris_[i];
        centroids_[i] = (m.positions[t.v[0]] + m.positions[t.v[1]] + m.positions[t.v[2]]) / 3.0f;
    }
    if (!tris_.empty()) {
        nodes_.reserve(2 * tris_.size() / 2 + 8);
        nodes_.push_back({});
        build_into_(m, 0, 0, static_cast<uint32_t>(tris_.size()));
    }
    built_ = true;
}

void TriangleBVH::refit(const EditMesh& m) {
    if (nodes_.empty()) return;
    for (size_t i = nodes_.size(); i-- > 0;) {
        Node& n = nodes_[i];
        if (n.count > 0) {
            n.lo = glm::vec3(1e30f); n.hi = glm::vec3(-1e30f);
            for (uint32_t k = n.first; k < n.first + n.count; ++k) {
                for (uint32_t v : tris_[order_[k]].v) { n.lo = glm::min(n.lo, m.positions[v]); n.hi = glm::max(n.hi, m.positions[v]); }
            }
        } else {
            n.lo = glm::min(nodes_[n.left].lo, nodes_[n.left + 1].lo);
            n.hi = glm::max(nodes_[n.left].hi, nodes_[n.left + 1].hi);
        }
    }
}

std::optional<TriangleBVH::Hit> TriangleBVH::raycast(const EditMesh& m, glm::vec3 o, glm::vec3 d, float t_min, float t_max) const {
    if (nodes_.empty()) return std::nullopt;
    const glm::vec3 inv(1.0f / (std::abs(d.x) > 1e-12f ? d.x : 1e-12f), 1.0f / (std::abs(d.y) > 1e-12f ? d.y : 1e-12f),
                        1.0f / (std::abs(d.z) > 1e-12f ? d.z : 1e-12f));
    std::optional<Hit> best;
    float best_t = t_max;
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& n = nodes_[stack[--sp]];
        if (!slab_(n, o, inv, t_min, best_t)) continue;
        if (n.count > 0) {
            for (uint32_t k = n.first; k < n.first + n.count; ++k) {
                const Tri& t = tris_[order_[k]];
                float tt;
                if (intersect_(m.positions[t.v[0]], m.positions[t.v[1]], m.positions[t.v[2]], o, d, tt) && tt > t_min && tt < best_t) {
                    best_t = tt;
                    Hit h;
                    h.t = tt;
                    h.face = t.face;
                    h.tri = order_[k];
                    h.p = o + d * tt;
                    h.normal = glm::cross(m.positions[t.v[1]] - m.positions[t.v[0]], m.positions[t.v[2]] - m.positions[t.v[0]]);
                    best = h;
                }
            }
        } else if (sp + 2 <= 64) {
            stack[sp++] = n.left;
            stack[sp++] = n.left + 1;
        }
    }
    return best;
}

bool TriangleBVH::occluded(const EditMesh& m, glm::vec3 eye, glm::vec3 target, float slack) const {
    const glm::vec3 d = target - eye;
    const float len = glm::length(d);
    if (len < 1e-9f) return false;
    auto h = raycast(m, eye, d / len, 0.0f, len * (1.0f - slack) - 1e-5f);
    return h.has_value();
}

uint32_t TriangleBVH::build_into_(const EditMesh& m, uint32_t slot, uint32_t first, uint32_t count) {
    glm::vec3 lo(1e30f), hi(-1e30f), clo(1e30f), chi(-1e30f);
    for (uint32_t k = first; k < first + count; ++k) {
        for (uint32_t v : tris_[order_[k]].v) { lo = glm::min(lo, m.positions[v]); hi = glm::max(hi, m.positions[v]); }
        clo = glm::min(clo, centroids_[order_[k]]);
        chi = glm::max(chi, centroids_[order_[k]]);
    }
    nodes_[slot].lo = lo;
    nodes_[slot].hi = hi;
    if (count <= 4) {
        nodes_[slot].first = first;
        nodes_[slot].count = count;
        nodes_[slot].left = 0;
        return slot;
    }
    const glm::vec3 ext = chi - clo;
    const int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
    const uint32_t mid = first + count / 2;
    std::nth_element(order_.begin() + first, order_.begin() + mid, order_.begin() + first + count,
                     [&](uint32_t a, uint32_t b) { return centroids_[a][axis] < centroids_[b][axis]; });
    const uint32_t left = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back({});
    nodes_.push_back({});
    nodes_[slot].left = left;
    nodes_[slot].count = 0;
    build_into_(m, left, first, mid - first);
    build_into_(m, left + 1, mid, first + count - mid);
    return slot;
}

bool TriangleBVH::slab_(const Node& n, glm::vec3 o, glm::vec3 inv, float t0, float t1) {
    for (int a = 0; a < 3; ++a) {
        float ta = (n.lo[a] - o[a]) * inv[a], tb = (n.hi[a] - o[a]) * inv[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

bool TriangleBVH::intersect_(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 o, glm::vec3 d, float& t) {
    const glm::vec3 e1 = b - a, e2 = c - a;
    const glm::vec3 p = glm::cross(d, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const glm::vec3 s = o - a;
    const float u = glm::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const glm::vec3 q = glm::cross(s, e1);
    const float v = glm::dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = glm::dot(e2, q) * inv;
    return true;
}

} // namespace editor
} // namespace toy

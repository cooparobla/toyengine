#include "editor/mesh/proportional.h"

namespace toy {
namespace editor {

const char* falloff_name(Falloff f) {
    switch (f) {
        case Falloff::Sphere: return "Sphere";
        case Falloff::Root: return "Root";
        case Falloff::InverseSquare: return "Inverse Square";
        case Falloff::Sharp: return "Sharp";
        case Falloff::Linear: return "Linear";
        case Falloff::Constant: return "Constant";
        default: return "Smooth";
    }
}

float falloff_weight(Falloff f, float t) {
    if (t >= 1.0f) return 0.0f;
    t = std::max(t, 0.0f);
    const float u = 1.0f - t;   // 1 at the selection, 0 at the radius
    switch (f) {
        case Falloff::Sphere: return std::sqrt(std::max(0.0f, 2.0f * u - u * u));
        case Falloff::Root: return std::sqrt(u);
        case Falloff::InverseSquare: return u * (2.0f - u);
        case Falloff::Sharp: return u * u;
        case Falloff::Linear: return u;
        case Falloff::Constant: return 1.0f;
        default: return 3.0f * u * u - 2.0f * u * u * u;   // smoothstep
    }
}

std::vector<std::pair<uint32_t, float>> proportional_weights(
        const EditMesh& m, const std::set<uint32_t>& selected, const ProportionalSettings& s, const glm::mat4& world,
        const std::function<std::optional<glm::vec2>(const glm::vec3&)>& project, float radius_px) {
    std::vector<std::pair<uint32_t, float>> out;
    const size_t n = m.positions.size();
    if (!s.enabled || selected.empty() || n == 0) return out;
    std::vector<glm::vec3> wp(n);
    for (size_t v = 0; v < n; ++v) wp[v] = glm::vec3(world * glm::vec4(m.positions[v], 1.0f));
    constexpr float kInf = std::numeric_limits<float>::infinity();
    std::vector<float> dist(n, kInf);

    if (s.connected) {
        // Multi-source Dijkstra along edges, stopping past the radius.
        std::vector<std::vector<uint32_t>> adj(n);
        for (const auto& e : m.edges()) {
            if (e.first >= n || e.second >= n) continue;
            adj[e.first].push_back(e.second);
            adj[e.second].push_back(e.first);
        }
        using Item = std::pair<float, uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
        for (uint32_t v : selected) if (v < n) { dist[v] = 0.0f; q.push({0.0f, v}); }
        while (!q.empty()) {
            const auto [d, v] = q.top();
            q.pop();
            if (d > dist[v]) continue;
            for (uint32_t u : adj[v]) {
                const float nd = d + glm::length(wp[u] - wp[v]);
                if (nd < dist[u] && nd < s.radius) { dist[u] = nd; q.push({nd, u}); }
            }
        }
        for (size_t v = 0; v < n; ++v) {
            if (selected.count(static_cast<uint32_t>(v)) || dist[v] == kInf) continue;
            const float w = falloff_weight(s.falloff, dist[v] / s.radius);
            if (w > 0.0f) out.push_back({static_cast<uint32_t>(v), w});
        }
        return out;
    }

    if (s.projected && project && radius_px > 0.0f) {
        std::vector<glm::vec2> sel_px;
        glm::vec2 lo(kInf), hi(-kInf);
        for (uint32_t v : selected) {
            if (v >= n) continue;
            if (auto p = project(wp[v])) { sel_px.push_back(*p); lo = glm::min(lo, *p); hi = glm::max(hi, *p); }
        }
        if (sel_px.empty()) return out;
        lo -= glm::vec2(radius_px);
        hi += glm::vec2(radius_px);
        for (size_t v = 0; v < n; ++v) {
            if (selected.count(static_cast<uint32_t>(v))) continue;
            const auto p = project(wp[v]);
            if (!p || p->x < lo.x || p->y < lo.y || p->x > hi.x || p->y > hi.y) continue;
            float best = kInf;
            for (const auto& q : sel_px) best = std::min(best, glm::length(*p - q));
            const float w = falloff_weight(s.falloff, best / radius_px);
            if (w > 0.0f) out.push_back({static_cast<uint32_t>(v), w});
        }
        return out;
    }

    // 3D: nearest selected vertex, culled by the selection's bounds grown by the radius.
    std::vector<glm::vec3> sel_w;
    glm::vec3 lo(kInf), hi(-kInf);
    for (uint32_t v : selected) {
        if (v >= n) continue;
        sel_w.push_back(wp[v]);
        lo = glm::min(lo, wp[v]);
        hi = glm::max(hi, wp[v]);
    }
    lo -= glm::vec3(s.radius);
    hi += glm::vec3(s.radius);
    for (size_t v = 0; v < n; ++v) {
        if (selected.count(static_cast<uint32_t>(v))) continue;
        const glm::vec3& p = wp[v];
        if (glm::any(glm::lessThan(p, lo)) || glm::any(glm::greaterThan(p, hi))) continue;
        float best = kInf;
        for (const auto& q : sel_w) best = std::min(best, glm::length(p - q));
        const float w = falloff_weight(s.falloff, best / s.radius);
        if (w > 0.0f) out.push_back({static_cast<uint32_t>(v), w});
    }
    return out;
}

} // namespace editor
} // namespace toy

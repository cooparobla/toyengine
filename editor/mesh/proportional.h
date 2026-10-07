/**
 * @file proportional.h
 * @brief Proportional editing (Blender's O): a move, rotation or scale of the selection also
 *        drags the unselected vertices around it, each by a weight that falls off with its
 *        distance from the selection out to a radius.
 *
 * Distance is measured one of three ways (ProportionalSettings):
 *   - projected (default): in screen pixels from the nearest selected vertex -- what lies inside
 *     the radius circle drawn around the selection is what moves (Blender's "Projected from View");
 *   - 3D: straight-line world distance to the nearest selected vertex;
 *   - connected: along the mesh's edges (world lengths), so separate pieces never move together.
 * The radius is a world-space length either way; projected mode turns it into pixels at the
 * pivot's depth, which is the size of the circle drawn.
 *
 * The caller applies a transform to a weighted vertex with every component scaled by its weight
 * (translation * w, angle * w, scale 1 + (s - 1) * w) -- see EditorApp::apply_transform_().
 */

#ifndef TOYEDITOR_MESH_PROPORTIONAL_H
#define TOYEDITOR_MESH_PROPORTIONAL_H

#include "edit_mesh.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <utility>
#include <vector>

namespace toy::editor {

/** @brief Falloff curves, Blender's names and shapes. */
enum class Falloff { Smooth, Sphere, Root, InverseSquare, Sharp, Linear, Constant };
inline constexpr int kFalloffCount = 7;

inline const char* falloff_name(Falloff f) {
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

struct ProportionalSettings {
    bool enabled = false;
    Falloff falloff = Falloff::Smooth;
    float radius = 1.0f;          ///< World units.
    bool projected = true;        ///< Measure in screen space (Projected from View).
    bool connected = false;       ///< Measure along edges (overrides projected).

    static constexpr float kMinRadius = 1e-4f;
    static constexpr float kMaxRadius = 1e4f;
    void set_radius(float r) { radius = std::clamp(r, kMinRadius, kMaxRadius); }
};

/**
 * @brief The weight at normalized distance `t` = distance / radius: 1 at the selection, 0 at
 *        and beyond the radius.
 */
inline float falloff_weight(Falloff f, float t) {
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

/**
 * @brief Per-vertex weights: 1 for `selected`, the falloff for vertices within the radius, 0
 *        for the rest. Only vertices with a non-zero weight and not in `selected` are returned.
 * @param world    Mesh -> world (distances are world-space).
 * @param project  World -> screen pixels; used (and required) when settings.projected.
 * @param radius_px The radius in pixels (projected mode).
 */
inline std::vector<std::pair<uint32_t, float>> proportional_weights(
        const EditMesh& m, const std::set<uint32_t>& selected, const ProportionalSettings& s, const glm::mat4& world,
        const std::function<std::optional<glm::vec2>(const glm::vec3&)>& project = {}, float radius_px = 0.0f) {
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

} // namespace toy::editor

#endif // TOYEDITOR_MESH_PROPORTIONAL_H

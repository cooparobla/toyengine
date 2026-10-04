/**
 * @file paint.h
 * @brief Blender-style Vertex Paint and Weight Paint on an EditMesh: brush settings and the
 *        per-dab operations. Pure CPU (no GPU, no UI), so every brush is unit-testable.
 *
 * Both modes paint VERTICES, as Blender's point-domain colour attributes and vertex groups do:
 * a dab finds the vertices within its radius (SculptCache / VertexGrid, shared with Sculpt
 * Mode), weights each by the brush falloff times strength, and changes that vertex's value.
 *  - Vertex Paint writes the colour of every corner of the vertex (EditMesh stores colour per
 *    corner, as the file does, so an imported per-corner split survives until painted over).
 *  - Weight Paint writes the vertex's weight in the active vertex group.
 *
 * Tools: Draw blends the brush value in (Mix / Add / Subtract / Multiply / Lighten / Darken
 * for colour; Mix / Add / Subtract for weight); Blur pulls each vertex toward the mean of its
 * neighbours; Average toward the mean under the whole brush. "Front faces only" skips vertices
 * facing away from the view, so a dab does not bleed through a thin wall.
 */

#ifndef TOYEDITOR_MESH_PAINT_H
#define TOYEDITOR_MESH_PAINT_H

#include "edit_mesh.h"
#include "sculpt.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace toy::editor {

enum class PaintTool { Draw = 0, Blur, Average };
enum class PaintBlend { Mix = 0, Add, Subtract, Multiply, Lighten, Darken };

inline const char* paint_tool_name(PaintTool t) {
    switch (t) {
        case PaintTool::Draw: return "Draw";
        case PaintTool::Blur: return "Blur";
        case PaintTool::Average: return "Average";
    }
    return "Draw";
}

inline const char* paint_blend_name(PaintBlend b) {
    switch (b) {
        case PaintBlend::Mix: return "Mix";
        case PaintBlend::Add: return "Add";
        case PaintBlend::Subtract: return "Subtract";
        case PaintBlend::Multiply: return "Multiply";
        case PaintBlend::Lighten: return "Lighten";
        case PaintBlend::Darken: return "Darken";
    }
    return "Mix";
}

/** @brief One paint mode's brush (Vertex Paint and Weight Paint each keep their own). */
struct PaintSettings {
    PaintTool tool = PaintTool::Draw;
    PaintBlend blend = PaintBlend::Mix;
    float radius_px = 50.0f;                   ///< Brush radius on screen (F).
    float strength = 1.0f;                     ///< 0..1 (Shift+F).
    float spacing = 0.1f;                      ///< Dab spacing, a fraction of the radius.
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};   ///< Vertex Paint: the colour Draw lays down.
    glm::vec4 secondary{0.0f, 0.0f, 0.0f, 1.0f};   ///< Vertex Paint: Ctrl paints this one (X swaps).
    float weight = 1.0f;                       ///< Weight Paint: the weight Draw lays down.
    bool front_faces_only = true;
    bool auto_normalize = false;               ///< Weight Paint: keep each vertex's weights summing to 1.
    MirrorSettings symmetry{{true, false, false}, false};
};

/** @brief One brush application, in mesh-local space. */
struct PaintDab {
    glm::vec3 center{0.0f};
    glm::vec3 view_dir{0, 0, -1};   ///< Into the surface; with front_faces_only, vertices facing along it are skipped.
    float radius = 0.1f;
    float strength = 1.0f;
    bool invert = false;            ///< Ctrl: the secondary colour, or subtract weight.
};

namespace paint_detail {

/** @brief Vertices under the dab with their falloff x strength factors (> 0). */
inline void gather(const EditMesh& m, const SculptCache& cache, const VertexGrid& grid, const PaintDab& d,
                   bool front_only, std::vector<uint32_t>& verts, std::vector<float>& f) {
    std::vector<uint32_t> in;
    grid.query(m, d.center, d.radius, in);
    verts.clear();
    f.clear();
    for (uint32_t v : in) {
        if (front_only && v < cache.vert_n.size() && glm::dot(cache.vert_n[v], d.view_dir) > 0.0f) continue;
        const float k = sculpt_falloff(glm::length(m.positions[v] - d.center) / d.radius) * std::clamp(d.strength, 0.0f, 1.0f);
        if (k <= 0.0f) continue;
        verts.push_back(v);
        f.push_back(k);
    }
}

inline glm::vec4 blend(PaintBlend b, const glm::vec4& c, const glm::vec4& paint, float k) {
    glm::vec4 target;
    switch (b) {
        case PaintBlend::Mix: target = paint; break;
        case PaintBlend::Add: target = c + paint; break;
        case PaintBlend::Subtract: target = c - paint; break;
        case PaintBlend::Multiply: target = c * paint; break;
        case PaintBlend::Lighten: target = glm::max(c, paint); break;
        case PaintBlend::Darken: target = glm::min(c, paint); break;
    }
    // Colour channels blend; alpha mixes toward the paint's (so painting never erases alpha by accident).
    glm::vec4 out = glm::mix(c, target, k);
    out.a = glm::mix(c.a, paint.a, k);
    return glm::clamp(out, glm::vec4(0.0f), glm::vec4(1.0f));
}

/** @brief The mean colour of vertex `v`'s corners (through the cache's vertex -> face lists). */
inline glm::vec4 vertex_color(const EditMesh& m, const SculptCache& cache, uint32_t v) {
    glm::vec4 sum(0.0f);
    float n = 0.0f;
    for (uint32_t k = cache.vf_off[v]; k < cache.vf_off[v + 1]; ++k) {
        for (const auto& c : m.faces[cache.vf[k]].corners) if (c.v == v) { sum += c.color; n += 1.0f; }
    }
    return n > 0.0f ? sum / n : glm::vec4(1.0f);
}

/** @brief Sets every corner of vertex `v` to `col`. */
inline void set_vertex_color(EditMesh& m, const SculptCache& cache, uint32_t v, const glm::vec4& col) {
    for (uint32_t k = cache.vf_off[v]; k < cache.vf_off[v + 1]; ++k) {
        for (auto& c : m.faces[cache.vf[k]].corners) if (c.v == v) c.color = col;
    }
}

}  // namespace paint_detail

/**
 * @brief One Vertex Paint dab. Turns the mesh's colour attribute on if it had none (every
 *        corner starts white). Appends the painted vertices to `changed`.
 */
inline void vertex_paint_dab(EditMesh& m, const SculptCache& cache, const VertexGrid& grid, const PaintSettings& s,
                             const PaintDab& d, std::vector<uint32_t>& changed) {
    if (!m.has_colors) {
        m.has_colors = true;
        for (auto& f : m.faces) for (auto& c : f.corners) c.color = glm::vec4(1.0f);
    }
    std::vector<uint32_t> verts;
    std::vector<float> f;
    paint_detail::gather(m, cache, grid, d, s.front_faces_only, verts, f);
    if (verts.empty()) return;
    std::vector<glm::vec4> cur(verts.size()), next(verts.size());
    glm::vec4 mean(0.0f);
    for (size_t i = 0; i < verts.size(); ++i) {
        cur[i] = paint_detail::vertex_color(m, cache, verts[i]);
        mean += cur[i];
    }
    mean /= static_cast<float>(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        const uint32_t v = verts[i];
        switch (s.tool) {
            case PaintTool::Draw: next[i] = paint_detail::blend(s.blend, cur[i], d.invert ? s.secondary : s.color, f[i]); break;
            case PaintTool::Average: next[i] = glm::mix(cur[i], mean, f[i]); break;
            case PaintTool::Blur: {
                glm::vec4 avg(0.0f);
                const uint32_t b = cache.nb_off[v], e = cache.nb_off[v + 1];
                for (uint32_t k = b; k < e; ++k) avg += paint_detail::vertex_color(m, cache, cache.nb[k]);
                next[i] = e > b ? glm::mix(cur[i], avg / static_cast<float>(e - b), f[i]) : cur[i];
                break;
            }
        }
    }
    for (size_t i = 0; i < verts.size(); ++i) {   // after reading every value: order-independent
        paint_detail::set_vertex_color(m, cache, verts[i], next[i]);
        changed.push_back(verts[i]);
    }
}

/**
 * @brief Rescales vertex `v`'s other groups so all its weights sum to 1, keeping `locked`'s
 *        (Blender's Auto Normalize). A vertex in no other group keeps its weight as it is.
 */
inline void normalize_vertex_weights(EditMesh& m, uint32_t v, uint32_t locked) {
    if (v >= m.weights.size()) return;
    const float keep = m.weight(v, locked);
    float others = 0.0f;
    for (const auto& w : m.weights[v]) if (w.group != locked) others += w.weight;
    if (others <= 1e-6f) return;
    const float scale = std::max(0.0f, 1.0f - keep) / others;
    for (auto& w : m.weights[v]) if (w.group != locked) w.weight *= scale;
    m.weights[v].erase(std::remove_if(m.weights[v].begin(), m.weights[v].end(),
                                      [](const VertexWeight& w) { return w.weight <= 1e-6f; }),
                       m.weights[v].end());
}

/** @brief One Weight Paint dab on vertex group `group`. Appends the painted vertices to `changed`. */
inline void weight_paint_dab(EditMesh& m, const SculptCache& cache, const VertexGrid& grid, const PaintSettings& s,
                             uint32_t group, const PaintDab& d, std::vector<uint32_t>& changed) {
    if (group >= m.groups.size()) return;
    m.sync_vertex_data();
    if (m.weights.size() < m.positions.size()) m.weights.resize(m.positions.size());
    std::vector<uint32_t> verts;
    std::vector<float> f;
    paint_detail::gather(m, cache, grid, d, s.front_faces_only, verts, f);
    if (verts.empty()) return;
    std::vector<float> cur(verts.size()), next(verts.size());
    float mean = 0.0f;
    for (size_t i = 0; i < verts.size(); ++i) {
        cur[i] = m.weight(verts[i], group);
        mean += cur[i];
    }
    mean /= static_cast<float>(verts.size());
    const float w = std::clamp(s.weight, 0.0f, 1.0f);
    for (size_t i = 0; i < verts.size(); ++i) {
        const uint32_t v = verts[i];
        float x = cur[i];
        switch (s.tool) {
            case PaintTool::Draw: {
                // Ctrl inverts: Mix toward 0 (erase), Add <-> Subtract.
                PaintBlend b = s.blend;
                if (d.invert) b = b == PaintBlend::Add ? PaintBlend::Subtract : b == PaintBlend::Subtract ? PaintBlend::Add : b;
                const float target = d.invert && b == s.blend ? 0.0f : w;
                if (b == PaintBlend::Add) x = cur[i] + w * f[i];
                else if (b == PaintBlend::Subtract) x = cur[i] - w * f[i];
                else x = cur[i] + (target - cur[i]) * f[i];
                break;
            }
            case PaintTool::Average: x = cur[i] + (mean - cur[i]) * f[i]; break;
            case PaintTool::Blur: {
                float avg = 0.0f;
                const uint32_t b = cache.nb_off[v], e = cache.nb_off[v + 1];
                for (uint32_t k = b; k < e; ++k) avg += m.weight(cache.nb[k], group);
                if (e > b) x = cur[i] + (avg / static_cast<float>(e - b) - cur[i]) * f[i];
                break;
            }
        }
        next[i] = std::clamp(x, 0.0f, 1.0f);
    }
    for (size_t i = 0; i < verts.size(); ++i) {
        m.set_weight(verts[i], group, next[i]);
        if (s.auto_normalize) normalize_vertex_weights(m, verts[i], group);
        changed.push_back(verts[i]);
    }
}

/** @brief Sets every corner of the mesh (or of `only`, if not empty) to `col` (Vertex Paint's Fill). */
inline void fill_vertex_colors(EditMesh& m, const glm::vec4& col, const std::vector<uint32_t>& only = {}) {
    if (!m.has_colors) {
        m.has_colors = true;
        for (auto& f : m.faces) for (auto& c : f.corners) c.color = glm::vec4(1.0f);
    }
    std::vector<bool> pick;
    if (!only.empty()) {
        pick.assign(m.positions.size(), false);
        for (uint32_t v : only) if (v < pick.size()) pick[v] = true;
    }
    for (auto& f : m.faces) for (auto& c : f.corners) if (pick.empty() || pick[c.v]) c.color = col;
}

/** @brief Inverts every corner's RGB (alpha kept). */
inline void invert_vertex_colors(EditMesh& m) {
    if (!m.has_colors) return;
    for (auto& f : m.faces) for (auto& c : f.corners) c.color = glm::vec4(glm::vec3(1.0f) - glm::vec3(c.color), c.color.a);
}

/** @brief Removes the colour attribute (the mesh is written without `colors`). */
inline void clear_vertex_colors(EditMesh& m) {
    m.has_colors = false;
    for (auto& f : m.faces) for (auto& c : f.corners) c.color = glm::vec4(1.0f);
}

/** @brief Gives every vertex (or every vertex of `only`) weight `w` in `group` (0 removes them). */
inline void assign_weight(EditMesh& m, uint32_t group, float w, const std::vector<uint32_t>& only = {}) {
    if (group >= m.groups.size()) return;
    m.sync_vertex_data();
    if (m.weights.size() < m.positions.size()) m.weights.resize(m.positions.size());
    if (only.empty()) {
        for (uint32_t v = 0; v < m.positions.size(); ++v) m.set_weight(v, group, w);
    } else {
        for (uint32_t v : only) m.set_weight(v, group, w);
    }
}

/** @brief Normalizes every vertex's weights across all groups to sum to 1 (Normalize All). */
inline void normalize_all_weights(EditMesh& m) {
    for (auto& list : m.weights) {
        float sum = 0.0f;
        for (const auto& w : list) sum += w.weight;
        if (sum <= 1e-6f) continue;
        for (auto& w : list) w.weight /= sum;
    }
}

/**
 * @brief The weight heatmap Weight Paint shows (Blender's): blue at 0 through green and
 *        yellow to red at 1. Vertices outside the group draw as the 0 colour too.
 */
inline glm::vec3 weight_heatmap(float w) {
    w = std::clamp(w, 0.0f, 1.0f);
    static const glm::vec3 k[5] = {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    const float x = w * 4.0f;
    const int i = std::min(3, static_cast<int>(x));
    return glm::mix(k[i], k[i + 1], x - static_cast<float>(i));
}

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_PAINT_H

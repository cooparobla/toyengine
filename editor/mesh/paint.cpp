#include "editor/mesh/paint.h"

namespace toy {
namespace editor {

const char* paint_tool_name(PaintTool t) {
    switch (t) {
        case PaintTool::Draw: return "Draw";
        case PaintTool::Blur: return "Blur";
        case PaintTool::Average: return "Average";
    }
    return "Draw";
}

const char* paint_blend_name(PaintBlend b) {
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

} // namespace editor
} // namespace toy

namespace toy {
namespace editor {
namespace paint_detail {

void gather(const EditMesh& m, const SculptCache& cache, const VertexGrid& grid, const PaintDab& d,
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

glm::vec4 blend(PaintBlend b, const glm::vec4& c, const glm::vec4& paint, float k) {
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

glm::vec4 vertex_color(const EditMesh& m, const SculptCache& cache, uint32_t v) {
    glm::vec4 sum(0.0f);
    float n = 0.0f;
    for (uint32_t k = cache.vf_off[v]; k < cache.vf_off[v + 1]; ++k) {
        for (const auto& c : m.faces[cache.vf[k]].corners) if (c.v == v) { sum += c.color; n += 1.0f; }
    }
    return n > 0.0f ? sum / n : glm::vec4(1.0f);
}

void set_vertex_color(EditMesh& m, const SculptCache& cache, uint32_t v, const glm::vec4& col) {
    for (uint32_t k = cache.vf_off[v]; k < cache.vf_off[v + 1]; ++k) {
        for (auto& c : m.faces[cache.vf[k]].corners) if (c.v == v) c.color = col;
    }
}

} // namespace paint_detail
} // namespace editor
} // namespace toy

namespace toy {
namespace editor {

void vertex_paint_dab(EditMesh& m, const SculptCache& cache, const VertexGrid& grid, const PaintSettings& s,
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

void normalize_vertex_weights(EditMesh& m, uint32_t v, uint32_t locked) {
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

void weight_paint_dab(EditMesh& m, const SculptCache& cache, const VertexGrid& grid, const PaintSettings& s,
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

void fill_vertex_colors(EditMesh& m, const glm::vec4& col, const std::vector<uint32_t>& only) {
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

void invert_vertex_colors(EditMesh& m) {
    if (!m.has_colors) return;
    for (auto& f : m.faces) for (auto& c : f.corners) c.color = glm::vec4(glm::vec3(1.0f) - glm::vec3(c.color), c.color.a);
}

void clear_vertex_colors(EditMesh& m) {
    m.has_colors = false;
    for (auto& f : m.faces) for (auto& c : f.corners) c.color = glm::vec4(1.0f);
}

void assign_weight(EditMesh& m, uint32_t group, float w, const std::vector<uint32_t>& only) {
    if (group >= m.groups.size()) return;
    m.sync_vertex_data();
    if (m.weights.size() < m.positions.size()) m.weights.resize(m.positions.size());
    if (only.empty()) {
        for (uint32_t v = 0; v < m.positions.size(); ++v) m.set_weight(v, group, w);
    } else {
        for (uint32_t v : only) m.set_weight(v, group, w);
    }
}

void normalize_all_weights(EditMesh& m) {
    for (auto& list : m.weights) {
        float sum = 0.0f;
        for (const auto& w : list) sum += w.weight;
        if (sum <= 1e-6f) continue;
        for (auto& w : list) w.weight /= sum;
    }
}

glm::vec3 weight_heatmap(float w) {
    w = std::clamp(w, 0.0f, 1.0f);
    static const glm::vec3 k[5] = {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    const float x = w * 4.0f;
    const int i = std::min(3, static_cast<int>(x));
    return glm::mix(k[i], k[i + 1], x - static_cast<float>(i));
}

} // namespace editor
} // namespace toy

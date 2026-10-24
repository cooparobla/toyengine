#include "editor/mesh/paint.h"

#include "editor/mesh/mesh_mirror.h"

#include <cctype>
#include <map>

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

void limit_total_weights(EditMesh& m, uint32_t max_influences) {
    if (max_influences == 0) return;
    for (auto& list : m.weights) {
        if (list.size() <= max_influences) continue;
        std::sort(list.begin(), list.end(), [](const VertexWeight& a, const VertexWeight& b) {
            return (a.weight != b.weight) ? a.weight > b.weight : a.group < b.group;
        });
        float dropped = 0.0f, kept = 0.0f;
        for (size_t i = 0; i < list.size(); ++i) (i < max_influences ? kept : dropped) += list[i].weight;
        list.resize(max_influences);
        // Hand the dropped weight to the influences that stay, so the vertex keeps following
        // its bones as closely as four of them can manage.
        if (kept > 1e-6f && dropped > 0.0f) {
            const float scale = (kept + dropped) / kept;
            for (auto& w : list) w.weight *= scale;
        }
    }
}

void clean_weights(EditMesh& m, float threshold, std::optional<uint32_t> group) {
    for (auto& list : m.weights) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const VertexWeight& w) {
                       return (!group || w.group == *group) && w.weight <= threshold;
                   }),
                   list.end());
    }
}

void smooth_weights(EditMesh& m, const SculptCache& cache, std::optional<uint32_t> group, float factor,
                    int iterations, const std::vector<uint32_t>& only) {
    if (iterations <= 0 || m.groups.empty()) return;
    m.sync_vertex_data();
    if (cache.nb_off.size() < m.positions.size() + 1) return;
    const float f = std::clamp(factor, 0.0f, 1.0f);
    std::vector<uint32_t> verts = only;
    if (verts.empty()) {
        verts.resize(m.positions.size());
        for (uint32_t v = 0; v < m.positions.size(); ++v) verts[v] = v;
    }
    for (int it = 0; it < iterations; ++it) {
        std::vector<std::vector<VertexWeight>> next;
        next.reserve(verts.size());
        for (uint32_t v : verts) {
            if (v >= m.positions.size()) { next.emplace_back(); continue; }
            const uint32_t begin = cache.nb_off[v], end = cache.nb_off[v + 1];
            if (end <= begin) { next.push_back(m.weights[v]); continue; }
            // Every group either vertex or any neighbour has a weight in: a group only one of
            // them is in must take part too, or smoothing could never spread into a vertex.
            std::map<uint32_t, float> sum;
            for (const auto& w : m.weights[v]) sum[w.group] = 0.0f;
            for (uint32_t k = begin; k < end; ++k) {
                for (const auto& w : m.weights[cache.nb[k]]) sum[w.group] += w.weight;
            }
            const float count = static_cast<float>(end - begin);
            std::vector<VertexWeight> result;
            for (const auto& [g, total] : sum) {
                if (group && g != *group) {
                    const float keep = m.weight(v, g);
                    if (keep > 1e-6f) result.push_back({g, keep});
                    continue;
                }
                const float blended = m.weight(v, g) * (1.0f - f) + (total / count) * f;
                if (blended > 1e-6f) result.push_back({g, blended});
            }
            next.push_back(std::move(result));
        }
        for (size_t i = 0; i < verts.size(); ++i) {
            if (verts[i] < m.weights.size()) m.weights[verts[i]] = std::move(next[i]);
        }
    }
}

namespace {

/** @brief The side marker `token` names, if any: 'l', 'r' or 0. */
char side_of_token(const std::string& token) {
    std::string lower;
    for (char ch : token) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower == "l" || lower == "left") return 'l';
    if (lower == "r" || lower == "right") return 'r';
    return 0;
}

/** @brief `token` with its side swapped, keeping its capitalisation. */
std::string flip_token(const std::string& token) {
    const char side = side_of_token(token);
    if (!side) return token;
    const bool upper = std::isupper(static_cast<unsigned char>(token[0])) != 0;
    const bool word = token.size() > 1;
    std::string out = (side == 'l') ? (word ? "right" : "r") : (word ? "left" : "l");
    if (upper) {
        if (word && token.size() > 1 && std::isupper(static_cast<unsigned char>(token[1]))) {
            for (char& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        } else {
            out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
        }
    }
    return out;
}

} // namespace

std::string flipped_group_name(const std::string& name) {
    static const std::string separators = "._- ";
    const size_t last = name.find_last_of(separators);
    if (last != std::string::npos && last + 1 < name.size()) {
        const std::string token = name.substr(last + 1);
        if (side_of_token(token)) return name.substr(0, last + 1) + flip_token(token);
    }
    const size_t first = name.find_first_of(separators);
    if (first != std::string::npos && first > 0) {
        const std::string token = name.substr(0, first);
        if (side_of_token(token)) return flip_token(token) + name.substr(first);
    }
    return {};
}

void mirror_weights(EditMesh& m, int axis, bool flip_group_names) {
    if (axis < 0 || axis > 2 || m.groups.empty()) return;
    m.sync_vertex_data();
    // Where each group's weights land. Resolved before any mirroring, since creating the
    // opposite group grows `m.groups` and would otherwise be walked into.
    std::vector<uint32_t> target(m.groups.size());
    for (uint32_t g = 0; g < target.size(); ++g) target[g] = g;
    if (flip_group_names) {
        const size_t count = m.groups.size();
        for (uint32_t g = 0; g < count; ++g) {
            const std::string flipped = flipped_group_name(m.groups[g]);
            if (!flipped.empty()) target[g] = m.group_index(flipped);
        }
    }
    m.sync_vertex_data();
    const std::vector<int32_t> partners = mirror_partners(m, axis);
    std::vector<std::vector<VertexWeight>> result(m.positions.size());
    for (uint32_t v = 0; v < m.positions.size(); ++v) {
        const int32_t src = (v < partners.size()) ? partners[v] : -1;
        if (src < 0) { result[v] = m.weights[v]; continue; }   // nothing opposite: keep its own
        for (const auto& w : m.weights[static_cast<uint32_t>(src)]) {
            result[v].push_back({w.group < target.size() ? target[w.group] : w.group, w.weight});
        }
    }
    m.weights = std::move(result);
}

void invert_group_weights(EditMesh& m, uint32_t group) {
    if (group >= m.groups.size()) return;
    m.sync_vertex_data();
    for (uint32_t v = 0; v < m.positions.size(); ++v) m.set_weight(v, group, 1.0f - m.weight(v, group));
}

void levels_group_weights(EditMesh& m, uint32_t group, float gain, float offset) {
    if (group >= m.groups.size()) return;
    m.sync_vertex_data();
    for (auto& list : m.weights) {
        for (auto& w : list) {
            if (w.group == group) w.weight = std::clamp(w.weight * gain + offset, 0.0f, 1.0f);
        }
    }
    clean_weights(m, 0.0f, group);
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

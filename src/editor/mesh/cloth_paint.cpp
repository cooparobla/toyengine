#include "editor/mesh/cloth_paint.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace toy {
namespace editor {

namespace {

/** @brief Per-vertex mirror partner across local `axis`, by quantized position (the same pairing
 *         rule mirror_weights uses); -1 where no vertex sits at the mirrored spot. */
std::vector<int32_t> cloth_mirror_partners(const EditMesh& m, int axis) {
    auto q = [](float v) { return static_cast<long long>(std::llround(v * 10000.0f)); };
    std::map<std::tuple<long long, long long, long long>, uint32_t> at;
    for (uint32_t v = 0; v < m.positions.size(); ++v) {
        at[{q(m.positions[v].x), q(m.positions[v].y), q(m.positions[v].z)}] = v;
    }
    std::vector<int32_t> out(m.positions.size(), -1);
    for (uint32_t v = 0; v < m.positions.size(); ++v) {
        glm::vec3 p = m.positions[v];
        p[axis] = -p[axis];
        auto it = at.find({q(p.x), q(p.y), q(p.z)});
        if (it != at.end()) out[v] = static_cast<int32_t>(it->second);
    }
    return out;
}

} // namespace

const char* cloth_channel_name(ClothChannel ch) {
    switch (ch) {
        case ClothChannel::MaxDistance: return "Max Distance";
        case ClothChannel::BackstopDistance: return "Backstop Distance";
        case ClothChannel::BackstopRadius: return "Backstop Radius";
        case ClothChannel::AnimDrive: return "Anim Drive";
    }
    return "";
}

float cloth_channel_limit(ClothChannel ch) {
    return ch == ClothChannel::AnimDrive ? 1.0f : 1e6f;
}

std::vector<float>& cloth_channel_data(EditMesh& m, ClothChannel ch) {
    // max_distance is the block's presence flag: materialise it with any channel.
    if (m.cloth_max_distance.empty()) m.cloth_max_distance.assign(m.positions.size(), 0.0f);
    std::vector<float>& data = *m.cloth_channels()[static_cast<size_t>(ch)];
    if (data.empty()) data.assign(m.positions.size(), 0.0f);
    m.sync_vertex_data();
    return data;
}

const std::vector<float>* cloth_channel_data(const EditMesh& m, ClothChannel ch) {
    const std::vector<float>* data = m.cloth_channels()[static_cast<size_t>(ch)];
    return data->empty() ? nullptr : data;
}

float cloth_channel_value(const EditMesh& m, ClothChannel ch, uint32_t v) {
    const std::vector<float>* data = cloth_channel_data(m, ch);
    return (data != nullptr && v < data->size()) ? (*data)[v] : 0.0f;
}

void cloth_paint_dab(EditMesh& m, const SculptCache& cache, const VertexGrid& grid,
                     const PaintSettings& s, ClothChannel ch, float value, const PaintDab& d,
                     std::vector<uint32_t>& changed) {
    std::vector<float>& data = cloth_channel_data(m, ch);
    std::vector<uint32_t> verts;
    std::vector<float> f;
    paint_detail::gather(m, cache, grid, d, s.front_faces_only, verts, f);
    if (verts.empty()) return;

    const float limit = cloth_channel_limit(ch);
    const float w = std::clamp(value, 0.0f, limit);
    std::vector<float> cur(verts.size()), next(verts.size());
    float mean = 0.0f;
    for (size_t i = 0; i < verts.size(); ++i) {
        cur[i] = data[verts[i]];
        mean += cur[i];
    }
    mean /= static_cast<float>(verts.size());

    for (size_t i = 0; i < verts.size(); ++i) {
        const uint32_t v = verts[i];
        float x = cur[i];
        switch (s.tool) {
            case PaintTool::Draw: {
                // Ctrl inverts: Mix toward 0 (erase), Add <-> Subtract -- weight_paint_dab's rule.
                PaintBlend b = s.blend;
                if (d.invert) {
                    b = b == PaintBlend::Add        ? PaintBlend::Subtract
                        : b == PaintBlend::Subtract ? PaintBlend::Add
                                                    : b;
                }
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
                for (uint32_t k = b; k < e; ++k) avg += data[cache.nb[k]];
                if (e > b) x = cur[i] + (avg / static_cast<float>(e - b) - cur[i]) * f[i];
                break;
            }
        }
        next[i] = std::clamp(x, 0.0f, limit);
    }
    for (size_t i = 0; i < verts.size(); ++i) {   // after reading every value: order-independent
        data[verts[i]] = next[i];
        changed.push_back(verts[i]);
    }
}

void fill_cloth_channel(EditMesh& m, ClothChannel ch, float value,
                        const std::vector<uint32_t>& only) {
    std::vector<float>& data = cloth_channel_data(m, ch);
    const float w = std::clamp(value, 0.0f, cloth_channel_limit(ch));
    if (only.empty()) {
        std::fill(data.begin(), data.end(), w);
        return;
    }
    for (uint32_t v : only) {
        if (v < data.size()) data[v] = w;
    }
}

void smooth_cloth_channel(EditMesh& m, const SculptCache& cache, ClothChannel ch, float factor,
                          int iterations) {
    std::vector<float>& data = cloth_channel_data(m, ch);
    const float k = std::clamp(factor, 0.0f, 1.0f);
    std::vector<float> next(data.size());
    for (int it = 0; it < iterations; ++it) {
        for (uint32_t v = 0; v < data.size(); ++v) {
            const uint32_t b = cache.nb_off[v], e = cache.nb_off[v + 1];
            if (e <= b) { next[v] = data[v]; continue; }
            float avg = 0.0f;
            for (uint32_t n = b; n < e; ++n) avg += data[cache.nb[n]];
            next[v] = data[v] + (avg / static_cast<float>(e - b) - data[v]) * k;
        }
        data = next;
    }
}

void mirror_cloth_channel(EditMesh& m, ClothChannel ch, int axis) {
    if (axis < 0 || axis > 2) return;
    std::vector<float>& data = cloth_channel_data(m, ch);
    const std::vector<int32_t> partners = cloth_mirror_partners(m, axis);
    std::vector<float> result(data.size());
    for (uint32_t v = 0; v < data.size(); ++v) {
        const int32_t src = v < partners.size() ? partners[v] : -1;
        result[v] = src >= 0 ? data[static_cast<uint32_t>(src)] : data[v];
    }
    data = std::move(result);
}

void gradient_cloth_channel(EditMesh& m, ClothChannel ch, const glm::vec3& from,
                            const glm::vec3& to, float lo, float hi) {
    std::vector<float>& data = cloth_channel_data(m, ch);
    const glm::vec3 axis = to - from;
    const float len2 = glm::dot(axis, axis);
    if (len2 < 1e-12f) return;
    const float limit = cloth_channel_limit(ch);
    for (uint32_t v = 0; v < data.size() && v < m.positions.size(); ++v) {
        const float t = std::clamp(glm::dot(m.positions[v] - from, axis) / len2, 0.0f, 1.0f);
        data[v] = std::clamp(lo + (hi - lo) * t, 0.0f, limit);
    }
}

void clear_cloth_channels(EditMesh& m) {
    for (auto* ch : m.cloth_channels()) ch->clear();
}

} // namespace editor
} // namespace toy

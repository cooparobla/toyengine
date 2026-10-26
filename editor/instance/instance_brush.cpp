#include "editor/instance/instance_brush.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include "editor/terrain/terrain_brush.h"

namespace toy::editor {

using toy::instance::ChunkCoord;
using toy::instance::InstanceRecord;
using toy::instance::InstanceSetData;

namespace {

/** @brief One 32-bit hash per (seed, cell): a candidate's randomness, with no state to carry. */
std::uint32_t hash_u32(std::uint32_t a, std::uint32_t b) {
    std::uint32_t h = a * 0x9E3779B9u ^ (b + 0x85EBCA6Bu + (a << 6) + (a >> 2));
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

/** @brief `hash_u32` as a float in [0, 1). */
float hash_01(std::uint32_t a, std::uint32_t b) {
    return static_cast<float>(hash_u32(a, b) >> 8) * (1.0f / 16777216.0f);
}

float lerp_(float a, float b, float t) { return a + (b - a) * t; }

} // namespace

const char* instance_tool_name(InstanceTool t) {
    switch (t) {
        case InstanceTool::Paint:   return "Paint";
        case InstanceTool::Erase:   return "Erase";
        case InstanceTool::Place:   return "Place";
        case InstanceTool::Cluster: return "Cluster";
        case InstanceTool::Select:  return "Select";
    }
    return "Paint";
}

glm::vec2 snap_to_grid(const InstanceBrushSettings& s, glm::vec2 p) {
    const float cell = std::max(s.grid_cell, 1e-3f);
    const glm::vec2 rel = p - s.grid_origin;
    return s.grid_origin + glm::vec2((std::floor(rel.x / cell) + 0.5f) * cell,
                                     (std::floor(rel.y / cell) + 0.5f) * cell);
}

std::vector<glm::vec2> brush_candidates(const InstanceBrushSettings& s, glm::vec2 centre, std::uint32_t seed) {
    std::vector<glm::vec2> out;
    const float radius = std::max(s.radius, 1e-3f);
    // Snapped painting walks whole cells: the jittered grid would land several candidates in one
    // cell and `one_per_cell` would throw all but one away, which reads as a thin, patchy brush.
    const float step = s.snap ? std::max(s.grid_cell, 1e-3f)
                              : 1.0f / std::sqrt(std::max(s.density, 1e-4f));
    const int n = std::max(1, static_cast<int>(std::ceil(2.0f * radius / step)));
    const glm::vec2 lo = centre - glm::vec2(radius);
    out.reserve(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const std::uint32_t cell = static_cast<std::uint32_t>(j) * 0x1000u + static_cast<std::uint32_t>(i);
            glm::vec2 p;
            if (s.snap) {
                p = snap_to_grid(s, lo + glm::vec2((static_cast<float>(i) + 0.5f) * step,
                                                   (static_cast<float>(j) + 0.5f) * step));
            } else {
                p = lo + glm::vec2((static_cast<float>(i) + hash_01(seed, cell)) * step,
                                   (static_cast<float>(j) + hash_01(seed ^ 0x5bf03635u, cell)) * step);
            }
            const float d01 = glm::length(p - centre) / radius;
            if (d01 >= 1.0f) continue;
            // The falloff is a keep PROBABILITY, so a stroke thins toward its rim rather than
            // ending in a hard ring of props at full density.
            if (hash_01(seed ^ 0x9e3779b9u, cell) > terrain_falloff(d01, s.falloff)) continue;
            out.push_back(p);
        }
    }
    return out;
}

std::vector<glm::vec2> cluster_candidates(const InstanceBrushSettings& s, glm::vec2 centre, std::uint32_t seed) {
    const int lo = std::max(1, std::min(s.cluster_count.x, s.cluster_count.y));
    const int hi = std::max(lo, std::max(s.cluster_count.x, s.cluster_count.y));
    const int count = lo + static_cast<int>(hash_u32(seed, 0x101u) % static_cast<std::uint32_t>(hi - lo + 1));
    const float spread = std::max(s.cluster_spread, 1e-3f);
    const float bias = std::clamp(s.cluster_bias, 0.0f, 1.0f);

    std::vector<glm::vec2> out;
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const std::uint32_t k = static_cast<std::uint32_t>(i);
        const float angle = hash_01(seed ^ 0x27d4eb2fu, k) * 6.28318531f;
        // sqrt(u) spreads points evenly over the disc; u itself crowds them at the centre.
        // `bias` mixes the two, so one slider goes from a scattered copse to a tight clump.
        const float u = hash_01(seed ^ 0x165667b1u, k);
        const float r = spread * lerp_(std::sqrt(u), u, bias);
        glm::vec2 p = centre + glm::vec2(std::cos(angle), std::sin(angle)) * r;
        if (s.snap) p = snap_to_grid(s, p);
        out.push_back(p);
    }
    return out;
}

int cluster_prototype(const InstanceBrushSettings& s, int palette_size, std::uint32_t seed) {
    if (!s.cluster_mixed || palette_size <= 1) return s.prototype;
    const int n = std::clamp(s.cluster_prototypes, 1, palette_size);
    if (n <= 1) return s.prototype;
    return static_cast<int>(hash_u32(seed, 0x202u) % static_cast<std::uint32_t>(n));
}

InstanceRecord make_record(const toy::instance::InstancePrototype& proto, int prototype_index,
                           const SurfaceSample& surface, const InstanceBrushSettings& s, std::uint32_t seed) {
    InstanceRecord r;
    r.prototype = static_cast<std::uint16_t>(std::max(0, prototype_index));
    r.seed = seed;

    const float scale = lerp_(std::min(proto.scale_range.x, proto.scale_range.y),
                              std::max(proto.scale_range.x, proto.scale_range.y),
                              hash_01(seed, 0x301u));
    r.scale = glm::vec3(scale);

    float yaw = glm::radians(lerp_(proto.yaw_range.x, proto.yaw_range.y, hash_01(seed, 0x302u)));
    if (s.snap && s.yaw_step > 0.0f) {
        const float step = glm::radians(s.yaw_step);
        yaw = std::round(yaw / step) * step;
    }

    // Stand it up, then tilt toward the surface normal by `align_to_normal`. Done as a rotation
    // BETWEEN two unit vectors rather than from Euler angles, so a vertical wall is as valid a
    // surface as a gentle slope and nothing gimbal-locks on the way.
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    glm::vec3 n = surface.normal;
    const float len = glm::length(n);
    n = len > 1e-5f ? n / len : up;
    const glm::quat spin = glm::angleAxis(yaw, up);
    const float align = std::clamp(proto.align_to_normal, 0.0f, 1.0f);
    const glm::quat tilt = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::rotation(up, n), align);
    r.rotation = tilt * spin;

    r.position = surface.position - n * (proto.sink * scale);
    return r;
}

// --- undo patches ---

std::size_t InstancePatch::bytes() const {
    std::size_t n = 0;
    for (const auto& [coord, records] : chunks) n += records.size() * sizeof(InstanceRecord);
    return n;
}

InstancePatch capture_instance_patch(const std::shared_ptr<InstanceSetData>& data,
                                     const std::vector<ChunkCoord>& chunks) {
    InstancePatch patch;
    if (!data) return patch;
    patch.data = data;
    patch.chunks.reserve(chunks.size());
    for (ChunkCoord c : chunks) {
        const std::vector<InstanceRecord>* records = data->records(c);
        patch.chunks.emplace_back(c, records != nullptr ? *records : std::vector<InstanceRecord>{});
    }
    return patch;
}

bool apply_instance_patch(const InstancePatch& patch) {
    const std::shared_ptr<InstanceSetData> data = patch.data.lock();
    if (!data) return false;
    for (const auto& [coord, records] : patch.chunks) data->set_records(coord, records);
    return true;
}

} // namespace toy::editor

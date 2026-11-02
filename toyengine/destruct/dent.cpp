#include <toyengine/destruct/dent.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace toy {
namespace destruct {

namespace {

/** @brief A position's bits, used as an unordered_map key -- positions written by the same
 *         computation (a literal duplicate, or two identical midpoint averages) come out
 *         bit-identical, so exact equality is enough; no epsilon merging is attempted. */
struct PositionKey {
    std::uint32_t x, y, z;
    bool operator==(const PositionKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct PositionKeyHash {
    std::size_t operator()(const PositionKey& k) const {
        std::size_t h = std::hash<std::uint32_t>()(k.x);
        h ^= std::hash<std::uint32_t>()(k.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::uint32_t>()(k.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

PositionKey key_of(const glm::vec3& p) {
    PositionKey k;
    std::memcpy(&k.x, &p.x, sizeof(float));
    std::memcpy(&k.y, &p.y, sizeof(float));
    std::memcpy(&k.z, &p.z, sizeof(float));
    return k;
}

/** @brief Mean triangle edge length, object space. */
float mean_edge_length(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices) {
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const glm::vec3& a = vertices[indices[t]].position;
        const glm::vec3& b = vertices[indices[t + 1]].position;
        const glm::vec3& c = vertices[indices[t + 2]].position;
        sum += glm::length(a - b) + glm::length(b - c) + glm::length(c - a);
        count += 3;
    }
    return count > 0 ? static_cast<float>(sum / static_cast<double>(count)) : 0.0f;
}

/** @brief Rewrites every vertex's normal from the triangles that use its own INDEX -- not
 *         averaged across a weld group -- so a flat-shaded face stays flat and a smooth patch
 *         stays smooth, whether or not this blow touched it. */
void recompute_normals(std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices) {
    std::vector<glm::vec3> accum(vertices.size(), glm::vec3(0.0f));
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const std::uint32_t ia = indices[t], ib = indices[t + 1], ic = indices[t + 2];
        const glm::vec3& a = vertices[ia].position;
        const glm::vec3& b = vertices[ib].position;
        const glm::vec3& c = vertices[ic].position;
        const glm::vec3 n = glm::cross(b - a, c - a); // unnormalised: area-weights the average
        accum[ia] += n;
        accum[ib] += n;
        accum[ic] += n;
    }
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const float len = glm::length(accum[i]);
        if (len > 1e-12f) vertices[i].normal = accum[i] / len;
        // A degenerate vertex (touches no non-zero-area triangle) keeps its existing normal.
    }
}

/** @brief Finds (or creates) the vertex at the midpoint of edge (ia, ib), sharing it with
 *         whichever other triangle already split this edge. */
std::uint32_t midpoint(std::vector<Vertex>& vertices, std::unordered_map<std::uint64_t, std::uint32_t>& cache,
                       std::uint32_t ia, std::uint32_t ib) {
    const std::uint32_t lo = std::min(ia, ib), hi = std::max(ia, ib);
    const std::uint64_t key = (static_cast<std::uint64_t>(lo) << 32) | hi;
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    const Vertex& a = vertices[ia];
    const Vertex& b = vertices[ib];
    Vertex mid;
    mid.position = (a.position + b.position) * 0.5f;
    const glm::vec3 n = a.normal + b.normal;
    const float n_len = glm::length(n);
    mid.normal = n_len > 1e-8f ? n / n_len : a.normal;
    mid.uv = (a.uv + b.uv) * 0.5f;
    const glm::vec3 t(a.tangent.x + b.tangent.x, a.tangent.y + b.tangent.y, a.tangent.z + b.tangent.z);
    const float t_len = glm::length(t);
    mid.tangent = t_len > 1e-8f ? glm::vec4(t / t_len, a.tangent.w) : a.tangent;

    const std::uint32_t idx = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back(mid);
    cache.emplace(key, idx);
    return idx;
}

} // namespace

float dent_falloff(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    // Flat-bottomed, not a single smoothstep dome: full weight out to kInnerFraction of the
    // radius gives the crater a floor, and confining the whole falloff to the outer band makes
    // that floor meet the untouched surface at a noticeably steeper slope -- a dent with no
    // crease anywhere gives recomputed normals nothing sharp to shade against. Same two-region
    // shape editor/terrain/terrain_brush.cpp's terrain_falloff() already uses in this repo.
    constexpr float kInnerFraction = 0.4f;
    if (x <= kInnerFraction) return 1.0f;
    const float t = (x - kInnerFraction) / (1.0f - kInnerFraction);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

void DentState::build(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices) {
    rest.clear();
    offset.clear();
    group_off.clear();
    group_members.clear();
    mean_edge = 0.0f;
    if (vertices.empty()) return;

    std::unordered_map<PositionKey, std::uint32_t, PositionKeyHash> unique_of;
    unique_of.reserve(vertices.size());
    std::vector<std::uint32_t> vertex_group(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const PositionKey k = key_of(vertices[i].position);
        const auto it = unique_of.find(k);
        std::uint32_t g;
        if (it == unique_of.end()) {
            g = static_cast<std::uint32_t>(rest.size());
            unique_of.emplace(k, g);
            rest.push_back(vertices[i].position);
        } else {
            g = it->second;
        }
        vertex_group[i] = g;
    }
    offset.assign(rest.size(), glm::vec3(0.0f));

    group_off.assign(rest.size() + 1, 0);
    for (const std::uint32_t g : vertex_group) group_off[g + 1] += 1;
    for (std::size_t g = 0; g < rest.size(); ++g) group_off[g + 1] += group_off[g];
    group_members.assign(vertices.size(), 0);
    std::vector<std::uint32_t> cursor(group_off.begin(), group_off.end() - 1);
    for (std::uint32_t i = 0; i < vertex_group.size(); ++i) {
        const std::uint32_t g = vertex_group[i];
        group_members[cursor[g]++] = i;
    }

    mean_edge = mean_edge_length(vertices, indices);
}

float DentState::max_offset() const {
    float m = 0.0f;
    for (const glm::vec3& o : offset) m = std::max(m, glm::length(o));
    return m;
}

std::size_t apply_dent(DentState& state, std::vector<Vertex>& vertices,
                       const std::vector<std::uint32_t>& indices, const DentBlow& blow,
                       const glm::mat4& world, const glm::mat4& inv_world) {
    if (state.empty() || blow.radius <= 0.0f || blow.depth == 0.0f) return 0;

    const float dir_len = glm::length(blow.direction);
    if (dir_len < 1e-6f) return 0;
    glm::vec3 dir = blow.direction / dir_len;

    if (blow.force_inward) {
        // Flip the whole blow once, not per vertex: a direction that points away from the
        // object's own world centre is pushing OUT of the surface, not into it. Per-vertex
        // flipping would let vertices on opposite sides of the footprint disagree on which way
        // is "in".
        const glm::vec3 centre(world[3]);
        const glm::vec3 outward = blow.point - centre;
        if (glm::length(outward) > 1e-6f && glm::dot(dir, glm::normalize(outward)) > 0.0f) dir = -dir;
    }

    const glm::mat3 inv_linear(inv_world);
    std::size_t touched = 0;
    for (std::size_t g = 0; g < state.rest.size(); ++g) {
        const glm::vec3 world_rest(world * glm::vec4(state.rest[g], 1.0f));
        const float dist = glm::length(world_rest - blow.point);
        if (dist >= blow.radius) continue;
        const float weight = dent_falloff(dist / blow.radius);
        if (weight <= 0.0f) continue;
        ++touched;

        glm::vec3 new_offset = state.offset[g] + dir * (blow.depth * weight);
        if (blow.max_offset > 0.0f) {
            const float len = glm::length(new_offset);
            if (len > blow.max_offset) new_offset *= (blow.max_offset / len);
        }
        state.offset[g] = new_offset;

        const glm::vec3 new_local = state.rest[g] + inv_linear * new_offset;
        for (std::uint32_t m = state.group_off[g]; m < state.group_off[g + 1]; ++m) {
            vertices[state.group_members[m]].position = new_local;
        }
    }

    if (touched > 0) recompute_normals(vertices, indices);
    return touched;
}

int subdivide_for_dent(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices,
                       float target_edge_world, float world_scale, int max_levels,
                       std::size_t max_vertices) {
    if (target_edge_world <= 0.0f || world_scale <= 0.0f) return 0;

    int levels = 0;
    while (levels < max_levels) {
        const float mean_world = mean_edge_length(vertices, indices) * world_scale;
        if (mean_world <= target_edge_world) break;
        // A closed triangle mesh gains roughly one new vertex per edge (~1.5x its triangle
        // count) each level, which for V ~ 0.5F is close to a 4x vertex-count ceiling -- a
        // conservative pre-check, not an exact one.
        if (vertices.size() * 4 > max_vertices) break;

        std::vector<std::uint32_t> new_indices;
        new_indices.reserve(indices.size() * 4);
        std::unordered_map<std::uint64_t, std::uint32_t> cache;
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            const std::uint32_t ia = indices[t], ib = indices[t + 1], ic = indices[t + 2];
            const std::uint32_t mab = midpoint(vertices, cache, ia, ib);
            const std::uint32_t mbc = midpoint(vertices, cache, ib, ic);
            const std::uint32_t mca = midpoint(vertices, cache, ic, ia);
            new_indices.insert(new_indices.end(), {ia, mab, mca, mab, ib, mbc, mca, mbc, ic, mab, mbc, mca});
        }
        indices.swap(new_indices);
        ++levels;
    }
    return levels;
}

} // namespace destruct
} // namespace toy

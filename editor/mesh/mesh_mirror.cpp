#include "editor/mesh/mesh_mirror.h"

namespace toy {
namespace editor {

glm::mat4 axis_reflection(int a) {
    glm::mat4 r(1.0f);
    r[a][a] = -1.0f;
    return r;
}

std::vector<MirrorImage> mirror_images(const MirrorSettings& s, const glm::mat4& world) {
    std::vector<MirrorImage> out;
    const glm::mat4 inv = glm::inverse(world);
    for (int mask = 1; mask < 8; ++mask) {
        bool ok = true;
        glm::mat4 r(1.0f);
        for (int a = 0; a < 3; ++a) {
            if (!((mask >> a) & 1)) continue;
            ok &= s.axis[a];
            r = axis_reflection(a) * r;
        }
        if (!ok) continue;
        out.push_back({s.global ? inv * r * world : r, mask});
    }
    return out;
}

float mirror_tolerance(const EditMesh& m) {
    if (m.positions.empty()) return 1e-5f;
    glm::vec3 lo = m.positions[0], hi = lo;
    for (const auto& p : m.positions) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    return std::max(1e-5f, glm::length(hi - lo) * 1e-4f);
}

MirrorMap build_mirror_map(const EditMesh& m, const std::set<uint32_t>& moved, const MirrorSettings& s,
                                  const glm::mat4& world) {
    MirrorMap out;
    if (!s.any() || moved.empty()) return out;
    const float tol = mirror_tolerance(m);
    const float cell = tol * 4.0f;
    auto key = [&](const glm::ivec3& c) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(c.x) & 0x1FFFFF) << 42) |
               (static_cast<uint64_t>(static_cast<uint32_t>(c.y) & 0x1FFFFF) << 21) |
               (static_cast<uint64_t>(static_cast<uint32_t>(c.z) & 0x1FFFFF));
    };
    auto cell_of = [&](const glm::vec3& p) { return glm::ivec3(glm::floor(p / cell)); };
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    for (uint32_t v = 0; v < m.positions.size(); ++v) grid[key(cell_of(m.positions[v]))].push_back(v);
    auto nearest = [&](const glm::vec3& q) -> int64_t {
        const glm::ivec3 c = cell_of(q);
        int64_t best = -1;
        float best_d = tol;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    auto it = grid.find(key(c + glm::ivec3(dx, dy, dz)));
                    if (it == grid.end()) continue;
                    for (uint32_t u : it->second) {
                        const float d = glm::length(m.positions[u] - q);
                        if (d <= best_d) { best_d = d; best = u; }
                    }
                }
        return best;
    };
    std::set<uint32_t> claimed;
    for (const MirrorImage& img : mirror_images(s, world)) {
        const bool single = img.mask == 1 || img.mask == 2 || img.mask == 4;
        for (uint32_t v : moved) {
            const glm::vec3 q = glm::vec3(img.local * glm::vec4(m.positions[v], 1.0f));
            const int64_t u = nearest(q);
            if (u < 0) continue;
            if (static_cast<uint32_t>(u) == v) {
                if (single) out.on_plane.push_back({v, img.local});
                continue;
            }
            if (moved.count(static_cast<uint32_t>(u)) || !claimed.insert(static_cast<uint32_t>(u)).second) continue;
            out.pairs.push_back({v, static_cast<uint32_t>(u), img.local});
        }
    }
    return out;
}

void apply_mirror_map(EditMesh& m, const MirrorMap& map) {
    for (const auto& o : map.on_plane) {
        glm::vec3& p = m.positions[o.v];
        p = 0.5f * (p + glm::vec3(o.m * glm::vec4(p, 1.0f)));   // midpoint with its reflection lies on the plane
    }
    for (const auto& pr : map.pairs) m.positions[pr.dst] = glm::vec3(pr.m * glm::vec4(m.positions[pr.src], 1.0f));
}

void mirror_selection(EditMesh& m, const MeshSelection& sel, const glm::mat4& reflect) {
    const std::set<uint32_t> verts = sel.affected_vertices(m);
    if (verts.empty()) return;
    for (uint32_t v : verts) m.positions[v] = glm::vec3(reflect * glm::vec4(m.positions[v], 1.0f));
    for (auto& f : m.faces) {
        bool all = true;
        for (const auto& c : f.corners) all &= verts.count(c.v) > 0;
        if (all) std::reverse(f.corners.begin(), f.corners.end());
    }
}

glm::mat4 mirror_plane_matrix(int a, bool global, const glm::vec3& pivot_local, const glm::mat4& world) {
    if (!global) {
        return glm::translate(glm::mat4(1.0f), pivot_local) * axis_reflection(a) * glm::translate(glm::mat4(1.0f), -pivot_local);
    }
    const glm::vec3 pw = glm::vec3(world * glm::vec4(pivot_local, 1.0f));
    const glm::mat4 rw = glm::translate(glm::mat4(1.0f), pw) * axis_reflection(a) * glm::translate(glm::mat4(1.0f), -pw);
    return glm::inverse(world) * rw * world;
}

} // namespace editor
} // namespace toy

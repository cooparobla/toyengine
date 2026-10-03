/**
 * @file mesh_mirror.h
 * @brief Mirroring for Edit and Sculpt Mode, as pure functions over an EditMesh (testable).
 *
 * Two features share this:
 *   - Symmetry (the X / Y / Z toggles in the Edit and Sculpt headers): an edit on one side is
 *     repeated on the other. Edit Mode moves each moved vertex's mirror partner to the
 *     mirrored position (and keeps vertices on the mirror plane on it); Sculpt Mode repeats
 *     every dab at its mirror images.
 *   - Mirror (Mesh > Mirror, Ctrl M): reflects the selection across a plane through the pivot.
 *
 * Both work in **Local** space (the mesh's own axes, through its origin, as Blender does) or
 * **Global** space (the world axes, through the world origin). Every mirror is expressed as a
 * mesh-local affine matrix, so a Global mirror is just W^-1 * R * W for the mesh's world W.
 */

#ifndef TOYEDITOR_MESH_MESH_MIRROR_H
#define TOYEDITOR_MESH_MESH_MIRROR_H

#include "edit_mesh.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <vector>

namespace toy::editor {

/** @brief Which axes edits are mirrored across, and in which space. */
struct MirrorSettings {
    bool axis[3] = {false, false, false};
    bool global = false;   ///< World axes through the world origin; else the mesh's local axes.
    bool any() const { return axis[0] || axis[1] || axis[2]; }
};

/** @brief The reflection across world/local plane `a` (the plane whose normal is axis a). */
inline glm::mat4 axis_reflection(int a) {
    glm::mat4 r(1.0f);
    r[a][a] = -1.0f;
    return r;
}

/** @brief One mirror image: the mesh-local matrix, and the axes it flips (bit mask). */
struct MirrorImage {
    glm::mat4 local{1.0f};
    int mask = 0;
};

/**
 * @brief Every mirror image the settings ask for (each non-empty combination of the enabled
 *        axes: X, Y, XY, Z, ...), as mesh-local matrices for a mesh at world matrix `world`.
 */
inline std::vector<MirrorImage> mirror_images(const MirrorSettings& s, const glm::mat4& world) {
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

/** @brief A matching tolerance for "the same position" on this mesh (scale-relative). */
inline float mirror_tolerance(const EditMesh& m) {
    if (m.positions.empty()) return 1e-5f;
    glm::vec3 lo = m.positions[0], hi = lo;
    for (const auto& p : m.positions) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    return std::max(1e-5f, glm::length(hi - lo) * 1e-4f);
}

/**
 * @brief Which vertex mirrors which, for the vertices a transform moves -- captured once at
 *        the start of the transform (positions before any movement).
 */
struct MirrorMap {
    struct Pair { uint32_t src, dst; glm::mat4 m; };
    std::vector<Pair> pairs;   ///< dst follows src through m.
    struct OnPlane { uint32_t v; glm::mat4 m; };
    std::vector<OnPlane> on_plane;   ///< Moved vertices on a single-axis mirror plane: kept on it.
    bool empty() const { return pairs.empty() && on_plane.empty(); }
};

/**
 * @brief Pairs every vertex in `moved` with its mirror partner (the vertex at its mirrored
 *        position, within a tolerance) for each mirror image. Partners that are themselves
 *        moved are left alone (they're transformed directly); unmatched vertices just don't
 *        mirror, as in Blender with asymmetric topology.
 */
inline MirrorMap build_mirror_map(const EditMesh& m, const std::set<uint32_t>& moved, const MirrorSettings& s,
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

/** @brief After the moved vertices have moved: keep plane vertices on their plane, move partners. */
inline void apply_mirror_map(EditMesh& m, const MirrorMap& map) {
    for (const auto& o : map.on_plane) {
        glm::vec3& p = m.positions[o.v];
        p = 0.5f * (p + glm::vec3(o.m * glm::vec4(p, 1.0f)));   // midpoint with its reflection lies on the plane
    }
    for (const auto& pr : map.pairs) m.positions[pr.dst] = glm::vec3(pr.m * glm::vec4(m.positions[pr.src], 1.0f));
}

/**
 * @brief Mesh > Mirror: reflects the selected vertices by the mesh-local matrix `reflect`.
 *        Faces that are wholly selected get their winding reversed so they keep facing out
 *        (a reflection turns a surface inside out).
 */
inline void mirror_selection(EditMesh& m, const MeshSelection& sel, const glm::mat4& reflect) {
    const std::set<uint32_t> verts = sel.affected_vertices(m);
    if (verts.empty()) return;
    for (uint32_t v : verts) m.positions[v] = glm::vec3(reflect * glm::vec4(m.positions[v], 1.0f));
    for (auto& f : m.faces) {
        bool all = true;
        for (const auto& c : f.corners) all &= verts.count(c.v) > 0;
        if (all) std::reverse(f.corners.begin(), f.corners.end());
    }
}

/**
 * @brief The mesh-local matrix that reflects across the plane through `pivot_local` whose
 *        normal is axis `a` -- of the world (`global`) or of the mesh itself.
 */
inline glm::mat4 mirror_plane_matrix(int a, bool global, const glm::vec3& pivot_local, const glm::mat4& world) {
    if (!global) {
        return glm::translate(glm::mat4(1.0f), pivot_local) * axis_reflection(a) * glm::translate(glm::mat4(1.0f), -pivot_local);
    }
    const glm::vec3 pw = glm::vec3(world * glm::vec4(pivot_local, 1.0f));
    const glm::mat4 rw = glm::translate(glm::mat4(1.0f), pw) * axis_reflection(a) * glm::translate(glm::mat4(1.0f), -pw);
    return glm::inverse(world) * rw * world;
}

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_MESH_MIRROR_H

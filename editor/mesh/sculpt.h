/**
 * @file sculpt.h
 * @brief Sculpt Mode's brushes, as pure functions over an EditMesh (no GPU, testable).
 *
 * A stroke is a series of dabs along the cursor path. Each dab finds the vertices within the
 * brush radius (a uniform grid over the vertices, cell = radius), weights them with a smooth
 * falloff, and moves them:
 *   Draw     along the area normal (Ctrl inverts: push in)
 *   Smooth   toward the average of their neighbours (Shift: a temporary Smooth)
 *   Inflate  along their own normals
 *   Flatten  onto the area plane
 *   Grab     with the cursor (sculpt_grab_*), the vertices captured at the stroke's start
 * All in mesh-local space; symmetry mirrors each dab across the enabled local axes.
 */

#ifndef TOYEDITOR_MESH_SCULPT_H
#define TOYEDITOR_MESH_SCULPT_H

#include "edit_mesh.h"
#include "mesh_bvh.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace toy::editor {

enum class SculptBrush { Draw = 0, Smooth, Inflate, Grab, Flatten };

inline const char* sculpt_brush_name(SculptBrush b) {
    switch (b) {
        case SculptBrush::Draw: return "Draw";
        case SculptBrush::Smooth: return "Smooth";
        case SculptBrush::Inflate: return "Inflate";
        case SculptBrush::Grab: return "Grab";
        case SculptBrush::Flatten: return "Flatten";
    }
    return "Draw";
}

struct SculptSettings {
    SculptBrush brush = SculptBrush::Draw;
    float radius_px = 50.0f;    ///< Brush radius on screen (F).
    float strength = 0.5f;      ///< 0..1 (Shift+F).
    float spacing = 0.1f;       ///< Dab spacing, a fraction of the radius.
    bool symmetry[3] = {true, false, false};   ///< Mirror across local X / Y / Z (Blender: X on).
};

/** @brief 1 at the centre, 0 at the rim, smooth at both (x = distance / radius). */
inline float sculpt_falloff(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return 1.0f - x * x * (3.0f - 2.0f * x);
}

/** @brief Adjacency, normals and a BVH for one topology; positions may move under it. */
struct SculptCache {
    std::vector<uint32_t> nb_off, nb;        ///< Vertex -> neighbour vertices (CSR).
    std::vector<uint32_t> vf_off, vf;        ///< Vertex -> faces (CSR).
    std::vector<glm::vec3> face_n, vert_n;   ///< Area-weighted (face) and normalized (vertex).
    TriangleBVH bvh;
    uint64_t topo_key = 0;

    /** @brief Hash of the face lists: equal keys mean only positions changed. */
    static uint64_t topology_key(const EditMesh& m) {
        uint64_t h = 1469598103934665603ull ^ m.positions.size();
        for (const auto& f : m.faces) {
            h = (h ^ (f.corners.size() * 2 + (f.smooth ? 1 : 0))) * 1099511628211ull;   // smooth changes the render split
            for (const auto& c : f.corners) h = (h ^ c.v) * 1099511628211ull;
        }
        return h;
    }

    void build(const EditMesh& m) {
        const size_t nv = m.positions.size();
        std::vector<std::vector<uint32_t>> adj(nv);
        for (const auto& f : m.faces) {
            const size_t k = f.corners.size();
            for (size_t i = 0; i < k; ++i) {
                const uint32_t a = f.corners[i].v, b = f.corners[(i + 1) % k].v;
                adj[a].push_back(b);
                adj[b].push_back(a);
            }
        }
        nb_off.assign(nv + 1, 0);
        nb.clear();
        for (size_t v = 0; v < nv; ++v) {
            auto& l = adj[v];
            std::sort(l.begin(), l.end());
            l.erase(std::unique(l.begin(), l.end()), l.end());
            nb.insert(nb.end(), l.begin(), l.end());
            nb_off[v + 1] = static_cast<uint32_t>(nb.size());
        }
        vf_off.assign(nv + 1, 0);
        for (const auto& f : m.faces) for (const auto& c : f.corners) ++vf_off[c.v + 1];
        for (size_t v = 0; v < nv; ++v) vf_off[v + 1] += vf_off[v];
        vf.assign(vf_off.back(), 0);
        std::vector<uint32_t> fill(vf_off.begin(), vf_off.end() - 1);
        for (uint32_t f = 0; f < m.faces.size(); ++f) for (const auto& c : m.faces[f].corners) vf[fill[c.v]++] = f;
        face_n.assign(m.faces.size(), glm::vec3(0.0f));
        for (uint32_t f = 0; f < m.faces.size(); ++f) face_n[f] = area_normal_(m, f);
        vert_n.assign(nv, glm::vec3(0, 0, 1));
        for (uint32_t v = 0; v < nv; ++v) vert_n[v] = vertex_normal_(v);
        bvh.build(m);
        topo_key = topology_key(m);
    }

    /** @brief Recomputes the normals around moved vertices (call after each dab). */
    void update_normals(const EditMesh& m, const std::vector<uint32_t>& moved) {
        std::vector<uint32_t> faces;
        for (uint32_t v : moved) for (uint32_t k = vf_off[v]; k < vf_off[v + 1]; ++k) faces.push_back(vf[k]);
        std::sort(faces.begin(), faces.end());
        faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
        std::vector<uint32_t> verts;
        for (uint32_t f : faces) {
            face_n[f] = area_normal_(m, f);
            for (const auto& c : m.faces[f].corners) verts.push_back(c.v);
        }
        std::sort(verts.begin(), verts.end());
        verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
        for (uint32_t v : verts) vert_n[v] = vertex_normal_(v);
    }

private:
    static glm::vec3 area_normal_(const EditMesh& m, uint32_t f) {
        glm::vec3 n(0.0f);
        const auto& c = m.faces[f].corners;
        for (size_t i = 0; i < c.size(); ++i) n += glm::cross(m.positions[c[i].v], m.positions[c[(i + 1) % c.size()].v]);
        return n * 0.5f;
    }
    glm::vec3 vertex_normal_(uint32_t v) const {
        glm::vec3 n(0.0f);
        for (uint32_t k = vf_off[v]; k < vf_off[v + 1]; ++k) n += face_n[vf[k]];
        const float l = glm::length(n);
        return l > 1e-12f ? n / l : glm::vec3(0, 0, 1);
    }
};

/** @brief A uniform hash grid over vertex positions, for radius queries. */
class VertexGrid {
public:
    void build(const EditMesh& m, float cell) {
        cell_ = std::max(cell, 1e-6f);
        cells_.clear();
        for (uint32_t v = 0; v < m.positions.size(); ++v) cells_[key_(m.positions[v])].push_back(v);
        moved_ = 0.0f;
    }
    float cell() const { return cell_; }
    /** @brief Note movement: the grid must be rebuilt once vertices may have left their cells. */
    void note_moved(float d) { moved_ = std::max(moved_, d); }
    bool stale() const { return moved_ > cell_ * 0.5f; }

    void query(const EditMesh& m, glm::vec3 c, float r, std::vector<uint32_t>& out) const {
        out.clear();
        const int reach = static_cast<int>(std::ceil(r / cell_));
        const glm::ivec3 base = cell_of_(c);
        const float r2 = r * r;
        for (int z = -reach; z <= reach; ++z)
            for (int y = -reach; y <= reach; ++y)
                for (int x = -reach; x <= reach; ++x) {
                    auto it = cells_.find(pack_(base + glm::ivec3(x, y, z)));
                    if (it == cells_.end()) continue;
                    for (uint32_t v : it->second) {
                        const glm::vec3 d = m.positions[v] - c;
                        if (glm::dot(d, d) <= r2) out.push_back(v);
                    }
                }
    }

private:
    glm::ivec3 cell_of_(glm::vec3 p) const { return glm::ivec3(glm::floor(p / cell_)); }
    static uint64_t pack_(glm::ivec3 c) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(c.x) & 0x1FFFFF) << 42) |
               (static_cast<uint64_t>(static_cast<uint32_t>(c.y) & 0x1FFFFF) << 21) | (static_cast<uint64_t>(static_cast<uint32_t>(c.z) & 0x1FFFFF));
    }
    uint64_t key_(glm::vec3 p) const { return pack_(cell_of_(p)); }
    float cell_ = 1.0f;
    float moved_ = 0.0f;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
};

/** @brief One brush application, in mesh-local space. */
struct SculptDab {
    glm::vec3 center{0.0f};
    glm::vec3 view_dir{0, 0, -1};   ///< Toward the surface (used by nothing yet but symmetry bookkeeping).
    float radius = 0.1f;
    float strength = 0.5f;
    bool invert = false;
    SculptBrush brush = SculptBrush::Draw;
};

/** @brief Calls fn for the dab and its mirror images across the enabled local axes. */
inline void for_each_symmetric(const SculptDab& d, const bool sym[3], const std::function<void(const SculptDab&)>& fn) {
    for (int mask = 0; mask < 8; ++mask) {
        bool ok = true;
        for (int a = 0; a < 3; ++a) if ((mask >> a) & 1) ok &= sym[a];
        if (!ok) continue;
        SculptDab m = d;
        for (int a = 0; a < 3; ++a) {
            if ((mask >> a) & 1) { m.center[a] = -m.center[a]; m.view_dir[a] = -m.view_dir[a]; }
        }
        // A dab on the mirror plane would hit the same vertices twice.
        if (mask != 0) {
            bool same = true;
            for (int a = 0; a < 3; ++a) if (((mask >> a) & 1) && std::abs(d.center[a]) > d.radius * 0.25f) same = false;
            if (same) continue;
        }
        fn(m);
    }
}

/**
 * @brief Applies one dab (not Grab). Appends the moved vertices to `moved` and returns the
 *        largest displacement (for the grid's staleness).
 */
inline float sculpt_dab(EditMesh& m, const SculptCache& cache, const VertexGrid& grid, const SculptDab& d,
                        std::vector<uint32_t>& moved) {
    std::vector<uint32_t> in;
    grid.query(m, d.center, d.radius, in);
    if (in.empty()) return 0.0f;
    std::vector<float> w(in.size());
    glm::vec3 area_n(0.0f), area_c(0.0f);
    float wsum = 0.0f;
    for (size_t i = 0; i < in.size(); ++i) {
        w[i] = sculpt_falloff(glm::length(m.positions[in[i]] - d.center) / d.radius) * std::clamp(d.strength, 0.0f, 1.0f);
        area_n += cache.vert_n[in[i]] * w[i];
        area_c += m.positions[in[i]] * w[i];
        wsum += w[i];
    }
    if (wsum <= 0.0f) return 0.0f;
    area_n = glm::length(area_n) > 1e-12f ? glm::normalize(area_n) : glm::vec3(0, 0, 1);
    area_c /= wsum;
    const float sign = d.invert ? -1.0f : 1.0f;
    const float push = 0.1f * d.radius;
    float maxd = 0.0f;
    std::vector<glm::vec3> next(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const uint32_t v = in[i];
        glm::vec3 p = m.positions[v];
        switch (d.brush) {
            case SculptBrush::Draw: p += area_n * (push * w[i] * sign); break;
            case SculptBrush::Inflate: p += cache.vert_n[v] * (push * w[i] * sign); break;
            case SculptBrush::Flatten: p -= area_n * (glm::dot(p - area_c, area_n) * w[i]); break;
            case SculptBrush::Smooth: {
                const uint32_t b = cache.nb_off[v], e = cache.nb_off[v + 1];
                if (e > b) {
                    glm::vec3 avg(0.0f);
                    for (uint32_t k = b; k < e; ++k) avg += m.positions[cache.nb[k]];
                    avg /= static_cast<float>(e - b);
                    p = glm::mix(p, avg, std::min(1.0f, w[i] * 2.0f));
                }
                break;
            }
            case SculptBrush::Grab: break;
        }
        next[i] = p;   // smoothing reads unmoved neighbours: order-independent
    }
    for (size_t i = 0; i < in.size(); ++i) {
        maxd = std::max(maxd, glm::length(next[i] - m.positions[in[i]]));
        m.positions[in[i]] = next[i];
        moved.push_back(in[i]);
    }
    return maxd;
}

/** @brief Grab: the vertices under the brush at the stroke's start, with their weights. */
struct SculptGrab {
    std::vector<uint32_t> verts;
    std::vector<float> weights;
    std::vector<glm::vec3> origin;
    glm::vec3 mirror{1.0f};   ///< Sign applied to the cursor delta (symmetry copies).
};

inline SculptGrab sculpt_grab_begin(const EditMesh& m, const VertexGrid& grid, glm::vec3 center, float radius, float strength,
                                    glm::vec3 mirror = glm::vec3(1.0f)) {
    SculptGrab g;
    g.mirror = mirror;
    grid.query(m, center, radius, g.verts);
    for (uint32_t v : g.verts) {
        g.weights.push_back(sculpt_falloff(glm::length(m.positions[v] - center) / radius) * std::clamp(strength * 2.0f, 0.0f, 1.0f));
        g.origin.push_back(m.positions[v]);
    }
    return g;
}

/** @brief Moves the grabbed vertices by `delta` (mesh-local), weighted. */
inline void sculpt_grab_apply(EditMesh& m, const SculptGrab& g, glm::vec3 delta, std::vector<uint32_t>& moved) {
    const glm::vec3 d = delta * g.mirror;
    for (size_t i = 0; i < g.verts.size(); ++i) {
        m.positions[g.verts[i]] = g.origin[i] + d * g.weights[i];
        moved.push_back(g.verts[i]);
    }
}

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_SCULPT_H

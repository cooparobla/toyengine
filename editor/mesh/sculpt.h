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
 * All in mesh-local space; symmetry repeats each dab at its mirror images across the enabled
 * axes -- the mesh's own (Local) or the world's (Global), see mesh_mirror.h.
 */

#ifndef TOYEDITOR_MESH_SCULPT_H
#define TOYEDITOR_MESH_SCULPT_H

#include "edit_mesh.h"
#include "mesh_bvh.h"
#include "mesh_mirror.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace toy::editor {

enum class SculptBrush { Draw = 0, Smooth, Inflate, Grab, Flatten };

const char* sculpt_brush_name(SculptBrush b);

struct SculptSettings {
    SculptBrush brush = SculptBrush::Draw;
    float radius_px = 50.0f;    ///< Brush radius on screen (F).
    float strength = 0.5f;      ///< 0..1 (Shift+F).
    float spacing = 0.1f;       ///< Dab spacing, a fraction of the radius.
    MirrorSettings symmetry{{true, false, false}, false};   ///< Mirror strokes across X / Y / Z (Blender: local X on).
};

/** @brief 1 at the centre, 0 at the rim, smooth at both (x = distance / radius). */
float sculpt_falloff(float x);

/** @brief Adjacency, normals and a BVH for one topology; positions may move under it. */
struct SculptCache {
    std::vector<uint32_t> nb_off, nb;        ///< Vertex -> neighbour vertices (CSR).
    std::vector<uint32_t> vf_off, vf;        ///< Vertex -> faces (CSR).
    std::vector<glm::vec3> face_n, vert_n;   ///< Area-weighted (face) and normalized (vertex).
    TriangleBVH bvh;
    uint64_t topo_key = 0;

    /** @brief Hash of the face lists: equal keys mean only positions changed. */
    static uint64_t topology_key(const EditMesh& m);

    void build(const EditMesh& m);

    /** @brief Recomputes the normals around moved vertices (call after each dab). */
    void update_normals(const EditMesh& m, const std::vector<uint32_t>& moved);

private:
    static glm::vec3 area_normal_(const EditMesh& m, uint32_t f);
    glm::vec3 vertex_normal_(uint32_t v) const;
};

/** @brief A uniform hash grid over vertex positions, for radius queries. */
class VertexGrid {
public:
    void build(const EditMesh& m, float cell);
    float cell() const { return cell_; }
    /** @brief Note movement: the grid must be rebuilt once vertices may have left their cells. */
    void note_moved(float d) { moved_ = std::max(moved_, d); }
    bool stale() const { return moved_ > cell_ * 0.5f; }

    void query(const EditMesh& m, glm::vec3 c, float r, std::vector<uint32_t>& out) const;

private:
    glm::ivec3 cell_of_(glm::vec3 p) const { return glm::ivec3(glm::floor(p / cell_)); }
    static uint64_t pack_(glm::ivec3 c);
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
    glm::mat3 mirror{1.0f};         ///< The mirror's linear part (identity for the dab itself); Grab maps its delta with it.
};

/**
 * @brief Calls fn for the dab, then for its mirror images (mesh-local matrices from
 *        mirror_images(); empty for no symmetry).
 */
void for_each_symmetric(const SculptDab& d, const std::vector<MirrorImage>& images,
                               const std::function<void(const SculptDab&)>& fn);

/** @brief Local-space mirror images for `sym` (convenience for a mesh at the origin). */
inline void for_each_symmetric(const SculptDab& d, const MirrorSettings& sym, const std::function<void(const SculptDab&)>& fn) {
    for_each_symmetric(d, mirror_images(sym, glm::mat4(1.0f)), fn);
}

/**
 * @brief Applies one dab (not Grab). Appends the moved vertices to `moved` and returns the
 *        largest displacement (for the grid's staleness).
 */
float sculpt_dab(EditMesh& m, const SculptCache& cache, const VertexGrid& grid, const SculptDab& d,
                        std::vector<uint32_t>& moved);

/** @brief Grab: the vertices under the brush at the stroke's start, with their weights. */
struct SculptGrab {
    std::vector<uint32_t> verts;
    std::vector<float> weights;
    std::vector<glm::vec3> origin;
    glm::mat3 mirror{1.0f};   ///< Maps the cursor delta for a symmetry copy (the mirror's linear part).
};

SculptGrab sculpt_grab_begin(const EditMesh& m, const VertexGrid& grid, glm::vec3 center, float radius, float strength,
                                    const glm::mat3& mirror = glm::mat3(1.0f));

/** @brief Moves the grabbed vertices by `delta` (mesh-local), weighted. */
void sculpt_grab_apply(EditMesh& m, const SculptGrab& g, glm::vec3 delta, std::vector<uint32_t>& moved);

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_SCULPT_H

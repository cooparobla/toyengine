/**
 * @file edit_mesh.h
 * @brief The editor's editable polygon mesh, and its round trip to the engine's mesh YAML.
 *
 * The engine's mesh files are Blender's per-corner export: `vertices`, `normals` and `uvs`
 * are parallel arrays indexed by `faces` (N-gons), with a vertex duplicated wherever two
 * corners differ in normal or UV. Gfxcoopa welds those duplicates at load. EditMesh keeps
 * the opposite split, the one modelling needs:
 *
 *   - `positions` are WELDED: one entry per topological vertex, so moving a vertex moves
 *     every face that uses it;
 *   - each face corner carries its own UV and colour; normals are not stored at all -- they
 *     are derived on export from the geometry (flat, or averaged across a face's `smooth`
 *     neighbours), so they can never go stale after an edit. A corner keeps the normal and
 *     tangent its file gave it, re-exported exactly as long as nothing around it changed (the
 *     editor still computes the normal it computed at import); a new corner, or one an edit
 *     moved, turned or reshaded, gets them derived afresh;
 *   - vertex-group weights (Blender's, the file's `weights:` -- one {group: weight} map per
 *     vertex) are per WELDED vertex, like `positions`. So are `joints` / `joint_weights`
 *     (SkinnedMeshSource's palette form), carried through untouched when present.
 *
 * Every other key of the file is carried through export unchanged (bones, lods, ...), except
 * the per-raw-vertex arrays, which are rebuilt (`k_regenerated_keys`).
 *
 * A face list rather than a half-edge structure: the file format is already N-gons with
 * per-corner attributes, the operations at authoring scale are cheap with a rebuilt edge
 * map, and non-manifold input (imported files) needs no special casing.
 */

#ifndef TOYEDITOR_MESH_EDIT_MESH_H
#define TOYEDITOR_MESH_EDIT_MESH_H

#include "../core/yaml_util.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toy::editor {

struct Corner {
    uint32_t  v = 0;
    glm::vec2 uv{0.0f};
    glm::vec4 color{1.0f};   ///< Vertex colour (linear RGBA); written only when EditMesh::has_colors.
    /// What the file authored for this corner -- its normal and tangent (zero = none: a corner an
    /// edit created) -- and `ref_normal`, the normal the EDITOR computed for it at import. While
    /// the editor still computes that same normal on export, nothing around the corner changed,
    /// so the authored normal and tangent are written back exactly (custom / smoothed normals
    /// and exporter tangents survive a save); otherwise both are derived afresh.
    glm::vec3 normal{0.0f};
    glm::vec4 tangent{0.0f};
    glm::vec3 ref_normal{0.0f};

    /** @brief Whether this corner's authored normal / tangent still apply, given its current computed normal. */
    bool authored_valid(const glm::vec3& computed) const;
};

/** @brief One vertex-group weight on a vertex. */
struct VertexWeight {
    uint32_t group = 0;      ///< Index into EditMesh::groups.
    float    weight = 0.0f;
    bool operator==(const VertexWeight& o) const { return group == o.group && weight == o.weight; }
};

struct Face {
    std::vector<Corner> corners;
    bool smooth = false;
    uint32_t slot = 0;   ///< Material slot (submesh) -- an index into EditMesh::slots.
};

using Edge = std::pair<uint32_t, uint32_t>;   ///< Always (min, max).
inline Edge make_edge(uint32_t a, uint32_t b) { return a < b ? Edge{a, b} : Edge{b, a}; }

struct EditMesh {
    std::vector<glm::vec3> positions;
    std::vector<Face> faces;
    /// Material slot names (`material_slots`); empty = one unnamed slot. Faces pick a slot
    /// with Face::slot; the engine draws each slot's faces as one part with its own material.
    std::vector<std::string> slots;
    /// The mesh has a colour attribute (Corner::color is written). Off for a mesh that never
    /// had one, so it is not given an all-white one.
    bool has_colors = false;
    /// Whether the file had `normals` / `tangents`. A file without them is written without them
    /// (the engine derives them on load, as it did from the original -- a collider has no use
    /// for normals); a mesh made in the editor has normals and leaves tangents to the engine.
    bool has_normals = true;
    bool has_tangents = false;
    /// Vertex groups (Blender's): names, and per vertex its sparse weights. `weights` is either
    /// empty (no vertex has any) or parallel to `positions` -- see sync_vertex_data().
    std::vector<std::string> groups;
    std::vector<std::vector<VertexWeight>> weights;
    /// SkinnedMeshSource's per-vertex palette form, carried through when the file has it
    /// (empty, or parallel to `positions`).
    std::vector<glm::ivec4> joints;
    std::vector<glm::vec4>  joint_weights;
    /// Keys carried through import -> export untouched (lods, cull_screen_size, bones, ...).
    Node passthrough = Node::mapping();

    bool operator==(const EditMesh& o) const;

    // --- per-vertex data ---

    /**
     * @brief Keeps the per-vertex arrays parallel to `positions` after an operation added
     *        vertices (new ones get no weights / an unused joint palette). Removals go through
     *        compact(), which remaps them. Cheap; call after any topology edit.
     */
    void sync_vertex_data();

    /**
     * @brief Adds a vertex at `p` whose per-vertex data (weights, skinning palette) is the
     *        weighted mix of `sources` (vertex, weight) -- what an operation calls for every
     *        vertex it creates, so vertex groups follow the geometry. Weights normalise.
     */
    uint32_t add_vertex_mix(const glm::vec3& p, const std::vector<std::pair<uint32_t, float>>& sources);
    /** @brief add_vertex_mix() with one source: a copy of `like`'s data at `p`. */
    uint32_t add_vertex_like(const glm::vec3& p, uint32_t like) { return add_vertex_mix(p, {{like, 1.0f}}); }
    /** @brief add_vertex_mix() between `a` and `b`, `t` of the way to `b`. */
    uint32_t add_vertex_lerp(const glm::vec3& p, uint32_t a, uint32_t b, float t) {
        return add_vertex_mix(p, {{a, 1.0f - t}, {b, t}});
    }

    /** @brief The colour of the corner of face `f` at vertex `v` (white if `f` lacks `v`). */
    glm::vec4 corner_color(uint32_t f, uint32_t v) const;

    /** @brief Vertex `v`'s weight in `group` (0 if it has none). */
    float weight(uint32_t v, uint32_t group) const;

    /** @brief Sets vertex `v`'s weight in `group`; a weight of 0 removes the vertex from it. */
    void set_weight(uint32_t v, uint32_t group, float w);

    /** @brief Index of vertex group `name`, adding it if missing. */
    uint32_t group_index(const std::string& name);

    /** @brief Removes vertex group `g`, dropping its weights and renumbering the later groups. */
    void remove_group(uint32_t g);

    /** @brief The colour of vertex `v`: the mean of its corners' colours (white if none). */
    glm::vec4 vertex_color(uint32_t v) const;

    // --- geometry queries ---

    glm::vec3 face_normal(size_t f) const;

    glm::vec3 face_center(size_t f) const;

    /** @brief Every edge with the faces using it (indices into `faces`). */
    std::map<Edge, std::vector<uint32_t>> edge_faces() const;

    std::vector<Edge> edges() const;

    /** @brief Faces using each vertex. */
    std::vector<std::vector<uint32_t>> vertex_faces() const;

    size_t triangle_count() const;

    void bounds(glm::vec3& lo, glm::vec3& hi) const;

    /** @brief Drops vertices no face uses, remapping indices (and the per-vertex arrays). */
    void compact();

    /** @brief Removes consecutive duplicate corners and faces left with fewer than 3. */
    void cleanup_faces();

    /** @brief Per-face-corner export normals (flat, or smooth-averaged). */
    std::vector<std::vector<glm::vec3>> corner_normals() const;
};

// =====================================================================================
// YAML round trip
// =====================================================================================

/**
 * @brief Drops a face's consecutive corners that welded onto the same vertex (a UV sphere's pole
 *        quads), keeping, of each duplicate pair, the corner the engine actually drew: of the
 *        possible choices, the one whose fan triangulation's non-degenerate triangles (vertex +
 *        UV + colour + authored data) are exactly the original face's. Ties and faces with more
 *        than a few duplicates fall back to keeping the first.
 */
void resolve_welded_duplicates(const EditMesh& m, Face& f);

/// Keys mesh_to_node() writes itself from the EditMesh (everything else is passthrough).
inline const std::vector<std::string> k_regenerated_keys = {
    "vertices", "normals", "uvs", "tangents", "colors", "weights", "joints", "joint_weights",
    "faces", "material_slots", "face_materials"};

/**
 * @brief Builds an EditMesh from a mesh document. Corner positions equal within `weld_eps`
 *        become one vertex; a face whose stored normals differ from its flat normal is
 *        marked smooth.
 */
EditMesh mesh_from_node(const Node& node, float weld_eps = 1e-5f);

/**
 * @brief Export tangents (xyz + handedness w), one per exported vertex (`corner_ids` maps each
 *        face corner to one). A corner's authored tangent (Corner::tangent, from the file) is
 *        kept, exactly, where `keep_authored` says the corner is untouched (Corner::authored_valid)
 *        -- the editor does not rewrite data an edit did not touch. Otherwise it is computed from the
 *        UVs: each triangle's UV-derived tangent and bitangent summed over the corners sharing
 *        the exported vertex (they share position, normal and UV, so tangents stay split at
 *        hard edges and seams), Gram-Schmidt'd against the normal, with the handedness sign.
 */
std::vector<glm::vec4> export_tangents(const EditMesh& m, const std::vector<std::vector<int64_t>>& corner_ids,
                                              const std::vector<glm::vec3>& normals, size_t count,
                                              const std::vector<std::vector<char>>& keep_authored);

/** @brief The engine mesh document for `m` (per-corner arrays, N-gon faces). */
Node mesh_to_node(const EditMesh& m);

} // namespace toy::editor

#endif // TOYEDITOR_MESH_EDIT_MESH_H

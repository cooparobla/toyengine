/**
 * @file paint.h
 * @brief Blender-style Vertex Paint and Weight Paint on an EditMesh: brush settings and the
 *        per-dab operations. Pure CPU (no GPU, no UI), so every brush is unit-testable.
 *
 * Both modes paint VERTICES, as Blender's point-domain colour attributes and vertex groups do:
 * a dab finds the vertices within its radius (SculptCache / VertexGrid, shared with Sculpt
 * Mode), weights each by the brush falloff times strength, and changes that vertex's value.
 *  - Vertex Paint writes the colour of every corner of the vertex (EditMesh stores colour per
 *    corner, as the file does, so an imported per-corner split survives until painted over).
 *  - Weight Paint writes the vertex's weight in the active vertex group.
 *
 * Tools: Draw blends the brush value in (Mix / Add / Subtract / Multiply / Lighten / Darken
 * for colour; Mix / Add / Subtract for weight); Blur pulls each vertex toward the mean of its
 * neighbours; Average toward the mean under the whole brush. "Front faces only" skips vertices
 * facing away from the view, so a dab does not bleed through a thin wall.
 */

#ifndef TOYEDITOR_MESH_PAINT_H
#define TOYEDITOR_MESH_PAINT_H

#include "edit_mesh.h"
#include "sculpt.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace toy::editor {

enum class PaintTool { Draw = 0, Blur, Average };
enum class PaintBlend { Mix = 0, Add, Subtract, Multiply, Lighten, Darken };

const char* paint_tool_name(PaintTool t);

const char* paint_blend_name(PaintBlend b);

/** @brief One paint mode's brush (Vertex Paint and Weight Paint each keep their own). */
struct PaintSettings {
    PaintTool tool = PaintTool::Draw;
    PaintBlend blend = PaintBlend::Mix;
    float radius_px = 50.0f;                   ///< Brush radius on screen (F).
    float strength = 1.0f;                     ///< 0..1 (Shift+F).
    float spacing = 0.1f;                      ///< Dab spacing, a fraction of the radius.
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};   ///< Vertex Paint: the colour Draw lays down.
    glm::vec4 secondary{0.0f, 0.0f, 0.0f, 1.0f};   ///< Vertex Paint: Ctrl paints this one (X swaps).
    float weight = 1.0f;                       ///< Weight Paint: the weight Draw lays down.
    bool front_faces_only = true;
    bool auto_normalize = false;               ///< Weight Paint: keep each vertex's weights summing to 1.
    MirrorSettings symmetry{{true, false, false}, false};
};

/** @brief One brush application, in mesh-local space. */
struct PaintDab {
    glm::vec3 center{0.0f};
    glm::vec3 view_dir{0, 0, -1};   ///< Into the surface; with front_faces_only, vertices facing along it are skipped.
    float radius = 0.1f;
    float strength = 1.0f;
    bool invert = false;            ///< Ctrl: the secondary colour, or subtract weight.
};

namespace paint_detail {

/** @brief Vertices under the dab with their falloff x strength factors (> 0). */
void gather(const EditMesh& m, const SculptCache& cache, const VertexGrid& grid, const PaintDab& d,
                   bool front_only, std::vector<uint32_t>& verts, std::vector<float>& f);

glm::vec4 blend(PaintBlend b, const glm::vec4& c, const glm::vec4& paint, float k);

/** @brief The mean colour of vertex `v`'s corners (through the cache's vertex -> face lists). */
glm::vec4 vertex_color(const EditMesh& m, const SculptCache& cache, uint32_t v);

/** @brief Sets every corner of vertex `v` to `col`. */
void set_vertex_color(EditMesh& m, const SculptCache& cache, uint32_t v, const glm::vec4& col);

}  // namespace paint_detail

/**
 * @brief One Vertex Paint dab. Turns the mesh's colour attribute on if it had none (every
 *        corner starts white). Appends the painted vertices to `changed`.
 */
void vertex_paint_dab(EditMesh& m, const SculptCache& cache, const VertexGrid& grid, const PaintSettings& s,
                             const PaintDab& d, std::vector<uint32_t>& changed);

/**
 * @brief Rescales vertex `v`'s other groups so all its weights sum to 1, keeping `locked`'s
 *        (Blender's Auto Normalize). A vertex in no other group keeps its weight as it is.
 */
void normalize_vertex_weights(EditMesh& m, uint32_t v, uint32_t locked);

/** @brief One Weight Paint dab on vertex group `group`. Appends the painted vertices to `changed`. */
void weight_paint_dab(EditMesh& m, const SculptCache& cache, const VertexGrid& grid, const PaintSettings& s,
                             uint32_t group, const PaintDab& d, std::vector<uint32_t>& changed);

/** @brief Sets every corner of the mesh (or of `only`, if not empty) to `col` (Vertex Paint's Fill). */
void fill_vertex_colors(EditMesh& m, const glm::vec4& col, const std::vector<uint32_t>& only = {});

/** @brief Inverts every corner's RGB (alpha kept). */
void invert_vertex_colors(EditMesh& m);

/** @brief Removes the colour attribute (the mesh is written without `colors`). */
void clear_vertex_colors(EditMesh& m);

/** @brief Gives every vertex (or every vertex of `only`) weight `w` in `group` (0 removes them). */
void assign_weight(EditMesh& m, uint32_t group, float w, const std::vector<uint32_t>& only = {});

/** @brief Normalizes every vertex's weights across all groups to sum to 1 (Normalize All). */
void normalize_all_weights(EditMesh& m);

/**
 * @brief The weight heatmap Weight Paint shows (Blender's): blue at 0 through green and
 *        yellow to red at 1. Vertices outside the group draw as the 0 colour too.
 */
glm::vec3 weight_heatmap(float w);

}  // namespace toy::editor

#endif  // TOYEDITOR_MESH_PAINT_H

/**
 * @file cloth_paint.h
 * @brief Cloth Paint Mode's brushes and whole-channel operations: pure functions over EditMesh's
 *        cloth channels (`cloth_max_distance` and friends -- the mesh YAML's `cloth:` key).
 *
 * The cloth channels are scalar fields in METRES (except anim_drive, 0..1), not normalized
 * vertex-group weights -- which is why these are separate functions rather than Weight Paint
 * with a different group: no normalization, no group list, per-channel value ranges. The brush
 * mechanics (VertexGrid gather, sculpt_falloff, front-face rejection, read-all-then-write
 * order-independence) mirror weight_paint_dab exactly.
 *
 * Painting any channel materialises `cloth_max_distance` too (zero-filled) when absent: it is
 * the presence flag for the whole `cloth:` block, engine-side and here.
 */

#ifndef TOYEDITOR_MESH_CLOTH_PAINT_H
#define TOYEDITOR_MESH_CLOTH_PAINT_H

#include "editor/mesh/edit_mesh.h"
#include "editor/mesh/paint.h"
#include "editor/mesh/sculpt.h"

#include <vector>

namespace toy::editor {

/** @brief The four paintable cloth channels, in EditMesh::cloth_channels() order. */
enum class ClothChannel { MaxDistance = 0, BackstopDistance, BackstopRadius, AnimDrive };

/** @brief Display / UI name of a channel. */
const char* cloth_channel_name(ClothChannel ch);

/** @brief The channel's hard value ceiling: 1 for AnimDrive (a gain), unbounded (a large
 *         sentinel) for the metre channels. The brush and fill clamp into [0, limit]. */
float cloth_channel_limit(ClothChannel ch);

/** @brief Channel `ch`'s array, created (zero-filled, parallel to positions) if absent --
 *         along with `cloth_max_distance`, the block's presence flag. */
std::vector<float>& cloth_channel_data(EditMesh& m, ClothChannel ch);

/** @brief Channel `ch`'s array, or null when the mesh has no cloth paint (const probe). */
const std::vector<float>* cloth_channel_data(const EditMesh& m, ClothChannel ch);

/** @brief Vertex `v`'s value in `ch` (0 when unpainted). */
float cloth_channel_value(const EditMesh& m, ClothChannel ch, uint32_t v);

/**
 * @brief One brush dab on channel `ch` -- weight_paint_dab's tools over a scalar field:
 *        Draw lays `value` down (Mix toward it; Add/Subtract step by it; Ctrl inverts),
 *        Average pulls toward the dab's mean, Blur toward each vertex's neighbours.
 *        Appends every touched vertex to `changed`.
 */
void cloth_paint_dab(EditMesh& m, const SculptCache& cache, const VertexGrid& grid,
                     const PaintSettings& s, ClothChannel ch, float value, const PaintDab& d,
                     std::vector<uint32_t>& changed);

/** @brief Sets channel `ch` to `value` on every vertex (or `only`). */
void fill_cloth_channel(EditMesh& m, ClothChannel ch, float value,
                        const std::vector<uint32_t>& only = {});

/** @brief Blurs channel `ch` toward each vertex's neighbours, `iterations` times. */
void smooth_cloth_channel(EditMesh& m, const SculptCache& cache, ClothChannel ch, float factor,
                          int iterations);

/** @brief Mirrors channel `ch` across the mesh's local `axis` (0=X, 1=Y, 2=Z): every vertex
 *         takes the value of the vertex at its mirrored position (kept where none exists). */
void mirror_cloth_channel(EditMesh& m, ClothChannel ch, int axis);

/**
 * @brief Writes a linear gradient into channel `ch` along the axis from `from` to `to` (mesh
 *        local): value `lo` at `from`'s plane, `hi` at `to`'s, clamped outside -- the quick way
 *        to author "pinned at the waistband, free at the hem".
 */
void gradient_cloth_channel(EditMesh& m, ClothChannel ch, const glm::vec3& from,
                            const glm::vec3& to, float lo, float hi);

/** @brief Removes every cloth channel (the mesh stops being cloth-paintable). */
void clear_cloth_channels(EditMesh& m);

} // namespace toy::editor

#endif // TOYEDITOR_MESH_CLOTH_PAINT_H

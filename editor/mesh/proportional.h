/**
 * @file proportional.h
 * @brief Proportional editing (Blender's O): a move, rotation or scale of the selection also
 *        drags the unselected vertices around it, each by a weight that falls off with its
 *        distance from the selection out to a radius.
 *
 * Distance is measured one of three ways (ProportionalSettings):
 *   - projected (default): in screen pixels from the nearest selected vertex -- what lies inside
 *     the radius circle drawn around the selection is what moves (Blender's "Projected from View");
 *   - 3D: straight-line world distance to the nearest selected vertex;
 *   - connected: along the mesh's edges (world lengths), so separate pieces never move together.
 * The radius is a world-space length either way; projected mode turns it into pixels at the
 * pivot's depth, which is the size of the circle drawn.
 *
 * The caller applies a transform to a weighted vertex with every component scaled by its weight
 * (translation * w, angle * w, scale 1 + (s - 1) * w) -- see EditorApp::apply_transform_().
 */

#ifndef TOYEDITOR_MESH_PROPORTIONAL_H
#define TOYEDITOR_MESH_PROPORTIONAL_H

#include "edit_mesh.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <utility>
#include <vector>

namespace toy::editor {

/** @brief Falloff curves, Blender's names and shapes. */
enum class Falloff { Smooth, Sphere, Root, InverseSquare, Sharp, Linear, Constant };
inline constexpr int kFalloffCount = 7;

const char* falloff_name(Falloff f);

struct ProportionalSettings {
    bool enabled = false;
    Falloff falloff = Falloff::Smooth;
    float radius = 1.0f;          ///< World units.
    bool projected = true;        ///< Measure in screen space (Projected from View).
    bool connected = false;       ///< Measure along edges (overrides projected).

    static constexpr float kMinRadius = 1e-4f;
    static constexpr float kMaxRadius = 1e4f;
    void set_radius(float r) { radius = std::clamp(r, kMinRadius, kMaxRadius); }
};

/**
 * @brief The weight at normalized distance `t` = distance / radius: 1 at the selection, 0 at
 *        and beyond the radius.
 */
float falloff_weight(Falloff f, float t);

/**
 * @brief Per-vertex weights: 1 for `selected`, the falloff for vertices within the radius, 0
 *        for the rest. Only vertices with a non-zero weight and not in `selected` are returned.
 * @param world    Mesh -> world (distances are world-space).
 * @param project  World -> screen pixels; used (and required) when settings.projected.
 * @param radius_px The radius in pixels (projected mode).
 */
std::vector<std::pair<uint32_t, float>> proportional_weights(
        const EditMesh& m, const std::set<uint32_t>& selected, const ProportionalSettings& s, const glm::mat4& world,
        const std::function<std::optional<glm::vec2>(const glm::vec3&)>& project = {}, float radius_px = 0.0f);

} // namespace toy::editor

#endif // TOYEDITOR_MESH_PROPORTIONAL_H

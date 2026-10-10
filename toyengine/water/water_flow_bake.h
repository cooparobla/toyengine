/**
 * @file water_flow_bake.h
 * @brief Load-time bake of a flowing water body's per-vertex surface current and turbulence --
 *        pure CPU, no GPU or physics dependency (obstacles arrive through a ray callback), so it
 *        is unit-testable on its own.
 *
 * The current is DERIVED from the mesh, not authored: water runs downhill. Per triangle, the
 * downhill direction is gravity projected onto the triangle's plane, and speed grows with the
 * slope (`min_speed + slope_gain * sqrt(sin(slope))`, all scaled by `speed`) -- so any river mesh
 * that descends visibly flows the right way, faster through rapids. Where a stretch is (nearly)
 * flat the downhill direction is noise, so the bake falls back to the direction in which the
 * mesh's UV `u` increases: a spline-generated river (u along its length, as
 * tools/gen_water_test_meshes.py writes) keeps flowing through pools and level reaches.
 *
 * Static obstacles (rocks, posts, piers -- anything with a near-vertical face) then bend the
 * field: a short ray along the current that hits something removes the into-obstacle component
 * (flow slides around it, slightly faster) and stirs turbulence; a longer ray against the current
 * finds the obstacle a vertex is in the wake of, slowing it and stirring more. A couple of Laplacian passes smooth the result over the
 * mesh's own adjacency so the field has no per-triangle facets.
 */

#ifndef TOYENGINE_WATER_WATER_FLOW_BAKE_H
#define TOYENGINE_WATER_WATER_FLOW_BAKE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace toy {
namespace water {

/** @brief Tunables for bake_flow(); mirror WaterBody's flow_* fields. */
struct FlowBakeParams {
    float speed           = 1.0f;   ///< Overall multiplier.
    float min_speed       = 0.6f;   ///< Speed on a level reach (m/s), before `speed`.
    float slope_gain      = 4.0f;   ///< Extra speed per sqrt(sin(slope)).
    float obstacle_radius = 1.5f;   ///< Look-ahead distance for obstacle deflection (m).
    float wake_length     = 4.0f;   ///< Look-behind distance for obstacle wakes (m).
    int   smooth_passes   = 2;
};

/** @brief A hit reported by the bake's obstacle ray callback. */
struct FlowRayHit {
    float     distance = 0.0f;
    glm::vec3 normal{0.0f};
};

/** @brief Casts a ray (origin, unit dir, max distance) against static geometry. */
using FlowRayFn = std::function<bool(const glm::vec3&, const glm::vec3&, float, FlowRayHit&)>;

/**
 * @brief Welds a per-corner triangle soup (positions + uvs) by quantized position. Returns the
 *        welded positions/uvs and rewrites `indices` to reference them. The first corner's UV
 *        wins at a seam -- only `u`'s gradient matters to the bake.
 */
void weld_by_position(const std::vector<glm::vec3>& positions, const std::vector<glm::vec2>& uvs,
                             const std::vector<uint32_t>& corner_indices,
                             std::vector<glm::vec3>& out_positions, std::vector<glm::vec2>& out_uvs,
                             std::vector<uint32_t>& out_indices, float quantum = 1e-4f);

/** @brief Direction (unit, in the triangle's plane) in which UV `u` increases; zero if degenerate. */
glm::vec3 uv_u_direction(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                float ua, float ub, float uc);

/**
 * @brief Bakes per-vertex surface current (`out_flow`, world m/s) and turbulence
 *        (`out_turbulence`, 0..1) for welded world-space `positions`/`uvs`/`indices`.
 *
 * @param ray Optional static-geometry ray cast for obstacle deflection/wakes; empty = none.
 */
void bake_flow(const std::vector<glm::vec3>& positions, const std::vector<glm::vec2>& uvs,
                      const std::vector<uint32_t>& indices, const FlowBakeParams& params,
                      const FlowRayFn& ray, std::vector<glm::vec3>& out_flow,
                      std::vector<float>& out_turbulence);

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_WATER_FLOW_BAKE_H

/**
 * @file view_math.h
 * @brief View-space depth, world-bounds and exponential-smoothing helpers.
 *
 * Header-only, dependency-free beyond glm, and exercised directly by toyengine_tests --
 * no Vulkan device is needed. Split out of toy_render_math.h, which still includes every
 * part, so an existing `#include <toyengine/render/toy_render_math.h>` keeps working.
 */

#ifndef TOYENGINE_RENDER_VIEW_MATH_H
#define TOYENGINE_RENDER_VIEW_MATH_H

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace toy {
namespace render {

/**
 * @brief View-space depth (metres along the camera's forward axis) of a world point.
 *
 * `view` follows this engine's usual convention -- glm::inverse(world_matrix), the
 * same one CameraComponent::get_view_matrix() and gfx's other view-space math use --
 * under which the camera looks down its own -Z, so a point in front of the eye has a
 * NEGATIVE view-space z. Negating gives a positive depth for anything in front,
 * matching gfx_linear_depth()'s convention (positive, growing from near to far) that
 * DofPass's dof_signed_coc() consumes as `view_depth_m`. A point behind the eye comes
 * back negative, which callers should treat as "no valid focus" rather than clamping
 * it positive (see resolve_dof_focus_()'s `depth > 0.0f` guard).
 *
 * @param view      Camera view matrix (world-to-view).
 * @param world_pos World-space point.
 * @return View-space depth in metres; negative if `world_pos` is behind the camera.
 */
inline float view_space_depth(const glm::mat4& view, const glm::vec3& world_pos) {
    return -(view * glm::vec4(world_pos, 1.0f)).z;
}

/**
 * @brief Half the extent of a transformed object-space AABB along the camera's forward axis.
 *
 * The standard "project an oriented box onto one axis" sum: for the model-view matrix's
 * third ROW (the one producing view-space z), the box's support along that axis is the sum
 * of the per-axis contributions taken in absolute value, which is exactly how far the box
 * reaches in front of and behind its own centre IN DEPTH. Taking `abs()` per term is also
 * what makes this independent of view_space_depth()'s sign convention -- a half-extent has
 * no direction, so the negation that turns view-space z into a positive depth is irrelevant.
 *
 * Written for DofPass::Params::focus_range: pairing this with the view depth of the SAME
 * box's centre gives the narrowest band that still holds a whole object sharp, which is the
 * one quantity the thin-lens CoC cannot supply (see resolve_dof_focus_()).
 *
 * @param view      Camera view matrix (world-to-view).
 * @param model     Object's world matrix.
 * @param local_min Object-space AABB minimum corner (e.g. Mesh::bounds_min()).
 * @param local_max Object-space AABB maximum corner (e.g. Mesh::bounds_max()).
 * @return Half the box's depth extent in metres; always >= 0.
 */
inline float view_depth_half_extent(const glm::mat4& view, const glm::mat4& model,
                                    const glm::vec3& local_min, const glm::vec3& local_max) {
    const glm::mat4 mv = view * model;
    const glm::vec3 he = (local_max - local_min) * 0.5f;
    return std::abs(mv[0].z * he.x) + std::abs(mv[1].z * he.y) + std::abs(mv[2].z * he.z);
}

/**
 * @brief World-space centre of an object-space AABB under a model matrix.
 *
 * The companion to view_depth_half_extent(): a mesh's transform ORIGIN is not generally its
 * centre (assets/meshes/cube_corner.yaml spans [0,1]^3, so its origin is a corner), and focusing on the origin biases the focal plane off the subject by half its
 * depth.
 *
 * @param model     Object's world matrix.
 * @param local_min Object-space AABB minimum corner.
 * @param local_max Object-space AABB maximum corner.
 * @return The box centre in world space.
 */
inline glm::vec3 world_bounds_center(const glm::mat4& model,
                                     const glm::vec3& local_min, const glm::vec3& local_max) {
    return glm::vec3(model * glm::vec4((local_min + local_max) * 0.5f, 1.0f));
}

/**
 * @brief One frame of framerate-independent exponential easing toward `target`.
 *
 * Same `1 - exp(-rate * dt)` form as CameraController::update_orbit_()'s target
 * smoothing, so a lens focus-rack (see dof_focus_smoothing) reads as consistent with
 * the rest of this engine's follow/smoothing knobs: two half-steps at `dt` land at the
 * same place as one step at `2*dt`, which a naive linear lerp does not guarantee.
 *
 * @param current Current smoothed value.
 * @param target  Value being eased toward.
 * @param rate    Smoothing rate, 1/sec; <= 0 snaps `current` straight to `target`.
 * @param dt      Frame delta time, seconds.
 * @return The eased value for this frame.
 */
inline float exp_smooth_toward(float current, float target, float rate, float dt) {
    if (rate <= 0.0f) return target;
    float alpha = 1.0f - std::exp(-rate * dt);
    return current + (target - current) * alpha;
}

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_VIEW_MATH_H

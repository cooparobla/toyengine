/**
 * @file toy_render_math.h
 * @brief Pure-CPU math for the toyengine render pipeline -- an umbrella over the four
 *        headers it was split into, so existing includes keep working.
 *
 * Prefer including the part you need: a translation unit that only wants `LetterboxRect`
 * should not parse 450 lines of shadow-frustum fitting. Everything here is header-only,
 * dependency-free beyond glm, and exercised directly by toyengine_tests -- no Vulkan device
 * is needed to validate any of it.
 *
 * | Header | Holds |
 * |---|---|
 * | render_types.h  | `LetterboxRect`, `RenderExtent` -- the two plain rects, no config dependency |
 * | render_extent.h | internal resolution, letterbox / fit / display rect, pixel-snap density |
 * | shadow_fit.h    | directional-shadow fitting: cascade splits, clipmap radii, atlas tiles |
 * | sdf_clip.h      | SDF screen-space clip and scissor rects |
 * | view_math.h     | view-space depth, world bounds, exponential smoothing |
 */

#ifndef TOYENGINE_RENDER_TOY_RENDER_MATH_H
#define TOYENGINE_RENDER_TOY_RENDER_MATH_H

#include <toyengine/render/render_types.h>
#include <toyengine/render/render_extent.h>
#include <toyengine/render/shadow_fit.h>
#include <toyengine/render/sdf_clip.h>
#include <toyengine/render/view_math.h>

#endif // TOYENGINE_RENDER_TOY_RENDER_MATH_H

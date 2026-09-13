/**
 * @file pixel_render_types.h
 * @brief GPU-layout structs shared across the pixel-art frame graph: the three
 *        push-constant blocks this engine appends to gfxcoopa's passes, and the
 *        per-frame SDF draw record.
 *
 * Plain data with no pipeline dependency, kept apart from pixel_render_pipeline.h so
 * the shader-facing layouts can be read (and their size budget checked) on their own.
 */

#ifndef TOYENGINE_RENDER_PIXEL_RENDER_TYPES_H
#define TOYENGINE_RENDER_PIXEL_RENDER_TYPES_H

#include <cstdint>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/components/sdf_renderer.h>
#include <gfxcoopa/engine/passes/transparent_pass.h>
#include <gfxcoopa/engine/passes/transparent_capture_pass.h>

#include <toyengine/render/pixel_math.h>

namespace toy {
namespace render {

/**
 * @struct PixelLightingPushConstants
 * @brief Matches pixel_lighting.frag's PixelParams push-constant block (24 bytes).
 *
 * This engine's own cel-shading tuning. gfxcoopa's DeferredLightingPass takes a trailing
 * push-constant range list, so declaring this block needs no fork of that pass.
 */
struct PixelLightingPushConstants {
    float light_bands       = 4.0f;
    float spec_threshold    = 0.55f;
    float rim_strength      = 0.0f;
    float ambient_intensity = 1.0f;
    float sky_intensity     = 1.0f;
    /// > 0.5 selects smooth Cook-Torrance direct lighting; otherwise the banded look
    /// (light_bands-quantized diffuse, spec_threshold-masked specular).
    float soft_lighting     = 0.0f;
};

/**
 * @struct TransparentRefractionPushConstants
 * @brief Matches transparent.frag's push-constant block over [32, 64) -- appended after
 *        TransparentPass::PushConstants' own 32-byte material block (see that pass's
 *        extra_pc_bytes ctor parameter).
 *
 * Pushed once per BLEND **mesh** in record_transparent_(), right after TransparentPass's
 * own [0, 32) push. SDF glass does not refract and never gets this block. The frame-level
 * lighting/indirect/SSR tuning lives in a UBO instead (see forward_globals.h) -- carrying
 * it here as well would put the combined block over Vulkan's guaranteed 128-byte minimum.
 */
struct TransparentRefractionPushConstants {
    glm::vec4 tint_thickness = {1.0f, 1.0f, 1.0f, 0.25f};  ///< rgb = refraction_tint, w = thickness.
    glm::vec4 ior_flags      = {1.45f, 0.0f, 0.0f, 0.0f};  ///< x = ior, y = refraction on/off, zw reserved.
};
// Checked against the COMBINED range TransparentPass's pipeline layout declares -- that
// struct's own static_assert cannot see what a caller appends on top.
static_assert(sizeof(coopa::gfx::engine::passes::TransparentPass::PushConstants) +
             sizeof(TransparentRefractionPushConstants) <= 128,
             "TransparentPass's combined push-constant range exceeds Vulkan's guaranteed "
             "maxPushConstantsSize (128 bytes).");

/**
 * @struct TransparentCaptureLightingPushConstants
 * @brief Matches transparent_capture.frag's push-constant block over [32, 56) -- appended
 *        after TransparentCapturePass::PushConstants' own 32-byte material block.
 *
 * Smaller than ForwardGlobals on purpose: the capture never traces its own SSR, so none of
 * that struct's ssr_enabled/ssgi/GfxSsrParams/refraction fields apply here.
 */
struct TransparentCaptureLightingPushConstants {
    float light_bands       = 4.0f;
    float spec_threshold    = 0.55f;
    float soft_lighting     = 0.0f;
    float rim_strength      = 0.0f;
    float ambient_intensity = 1.0f;
    float sky_intensity     = 1.0f;
};
// See TransparentRefractionPushConstants' identical assertion above.
static_assert(sizeof(coopa::gfx::engine::passes::TransparentCapturePass::PushConstants) +
             sizeof(TransparentCaptureLightingPushConstants) <= 128,
             "TransparentCapturePass's combined push-constant range exceeds Vulkan's "
             "guaranteed maxPushConstantsSize (128 bytes).");

/**
 * @struct SdfDrawItem
 * @brief One SdfRenderer gathered this frame: its index into the SdfData SSBO, which draw
 *        list it belongs to, and the world/screen bounds every recording site needs.
 *
 * Built once per frame by the SDF gather and threaded through record_gbuffer_(),
 * record_directional_shadow_(), record_point_shadow_(), record_transparent_capture_() and
 * record_transparent_() -- the role `renderers`/`world_matrices`/`instance_idx` play for
 * MeshRenderer.
 */
struct SdfDrawItem {
    coopa::gfx::engine::components::SdfRenderer* comp = nullptr;
    uint32_t  gpu_index    = 0;     ///< Index into this frame's SdfData renderer SSBO.
    bool      is_blend     = false; ///< True -> forward/capture path; false -> G-buffer path.
    bool      cast_shadows = true;  ///< Already folds in PixelRenderConfig::sdf_shadows_enabled.
    glm::vec3 world_min{0.0f};
    glm::vec3 world_max{0.0f};
    glm::vec3 world_center{0.0f};   ///< For the shadow AABB fit and the back-to-front sort.
    PixelRect px_rect{};            ///< Scissor rect in low-res render-target pixel space.
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PIXEL_RENDER_TYPES_H

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
#include <vector>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/components/sdf_renderer.h>
#include <gfxcoopa/engine/passes/transparent_pass.h>
#include <gfxcoopa/engine/passes/transparent_capture_pass.h>

#include <toyengine/render/pixel_math.h>

namespace toy {
namespace render {

/**
 * @struct WaterFrameState
 * @brief What the renderer needs from the water system each frame, handed over by Engine
 *        (PixelRenderPipeline::set_water_state()) so render/ never depends on toyengine/water/.
 */
struct WaterFrameState {
    /// An expanding ring (see toy::water::WaterSystem::Ripple), already aged.
    struct Ripple {
        glm::vec2 position{0.0f};
        float     age = 0.0f;
        float     strength = 0.0f;
        float     radius = 0.0f;
    };
    std::vector<Ripple> ripples;  ///< At most kMaxWaterRipples are drawn (newest kept).

    /// Water clock (s) the water shader animates waves at -- toy::water::WaterSystem's own, so
    /// the drawn surface and buoyancy's CPU queries share one wave phase. Negative (no water
    /// system this frame) falls back to the renderer's clock.
    float time = -1.0f;

    /// Shader detail (toy::water::WaterSettings): flow-ripple layers (1-2), the distance (m) past
    /// which ripples/foam noise give way to a rougher surface, and the ring draw range (m).
    int   ripple_layers = 2;
    float detail_distance = 150.0f;
    float ripple_range = 80.0f;

    /// UnderwaterPass: the camera is below `surface_level`. Pixels whose near-plane point is
    /// still below that level get the underwater look; the rest (a camera straddling the
    /// waterline) draw as normal.
    bool      underwater = false;
    float     surface_level = 0.0f;
    glm::vec3 fog_color{0.05f, 0.24f, 0.28f};
    float     visibility = 14.0f;                    ///< Metres until ~95% fogged.
    glm::vec3 absorption{0.35f, 0.09f, 0.06f};       ///< Per-metre extinction, r/g/b.
    float     caustics = 1.0f;
};

/**
 * @struct PixelLightingPushConstants
 * @brief Matches pixel_lighting.frag's PixelParams push-constant block (96 bytes).
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
    /// How much occlusion darkens direct lighting (Unity HDRP's Direct Lighting
    /// Strength): 0 = occlusion affects indirect only, 1 = full-strength on direct too.
    float ssao_direct_strength = 0.25f;
    /// Unreal's AO "Intensity": the shader reads mix(1, ssao, intensity). Occupies what was
    /// the padding slot that aligns the mat4 below to 16 bytes (offset 32).
    float ssao_intensity = 1.0f;
    /// inverse(proj * view) -- the lighting draw also writes the SKY at background pixels
    /// (formerly a separate SkyboxPass draw) and reconstructs their view ray from this,
    /// computed on the CPU exactly as SkyboxPass did so the sky is bit-identical.
    glm::mat4 sky_inv_view_proj = glm::mat4(1.0f);
};
static_assert(sizeof(PixelLightingPushConstants) == 96, "must match pixel_lighting.frag's PixelParams");

/**
 * @struct DebugViewPushConstants
 * @brief Matches debug_view.frag's DebugViewParams push-constant block (48 bytes).
 *
 * All plain scalars (int32_t + float), same as PixelLightingPushConstants above, so
 * this struct's layout matches the GLSL push_constant block byte-for-byte with no
 * manual padding: push constants pack scalars at their natural alignment rather than
 * the vec4-rounding uniform blocks require.
 */
struct DebugViewPushConstants {
    /// DebugView enum value (pixel_render_config.h) -- MUST match debug_view.frag's
    /// DBG_* constants member-for-member.
    int32_t channel               = 0;
    float   light_bands           = 4.0f;
    float   spec_threshold        = 0.55f;
    float   ambient_intensity     = 1.0f;
    float   sky_intensity         = 1.0f;
    float   soft_lighting         = 0.0f;
    float   ssao_direct_strength  = 0.25f;
    float   camera_near           = 0.1f;
    float   camera_far            = 1000.0f;
    float   camera_is_perspective = 1.0f;
    /// Editor shading modes (Solid / Material Preview): how much SSAO darkens them, 0..1.
    float   editor_ao             = 0.0f;
    /// Editor shading modes: surface opacity over the backdrop (X-Ray), 1 = opaque.
    float   editor_xray_alpha     = 1.0f;
};

/**
 * @struct TransparentRefractionPushConstants
 * @brief Matches transparent.frag's push-constant block over [64, 128) -- appended after
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
    /// PBRMaterial::shader_params_ext, verbatim -- transparent_fs.glsl's gfx_params_ext0/1 over
    /// [96, 128). Rides in this per-object block (rather than its own) since it is pushed at
    /// exactly the same point, per BLEND mesh; brings the combined range to the full 128 bytes.
    glm::vec4 shader_ext0    = {0.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 shader_ext1    = {0.0f, 0.0f, 0.0f, 0.0f};
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

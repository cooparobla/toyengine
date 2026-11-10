/**
 * @file ssr_pass.h
 * @brief Screen-Space Reflections (SSR) pass with Hi-Z raymarching and BRDF compositing.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_SSR_PASS_H
#define GFXCOOPA_ENGINE_PASSES_SSR_PASS_H

#include <volk/volk.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/targets/gbuffer_target.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/memory/image_upload.h>
#include <gfxcoopa/engine/passes/hiz_pass.h>
#include <gfxcoopa/engine/passes/extra_sets.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class SsrPass
 * @brief Screen-space reflections: Hi-Z raymarch, temporal resolve, optional
 *        bilateral blur, then a BRDF composite back into the HDR frame.
 *
 * Owns the trace/resolve/blur targets and one pipeline per stage. The resolve
 * target is a ping-pong pair: each frame resolves into one while reading the
 * other as last frame's history, so no copy is needed (every reader of the
 * resolved buffer holds one descriptor set per parity, selected in execute()).
 * The march can run at half resolution (see the constructor's half_res flag);
 * the composite always runs at full resolution.
 */
class SsrPass {
public:
    struct SsrPushConstants {
        glm::mat4 inv_proj;
        float max_distance;      // world-space ray length (ssr_max_distance)
        float bias_texels;       // normal bias, in full-res screen texels (ssr_bias_texels)
        float thickness_min;     // world-space floor for the thickness test (ssr_thickness)
        float thickness_scale;   // thickness as a fraction of |view z| (ssr_thickness_scale)
        float roughness_cutoff;
        int   max_iterations;
        int   max_hiz_mip;
        int   start_mip;         // Hi-Z mip the march starts at (ssr_start_mip)
        int   min_mip0_steps;    // self-reflection gate (ssr_min_mip0_steps)
        int   max_color_mip = 0; // top mip of the prefiltered scene-colour chain
        float jitter_strength = 0.0f; // 0 = deterministic mirror-ray trace (ssr_jitter)
        int   frame_index     = 0;    // decorrelates the jitter's noise frame to frame

        static constexpr int kFlagPrevFrameColor = 1;
        static constexpr int kFlagSkipBehind     = 2;
        // kFlag* bits: kFlagPrevFrameColor (u_scene_color holds the PREVIOUS frame's final
        // HDR, so hit colour is fetched where the hit surface was last frame, via G4) and
        // kFlagSkipBehind (see Params::skip_behind).
        int   flags           = 0;
        // Rays per pixel (GGX VNDF samples averaged); 1 = one stochastic ray (ssr_rays_per_pixel).
        int   rays_per_pixel  = 1;
        // Scales the GGX-lobe cone in the hit-colour mip selection (ssr_cone_prefilter):
        // 1 = prefilter the whole lobe from the colour chain, 0 = only the ray's own pixel
        // footprint, the lobe then integrated by the sampled rays alone.
        float cone_prefilter  = 1.0f;

        // > 0: ssr.frag skips the trace for a pixel whose best-case reflection weight (split-sum
        // specular scale x the pre-trace fades) is below this -- see Params::skip_threshold.
        // Brings the block to exactly 128 bytes, Vulkan's guaranteed push-constant minimum.
        float skip_threshold  = 0.0f;
    };
    static_assert(sizeof(SsrPushConstants) == 128, "ssr.frag's SsrPushConstants block must match this layout byte-for-byte");

    /// Matches ssr_blur.frag's push-constant block exactly.
    struct BlurPushConstants {
        float    radius;
        int32_t  flags;     // kBlurLight | kBlurZeroSkip
    };
    static_assert(sizeof(BlurPushConstants) == 8, "ssr_blur.frag's BlurPushConstants block must match this layout byte-for-byte");
    static constexpr int32_t kBlurLight    = 1;   ///< 3x3 footprint instead of 5x5.
    static constexpr int32_t kBlurZeroSkip = 2;   ///< Write 0 where every tap is 0 (exact; SSR only).
    static constexpr int32_t kBlurRoughnessAware = 4; ///< Blend by roughness, keep mirrors sharp (SSR only).

    struct ResolvePushConstants {
        // Current clip space -> previous frame's clip space. mat4 must be 16-byte aligned, so it
        // goes first; vec2 is flattened to two floats below, matching SsaoPass's own house rule.
        glm::mat4 reproject;        // offset 0
        float     resolution_x;     // offset 64
        float     resolution_y;     // offset 68
        // Accumulation cap for the chain being resolved -- the specular and diffuse chains share
        // one count buffer and clamp it to different depths. 0 makes the resolve a passthrough.
        float     max_accum;        // offset 72
        // Fallback fixed-rate history weight, used only where the shared count buffer is
        // unavailable (the 1x1 neutral texture).
        float     blend_factor;     // offset 76
        int       history_valid;    // offset 80
        float     gamma = 1.0f;     // offset 84 -- variance-clipping width, in std deviations
                                     // (ssr_temporal_gamma); see ssr_resolve.frag's own doc
        int       frozen = 0;       // offset 88 -- hold accepted history verbatim (still camera)
        int       use_velocity = 0; // offset 92 -- reproject the surface by the G-buffer velocity
        // offset 96 -- 0..1 share of the reflection-parallax ("virtual point") reprojection for
        // mirror-like surfaces; fades out with roughness in the shader. 0 for the SSGI chain.
        float     virtual_blend = 0.0f;
    };                              // 100 bytes
    static_assert(sizeof(ResolvePushConstants) == 100, "ssr_resolve.frag's PushConstants block must match this layout byte-for-byte");

    /// Union layout shared with the composite's GLSL push-constant block, so
    /// every consumer's ssr_composite.frag -- whatever else it does -- reads
    /// max_color_mip/sky_intensity/ssgi_intensity/ssgi_distance at the same
    /// offsets. A consumer that doesn't use a field (blendy has no SSGI
    /// bounce term) simply never reads it; the bytes are harmless.
    struct CompositePushConstants {
        glm::vec2 ssr_resolution;      // offset 0  -- resolution of u_ssr_map (trace res under half-res)
        glm::vec2 screen_resolution;   // offset 8  -- always full screen res
        int       half_res;            // offset 16
        int       max_color_mip  = 0;  // offset 20 -- top mip of the prefiltered scene-colour chain
        float     sky_intensity  = 1.0f; // offset 24 -- MUST match the lighting pass's sky_intensity,
                                          // or the env-specular subtraction below leaves a residue
        float     ssgi_intensity = 0.0f; // offset 28 -- 0 disables the diffuse-bounce term
        float     ssgi_distance  = 0.5f; // offset 32 -- world-space offset along N for the FALLBACK bounce tap
        float     ssgi_traced    = 0.0f; // offset 36 -- > 0.5: the composite reads the traced+resolved
                                          // SSGI buffer (u_ssgi_map) instead of the normal-offset mip tap
        // offset 40..47: explicit padding up to the next 16-byte boundary, so the vec4s below
        // land at the same offset a hand-written GLSL push_constant block would place them at
        // (std430 requires vec4 to start on a 16-byte boundary).
        float     _pad1 = 0.0f;
        float     _pad2 = 0.0f;
        // Sky colours -- MUST match the lighting pass's, same reasoning as sky_intensity above.
        glm::vec4 sky_zenith  = glm::vec4(0.05f, 0.18f, 0.55f, 0.0f);  // offset 48
        glm::vec4 sky_horizon = glm::vec4(0.25f, 0.35f, 0.45f, 0.0f);  // offset 64
        glm::vec4 sky_ground  = glm::vec4(0.05f, 0.045f, 0.04f, 0.0f); // offset 80
    };                                 // 96 bytes
    static_assert(sizeof(CompositePushConstants) == 96,
                 "ssr_composite.frag's PushConstants block must match this layout byte-for-byte");

    /// Per-frame parameters for execute(). Collapsed into a struct because the argument list
    /// kept growing phase over phase (scale-relative tuning, then reprojection, then the
    /// glossy/half-res knobs) -- past ~6 positional args of the same type, a call site is not
    /// self-documenting and is easy to mis-order.
    /// Sub-passes of execute(), in record order -- see set_stage_hook().
    enum class Stage { Trace, Resolve, Blur, SsgiTrace, SsgiResolve, SsgiBlur, Composite };

    /**
     * @brief Optional callback run after each sub-pass of execute(), between render passes
     *        (e.g. to write a GPU timestamp per stage). Unset = no calls.
     */
    void set_stage_hook(std::function<void(coopa::gfx::command::CommandBuffer&, Stage)> hook) {
        stage_hook_ = std::move(hook);
    }

    struct Params {
        glm::mat4 proj;
        int   max_iterations   = 64;
        float thickness_min    = 0.05f;
        float thickness_scale  = 0.01f;
        float max_distance     = 15.0f;
        float bias_texels      = 3.5f;
        float roughness_cutoff = 0.6f;
        int   max_hiz_mip      = 0;      // hiz_pass_->max_mip_level(), filled by the caller
        int   start_mip        = 0;
        int   min_mip0_steps   = 1;
        int   max_color_mip    = 0;      // scene_color_mip_pass_->max_mip_level(), filled by caller
        // Stochastic ray jitter -- see GfxSsrParams' own doc (gfx/ssr_trace_body.glsl) for why
        // this exists. 0 (the default) reproduces the pre-jitter single-ray trace exactly, so
        // every existing consumer of this pass is unaffected unless it opts in. frame_index is
        // meaningless at jitter_strength == 0.
        float jitter_strength  = 0.0f;
        int   frame_index      = 0;
        bool  temporal_enabled = true;
        // Accumulation depth of the specular chain's temporal resolve: each pixel averages this
        // many frames of the jittered trace (frame N blended at 1/N against the shared count
        // TemporalHistoryPass publishes) before the running mean becomes a fixed-rate blend.
        int   temporal_frames  = 32;
        // Same, for the traced-SSGI chain. Deeper by default: one cosine-hemisphere ray has far
        // higher variance than a near-mirror reflection ray.
        int   ssgi_temporal_frames = 48;
        // Fallback fixed-rate history weight, used only where the shared count buffer is
        // unavailable (see set_temporal_count_image() -- a consumer that never calls it).
        float temporal_blend   = 0.85f;
        // Variance-clipping gamma for the temporal resolve's history rejection (see
        // ResolvePushConstants' own doc) -- widens or tightens the accepted history band as a
        // multiple of the 3x3 neighbourhood's standard deviation.
        float temporal_gamma   = 1.0f;
        // True once the camera has been still long enough for the accumulated average to top up;
        // the resolve then holds accepted history verbatim, which is what makes a resting image
        // byte-static. Same signal SsaoPass::Params::frozen carries.
        bool  temporal_frozen  = false;
        // Reprojection: current clip space -> previous frame's clip space, composed in DOUBLE
        // precision by the caller (glm::dmat4(prev_view_proj) * glm::inverse(glm::dmat4(proj) *
        // glm::dmat4(view))) before truncating to float. World-scale magnitudes cancel inside the
        // double product; a float composition -- or reprojecting the RGBA16F G-buffer position --
        // drifts by whole pixels at scene scales of a few hundred units, making the
        // accumulated reflection slide and boil against the geometry in motion. Same scheme
        // SsaoPass::Params::reproject and TaaPass use. reproject_valid says whether it (and the
        // history buffer) exist yet -- false for the first two frames and right after a resize.
        glm::mat4 reproject       = glm::mat4(1.0f);
        bool      reproject_valid = false;
        // Indirect-specular/SSGI terms fed straight into CompositePushConstants -- see that
        // struct's doc. sky_intensity must match whatever the lighting pass used for the same
        // frame; ssgi_intensity 0 (the default) makes the diffuse-bounce term a no-op for
        // consumers whose composite shader doesn't implement it at all (e.g. blendy's).
        float sky_intensity    = 1.0f;
        float ssgi_intensity   = 0.0f;
        float ssgi_distance    = 0.5f;
        // Ray length for the traced-SSGI stage (see the ctor's ssgi_frag_spv doc);
        // meaningless when that stage wasn't built. Every other march knob
        // (iterations/bias/thickness/start_mip) is shared with the specular trace.
        float ssgi_max_distance = 8.0f;
        // Hi-Z iteration budget for the SSGI march. Its own knob rather than a share of
        // max_iterations above: the bounce ray is short and lands in a coarse cone mip, so
        // it converges in far fewer steps than a mirror reflection does.
        int   ssgi_max_iterations = 32;
        // World-space blur sigma for the traced-SSGI denoise, when both that stage and the
        // blur stage were built. Wider than ssr_blur_radius by default and deliberately so:
        // a diffuse bounce is low-frequency, so a wide kernel costs it no real detail while
        // removing variance a single hemisphere ray per pixel cannot avoid producing.
        float ssgi_blur_radius = 1.0f;
        // 3x3 footprint for the SSGI denoise instead of 5x5 (9 taps, a third of the reads).
        bool  ssgi_blur_light  = false;
        // Sky colours the env-specular subtraction must cancel exactly -- same
        // IndirectParams instance the lighting pass reads, same requirement as
        // sky_intensity above (see CompositePushConstants' doc).
        glm::vec3 sky_zenith   = glm::vec3(0.05f, 0.18f, 0.55f);
        glm::vec3 sky_horizon  = glm::vec3(0.25f, 0.35f, 0.45f);
        glm::vec3 sky_ground   = glm::vec3(0.05f, 0.045f, 0.04f);
        /// Skip the SSR trace where the reflection's best-case weight -- the composite's
        /// split-sum specular scale (Fresnel, roughness) times the direction/roughness/grazing
        /// fades, all known before marching -- is below this. 0 traces every pixel the
        /// roughness cutoff admits. Never applied to the SSGI trace.
        float skip_threshold   = 0.0f;
        /// The scene-colour chain the trace samples holds the PREVIOUS frame's final HDR
        /// (Unreal's PrevSceneColor) rather than this frame's lit opaques: hit colour is then
        /// fetched where the hit surface was last frame (G4 motion), and reflections include
        /// transparents, fog and earlier reflections. False on frames with no history yet.
        bool  prev_frame_color = false;
        /// Rays per pixel, GGX VNDF-sampled (see SsrPushConstants::rays_per_pixel).
        int   rays_per_pixel   = 1;
        /// See SsrPushConstants::cone_prefilter.
        float cone_prefilter   = 1.0f;
        /// At mip 0, a candidate the ray has passed BEHIND by more than the thickness steps to
        /// the cell exit and climbs a mip instead of crawling one texel per iteration.
        bool  skip_behind      = false;
        // Temporal resolve: reproject through the G-buffer velocity (moving objects keep their
        // history) and, for mirror-like surfaces, through the reflected image's virtual point
        // (reflections stay attached to what they reflect while the camera moves). Both need
        // the velocity image passed to update_descriptors().
        bool  resolve_use_velocity = true;
        // Spatial denoise of the specular chain scales with surface roughness (mirrors stay
        // sharp) instead of blurring every reflection alike.
        bool  ssr_blur_roughness_aware = true;
        float virtual_blend        = 1.0f;
        // World-space blur radius for the spatial SSR denoise (see the ctor's blur_frag_spv
        // doc) -- meaningless when the blur stage wasn't built (blur_frag_spv empty at
        // construction). Same role as SsaoPass::Params::radius plays for ssao_blur.frag.
        float ssr_blur_radius  = 0.5f;
        // 3x3 footprint for the SSR denoise instead of 5x5 (9 taps, a third of the reads).
        bool  ssr_blur_light   = false;
        // Which of the two count images set_temporal_count_image() bound holds THIS frame's
        // counts (TemporalHistoryPass::current_parity()). Ignored when that was never called.
        uint32_t count_parity  = 0;
        // Skip the SSR kernel where the resolved buffer is zero across the whole footprint
        // (output is 0 either way -- see ssr_blur.frag); a pure saving on rough/skipped pixels.
        bool  ssr_blur_zero_skip = false;
    };

    /**
     * @param blur_vert_spv Vertex shader for the optional spatial SSR denoise stage (reuses
     *                      the same fullscreen-triangle vertex shader as resolve/composite --
     *                      pass the same path as resolve_vert_spv). Leave both this and
     *                      blur_frag_spv empty (the default) to omit the blur stage
     *                      entirely; composite then reads the resolved target directly.
     * @param blur_frag_spv Fragment shader (ssr_blur.frag.spv) -- bilateral, edge-aware blur
     *                      of the temporally-resolved SSR buffer, same technique as
     *                      SsaoPass's own ssao_blur.frag (5x5 footprint weighted by
     *                      G-buffer normal/position similarity), addressing hit/miss noise at
     *                      reflection boundaries that temporal accumulation alone doesn't
     *                      fully resolve, especially under continuous camera motion.
     */
    SsrPass(coopa::gfx::core::Device& device,
            coopa::gfx::memory::Allocator& allocator,
            const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
            uint32_t width,
            uint32_t height,
            const std::string& ssr_vert_spv,
            const std::string& ssr_frag_spv,
            const std::string& comp_vert_spv,
            const std::string& comp_frag_spv,
            const std::string& resolve_vert_spv,
            const std::string& resolve_frag_spv,
            bool half_res = false,
            ExtraSets composite_extra = {},
            const std::string& blur_vert_spv = "",
            const std::string& blur_frag_spv = "",
            // Optional traced-SSGI stage (ssgi.frag): one cosine-hemisphere diffuse ray
            // per pixel against the same Hi-Z/scene-colour inputs as the specular trace,
            // temporally resolved through a second instance of the resolve pipeline, and
            // consumed by the composite's diffuse-bounce term in place of its single
            // normal-offset mip tap. Empty (the default) omits the stage entirely; the
            // composite's u_ssgi_map binding then points at the permanent 1x1 zero
            // fallback and pc.ssgi_traced stays 0.
            const std::string& ssgi_frag_spv = "",
            uint32_t ssgi_res_divisor = 1)
        : device_(device), allocator_(allocator), width_(width), height_(height), half_res_(half_res),
          blur_enabled_(!blur_vert_spv.empty() && !blur_frag_spv.empty()),
          ssgi_enabled_(!ssgi_frag_spv.empty()),
          composite_extra_(std::move(composite_extra))
    {
        composite_extra_.validate("SsrPass composite");

        // Trace resolution. The march, the temporal resolve, and the history buffer run here;
        // the composite always runs at full screen resolution and bilaterally upsamples. Floor
        // division (not +1 rounding) keeps the composite's "full = 2*half + 1" tap mapping exact
        // at even screen dimensions.
        trace_width_  = half_res_ ? std::max(1u, width_  / 2) : width_;
        trace_height_ = half_res_ ? std::max(1u, height_ / 2) : height_;
        ssgi_res_divisor_ = std::max(1u, ssgi_res_divisor);
        ssgi_width_   = std::max(1u, trace_width_  / ssgi_res_divisor_);
        ssgi_height_  = std::max(1u, trace_height_ / ssgi_res_divisor_);
        // 0. Nearest-filtered sampler for the in-march G-buffer point-lookups in ssr.frag
        // (self-hit rejection, backface test). Those sample at an arbitrary marched UV, not
        // a texel center, so a LINEAR sampler bilinearly blends world positions/normals across
        // silhouette edges into values that exist on no real surface -- a direct source of
        // edge speckle. The G-buffer is discrete per-pixel data; it should never be smoothed.
        nearest_sampler_ = std::make_unique<util::Sampler>(device, nearest_clamp_desc());

        // 1. Create Offscreen Targets. target_ and the resolved pair run at trace resolution
        // (full res, or half under ssr_half_res); composite_target_ always runs at full screen
        // resolution and bilaterally upsamples the resolved SSR buffer into it.
        target_ = std::make_unique<targets::OffscreenTarget>(
            device, allocator, trace_width_, trace_height_, coopa::gfx::Format::RGBA16_Sfloat, targets::kColorOnly
        );
        // The trace's second output: world-space distance to the hit (0 = miss), read by the
        // resolve's virtual-point reprojection and the roughness-aware blur. Only its image is
        // used; the trace renders into it and target_ together through trace_render_pass_.
        hit_target_ = std::make_unique<targets::OffscreenTarget>(
            device, allocator, trace_width_, trace_height_, coopa::gfx::Format::R16_Sfloat, targets::kColorOnly
        );
        create_trace_render_pass_();
        create_trace_framebuffer_();
        composite_target_ = std::make_unique<targets::OffscreenTarget>(
            device, allocator, width, height, coopa::gfx::Format::RGBA16_Sfloat, targets::kColorOnly
        );
        // Temporal resolve output: the raymarch result (target_) blended with history, read
        // by the blur/composite in place of the raw raymarch output. Two in ping-pong: parity
        // p is resolved into on one frame and read back as the history on the next.
        for (uint32_t i = 0; i < 2; ++i) {
            resolved_targets_[i] = std::make_unique<targets::OffscreenTarget>(
                device, allocator, trace_width_, trace_height_, coopa::gfx::Format::RGBA16_Sfloat, targets::kColorOnly
            );
        }

        // Spatial denoise output (see blur_frag_spv's doc) -- same shape/resolution as
        // the resolved pair, since it blurs that buffer, one pass later.
        if (blur_enabled_) {
            blurred_target_ = std::make_unique<targets::OffscreenTarget>(
                device, allocator, trace_width_, trace_height_, coopa::gfx::Format::RGBA16_Sfloat, targets::kColorOnly
            );
        }

        // Traced-SSGI stage (see the ctor's ssgi_frag_spv doc): its own trace and
        // resolved targets at the same trace resolution.
        if (ssgi_enabled_) {
            ssgi_target_ = std::make_unique<targets::OffscreenTarget>(
                device, allocator, ssgi_width_, ssgi_height_, coopa::gfx::Format::RGBA16_Sfloat, targets::kColorOnly
            );
            for (uint32_t i = 0; i < 2; ++i) {
                ssgi_resolved_targets_[i] = std::make_unique<targets::OffscreenTarget>(
                    device, allocator, ssgi_width_, ssgi_height_, coopa::gfx::Format::RGBA16_Sfloat, targets::kColorOnly
                );
            }
            // Spatial denoise for the bounce, when the blur stage exists. Not optional
            // polish: one cosine-hemisphere ray per pixel has far higher variance than
            // the specular trace's near-mirror ray, and the temporal resolve alone
            // leaves a residual that keeps the image visibly settling for several
            // frames after the camera stops. A diffuse bounce is low-frequency by
            // definition, so the bilateral blur costs it no real detail.
            if (blur_enabled_) {
                ssgi_blurred_target_ = std::make_unique<targets::OffscreenTarget>(
                    device, allocator, ssgi_width_, ssgi_height_, coopa::gfx::Format::RGBA16_Sfloat, targets::kColorOnly
                );
            }
        }

        history_initialized_ = false;

        // 2. Shaders
        ssr_vert_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, ssr_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        ssr_frag_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, ssr_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        comp_vert_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, comp_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        comp_frag_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, comp_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        resolve_vert_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, resolve_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        resolve_frag_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, resolve_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (blur_enabled_) {
            blur_vert_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, blur_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
            blur_frag_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, blur_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        }
        if (ssgi_enabled_) {
            ssgi_frag_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, ssgi_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        }

        // 3. Descriptor Set Layouts
        // G-Buffer layout (3 images: G0, G1, G2) for the SSR raymarch set. G_depth is not
        // bound here -- the Hi-Z pyramid supplies depth for the march.
        // G0/G1/G2 plus binding 3, the G-buffer's velocity attachment (G4): the trace fetches a
        // hit's colour where that surface was LAST frame when the colour chain holds the
        // previous frame (Params::prev_frame_color). Shared by the forward passes that trace
        // through gfx/ssr_trace_body.glsl (trace_gbuffer_layout()), so they get it too.
        gbuf3_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(1, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(2, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(3, coopa::gfx::ShaderStage::Fragment)
                .build(device));

        // Composite G-buffer layout: same G0-G2 plus an SSAO sampler (binding 3), so the
        // composite can attenuate the SSR/env delta by the same ao * ssao term the
        // lighting pass applies to indirect_specular (see toyengine's ssr_composite.frag).
        comp_gbuf_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(1, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(2, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(3, coopa::gfx::ShaderStage::Fragment)
                .build(device));

        // Single image sampler layouts
        hiz_layout_         = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder().combined_sampler(0, coopa::gfx::ShaderStage::Fragment).build(device));
        scene_color_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder().combined_sampler(0, coopa::gfx::ShaderStage::Fragment).build(device));
        raw_ssr_layout_     = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder().combined_sampler(0, coopa::gfx::ShaderStage::Fragment).build(device));


        // Blur pass layout: matches ssr_blur.frag's set 0 exactly -- 0 = the buffer being
        // blurred (the resolved target), 1/2 = G-buffer normal/position for the edge-aware weights.
        if (blur_enabled_) {
            blur_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
                coopa::gfx::pipeline::DescriptorLayoutBuilder()
                    .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                    .combined_sampler(1, coopa::gfx::ShaderStage::Fragment)
                    .combined_sampler(2, coopa::gfx::ShaderStage::Fragment)
                    .build(device));
        }

        // Composite scene-colour layout: binding 0 = raw full-res scene colour (the surface
        // being composited into), binding 1 = the prefiltered mip chain, for the SSGI diffuse
        // bounce sample. A consumer whose ssr_composite.frag doesn't implement SSGI (blendy)
        // simply never samples binding 1 -- Vulkan doesn't require every declared binding to be
        // read -- but the descriptor is still always written (see update_descriptors()), since
        // an unwritten descriptor in a bound set is undefined behaviour even if unsampled.
        // Binding 2: the traced-SSGI resolved buffer (or the permanent 1x1 zero
        // fallback when the stage is off) -- same always-written rule as binding 1.
        comp_scene_color_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(1, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(2, coopa::gfx::ShaderStage::Fragment)
                .build(device));

        // Resolve pass layout: 0 = current raymarch output, 1 = history, 2 = the rasterized scene
        // depth the reprojection reconstructs this pixel's clip position from, 3 = the shared
        // per-pixel accumulation count (see set_temporal_count_image(), which also explains the
        // permanent neutral fallback binding 3 starts out holding).
        //
        // Depth, NOT the G-buffer world position: G2 is RGBA16F, and at
        // world coordinates of a few hundred units its quantization alone is multiple pixels of
        // reprojection error. See ssr_resolve.frag's file doc.
        // 4 = the trace's per-pixel hit distance (hit_target_; the zero image for SSGI), 5 = the
        // G-buffer velocity, 6/7 = G-buffer normal / position-roughness -- the virtual-point
        // reprojection's inputs (see ssr_resolve.frag).
        resolve_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(1, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(2, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(3, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(4, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(5, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(6, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(7, coopa::gfx::ShaderStage::Fragment)
                .build(device));

        // 4. Descriptor Pool -- sizes derived from the layouts above rather than hand-computed
        // headroom. Every set that reads a resolved buffer exists once per ping-pong parity
        // (the resolve sets once per resolve parity AND per count-image parity: 2x2), so that
        // nothing is ever rebound per frame; execute() picks the instance.
        coopa::gfx::pipeline::DescriptorPoolBuilder pool_builder;
        pool_builder.add_sets(*gbuf3_layout_, 1).add_sets(*hiz_layout_, 1).add_sets(*scene_color_layout_, 1)
            .add_sets(*resolve_layout_, 4).add_sets(*comp_gbuf_layout_, 1).add_sets(*raw_ssr_layout_, 2)
            .add_sets(*comp_scene_color_layout_, 2);
        if (blur_enabled_) {
            pool_builder.add_sets(*blur_layout_, 2);
        }
        if (ssgi_enabled_) {
            // A second instance of the resolve layout, for the SSGI resolve chain.
            pool_builder.add_sets(*resolve_layout_, 4);
            if (blur_enabled_) {
                pool_builder.add_sets(*blur_layout_, 2);
            }
        }
        desc_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(pool_builder.build(device));

        // Allocate Descriptor Sets
        ssr_gbuf_set_    = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *gbuf3_layout_);
        hiz_set_         = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *hiz_layout_);
        scene_color_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *scene_color_layout_);
        for (uint32_t o = 0; o < 2; ++o) {
            for (uint32_t c = 0; c < 2; ++c) {
                resolve_sets_[o][c] = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *resolve_layout_);
            }
        }

        comp_gbuf3_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *comp_gbuf_layout_);
        for (uint32_t o = 0; o < 2; ++o) {
            comp_raw_ssr_sets_[o]     = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *raw_ssr_layout_);
            comp_scene_color_sets_[o] = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *comp_scene_color_layout_);
        }


        for (uint32_t o = 0; o < 2; ++o) {
            if (blur_enabled_) {
                blur_sets_[o] = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *blur_layout_);
            }
            if (ssgi_enabled_) {
                for (uint32_t c = 0; c < 2; ++c) {
                    ssgi_resolve_sets_[o][c] = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *resolve_layout_);
                }
                if (blur_enabled_) {
                    ssgi_blur_sets_[o] = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *blur_layout_);
                }
            }
        }

        // Permanent 1x1 all-zero image: the "no count buffer" sentinel for the resolves below
        // (and zero_view_typed() for consumers that bind a never-written SSR output).
        {
            coopa::gfx::command::CommandPool one_shot_pool(device, device.queue_family_indices().graphics.value(), true);

            uint16_t zero_half4[4] = {0, 0, 0, 0}; // IEEE-754 half-float zero bit pattern is all-zero bytes
            zero_rgba_ = coopa::gfx::memory::upload_image_2d(
                device, allocator, one_shot_pool, zero_half4, 1, 1, coopa::gfx::Format::RGBA16_Sfloat, 8);
        }
        neutral_sampler_ = std::make_unique<util::Sampler>(device, nearest_clamp_desc());
        // Resolve binding 3 (the shared accumulation count) starts on the same all-zero fallback:
        // a zero count is ssr_resolve.frag's "no count buffer" sentinel, which selects the
        // fixed-rate blend_factor path. A consumer that wants the converging average calls
        // set_temporal_count_image().
        for (uint32_t o = 0; o < 2; ++o) {
            for (uint32_t c = 0; c < 2; ++c) {
                resolve_sets_[o][c]->bind_image(3, zero_rgba_->view_typed(), *neutral_sampler_);
                if (ssgi_enabled_) ssgi_resolve_sets_[o][c]->bind_image(3, zero_rgba_->view_typed(), *neutral_sampler_);
            }
        }

        // 5. SSR Pipeline Creation
        coopa::gfx::pipeline::PipelineDesc common_desc;
        common_desc.vertex = coopa::gfx::VertexLayout::none();
        common_desc.raster.cull = coopa::gfx::CullMode::None;
        common_desc.depth.test  = false;
        common_desc.depth.write = false;

        coopa::gfx::pipeline::PipelineDesc ssr_desc = common_desc;
        ssr_desc.shaders = {ssr_vert_.get(), ssr_frag_.get()};
        ssr_desc.descriptor_layouts = {
            &camera_layout,
            gbuf3_layout_.get(),
            hiz_layout_.get(),
            scene_color_layout_.get()
        };
        ssr_desc.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(SsrPushConstants)}};
        ssr_desc.blend.color_attachment_count = 2;   // colour+confidence, hit distance
        ssr_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
            device, coopa::gfx::detail::RawRenderPass{trace_render_pass_}, ssr_desc);

        // 5b. Traced-SSGI pipeline: ssr.frag's own sets 0-3 and the same push-constant block, so execute() reuses the
        // SsrPushConstants it already built with only the distance field swapped.
        if (ssgi_enabled_) {
            coopa::gfx::pipeline::PipelineDesc ssgi_desc = common_desc;
            ssgi_desc.shaders = {ssr_vert_.get(), ssgi_frag_.get()};
            ssgi_desc.descriptor_layouts = {
                &camera_layout,
                gbuf3_layout_.get(),
                hiz_layout_.get(),
                scene_color_layout_.get()
            };
            ssgi_desc.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(SsrPushConstants)}};
            ssgi_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(device, ssgi_target_->render_pass_object(), ssgi_desc);
        }

        // 6. SSR Composite Pipeline Creation
        std::vector<const coopa::gfx::pipeline::DescriptorSetLayout*> comp_layouts = {
            &camera_layout,
            comp_gbuf_layout_.get(),
            raw_ssr_layout_.get(),
            comp_scene_color_layout_.get()
        };
        // Never hardcode this index at the bind site -- it shifts if this pass ever gains
        // another owned set ahead of the caller's extras.
        comp_first_extra_set_ = static_cast<uint32_t>(comp_layouts.size());
        comp_layouts.insert(comp_layouts.end(), composite_extra_.layouts.begin(), composite_extra_.layouts.end());

        coopa::gfx::pipeline::PipelineDesc comp_desc = common_desc;
        comp_desc.shaders = {comp_vert_.get(), comp_frag_.get()};
        comp_desc.descriptor_layouts = comp_layouts;
        comp_desc.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(CompositePushConstants)}};
        comp_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(device, composite_target_->render_pass_object(), comp_desc);

        // 7. Temporal Resolve Pipeline Creation
        coopa::gfx::pipeline::PipelineDesc resolve_desc = common_desc;
        resolve_desc.shaders = {resolve_vert_.get(), resolve_frag_.get()};
        resolve_desc.descriptor_layouts = {resolve_layout_.get(), &camera_layout};
        resolve_desc.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(ResolvePushConstants)}};
        // Both parities' targets share the format and attachment shape, so one pipeline against
        // either render pass is compatible with both (Vulkan render-pass compatibility).
        resolve_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(device, resolved_targets_[0]->render_pass_object(), resolve_desc);

        // 8. Blur Pipeline Creation (optional -- see blur_frag_spv's ctor doc)
        if (blur_enabled_) {
            coopa::gfx::pipeline::PipelineDesc blur_desc = common_desc;
            blur_desc.shaders = {blur_vert_.get(), blur_frag_.get()};
            blur_desc.descriptor_layouts = {blur_layout_.get()};
            blur_desc.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(BlurPushConstants)}};
            blur_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(device, blurred_target_->render_pass_object(), blur_desc);
        }
    }

    ~SsrPass() {
        if (trace_framebuffer_ != VK_NULL_HANDLE) vkDestroyFramebuffer(device_.handle(), trace_framebuffer_, nullptr);
        if (trace_render_pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_.handle(), trace_render_pass_, nullptr);
    }
    SsrPass(const SsrPass&) = delete;
    SsrPass& operator=(const SsrPass&) = delete;

    void recreate(uint32_t width, uint32_t height) {
        width_ = width;
        height_ = height;
        trace_width_  = half_res_ ? std::max(1u, width_  / 2) : width_;
        trace_height_ = half_res_ ? std::max(1u, height_ / 2) : height_;
        ssgi_width_   = std::max(1u, trace_width_  / ssgi_res_divisor_);
        ssgi_height_  = std::max(1u, trace_height_ / ssgi_res_divisor_);

        target_->recreate(trace_width_, trace_height_);
        hit_target_->recreate(trace_width_, trace_height_);
        create_trace_framebuffer_();
        composite_target_->recreate(width, height);
        for (uint32_t i = 0; i < 2; ++i) resolved_targets_[i]->recreate(trace_width_, trace_height_);
        if (blur_enabled_) {
            blurred_target_->recreate(trace_width_, trace_height_);
        }
        if (ssgi_enabled_) {
            ssgi_target_->recreate(ssgi_width_, ssgi_height_);
            for (uint32_t i = 0; i < 2; ++i) ssgi_resolved_targets_[i]->recreate(ssgi_width_, ssgi_height_);
            if (blur_enabled_) {
                ssgi_blurred_target_->recreate(ssgi_width_, ssgi_height_);
            }
        }

        // History no longer matches the new resolution -- the fresh pair starts from nothing.
        history_initialized_ = false;
        write_parity_   = 0;
        current_parity_ = 0;
    }

    void update_descriptors(const targets::GBufferTarget& gbuffer,
                            coopa::gfx::TextureView hiz_view,
                            const util::Sampler& hiz_sampler,
                            coopa::gfx::TextureView scene_color_mip_view,
                            const util::Sampler& scene_color_mip_sampler,
                            coopa::gfx::TextureView scene_color_view,
                            const util::Sampler& linear_sampler,
                            coopa::gfx::TextureView velocity_view = coopa::gfx::TextureView::null())
    {
        // SSR Raymarching descriptors. Nearest sampler: see constructor comment -- these are
        // read both at the texel-centered in_uv (where nearest == linear, no change) and at
        // arbitrary marched UVs during the hit tests (where nearest is required for correctness).
        ssr_gbuf_set_->bind_image(0, gbuffer.g0_view_typed(), *nearest_sampler_);
        ssr_gbuf_set_->bind_image(1, gbuffer.g1_view_typed(), *nearest_sampler_);
        ssr_gbuf_set_->bind_image(2, gbuffer.g2_view_typed(), *nearest_sampler_);
        // Binding 3: the G-buffer's velocity attachment (G4). Without one the shaders never read
        // it (Params::prev_frame_color stays false), but the binding still needs a valid image.
        const coopa::gfx::TextureView velocity = velocity_view != coopa::gfx::TextureView::null()
                                                     ? velocity_view : gbuffer.g4_view_typed();
        ssr_gbuf_set_->bind_image(3, velocity, *nearest_sampler_);

        hiz_set_->bind_image(0, hiz_view, hiz_sampler);
        // The march samples the PREFILTERED chain (cone footprint -> textureLod), while the
        // composite below samples the raw full-res scene colour it is compositing INTO. Same
        // descriptor layout, deliberately different images.
        scene_color_set_->bind_image(0, scene_color_mip_view, scene_color_mip_sampler);

        // Temporal resolve descriptors: current frame's raw raymarch output + last frame's
        // resolved history + the rasterized scene depth the reprojection reconstructs from. The
        // current buffer is still sampled at texel-centered in_uv, so nearest_sampler_ avoids
        // implying this HDR data buffer should ever be blurred. Resolve parity o renders into
        // resolved_targets_[o] and reads resolved_targets_[1-o] as its history; the count
        // parity c only differs at binding 3 (set_temporal_count_image()).
        for (uint32_t o = 0; o < 2; ++o) {
            for (uint32_t c = 0; c < 2; ++c) {
                auto& set = *resolve_sets_[o][c];
                set.bind_image(0, target_->color_view_typed(), *nearest_sampler_);
                // LINEAR, not nearest: reprojected UVs are no longer texel-centred, and
                // point-sampling them makes the accumulated reflection stair-step and crawl
                // under camera motion.
                set.bind_image(1, resolved_targets_[1 - o]->color_view_typed(), linear_sampler);
                // NEAREST is mandatory, not a preference: D32_SFLOAT is not guaranteed to
                // support linear filtering, and a blended depth would reproject to a point on
                // no real surface anyway.
                set.bind_image(2, gbuffer.depth_view_typed(), *nearest_sampler_);
                set.bind_image(4, hit_target_->color_view_typed(), *nearest_sampler_);
                set.bind_image(5, velocity, *nearest_sampler_);
                set.bind_image(6, gbuffer.g1_view_typed(), *nearest_sampler_);
                set.bind_image(7, gbuffer.g2_view_typed(), *nearest_sampler_);
            }
        }

        // SSR Composite descriptors
        comp_gbuf3_set_->bind_image(0, gbuffer.g0_view_typed(), linear_sampler);
        comp_gbuf3_set_->bind_image(1, gbuffer.g1_view_typed(), linear_sampler);
        comp_gbuf3_set_->bind_image(2, gbuffer.g2_view_typed(), linear_sampler);

        // Everything downstream of the resolve holds one set per resolve parity o (the
        // target the resolve wrote this frame).
        for (uint32_t o = 0; o < 2; ++o) {
            // Blur descriptors (optional -- see blur_frag_spv's ctor doc): the buffer being
            // blurred (this parity's temporally-resolved output, NEAREST -- same texel-exact
            // reasoning as every other in-shader G-buffer point-lookup in this engine) plus
            // the G-buffer normal/position the bilateral weights are computed from.
            if (blur_enabled_) {
                blur_sets_[o]->bind_image(0, resolved_targets_[o]->color_view_typed(), *nearest_sampler_);
                blur_sets_[o]->bind_image(1, gbuffer.g1_view_typed(), *nearest_sampler_);
                blur_sets_[o]->bind_image(2, gbuffer.g2_view_typed(), *nearest_sampler_);
            }

            // Composite reads the BLURRED buffer when the blur stage is enabled (execute()
            // draws resolved -> blurred_target_ every frame before composite runs), or the
            // temporally-resolved buffer directly otherwise -- exactly this pass's original
            // behaviour before the blur stage existed.
            comp_raw_ssr_sets_[o]->bind_image(0, blur_enabled_ ? blurred_target_->color_view_typed()
                                                               : resolved_targets_[o]->color_view_typed(),
                                              linear_sampler);
            comp_scene_color_sets_[o]->bind_image(0, scene_color_view, linear_sampler);
            // Same prefiltered mip chain the raymarch's scene_color_set_ (binding 0 above)
            // reads -- for the composite's SSGI diffuse-bounce sample, when the bound shader
            // implements one.
            comp_scene_color_sets_[o]->bind_image(1, scene_color_mip_view, scene_color_mip_sampler);
            // Traced-SSGI buffer the composite reads -- the denoised one when the blur stage
            // exists, else the temporally-resolved one, else (stage off) the permanent zero
            // fallback (an unwritten descriptor in a bound set is undefined behaviour even if
            // unsampled). Same three-way choice comp_raw_ssr_sets_ makes just above.
            coopa::gfx::TextureView ssgi_view = zero_rgba_->view_typed();
            if (ssgi_enabled_) {
                ssgi_view = blur_enabled_ ? ssgi_blurred_target_->color_view_typed()
                                          : ssgi_resolved_targets_[o]->color_view_typed();
            }
            comp_scene_color_sets_[o]->bind_image(2, ssgi_view, linear_sampler);

            // SSGI resolve/blur descriptors: mirror resolve_sets_/blur_sets_ above, over the
            // SSGI chain's own images.
            if (ssgi_enabled_) {
                for (uint32_t c = 0; c < 2; ++c) {
                    auto& set = *ssgi_resolve_sets_[o][c];
                    set.bind_image(0, ssgi_target_->color_view_typed(), *nearest_sampler_);
                    set.bind_image(1, ssgi_resolved_targets_[1 - o]->color_view_typed(), linear_sampler);
                    set.bind_image(2, gbuffer.depth_view_typed(), *nearest_sampler_);
                    // No hit distance for the diffuse bounce (virtual_blend is 0 there).
                    set.bind_image(4, zero_rgba_->view_typed(), *neutral_sampler_);
                    set.bind_image(5, velocity, *nearest_sampler_);
                    set.bind_image(6, gbuffer.g1_view_typed(), *nearest_sampler_);
                    set.bind_image(7, gbuffer.g2_view_typed(), *nearest_sampler_);
                }
                if (blur_enabled_) {
                    ssgi_blur_sets_[o]->bind_image(0, ssgi_resolved_targets_[o]->color_view_typed(), *nearest_sampler_);
                    ssgi_blur_sets_[o]->bind_image(1, gbuffer.g1_view_typed(), *nearest_sampler_);
                    ssgi_blur_sets_[o]->bind_image(2, gbuffer.g2_view_typed(), *nearest_sampler_);
                }
            }
        }
    }

    /// Binding 3 of the composite G-buffer set must be rebound every frame -- callers pass the
    /// SSAO pass's blurred output when enabled, or its permanent neutral (fully-unoccluded)
    /// texture when disabled/absent, mirroring DeferredLightingPass::set_ssao_image() so the two
    /// passes always attenuate indirect specular by the identical ao * ssao term.
    void set_ssao_image(VkImageView ssao_view, VkSampler ssao_sampler) {
        comp_gbuf3_set_->bind_image(3, ssao_view, ssao_sampler);
    }

    /// Points both resolve chains at the shared per-pixel accumulation count
    /// (TemporalHistoryPass's output), which is what turns their temporal blend from a fixed-rate
    /// exponential -- incapable of converging on a per-frame-rejittered trace -- into a running
    /// average over `Params::temporal_frames` draws.
    ///
    /// Optional: the constructor leaves binding 3 on a permanent
    /// all-zero 1x1 texture, and a zero count is ssr_resolve.frag's sentinel for "no count buffer
    /// here", which selects the Params::temporal_blend path instead. A consumer with no
    /// TemporalHistoryPass never needs to call this. Bound once at setup, like
    /// update_descriptors(): the count buffer is a ping-pong pair whose two images are stable,
    /// so both are bound here (one resolve set per count parity) and execute() selects with
    /// Params::count_parity -- the parity that pass wrote on the frame being recorded.
    void set_temporal_count_image(coopa::gfx::TextureView count_view0, coopa::gfx::TextureView count_view1,
                                  const util::Sampler& count_sampler) {
        for (uint32_t o = 0; o < 2; ++o) {
            resolve_sets_[o][0]->bind_image(3, count_view0, count_sampler);
            resolve_sets_[o][1]->bind_image(3, count_view1, count_sampler);
            if (ssgi_enabled_) {
                ssgi_resolve_sets_[o][0]->bind_image(3, count_view0, count_sampler);
                ssgi_resolve_sets_[o][1]->bind_image(3, count_view1, count_sampler);
            }
        }
    }

    /// @name Trace-input accessors
    /// Expose the raymarch's own three input sets (bound once by update_descriptors(), never
    /// per-frame) so a forward-shaded consumer that also wants to trace gfx/ssr_trace_body.glsl
    /// -- e.g. a transparent pass, whose geometry never appears in the G-buffer the way this
    /// pass's own ssr.frag draw does -- can bind the identical descriptors rather than
    /// duplicating the images. See gfx/ssr_trace_body.glsl's required-before-include contract
    /// for the exact sampler/UBO names each set must resolve to.
    /// @{
    const coopa::gfx::pipeline::DescriptorSetLayout& trace_gbuffer_layout() const { return *gbuf3_layout_; }
    const coopa::gfx::pipeline::DescriptorSetLayout& hiz_layout() const { return *hiz_layout_; }
    const coopa::gfx::pipeline::DescriptorSetLayout& scene_color_layout() const { return *scene_color_layout_; }
    const coopa::gfx::pipeline::DescriptorSet& trace_gbuffer_set() const { return *ssr_gbuf_set_; }
    const coopa::gfx::pipeline::DescriptorSet& hiz_set() const { return *hiz_set_; }
    const coopa::gfx::pipeline::DescriptorSet& scene_color_set() const { return *scene_color_set_; }
    /// @}

    void execute(coopa::gfx::command::CommandBuffer& cmd,
                 const coopa::gfx::pipeline::DescriptorSet& camera_set,
                 const Params& params)
    {
        // Resolve parity for this frame: the target the resolves write; the other one is
        // the history they read. Toggled only here, so a frame that skips this pass can't
        // put "history" out of step with "the previous execute's output".
        const uint32_t o  = write_parity_;
        const uint32_t cp = params.count_parity & 1;

        // On the very first frame (or right after a resize), this parity's history -- the
        // OTHER resolved target -- is UNDEFINED: transition it once before it's bound as a
        // sampled image in the resolve pass below. The resolve shader doesn't read it in that
        // case (history_valid = 0 in the push constant), but the descriptor binding still
        // needs a valid layout at draw time regardless of the runtime branch. The target
        // being written needs nothing: its render pass starts from UNDEFINED every frame.
        if (!history_initialized_) {
            VkImageMemoryBarrier barriers[2]{};
            uint32_t barrier_count = ssgi_enabled_ ? 2u : 1u;
            for (uint32_t i = 0; i < barrier_count; ++i) {
                barriers[i].sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barriers[i].oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
                barriers[i].newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barriers[i].srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
                barriers[i].dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
                barriers[i].image                           = (i == 0) ? resolved_targets_[1 - o]->color_image_object()->handle()
                                                                       : ssgi_resolved_targets_[1 - o]->color_image_object()->handle();
                barriers[i].subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                barriers[i].subresourceRange.baseMipLevel   = 0;
                barriers[i].subresourceRange.levelCount     = 1;
                barriers[i].subresourceRange.baseArrayLayer = 0;
                barriers[i].subresourceRange.layerCount     = 1;
                barriers[i].srcAccessMask                   = 0;
                barriers[i].dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;
            }

            vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, barrier_count, barriers);
        }

        // 1. Raymarching Pass -- runs at trace resolution (full res, or half under ssr_half_res).
        // Two attachments: target_ (colour, confidence) and hit_target_ (hit distance).
        {
            VkClearValue clears[2]{};
            VkRenderPassBeginInfo rp_info{};
            rp_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp_info.renderPass        = trace_render_pass_;
            rp_info.framebuffer       = trace_framebuffer_;
            rp_info.renderArea.extent = {trace_width_, trace_height_};
            rp_info.clearValueCount   = 2;
            rp_info.pClearValues      = clears;
            vkCmdBeginRenderPass(cmd.handle(), &rp_info, VK_SUBPASS_CONTENTS_INLINE);
        }
        cmd.bind_pipeline(*ssr_pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(trace_width_), static_cast<float>(trace_height_));
        cmd.set_scissor(0, 0, trace_width_, trace_height_);

        SsrPushConstants pc{};
        pc.inv_proj         = glm::inverse(params.proj);
        pc.max_distance     = params.max_distance;
        pc.bias_texels      = params.bias_texels;
        pc.thickness_min    = params.thickness_min;
        pc.thickness_scale  = params.thickness_scale;
        pc.roughness_cutoff = params.roughness_cutoff;
        pc.max_iterations   = params.max_iterations;
        pc.max_hiz_mip      = params.max_hiz_mip;
        pc.start_mip        = params.start_mip;
        pc.min_mip0_steps   = params.min_mip0_steps;
        pc.max_color_mip    = params.max_color_mip;
        pc.jitter_strength  = params.jitter_strength;
        // 0 when there is no temporal accumulation running. Every stochastic term in the two
        // traces re-seeds from this -- ssr.frag's GGX ray jitter AND ssgi.frag's cosine
        // hemisphere direction, which samples from it unconditionally, not only when
        // jitter_strength is nonzero. Advancing it without an accumulator downstream is pure
        // per-frame noise with nothing left to integrate it, so a consumer running the resolves
        // as passthroughs (an A/B capture) would see an image that never repeats. Same rule
        // SsaoPass applies to its own noise_rotation, for the same reason.
        pc.frame_index      = params.temporal_enabled ? params.frame_index : 0;
        pc.skip_threshold   = params.skip_threshold;
        pc.flags            = (params.prev_frame_color ? SsrPushConstants::kFlagPrevFrameColor : 0) |
                              (params.skip_behind      ? SsrPushConstants::kFlagSkipBehind     : 0);
        pc.rays_per_pixel   = std::max(params.rays_per_pixel, 1);
        pc.cone_prefilter   = params.cone_prefilter;

        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, pc);

        cmd.bind_descriptor_set(camera_set, 0);
        cmd.bind_descriptor_set(*ssr_gbuf_set_, 1);
        cmd.bind_descriptor_set(*hiz_set_, 2);
        cmd.bind_descriptor_set(*scene_color_set_, 3);

        cmd.draw(3);
        cmd.end_render_pass();
        if (stage_hook_) stage_hook_(cmd, Stage::Trace);

        // 2. Temporal Resolve Pass: blend target_ (this frame's raw trace) with last frame's
        // resolved output (resolved_targets_[1-o]) into resolved_targets_[o], which the blur /
        // composite below read instead of target_ directly. When temporal_enabled is false,
        // blend_factor = 0 degenerates this into a pure passthrough of the current frame --
        // one code path, no branching pipeline structure. Last frame's readers of this target
        // are ordered ahead of its clear by the render pass's COLOR_ATTACHMENT_OUTPUT entry
        // dependency (every reader is a draw), as SsaoPass argues for its own ping-pong.
        resolved_targets_[o]->begin(cmd);
        cmd.bind_pipeline(*resolve_pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(trace_width_), static_cast<float>(trace_height_));
        cmd.set_scissor(0, 0, trace_width_, trace_height_);

        ResolvePushConstants rpc{};
        rpc.reproject    = params.reproject;
        rpc.resolution_x = static_cast<float>(trace_width_);
        rpc.resolution_y = static_cast<float>(trace_height_);
        // 0 makes the resolve a passthrough of the current frame -- one code path, no branching
        // pipeline structure, matching how SsaoPass expresses the same toggle.
        rpc.max_accum    = params.temporal_enabled ? static_cast<float>(params.temporal_frames) : 0.0f;
        rpc.blend_factor = params.temporal_enabled ? params.temporal_blend : 0.0f;
        // Both a history IMAGE (history_initialized_) and a reprojection MATRIX (reproject_valid)
        // must exist -- the matrix lags a frame behind the image on a fresh start (frames 0 and
        // 1), so ANDing them is what keeps the very first reprojected sample from reading a
        // stale/identity matrix.
        rpc.history_valid = (history_initialized_ && params.reproject_valid) ? 1 : 0;
        rpc.gamma         = params.temporal_gamma;
        rpc.frozen        = params.temporal_frozen ? 1 : 0;
        rpc.use_velocity  = params.resolve_use_velocity ? 1 : 0;
        rpc.virtual_blend = params.resolve_use_velocity ? params.virtual_blend : 0.0f;

        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, rpc);
        cmd.bind_descriptor_set(*resolve_sets_[o][cp], 0);
        cmd.bind_descriptor_set(camera_set, 1);

        cmd.draw(3);
        resolved_targets_[o]->end(cmd);
        if (stage_hook_) stage_hook_(cmd, Stage::Resolve);

        // 2b. Spatial denoise (optional -- see the ctor's blur_frag_spv doc): bilateral blur of
        // the resolved target into blurred_target_, which the composite pass below reads instead
        // when this stage is enabled (see update_descriptors()'s comp_raw_ssr_sets_ binding).
        // Runs every frame regardless of temporal_enabled -- it addresses spatial hit/miss
        // noise the temporal resolve above doesn't remove, not a replacement for it.
        if (blur_enabled_) {
            blurred_target_->begin(cmd);
            cmd.bind_pipeline(*blur_pipeline_);
            cmd.set_viewport(0.0f, 0.0f, static_cast<float>(trace_width_), static_cast<float>(trace_height_));
            cmd.set_scissor(0, 0, trace_width_, trace_height_);

            BlurPushConstants bpc{};
            bpc.radius = params.ssr_blur_radius;
            bpc.flags  = (params.ssr_blur_light ? kBlurLight : 0) | (params.ssr_blur_zero_skip ? kBlurZeroSkip : 0) |
                         (params.ssr_blur_roughness_aware ? kBlurRoughnessAware : 0);
            cmd.push_constants(coopa::gfx::ShaderStage::Fragment, bpc);
            cmd.bind_descriptor_set(*blur_sets_[o], 0);

            cmd.draw(3);
            blurred_target_->end(cmd);
            if (stage_hook_) stage_hook_(cmd, Stage::Blur);
        }

        // 2c. Traced-SSGI stage (optional -- see the ctor's ssgi_frag_spv doc): one
        // cosine-hemisphere ray per pixel through ssgi.frag, then a second run of the
        // resolve pipeline over the SSGI chain's own current/history pair. The trace
        // reuses this frame's SsrPushConstants with only the ray length swapped (and the
        // roughness cutoff lifted clear of ssgi.frag's fixed 0.9 trace roughness, so the
        // shared roughness fade stays 1 -- the cutoff is a specular concept).
        if (ssgi_enabled_) {
            ssgi_target_->begin(cmd);
            cmd.bind_pipeline(*ssgi_pipeline_);
            cmd.set_viewport(0.0f, 0.0f, static_cast<float>(ssgi_width_), static_cast<float>(ssgi_height_));
            cmd.set_scissor(0, 0, ssgi_width_, ssgi_height_);

            SsrPushConstants gi_pc = pc;
            gi_pc.max_distance     = params.ssgi_max_distance;
            gi_pc.max_iterations   = params.ssgi_max_iterations;
            gi_pc.roughness_cutoff = 2.0f;
            gi_pc.skip_threshold   = 0.0f;   // SSGI's weight is diffuse; never skip it
            // One cosine ray, colour from the WIDE lobe-cone mip: a diffuse gather wants each
            // hit's neighbourhood average, whatever the specular chain's settings are.
            gi_pc.rays_per_pixel   = 1;
            gi_pc.cone_prefilter   = 1.0f;
            cmd.push_constants(coopa::gfx::ShaderStage::Fragment, gi_pc);

            cmd.bind_descriptor_set(camera_set, 0);
            cmd.bind_descriptor_set(*ssr_gbuf_set_, 1);
            cmd.bind_descriptor_set(*hiz_set_, 2);
            cmd.bind_descriptor_set(*scene_color_set_, 3);

            cmd.draw(3);
            ssgi_target_->end(cmd);
            if (stage_hook_) stage_hook_(cmd, Stage::SsgiTrace);

            ssgi_resolved_targets_[o]->begin(cmd);
            cmd.bind_pipeline(*resolve_pipeline_);
            cmd.set_viewport(0.0f, 0.0f, static_cast<float>(ssgi_width_), static_cast<float>(ssgi_height_));
            cmd.set_scissor(0, 0, ssgi_width_, ssgi_height_);
            // Same push block as the specular resolve above with only the accumulation depth
            // swapped: the bounce averages deeper, since one cosine-hemisphere ray carries far
            // more variance than a near-mirror reflection ray. Both clamp the SAME shared count
            // buffer, which is why one buffer can serve two schedules.
            ResolvePushConstants gi_rpc = rpc;
            gi_rpc.max_accum = params.temporal_enabled
                ? static_cast<float>(params.ssgi_temporal_frames) : 0.0f;
            gi_rpc.virtual_blend = 0.0f;   // a diffuse bounce has no single reflected image
            gi_rpc.resolution_x  = static_cast<float>(ssgi_width_);
            gi_rpc.resolution_y  = static_cast<float>(ssgi_height_);
            cmd.push_constants(coopa::gfx::ShaderStage::Fragment, gi_rpc);
            cmd.bind_descriptor_set(*ssgi_resolve_sets_[o][cp], 0);
            cmd.bind_descriptor_set(camera_set, 1);
            cmd.draw(3);
            ssgi_resolved_targets_[o]->end(cmd);
            if (stage_hook_) stage_hook_(cmd, Stage::SsgiResolve);

            // Spatial denoise -- see ssgi_blurred_target_'s construction for why this is
            // load-bearing for the bounce rather than optional polish.
            if (blur_enabled_) {
                ssgi_blurred_target_->begin(cmd);
                cmd.bind_pipeline(*blur_pipeline_);
                cmd.set_viewport(0.0f, 0.0f, static_cast<float>(ssgi_width_), static_cast<float>(ssgi_height_));
                cmd.set_scissor(0, 0, ssgi_width_, ssgi_height_);

                BlurPushConstants gi_bpc{};
                gi_bpc.radius = params.ssgi_blur_radius;
                gi_bpc.flags  = params.ssgi_blur_light ? kBlurLight : 0;   // never all-zero: no zero skip
                cmd.push_constants(coopa::gfx::ShaderStage::Fragment, gi_bpc);
                cmd.bind_descriptor_set(*ssgi_blur_sets_[o], 0);

                cmd.draw(3);
                ssgi_blurred_target_->end(cmd);
                if (stage_hook_) stage_hook_(cmd, Stage::SsgiBlur);
            }
        }

        // 3. Composite Pass -- always full screen resolution; bilaterally upsamples the
        // (possibly trace-resolution) resolved SSR buffer.
        composite_target_->begin(cmd);
        cmd.bind_pipeline(*comp_pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
        cmd.set_scissor(0, 0, width_, height_);

        CompositePushConstants cpc{};
        cpc.ssr_resolution    = glm::vec2(static_cast<float>(trace_width_), static_cast<float>(trace_height_));
        cpc.screen_resolution = glm::vec2(static_cast<float>(width_), static_cast<float>(height_));
        cpc.half_res          = half_res_ ? 1 : 0;
        cpc.max_color_mip     = params.max_color_mip;
        cpc.sky_intensity     = params.sky_intensity;
        cpc.ssgi_intensity    = params.ssgi_intensity;
        cpc.ssgi_distance     = params.ssgi_distance;
        cpc.ssgi_traced       = ssgi_enabled_ ? 1.0f : 0.0f;
        cpc.sky_zenith        = glm::vec4(params.sky_zenith, 0.0f);
        cpc.sky_horizon       = glm::vec4(params.sky_horizon, 0.0f);
        cpc.sky_ground        = glm::vec4(params.sky_ground, 0.0f);
        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, cpc);

        cmd.bind_descriptor_set(camera_set, 0);
        cmd.bind_descriptor_set(*comp_gbuf3_set_, 1);
        cmd.bind_descriptor_set(*comp_raw_ssr_sets_[o], 2);
        cmd.bind_descriptor_set(*comp_scene_color_sets_[o], 3);
        if (composite_extra_.bind) {
            composite_extra_.bind(cmd, comp_first_extra_set_);
        }

        cmd.draw(3);
        composite_target_->end(cmd);

        if (stage_hook_) stage_hook_(cmd, Stage::Composite);

        // 4. This frame's resolved targets ARE next frame's histories -- flip the parity.
        current_parity_      = o;
        write_parity_        = 1 - o;
        history_initialized_ = true;
    }

    VkImageView output_view() const { return composite_target_->color_view(); }
    coopa::gfx::TextureView output_view_typed() const { return composite_target_->color_view_typed(); }
    targets::OffscreenTarget& composite_target() { return *composite_target_; }

    /// @brief The resolved reflection buffer the composite reads (post spatial-denoise
    /// blur when enabled, matching comp_raw_ssr_sets_'s own choice above) -- for a caller
    /// that wants to inspect the reflection term itself rather than the finished
    /// scene-colour-plus-reflection composite output_view() returns.
    /// Without the blur stage the answer alternates with the resolve parity (two images), so
    /// a caller that binds it once must do so per parity and select with current_parity().
    coopa::gfx::TextureView reflection_view_typed() const {
        return blur_enabled_ ? blurred_target_->color_view_typed() : resolved_targets_[current_parity_]->color_view_typed();
    }
    /// @brief The resolve parity the most recent execute() wrote (see reflection_view_typed()).
    uint32_t current_parity() const { return current_parity_; }

    /// @brief The resolved traced-SSGI bounce buffer (post spatial-denoise blur when
    /// enabled), or a 1x1 neutral zero texture when this instance has no SSGI stage
    /// (ssgi_enabled_ false) -- the same fallback comp_scene_color_sets_'s own SSGI
    /// binding uses, so a caller never binds a descriptor still in VK_IMAGE_LAYOUT_UNDEFINED.
    coopa::gfx::TextureView ssgi_view_typed() const {
        if (!ssgi_enabled_) return zero_rgba_->view_typed();
        return blur_enabled_ ? ssgi_blurred_target_->color_view_typed() : ssgi_resolved_targets_[current_parity_]->color_view_typed();
    }

    /// @brief The same 1x1 neutral zero texture ssgi_view_typed() falls back to when this
    /// instance has no SSGI stage -- exposed unconditionally for a caller (e.g. a debug
    /// view) that needs a safe fallback for reflection_view_typed()/ssgi_view_typed() too
    /// on a frame where execute() never runs at all (this pass is always CONSTRUCTED, per
    /// this class's own file doc, but its targets only leave VK_IMAGE_LAYOUT_UNDEFINED
    /// once execute() has run at least once).
    coopa::gfx::TextureView zero_view_typed() const { return zero_rgba_->view_typed(); }

private:
    /// NEAREST + ClampToEdge, matching the raw ctor's default mipmap_mode (NEAREST) too -- see
    /// nearest_sampler_/neutral_sampler_'s uses. All of this pass's own samplers (aside from the
    /// caller-supplied linear_sampler passed into update_descriptors()) share this exact config.
    static coopa::gfx::SamplerDesc nearest_clamp_desc() {
        coopa::gfx::SamplerDesc d;
        d.min = d.mag = coopa::gfx::Filter::Nearest;
        d.mipmap  = coopa::gfx::MipmapMode::Nearest;
        d.address = coopa::gfx::AddressMode::ClampToEdge;
        return d;
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;

    uint32_t width_;
    uint32_t height_;

    // Trace resolution -- ctor param, not runtime-toggleable (changing it reallocates targets).
    bool     half_res_ = false;
    /// Two-colour-attachment pass for the trace (RGBA16F colour+confidence, R16F hit distance):
    /// render_pass.h's RenderPass is single-colour. Same load/store/layout and dependencies as
    /// an OffscreenTarget's colour-only pass ending in SHADER_READ_ONLY_OPTIMAL.
    void create_trace_render_pass_() {
        VkAttachmentDescription att[2]{};
        const VkFormat formats[2] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16_SFLOAT};
        VkAttachmentReference refs[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            att[i].format         = formats[i];
            att[i].samples        = VK_SAMPLE_COUNT_1_BIT;
            att[i].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att[i].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            att[i].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[i].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            att[i].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            refs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 2;
        subpass.pColorAttachments    = refs;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = 0;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 2;
        ci.pAttachments    = att;
        ci.subpassCount    = 1;
        ci.pSubpasses      = &subpass;
        ci.dependencyCount = 2;
        ci.pDependencies   = deps;
        GFX_VK_CHECK(vkCreateRenderPass(device_.handle(), &ci, nullptr, &trace_render_pass_));
    }

    /// (Re)builds the trace framebuffer over target_'s and hit_target_'s current images.
    void create_trace_framebuffer_() {
        if (trace_framebuffer_ != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_.handle(), trace_framebuffer_, nullptr);
            trace_framebuffer_ = VK_NULL_HANDLE;
        }
        VkImageView views[2] = {target_->color_view(), hit_target_->color_view()};
        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = trace_render_pass_;
        fb.attachmentCount = 2;
        fb.pAttachments    = views;
        fb.width           = trace_width_;
        fb.height          = trace_height_;
        fb.layers          = 1;
        GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &fb, nullptr, &trace_framebuffer_));
    }

    uint32_t trace_width_  = 0;
    uint32_t trace_height_ = 0;
    /// The traced-SSGI chain's resolution: trace resolution / ssgi_res_divisor_. A diffuse
    /// bounce is low-frequency, so it can trace at a quarter of the specular chain's pixels
    /// (the composite upsamples it bilinearly) -- the GI resolution tier Unreal's lower
    /// quality levels use too.
    uint32_t ssgi_res_divisor_ = 1;
    uint32_t ssgi_width_  = 0;
    uint32_t ssgi_height_ = 0;

    std::unique_ptr<util::Sampler>          nearest_sampler_;

    std::unique_ptr<targets::OffscreenTarget> target_;
    std::unique_ptr<targets::OffscreenTarget> hit_target_;   ///< R16F trace hit distance (MRT 1).
    VkRenderPass  trace_render_pass_  = VK_NULL_HANDLE;      ///< target_ + hit_target_, see create_trace_render_pass_().
    VkFramebuffer trace_framebuffer_  = VK_NULL_HANDLE;
    std::unique_ptr<targets::OffscreenTarget> composite_target_;
    /// Temporal resolve ping-pong pair: parity p is written on one frame and read as the
    /// history on the next (see execute()).
    std::unique_ptr<targets::OffscreenTarget> resolved_targets_[2];
    uint32_t write_parity_   = 0;   ///< Resolve target the next execute() writes.
    uint32_t current_parity_ = 0;   ///< Resolve target the last execute() wrote.

    // Spatial SSR denoise (optional -- see the ctor's blur_frag_spv doc). blur_enabled_ is set
    // once at construction from whether blur shader paths were provided; every other blur_*
    // member stays null when it's false.
    bool blur_enabled_ = false;
    std::unique_ptr<targets::OffscreenTarget> blurred_target_;

    // Traced-SSGI stage (optional -- see the ctor's ssgi_frag_spv doc). ssgi_enabled_
    // is set once at construction; every other ssgi_* member stays null when false.
    bool ssgi_enabled_ = false;
    std::unique_ptr<targets::OffscreenTarget>  ssgi_target_;
    std::unique_ptr<targets::OffscreenTarget>  ssgi_resolved_targets_[2];   ///< Same ping-pong as resolved_targets_.
    std::unique_ptr<targets::OffscreenTarget>  ssgi_blurred_target_;

    bool history_initialized_ = false;

    std::unique_ptr<coopa::gfx::pipeline::Shader> ssr_vert_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> ssr_frag_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> comp_vert_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> comp_frag_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> resolve_vert_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> resolve_frag_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> blur_vert_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> blur_frag_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> ssgi_frag_;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> gbuf3_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> comp_gbuf_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> hiz_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> scene_color_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> comp_scene_color_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> raw_ssr_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> resolve_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> blur_layout_;

    ExtraSets composite_extra_;
    uint32_t  comp_first_extra_set_ = 0;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool> desc_pool_;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> ssr_gbuf_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> hiz_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> scene_color_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> resolve_sets_[2][2];   ///< [resolve parity][count parity]

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> comp_gbuf3_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> comp_raw_ssr_sets_[2];       ///< [resolve parity]
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> comp_scene_color_sets_[2];   ///< [resolve parity]

    std::unique_ptr<util::Sampler>              neutral_sampler_;
    std::unique_ptr<coopa::gfx::memory::Image>  zero_rgba_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> blur_sets_[2];              ///< [resolve parity]
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> ssgi_resolve_sets_[2][2];   ///< [resolve parity][count parity]
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> ssgi_blur_sets_[2];         ///< [resolve parity]

    std::unique_ptr<coopa::gfx::pipeline::Pipeline> ssr_pipeline_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> comp_pipeline_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> resolve_pipeline_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> blur_pipeline_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> ssgi_pipeline_;

    std::function<void(coopa::gfx::command::CommandBuffer&, Stage)> stage_hook_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_SSR_PASS_H

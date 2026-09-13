/**
 * @file pixel_render_config.h
 * @brief Tunable parameters for the pixel-art render pipeline.
 */

#ifndef TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H
#define TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/pipeline/shader_library.h>
#include <gfxcoopa/pipeline/surface_shader.h>
#include <gfxcoopa/engine/render_features.h>

namespace toy {
namespace render {

/**
 * @struct PixelRenderConfig
 * @brief Configuration for PixelRenderPipeline: internal resolution, upscaling,
 *        banded lighting, hard shadows, and the pixel-art post-process stack
 *        (outline, palette quantization, ordered dithering), plus the
 *        independently-toggleable SSAO / SSR add-ons.
 */
struct PixelRenderConfig {
    // --- Feature toggles ---
    bool outline_enabled   = true;
    bool palette_enabled   = true;  /**< Independent of palette_path, so toggling off keeps the configured path. */
    bool dither_enabled    = true;  /**< Independent of dither_strength, so toggling off keeps the configured strength. */
    bool camera_pixel_snap = true;  /**< Orthographic cameras only. */
    bool soft_lighting     = false; /**< true = smooth Cook-Torrance direct lighting; false = this engine's default banded/ramped cel-shaded look. */
    bool ssao_enabled      = true;
    /**
     * Debug view: draws SsaoPass's bound output (its blurred occlusion buffer when
     * ssao_enabled, or its neutral 1.0 texture otherwise -- see PixelRenderPipeline's
     * ssao_view selection) fullscreen in place of lighting, via ssao_debug.frag. A runtime
     * flag re-read every frame, same policy as ssr_enabled below. Skips the SSR composite
     * and forward transparent pass for that frame (nothing left to composite onto); bloom
     * and tonemapping still run over the debug image.
     */
    bool ssao_debug_view   = false;
    bool ssr_enabled       = true;  /**< Also gates the SSGI diffuse-bounce term (ssgi_intensity). */
    bool transparency_enabled = false;  /**< Forward BLEND-material pass, drawn after SSR compositing. */
    /**
     * Opaque surfaces (e.g. the floor) also reflect transparent geometry, via a second forward
     * capture of BLEND objects (depth/normal/position/shaded-color) and a second Hi-Z pyramid
     * ssr.frag's raymarch tries alongside its primary opaque source -- see
     * PixelRenderPipeline::record_transparent_capture_(). Meaningless without ssr_enabled AND
     * transparency_enabled also true. Opt-in (default false), not implied by those two: this
     * roughly doubles the opaque raymarch's per-pixel cost and adds an extra forward draw +
     * Hi-Z build + scene-colour-mip build every frame it's on.
     */
    bool ssr_reflect_transparent = false;

    /**
     * Screen-space refraction for BLEND MESH objects only -- MeshRenderer, not SdfRenderer;
     * SDF glass is deliberately excluded. Bends the background
     * sample by the surface's IOR/thickness, applies Beer-Lambert tint absorption and
     * roughness-driven blur, and optionally chromatic aberration and a Fresnel falloff --
     * see assets/shaders/refraction.glsl. Requires transparency_enabled; meaningless
     * without it. Per-object ior/refraction_thickness/refraction_tint/refraction override
     * these defaults via PBRMaterial (see mesh_renderer.h).
     */
    bool      refraction_enabled       = true;
    float     refraction_ior           = 1.45f; /**< Default IOR; glass ~1.45, water ~1.33. */
    float     refraction_thickness     = 0.25f; /**< Default world-space distance the ray travels through the object. */
    float     refraction_strength      = 1.0f;  /**< Global multiplier on the screen-space UV offset. */
    float     refraction_max_offset    = 0.08f; /**< Clamp in UV units; stops smearing at grazing angles. */
    float     refraction_chromatic     = 0.0f;  /**< RGB IOR split (chromatic aberration); 0 = single tap. */
    float     refraction_blur          = 1.0f;  /**< Roughness -> scene-colour mip scale; frosted glass. */
    float     refraction_density       = 1.0f;  /**< Beer-Lambert absorption strength through refraction_tint. */
    bool      refraction_fresnel       = true;  /**< Dim transmission at grazing angles (energy already in the SSR/sky term). */
    glm::vec3 refraction_tint          = glm::vec3(1.0f, 1.0f, 1.0f); /**< Default transmission tint. */
    /**
     * When true (and refraction_enabled), refraction_scene_color_mip_pass_ -- the dedicated
     * scene-colour chain the MESH forward pass's u_scene_color reads whenever refraction is
     * active (see that member's own doc in pixel_render_pipeline.h) -- is built from the SSR
     * composite output each frame, so refraction sees a background that includes SSR
     * reflections. When false, it's built from the same pre-SSR image ssr_pass_'s own trace
     * uses instead. Either way this is a SEPARATE, independent SceneColorMipPass instance
     * from scene_color_mip_pass_ -- not a second execute() on that shared one, which would
     * corrupt its own internal per-mip descriptor sets (see that member's doc for the
     * validation-layer error this was caught by). MESH-only: BLEND SdfRenderers are unaffected
     * either way, since SDF glass is excluded from refraction entirely.
     */
    bool      refraction_include_reflections = true;

    /**
     * Unity-style GLOBAL fog (Linear/Exponential/Exp2), analytic and config-driven. There
     * is no fog component and no local fog volume: anything bounded is a VolumeComponent
     * on the volumetrics pass instead. Drawn after the transparent pass (so BLEND geometry
     * is fogged too) and before pixel_stylize_pass_ (so fog sits in linear HDR, ahead of tonemap/
     * outline/dither/palette -- see gfxcoopa's FogPass and PixelRenderPipeline's
     * pre_fog_view_typed_). Always constructed; render() checks this per frame. The pass's
     * SOURCE image is chosen once at construction from config_.ssr_enabled's startup value,
     * matching pixel_stylize_pass_'s own binding -- see that call site's comment for why a
     * per-frame rebind isn't safe under this pipeline's frame-overlap model.
     */
    bool fog_enabled = false;

    /**
     * Raymarched LOCAL volumes -- scene-placed VolumeComponents, each `kind: fog` (a
     * static pocket), `wind` (advected ribbons) or `haze` (drifting billows). Runs in
     * linear HDR immediately after fog and before DOF/bloom, so volumes defocus and
     * sun-lit ones glow (see gfxcoopa's VolumetricsPass / gfx/volumetrics.glsl).
     *
     * A separate pass from fog rather than more fog parameters, because fog is an
     * analytic medium that is everywhere, while these are bounded and their density
     * varies within them -- which has no closed form, so they must march. The two
     * toggle independently. The march clips to the union of the volumes' bounds, so a
     * view with no volume in it skips the march entirely.
     *
     * Startup-fixed, same policy as fog_enabled/bloom_enabled/dof_enabled above: the
     * pass's source image and DOF's source image are both chosen once at construction
     * from this flag's value.
     */
    bool volumetrics_enabled = false;

    /**
     * Signed-distance-field raymarching system (see gfxcoopa's SdfRenderer/SdfShape
     * components and toyengine's sdf_gbuffer.frag/sdf_forward.frag/sdf_shadow*.frag/
     * sdf_capture.frag). sdf_enabled is the single per-frame gate: when false, the SDF
     * gather in PixelRenderPipeline::render() produces an empty draw list and every
     * downstream recording site (G-buffer/shadow/capture/forward) is naturally a no-op --
     * the five SDF passes themselves are always constructed (same always-on-but-gated
     * policy as SSR/fog/bloom above).
     */
    bool sdf_enabled = true;
    /** Independent of sdf_enabled/shadows_enabled -- an SDF object can be visible but never
     *  cast a shadow (cheaper), same as shadows_enabled gates mesh shadow casting globally. */
    bool sdf_shadows_enabled = true;
    /** Global ceiling on SdfRenderer::max_steps; a per-renderer value above this is clamped down. */
    uint32_t sdf_max_steps = 64;
    /** Cheaper step budget for the depth-only shadow march (see SdfShadowPass). */
    uint32_t sdf_shadow_max_steps = 32;
    /** SdfData's renderer/shape SSBO capacities -- startup-fixed (SSBO sizing), not a runtime toggle. */
    uint32_t sdf_max_renderers = 64;
    uint32_t sdf_max_shapes    = 512;

    // --- Internal resolution ---
    std::string resolution_mode = "fixed";   /**< "fixed" or "divisor". */
    uint32_t    render_width    = 480;       /**< Used when resolution_mode == "fixed". */
    uint32_t    render_height   = 270;       /**< Used when resolution_mode == "fixed". */
    uint32_t    scale_divisor   = 4;         /**< Used when resolution_mode == "divisor". */
    std::string upscale_mode    = "fit";     /**< "fit" (aspect-preserving best fit, default,
                                                   letterboxed only on the mismatched axis) or
                                                   "integer" (whole-number scale, more letterboxing
                                                   but every texel is an exact NxN block). */

    // --- Lighting ---
    float exposure          = 1.0f;
    float light_bands       = 4.0f;   /**< Discrete shading steps per light; <= 1 disables banding. */
    float spec_threshold    = 0.55f;  /**< Hard specular highlight cutoff. */
    float rim_strength      = 0.0f;   /**< 0 disables the rim term. */
    // ambient_intensity/sky_intensity/ssgi_intensity/ssgi_distance/sky_zenith/sky_horizon/
    // sky_ground live in `indirect` below (shared with SsrPass::Params so the two can never
    // disagree -- see render_features.h).

    // --- Shadows ---
    bool     shadows_enabled        = true;
    uint32_t shadow_map_resolution  = 2048;
    uint32_t cube_shadow_resolution = 512;
    float    shadow_bias            = 0.005f;
    /**
     * @brief How far from the camera, in world units, the directional shadow is computed at
     *        all -- Unity's own "Shadow Distance" quality setting. update_dir_shadow_matrix_()
     *        fits the shadow frustum to a bounding sphere of the camera's OWN view frustum out
     *        to this distance (clipped to the camera's far clip plane), and separately uses it
     *        to size the near-side margin behind that sphere (so an off-frustum caster between
     *        the light and the visible sphere still shadows into frame). This is the only knob
     *        that affects the fit -- unlike the AABB-over-scene-content fit this replaced, no
     *        renderer/SDF/physics-body position is ever read, so nothing in the scene (however
     *        far off, however it got there) can perturb or blow out the shadow frustum.
     */
    float    shadow_distance        = 60.0f;
    bool     soft_shadows           = true;  /**< false = single hard depth compare (the pre-existing look). */
    /**
     * @brief Directional PCF penumbra radius in WORLD units, not texels. Converted to a
     *        texel count every frame against the ortho box actually in effect (see
     *        update_dir_shadow_matrix_()), so the penumbra stays visually constant in world
     *        units even as that box refits to the camera -- a fixed texel-count radius would
     *        otherwise shrink to imperceptible on a scene-spanning box and grow huge on a
     *        tight one. Ignored when soft_shadows is false.
     */
    float    shadow_softness        = 0.15f;
    /**
     * @brief Point-light cube-map PCF penumbra radius, in cube-map TEXELS (comparable in
     *        spirit to shadow_softness above, though that one is world units since a
     *        directional shadow map has a single, frame-varying world-per-texel scale while
     *        a point light's cube faces do not). Converted to a tangent-space offset on a
     *        unit sample direction every frame against cube_shadow_resolution (see
     *        gfx_shadow_cube_pcf_vogel's doc for that unit) and clamped to 8 texels, the
     *        kernel's own practical limit before the penumbra swallows the whole shadow.
     *        Ignored when soft_shadows is false.
     */
    float    point_shadow_softness  = 3.0f;
    uint32_t shadow_pcf_samples     = 24;    /**< Vogel disk taps for directional soft shadows; clamped to 1..32, the kernel's own hard limit. */

    // --- Outline ---
    float     outline_thickness = 1.0f;     /**< In low-resolution texels. */
    glm::vec4 outline_color     = glm::vec4(0.05f, 0.04f, 0.08f, 1.0f);
    float     depth_threshold   = 0.02f;    /**< Relative depth step (fraction of the fragment's own view-space distance) that counts as an edge; scale-invariant, so one value works at any camera distance. */
    float     normal_threshold  = 0.75f;

    // --- Palette ---
    std::string palette_path;                 /**< Empty disables palette quantization. */

    // --- Dither ---
    float dither_strength = 0.0f;             /**< 0 disables ordered dithering. */

    // --- SSAO ---
    float ssao_radius           = 0.5f;
    float ssao_bias             = 0.025f;
    float ssao_power            = 1.5f;
    int   ssao_kernel_size      = 24;
    bool  ssao_temporal_enabled = true;
    float ssao_temporal_blend   = 0.85f;

    // --- SSR + SSGI ---
    float ssr_max_distance     = 15.0f;
    int   ssr_max_iterations   = 64;
    float ssr_thickness        = 0.05f;
    float ssr_thickness_scale  = 0.01f;
    float ssr_bias_texels      = 3.5f;
    float ssr_roughness_cutoff = 1.0f;  /**< 1.0 so rough surfaces still trace and feed the SSGI bounce. */
    int   ssr_start_mip        = 0;
    int   ssr_min_mip0_steps   = 1;
    bool  ssr_temporal_enabled = true;
    float ssr_temporal_blend   = 0.85f;
    float ssr_blur_radius      = 0.5f;  /**< World-space sigma for the spatial SSR denoise (ssr_blur.frag). */
    float ssr_jitter           = 0.0f;  /**< Stochastic ray jitter strength, as a fraction of the
                                              GGX lobe cone; 0 reproduces the old single-ray trace. */
    float ssr_temporal_gamma   = 1.0f;  /**< Variance-clipping width for the SSR temporal resolve,
                                              in std deviations of the 3x3 neighbourhood. */

    /**
     * Additive glow from a dedicated BloomPass pyramid (bright-pass threshold -> multi-tap
     * downsample -> tent-filter upsample+combine -- see gfxcoopa's bloom_pass.h), sourced
     * from the FINAL pre-tonemap HDR image (fog output when fog is on, else
     * pre_fog_view_typed_) -- the same image pixel_stylize_pass_ itself reads, so SSR
     * reflections, transparent geometry and fog all bloom too.
     *
     * An independent pyramid with its own construction-fixed descriptors, deliberately not a
     * reuse of SSR's mip chain: sharing that would tie bloom's availability to SSR being on,
     * and its hard 2x2 `texelFetch` box downsample has no sub-texel interpolation, so the
     * sampled brightness pops discretely as the camera moves. Every kernel here is a smooth
     * multi-tap `texture()` read. Needing no per-frame rebind, it also costs no device wait.
     *
     * bloom_enabled is startup-fixed (the descriptor binding it controls is decided once at
     * construction); bloom_intensity <= 0 is a genuine per-frame no-op either way.
     */
    bool  bloom_enabled   = false;
    float bloom_threshold = 1.0f;   /**< Brightness (max3 of RGB) below which nothing glows. 1.0 = "brighter than white". */
    float bloom_soft_knee = 0.5f;   /**< 0..1 fraction of bloom_threshold the quadratic knee spans; 0 = hard cutoff (pops). */
    float bloom_intensity = 1.0f;   /**< Final multiplier on the composited glow; <= 0 disables. */
    float bloom_scatter   = 0.7f;   /**< Per-level blend toward the coarser tent; Unity URP's Bloom "Scatter". Higher = wider, softer halo. */
    float bloom_radius    = 1.0f;   /**< Tent-filter width multiplier on the upsample; > 1 widens further at a small cost in sharpness. */
    float bloom_clamp     = 20.0f;  /**< Per-tap HDR ceiling applied before thresholding; suppresses single-texel fireflies. */

    // --- Fog (see gfxcoopa's FogPass / gfx/fog.glsl) ---
    int       fog_mode           = 2;      /**< 0 Linear, 1 Exponential, 2 Exp2 (Unity's default). */
    float     fog_density        = 0.02f;
    float     fog_linear_start   = 5.0f;
    float     fog_linear_end     = 60.0f;
    glm::vec3 fog_color          = glm::vec3(0.55f, 0.62f, 0.72f);
    float     fog_height_base    = 0.0f;   /**< World Z (engine is Z-up); fog_height_falloff <= 0 disables height fog. */
    float     fog_height_falloff = 0.0f;
    float     fog_sky_blend      = 0.0f;   /**< 0 = flat fog_color, 1 = fully blended toward the sky gradient. */
    float     fog_sun_amount     = 0.0f;   /**< Additive Henyey-Greenstein sun in-scatter tint strength; 0 disables. */
    float     fog_sun_anisotropy = 0.7f;   /**< HG g; 0 isotropic, close to 1 = tight forward scatter toward the sun. */
    float     fog_max_opacity    = 1.0f;   /**< Ceiling on how much fog can occlude the scene (1 = fully opaque at max density). */
    float     fog_max_distance   = 150.0f; /**< Distance the global fog term saturates at, and the distance sky pixels are
                                            evaluated at -- prevents a hard seam where a grazing near-horizon ray's apparent
                                            distance blows up against an otherwise-unfogged sky. */

    // --- Volumetrics (raymarched LOCAL volumes; see gfxcoopa's VolumetricsPass) ---
    // Only genuinely SHARED march settings live here. Everything about how a volume
    // looks -- density, noise, advection, colour -- is per-volume and lives on
    // VolumeComponent in the scene, because volumes are local by definition.
    int   volumetrics_step_count     = 48;    /**< Raymarch steps. The primary perf knob, and what resolves
                                                thin ribbons; the start offset is dithered per pixel and per
                                                frame, so TAA recovers much of what a low count costs. */
    float volumetrics_max_distance   = 40.0f; /**< Distance the march stops at. */
    float volumetrics_max_opacity    = 0.85f; /**< Ceiling on how much volumetrics can occlude the scene. */
    float volumetrics_sun_anisotropy = 0.6f;  /**< HG g; 0 isotropic, close to 1 = tight forward scatter.
                                                Shared, not per-volume: it is a property of the light's
                                                phase function, not of which medium a sample sits in. */
    bool  volumetrics_debug_view     = false; /**< Output accumulated density alone, scene colour suppressed. */

    /**
     * Diorama-style tilt-shift blur (Zelda: Link's Awakening [Switch] reference) -- see
     * gfxcoopa's TiltShiftPass. Unlike every other post effect here, it runs at DISPLAY
     * resolution, after the pixel-art upscale: it's a lens effect layered on the final
     * image, not a pixel-grid effect, so running it at the low internal resolution would
     * quantize the blur kernel and the focus ramp to a handful of steps (see
     * TiltShiftPass's own file doc). The circle of confusion is a function of screen
     * position only (no depth sampling), so it stays exact under a separable
     * horizontal-then-vertical Gaussian with no silhouette bleeding.
     *
     * tilt_shift_enabled is a startup-fixed toggle (same policy as bloom_enabled/
     * fog_enabled): its descriptor binding -- whether upscale_pass_ reads
     * tilt_shift_pass_'s result or post_target_ directly -- is decided once at
     * construction from this flag's startup value.
     */
    bool  tilt_shift_enabled      = false;
    float tilt_shift_focus_center = 0.55f; /**< 0..1 screen position of the sharp band's centre (0 = top, at angle 0). */
    float tilt_shift_focus_width  = 0.18f; /**< 0..1 half-height of the fully-sharp band. */
    float tilt_shift_ramp_width   = 0.22f; /**< 0..1 distance the blur ramps in over, smoothstepped. */
    float tilt_shift_blur_top     = 1.0f;  /**< Strength multiplier on the "far" side of the band. */
    float tilt_shift_blur_bottom  = 0.7f;  /**< Strength multiplier on the "near" side of the band. */
    float tilt_shift_max_radius   = 6.0f;  /**< Blur radius in DISPLAY pixels at full strength. */
    float tilt_shift_angle        = 0.0f;  /**< Degrees; rotates the focus band off horizontal. */

    /**
     * Physically-based depth of field -- see gfxcoopa's DofPass. Unlike
     * tilt_shift above, this runs at RENDER resolution, on linear HDR, before
     * bloom_enabled's pyramid: the circle of confusion is a function of scene
     * DEPTH (a thin-lens formula from focal length/aperture/focus distance), and
     * defocused HDR highlights should bloom into real bokeh rather than DOF
     * blurring an already-tonemapped image. See DofPass's own file doc for why a
     * depth-driven CoC needs a single-pass gather rather than tilt_shift's
     * separable blur.
     *
     * dof_focal_length/dof_sensor_width <= 0 fall back to the active camera's
     * `lens`/`sensor_width` (CameraComponent) -- and a camera's own `aperture`/
     * `focus_distance` (also <= 0 = inherit) override dof_aperture/
     * dof_focus_distance below, so a scene can DOF one camera differently from
     * another without touching this global config.
     *
     * dof_focus_mode == "object" focuses on a named scene object instead of a fixed
     * distance: dof_focus_object gives its ':'-separated scene path (e.g.
     * "sdf_blob:sdf_blob_sphere", resolved via Scene::find_object_by_path()), and the
     * view-space depth of its Transform each frame becomes the focal plane. A camera's
     * own CameraComponent::focus_object (see that class's doc) overrides this field AND
     * self-activates object-focus mode for that camera even when dof_focus_mode here is
     * "manual" -- see PixelRenderPipeline::resolve_dof_focus_() for the exact precedence.
     * dof_focus_smoothing eases the focal plane toward a moving/retargeted object
     * (1/sec, like CameraController::follow_smoothing; <= 0 snaps instead) -- it applies
     * ONLY to object-focus mode, not orbit_target, which is already smoothed twice over
     * by CameraController's own follow_smoothing/movement_smoothing.
     *
     * dof_enabled is a startup-fixed toggle (same policy as bloom_enabled/
     * fog_enabled/tilt_shift_enabled above): its descriptor binding -- whether
     * bloom_pass_/pixel_stylize_pass_ read dof_pass_'s result or the pre-DOF
     * image directly -- is decided once at construction from this flag's
     * startup value. dof_debug_view, dof_focus_mode/object/smoothing are RUNTIME
     * fields, like ssao_debug_view: they only change push constants (or which scene
     * object CPU code reads), not a descriptor binding, so they're safe to change
     * every frame.
     */
    bool        dof_enabled         = false;
    std::string dof_focus_mode      = "manual";  /**< manual | orbit_target | object. */
    std::string dof_focus_object    = "";        /**< ':'-separated scene path; used when dof_focus_mode == "object". Overridden by CameraComponent::focus_object. */
    float       dof_focus_smoothing = 8.0f;      /**< Focus-rack rate (1/sec) for object-focus mode; <= 0 snaps. */
    float       dof_focus_distance  = 8.0f;      /**< Metres; used when dof_focus_mode == "manual". */
    float       dof_aperture        = 2.8f;      /**< f-stop; lower = shallower depth of field. */
    float       dof_focal_length    = 0.0f;      /**< mm; <= 0 takes the active camera's `lens`. */
    float       dof_sensor_width    = 0.0f;      /**< mm; <= 0 takes the active camera's `sensor_width`. */
    float       dof_max_radius      = 12.0f;     /**< |CoC| ceiling, in full-res pixels. */
    int         dof_sample_count    = 32;        /**< Spiral gather taps; clamped to [8, 48] in-shader. */
    int         dof_blade_count     = 0;         /**< < 3 = perfect disc bokeh; else an N-sided polygonal iris. */
    float       dof_blade_rotation  = 0.0f;      /**< Iris rotation, degrees. */
    bool        dof_debug_view      = false;     /**< Renders the signed CoC field in place of the image. */

    /**
     * Anti-aliasing. Three modes; MSAA is deliberately absent, since every target here is
     * SampleCount::X1.
     *
     * aa_mode is STARTUP-FIXED: going from "off" to any AA mode (or back) changes which
     * descriptor the composite and tilt shift are bound to, and that is decided once at
     * pipeline construction. Switching AMONG "fxaa"/"smaa"/"taa" at runtime IS safe -- all
     * three passes are constructed together whenever aa_mode != "off" and all three write the
     * same target, so render() just branches per frame on which draw() to call.
     *
     * "taa" jitters the camera projection every frame, which is in inherent tension with
     * camera_pixel_snap's whole-texel snapping above -- not a bug to fix. The TAA resolve has
     * no motion vectors and no history reprojection: history is sampled at the current
     * frame's UV and merely clamped to a YCoCg 3x3 neighbourhood, so it ghosts under camera
     * motion at the default taa_blending_weight.
     */
    std::string aa_mode = "off"; /**< "off" | "fxaa" | "smaa" | "taa". */
    float fxaa_subpixel           = 0.75f;   /**< Blend weight of FXAA's subpixel-aliasing term. */
    float fxaa_edge_threshold     = 0.166f;  /**< Local contrast (fraction of lumaMax) below which FXAA does nothing. */
    float fxaa_edge_threshold_min = 0.0312f; /**< Absolute contrast floor -- avoids AA-ing near-black noise. */
    float smaa_threshold          = 0.1f;    /**< SMAA edge-detection local contrast threshold. */
    int   smaa_max_search_steps   = 16;      /**< SMAA blend-weight pass's max horizontal/vertical search distance, in texels. */
    float taa_blending_weight     = 0.9f;    /**< TAA's blend weight toward the (AABB-clamped) history sample. */
    /** Plumbed for config parity with blendy only -- gfxcoopa's taa.frag declares this push-
     *  constant field but never reads it (an older velocity-based weight-attenuation term that
     *  was replaced by the neighborhood clamp; see TaaPass::PushConstants). */
    float taa_weight_scale        = 30.0f;

    /**
     * @brief Draws physics collider wireframes/contact normals (DebugLinePass), gathered each
     *        frame from PhysicsWorld::debug_draw() -- see toyengine/render/passes/debug_line_pass.h.
     *        Per-frame safe to toggle (it only gates whether the pass records draws, binds
     *        nothing), unlike bloom_enabled/tilt_shift_enabled above.
     */
    bool  debug_lines_enabled     = false;

    /**
     * @brief Draws every WorldSpace uicoopa CanvasComponent in the scene (UiWorldPass) as a
     *        guest inside post_target_'s bracket -- see uicoopa/render/ui_world_pass.h and
     *        CanvasRenderMode.
     *
     * STARTUP-FIXED, unlike debug_lines_enabled above: turning it on builds the UI pipelines
     * and binds the G-buffer depth into a descriptor set, neither of which can happen once
     * the frame loop is running (DescriptorSet::bind_image() updates descriptors
     * immediately). Off leaves the pass unconstructed and the descriptor unallocated, so it
     * costs literally nothing. See PixelRenderPipeline::apply_live_config().
     *
     * Screen-space canvases are unaffected by this flag -- they are drawn by a separate pass
     * with its own toggle, screen_ui_enabled below.
     */
    bool  world_ui_enabled        = true;

    /**
     * @brief Draws ScreenSpaceOverlay canvases (uicoopa's UiPass), at WINDOW resolution.
     *
     * The companion to world_ui_enabled, and STARTUP-FIXED for the same reason: it decides
     * whether the pass and its descriptor pool are built at all.
     *
     * Unlike the world-space layer, this one is not part of the pixel-art image: it draws
     * last, straight into the swapchain-sized overlay target, at full window resolution, so
     * an HUD stays crisp instead of being quantised to the internal render grid. Both UI
     * layers land after every post effect -- see PixelRenderPipeline's ui_composite_pass_.
     */
    bool  screen_ui_enabled       = true;

    // Indirect-lighting terms shared with SsrPass::Params (gfxcoopa/engine/render_features.h) --
    // fed to both the lighting pass and the SSR composite from this single instance so the two
    // can never disagree. ssgi_intensity defaults to 0.6 here (nonzero -- SSGI on by default),
    // overriding IndirectParams' own 0.0 "no consumer has this concept" default.
    coopa::gfx::engine::IndirectParams indirect{1.0f, 1.0f, 0.6f, 0.5f};

    std::string shader_dir;                   /**< Absolute path to assets/shaders. */
    // Ordered search path resolving a logical shader name (e.g. "gbuffer.vert") to a compiled
    // .spv path -- this app's own directory first, then gfxcoopa's shared base library. Set
    // alongside shader_dir (see engine.h's make_render_config_); shader_dir is kept for
    // logging/debugging, `shaders` is what every pass construction actually resolves through.
    coopa::gfx::pipeline::ShaderLibrary shaders;

    // Derived surface shaders a scene's materials may select by name (PBRMaterial::shader) --
    // see gfxcoopa/pipeline/surface_shader.h. Populated alongside
    // `shaders` in engine.h's make_render_config_(); empty by default, so a scene that never
    // references a custom shader behaves exactly as if this field didn't exist. Every entry's
    // logical shader names are resolved through `shaders` above at pass-construction time (see
    // PixelRenderPipeline's ctor), the same two-tier app-over-base search every stock entry
    // point already goes through.
    coopa::gfx::pipeline::SurfaceShaderRegistry surface_shaders;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H

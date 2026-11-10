/**
 * @file light_data.h
 * @brief Per-frame light UBO: one directional light (with shadow cascades), point and
 *        spot lights, sky colours, and the local-light shadow records.
 *
 * Uploaded as a uniform buffer updated once per frame.
 */

#ifndef GFXCOOPA_ENGINE_DATA_LIGHT_DATA_H
#define GFXCOOPA_ENGINE_DATA_LIGHT_DATA_H

#include <glm/glm.hpp>
#include <cstddef>
#include <memory>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace data {

static constexpr uint32_t MAX_POINT_LIGHTS = 16;
static constexpr uint32_t MAX_SPOT_LIGHTS  = 8;
/** Cascades the directional shadow atlas can hold. 4 fills a 2x2 tile grid exactly;
 *  the limit is the grid, not the UBO -- see LightUBO::dir_cascade_matrix. */
static constexpr uint32_t MAX_DIR_CASCADES = 4;
/** @brief Directional shadow LEVELS the extended block carries (clipmap or cascade fit).
 *
 *  Distinct from MAX_DIR_CASCADES, which is frozen at 4: dir_cascade_matrix sits mid-struct
 *  (LightUBO's local_shadows/sky/fog/cloud/toon blocks follow it, and VolumetricsUBO shares
 *  the constant), so raising it would shift every later field in two UBOs and desync every
 *  hand-written GLSL mirror. The full level set lives in the APPENDED dir_level_* block
 *  instead, which costs nothing to grow. 8 is the ceiling because 8 tiles pack exactly into
 *  the 4x2 atlas grid, 8 per-level scalars pack exactly into vec4[2], and at the default
 *  growth ratio the outermost ring already covers 128x the finest one's radius. */
static constexpr uint32_t MAX_DIR_SHADOW_LEVELS = 8;

/** Point and spot lights that can hold a shadow at once, in the shared local-light shadow
 *  atlas (see LocalShadowBlock). A point light takes 6 views, a spot 1. */
static constexpr uint32_t MAX_LOCAL_SHADOWS      = 12;
/** Views (atlas tiles) across all local shadows: enough for 6 shadowed points plus 12 spots. */
static constexpr uint32_t MAX_LOCAL_SHADOW_VIEWS = 48;

/**
 * @struct LocalShadowGPU
 * @brief One point or spot light's shadow in the local-light shadow atlas.
 *
 * A spot owns one square tile; a point light owns a 3x2 block of six tiles, one per cube face
 * (+X,-X,+Y,-Y,+Z,-Z, face f at column f%3, row f/3), each rendered with a field of view a
 * little wider than 90 degrees so the outer `pcf.z` texels of every face form a guard band
 * a PCF kernel can read without crossing into the next face. Depth is ordinary hardware
 * perspective depth (RH_ZO), so the casters keep early-Z.
 */
struct alignas(16) LocalShadowGPU {
    glm::vec4 tile   = glm::vec4(0.0f); /**< xy = atlas uv of the light's first tile, z = one tile's
                                             edge in atlas uv, w = kind: 0 none, 1 spot, 2 point. */
    glm::vec4 light  = glm::vec4(0.0f); /**< xyz = light world position, w = far plane (its range). */
    glm::vec4 proj   = glm::vec4(0.0f); /**< x = near plane, y = tan(half fov) of every view of this
                                             light, z = tile edge in texels, w = index of its first
                                             view in LocalShadowBlock::view_proj. */
    glm::vec4 pcf    = glm::vec4(0.0f); /**< x = spot penumbra width in WORLD units (0 = hard),
                                             y = point penumbra radius in texels (0 = hard),
                                             z = largest PCF radius in texels (the guard band),
                                             w = shadow darkness 0..1. */
};

/**
 * @struct LocalShadowBlock
 * @brief Every point/spot shadow of the frame: the per-light records and their view matrices.
 *        Appended to both LightUBO and VolumetricsUBO, and read in GLSL through
 *        gfx/local_shadow.glsl. A light points at its record with a 1-based slot number
 *        (PointLightGPU::attenuation.w / SpotLightGPU::params.z; 0 = unshadowed).
 */
struct alignas(16) LocalShadowBlock {
    glm::vec4 info = glm::vec4(0.0f);   /**< x = record count, y = atlas edge in texels,
                                             z = normal-offset bias in texels, w = reserved. */
    glm::vec4 bias = glm::vec4(1.0f, 1.0f, 5.0f, 0.0f); /**< x = constant depth bias (texels),
                                             y = slope depth bias (texels per tan), z = largest
                                             tan honoured, w = reserved. */
    LocalShadowGPU shadows[MAX_LOCAL_SHADOWS];
    glm::mat4      view_proj[MAX_LOCAL_SHADOW_VIEWS] = {}; /**< World -> each view's clip. */
};

/**
 * @struct PointLightGPU
 * @brief std140-aligned Point Light data uploaded to GPU UBO.
 */
struct alignas(16) PointLightGPU {
    glm::vec4 position_range  = glm::vec4(0.0f);            /**< xyz = position, w = range */
    glm::vec4 color_intensity = glm::vec4(0.0f);            /**< xyz = RGB color, w = intensity */
    glm::vec4 attenuation     = glm::vec4(1.0f, 0.09f, 0.032f, 0.0f); /**< x=const, y=linear, z=quad, w=shadow slot in
                                                                          LocalShadowBlock, 1-based (0 = unshadowed) */
};

/**
 * @struct SpotLightGPU
 * @brief std140-aligned Spot Light data uploaded to GPU UBO.
 */
struct alignas(16) SpotLightGPU {
    glm::vec4 position_range  = glm::vec4(0.0f); /**< xyz = world position, w = range */
    glm::vec4 direction_cone  = glm::vec4(0.0f, 0.0f, -1.0f, 0.7071f); /**< xyz = normalized aim (direction rays travel), w = cos(outer half-angle) */
    glm::vec4 color_intensity = glm::vec4(0.0f); /**< xyz = RGB color, w = intensity */
    glm::vec4 params          = glm::vec4(1.0f, 0.9f, 0.0f, 0.0f); /**< x=falloff sharpness, y=cos(inner half-angle), z=shadow slot in
                                                                       LocalShadowBlock, 1-based (0 = unshadowed), w=reserved */
};

/**
 * @struct LightUBO
 * @brief std140-aligned directional and point light data uploaded to the GPU.
 */
struct alignas(16) LightUBO {
    // Directional Light
    glm::vec4 dir_direction          = glm::vec4(0.0f); /**< xyz = light direction (normalized), w = intensity */
    glm::vec4 dir_color              = glm::vec4(0.0f); /**< xyz = RGB color, w = unused */
    /**< Directional/point soft-shadow tuning. Its position in the struct is
     * fixed: every shader-side LightUBO block (gfxcoopa's probe_capture.frag,
     * toyengine's and blendy's) must byte-match this layout, so moving it shifts every later field.
     *   x = directional shadow intensity, 0..1 (DirectionalLightComponent::shadow_intensity)
     *   y = point-light PCF disk radius, as a tangent-space offset on a unit sample
     *       direction (toyengine converts this from ToyRenderConfig::point_shadow_softness
     *       texels each frame, the same way dir_shadow_params.y below converts
     *       shadow_softness from world units); 0 = single hard compare
     *   z = directional PCF tap count (float; int()-cast in the shader)
     *   w = per-frame golden-angle rotation offset, for TAA decorrelation
     * A consumer that never writes this (e.g. blendy, which transports the
     * same values via its own push constants) gets the default, which is a
     * fully dark, hard-compared shadow.
     * Spot shadows reuse z/w (PCF taps, rotation offset) rather than adding their
     * own copies -- see spot_shadow_params below for the spot-specific fields. */
    glm::vec4 dir_shadow_extra = glm::vec4(1.0f, 0.0f, 16.0f, 0.0f);
    glm::mat4 dir_light_space_matrix = glm::mat4(1.0f); /**< Light projection * view matrix for directional shadows */
    glm::vec4 dir_shadow_params      = glm::vec4(0.005f, 0.0f, 1.0f, 0.0f); /**< x=bias, y=pcf_radius_texels (0=hard), z=shadow_enabled (1 or 0), w=normal_bias */

    // Light counts, Point Lights and Spot Lights
    glm::uvec4 light_counts = glm::uvec4(0); /**< x = num_directional (0 or 1), y = num_point_lights,
                                                   z = num_spot_lights, w = index into spot_lights[] of
                                                   the one spot that owns the shadow map, or 0xFFFFFFFF
                                                   if none does (see find_first_shadow_casting_spot_light_
                                                   in toy_render_pipeline.h) */
    PointLightGPU point_lights[MAX_POINT_LIGHTS];

    // Configurable sky/ambient colour (see IndirectParams in render_features.h).
    // Appended after point_lights so no existing member's offset moves --
    // a shader that doesn't care about sky colour can keep declaring this
    // block without these three fields; std140 only requires a matching
    // *prefix*.
    glm::vec4 sky_zenith  = glm::vec4(0.05f, 0.18f, 0.55f, 0.0f);  /**< xyz = zenith colour, straight up. */
    glm::vec4 sky_horizon = glm::vec4(0.25f, 0.35f, 0.45f, 0.0f);  /**< xyz = horizon colour. */
    glm::vec4 sky_ground  = glm::vec4(0.05f, 0.045f, 0.04f, 0.0f); /**< xyz = ground colour, straight down. */

    // Spot Lights -- appended after sky_ground for the same reason point_lights'
    // doc gives: nothing above this line moves, so a shader with no spot support
    // (one whose block stops at sky_ground) keeps compiling against the shorter prefix
    // unchanged.
    glm::mat4     spot_light_space_matrix = glm::mat4(1.0f); /**< Light projection * view matrix for the one shadow-casting spot (see light_counts.w). */
    glm::vec4     spot_shadow_params      = glm::vec4(0.005f, 0.0f, 0.0f, 0.05f); /**< x=bias, y=PCF penumbra scale K in texels*distance -- the shader divides by the fragment's light-space depth for a constant world-width penumbra; 0=hard (see calc_spot_shadow in toy_shadow_body.glsl), z=shadow_enabled (1 or 0), w=normal_bias */
    SpotLightGPU  spot_lights[MAX_SPOT_LIGHTS];

    // Appended after spot_lights per this struct's own append-only rule: consumers
    // with no PCSS/contact-shadow support keep compiling against the shorter prefix.
    glm::vec4     pcss_params    = glm::vec4(0.0f, 0.0f, 8.0f, 8.0f); /**< PCSS contact hardening (calc_dir_shadow):
                                        x=enabled (1 or 0), y=penumbra texels per unit [0,1] light-space
                                        depth gap, for CASCADE 0 (the per-cascade values live in
                                        dir_cascade_pcss_scale below), z=blocker search radius in texels,
                                        w=blocker search tap count (the quality dial -- the search runs on
                                        every shadowed pixel). */
    glm::vec4     contact_params = glm::vec4(0.0f, 0.5f, 0.15f, 8.0f); /**< Screen-space contact shadows
                                        (toy_lighting.frag): x=strength (0 disables), y=march length in
                                        world units, z=thickness tolerance in world units, w=step count. */
    glm::vec4     contact_soft_params = glm::vec4(0.0f); /**< Soft contact-shadow penumbra
                                        (toy_lighting.frag): x=cone half-angle tangent (the sun's angular
                                        size, shadow_pcss_light_size, when soft_shadows is on; 0 selects the
                                        hard single-ray march). y = per-frame step-phase rotation (frame
                                        index while the contact resolve accumulates, else 0). z/w reserved. */

    // --- Directional shadow cascades ---
    //
    // Appended per this struct's own append-only rule. The directional shadow map is
    // an ATLAS of up to MAX_DIR_CASCADES tiles (see ShadowMapTarget), each tile a separate
    // ortho fit to one slice of the camera's depth range, so near-camera geometry gets a
    // box a few metres across while distant geometry keeps a coarse one.
    //
    // dir_light_space_matrix / dir_shadow_params.y / dir_shadow_params.w / pcss_params.y
    // above hold CASCADE 0's values. A consumer with no cascade support therefore keeps
    // shading against the near cascade through the shorter prefix, rather than reading
    // garbage.
    glm::mat4 dir_cascade_matrix[MAX_DIR_CASCADES] = {
        glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)
    };                                              /**< Per-cascade light projection * view;
                                        [0] duplicates dir_light_space_matrix. Unused slots
                                        hold the last live cascade's matrix. */
    glm::vec4 dir_cascade_pcf_texels   = glm::vec4(0.0f); /**< Per-cascade PCF radius in ATLAS
                                        texels (0 = hard compare). Per-cascade because each
                                        tile has its own world-per-texel scale, and
                                        shadow_softness is a WORLD-space width. */
    glm::vec4 dir_cascade_normal_bias  = glm::vec4(0.0f); /**< Per-cascade normal-offset bias in
                                        world units (compute_shadow_normal_bias() against that
                                        cascade's own texel size). */
    glm::vec4 dir_cascade_pcss_scale   = glm::vec4(0.0f); /**< Per-cascade PCSS penumbra texels
                                        per unit [0,1] light-space depth gap -- pcss_params.y's
                                        quantity, resolved per tile. */
    glm::vec4 dir_cascade_info = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f); /**< x = live cascade count,
                                        y = tiles per atlas row (grid_x), z = selection inset in
                                        TILE uv (keeps a PCF disk from reaching out of its tile),
                                        w = dither-transition band width in tile uv. */
    glm::vec4 dir_shadow_receiver = glm::vec4(0.0f, 4.0f, 0.0f, 0.0f); /**< Receiver-plane depth
                                        bias for the directional PCF: x = 1 to tilt every tap's
                                        compare depth along the receiver's own plane (so the
                                        normal offset no longer has to clear the PCF disk), y =
                                        steepest receiver slope honoured, as tan(angle to the
                                        light's perpendicular plane). z/w reserved. */

    // --- Directional shadow bias and fade (appended per the append-only rule) ---
    glm::vec4 dir_cascade_depth_bias = glm::vec4(0.0f); /**< Per-cascade size of ONE shadow texel in
                                        that cascade's [0,1] light-space depth (texel world size /
                                        depth range). The shader's depth bias is a texel count times
                                        this, so it scales with each cascade's resolution instead of
                                        being one normalised constant that means centimetres in one
                                        cascade and decimetres in the next. */
    glm::vec4 dir_shadow_bias_texels = glm::vec4(1.0f, 2.0f, 5.0f, 0.0f); /**< x = constant depth
                                        bias in texels, y = slope depth bias in texels per unit
                                        tan(angle between N and L), z = largest tan honoured, w
                                        reserved. Shared by the directional and local-light maps. */
    glm::vec4 dir_shadow_fade = glm::vec4(0.0f); /**< xyz = camera world position, w = distance at
                                        which directional shadows have fully faded (0 = no distance
                                        fade). */
    glm::vec4 dir_shadow_fade_params = glm::vec4(0.0f, 0.1f, 0.0f, 0.0f); /**< x = distance where
                                        the fade starts, y = width of the outer band of the LAST
                                        cascade's tile (tile uv) over which shadows fade out, z = 1
                                        to blend two cascades across the transition band, 0 to
                                        dither between them for TAA to resolve, w reserved. */

    // --- Point/spot shadows: the local-light shadow atlas ---
    LocalShadowBlock local_shadows;

    // --- Physical sky (appended last per the append-only rule) ---
    // Read by an app's sky drawing (toyengine's physical sky, sky_physical.glsl). The defaults
    // (sky_params.x = 0) select the app's plain gradient sky.
    glm::vec4 sky_sun    = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f); /**< xyz = unit direction TO the sun,
                                        w = cos of the sun disc's angular radius. */
    glm::vec4 sky_moon   = glm::vec4(0.0f, 0.0f, -1.0f, 1.0f); /**< xyz = unit direction TO the moon,
                                        w = cos of the moon disc's angular radius. */
    glm::vec4 sky_params = glm::vec4(0.0f); /**< x = 1 physical sky (0 = gradient), y = 1 clouds
                                        composited, z = star visibility 0..1, w = sky illuminance
                                        (scales the sky-view LUT). */
    glm::vec4 sky_extra  = glm::vec4(0.0f); /**< x = sun disc radiance, y = moon disc radiance,
                                        z = night-sky floor, w = time in seconds (star twinkle). */

    // --- Global exponential height fog (appended last per the append-only rule) ---
    // In the light UBO rather than a UBO of its own because every shader that fogs reads it:
    // the opaque fog pass (FogPass) AND each forward shader (BLEND meshes, water, particles,
    // SDF glass), which fogs its own fragment at its own distance. See toyengine's
    // assets/shaders/gfx/fog.glsl for the model and GfxFogBlock (gfx/fog_types.glsl) for the
    // GLSL mirror. fog_color.w = 0 (the default) disables fog everywhere.
    glm::vec4 fog_color   = glm::vec4(0.0f); /**< rgb = in-scatter colour, w = 1 enabled / 0 off. */
    glm::vec4 fog_density = glm::vec4(0.0f); /**< x = mode (0 Linear, 1 Exponential), y = density
                                                  (extinction per metre at/below the height base),
                                                  z = linear start, w = linear end. */
    glm::vec4 fog_height  = glm::vec4(0.0f); /**< x = height base (world Z; constant density below),
                                                  y = height falloff in metres (<= 0: flat fog),
                                                  z = sky blend, w = max opacity. */
    glm::vec4 fog_range   = glm::vec4(0.0f); /**< x = start distance, y = cutoff distance (0 = none),
                                                  z = sky distance (sky pixels integrate to it), w unused. */
    glm::vec4 fog_sun     = glm::vec4(0.0f); /**< rgb = sun colour * intensity * sun amount
                                                  (directional in-scatter), w = HG anisotropy g. */
    glm::vec4 fog_sun_dir = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f); /**< xyz = direction the sunlight
                                                  travels, w = directional in-scatter start distance. */
    glm::vec4 fog_water   = glm::vec4(0.0f); /**< x = water surface level (world Z), y = 1 when the
                                                  camera is under that surface: fog then integrates
                                                  only the part of each ray above the water. */

    // --- An app's cloud shadow map (appended last per the append-only rule) ---
    // toyengine's cloud layer shadows the directional light through a map of the light's
    // transmittance on the layer's base plane (its assets/shaders/cloud_shadow.glsl).
    // cloud_shadow.w = 0 (the default) disables it.
    glm::vec4 cloud_shadow       = glm::vec4(0.0f); /**< xy = the map's corner (world), z = 1 / its side,
                                                         w = strength (0 = off). */
    glm::vec4 cloud_shadow_layer = glm::vec4(0.0f); /**< x = the layer's base height (world z),
                                                         y = its thickness, z = low-light fade 0..1, w unused. */

    // --- Per-material toon shading, scene-wide tuning (appended last per the append-only rule) ---
    // The per-material part (how many bands, which specular shape, how much rim) rides the
    // G-buffer's emissive alpha as a packed code -- see PBRMaterial::gpu_toon_code(). These are
    // the continuous knobs that would not survive that packing, shared by every band in the
    // frame so a scene can retune the whole look without re-authoring materials. See toyengine's
    // assets/shaders/gfx/toon.glsl. All zero (toon_params0.w = 0) makes every toon material
    // shade as plain PBR, whatever its own code says.
    glm::vec4 toon_params0 = glm::vec4(0.0f); /**< x = specular step threshold as a fraction of the
                                                   GGX lobe's own peak, y = that step's edge width,
                                                   z = rim falloff exponent, w = master strength
                                                   (0 disables toon shading frame-wide). */
    glm::vec4 toon_params1 = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f); /**< rgb = shadow tint the darkest
                                                   bands are pulled toward (white = no tint),
                                                   w = 1 bands point/spot distance falloff too. */

    // --- Extended directional shadow levels (appended last per the append-only rule) ---
    //
    // The FULL level set, 0..dir_level_info.x-1 -- not merely the levels past the fourth.
    // Every toyengine shading path reads THESE; the dir_cascade_* block above keeps holding
    // levels 0..min(count,4)-1 so a consumer compiled against the shorter std140 prefix
    // (gfxcoopa's probe_capture.frag, blendy) still shades from the near levels rather than
    // reading garbage. The CPU fills both every frame, so nothing needs a dual path.
    //
    // Per-level scalars are packed four to a vec4 -- level L is [L >> 2][L & 3] -- because
    // std140 gives a float[] a 16-byte stride, which would waste 3/4 of each array.
    glm::mat4 dir_level_matrix[MAX_DIR_SHADOW_LEVELS] = {
        glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f),
        glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)
    };                                                     /**< Per-level light projection * view.
                                        Slots past the live count repeat the last live level. */
    glm::vec4 dir_level_pcf_texels[MAX_DIR_SHADOW_LEVELS / 4]  = {glm::vec4(0.0f), glm::vec4(0.0f)};
                                                           /**< Per-level PCF radius in atlas
                                        texels (0 = hard compare); dir_cascade_pcf_texels' quantity. */
    glm::vec4 dir_level_normal_bias[MAX_DIR_SHADOW_LEVELS / 4] = {glm::vec4(0.0f), glm::vec4(0.0f)};
                                                           /**< Per-level normal offset, world units. */
    glm::vec4 dir_level_pcss_scale[MAX_DIR_SHADOW_LEVELS / 4]  = {glm::vec4(0.0f), glm::vec4(0.0f)};
                                                           /**< Per-level PCSS penumbra texels per
                                        unit [0,1] light-space depth gap. */
    glm::vec4 dir_level_depth_bias[MAX_DIR_SHADOW_LEVELS / 4]  = {glm::vec4(0.0f), glm::vec4(0.0f)};
                                                           /**< Per-level size of one texel in that
                                        level's [0,1] light depth. */
    glm::vec4 dir_level_info = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f); /**< x = live level count,
                                        y = tiles per atlas row (grid_x), z = selection inset in
                                        TILE uv, w = transition band in tile uv. Supersedes
                                        dir_cascade_info, which keeps the first four levels' copy. */
    glm::vec4 dir_level_age[MAX_DIR_SHADOW_LEVELS / 4] = {glm::vec4(0.0f), glm::vec4(0.0f)};
                                                           /**< Per-level frames since its tile was
                                        last re-rendered (0 = this frame). Drives the ShadowCascades
                                        debug view's staleness tint; ignored by shading. */
};

// Pins the offset of sky_zenith -- where a shader LightUBO prefix with no spot support
// ends -- immediately after point_lights, so an edit that inserts something ahead of it
// (rather than appending at the end) fails to compile instead of silently desyncing every
// hand-written GLSL block from the C++ layout.
static_assert(offsetof(LightUBO, sky_zenith) == offsetof(LightUBO, point_lights) + sizeof(PointLightGPU) * MAX_POINT_LIGHTS,
    "LightUBO::sky_zenith moved -- every shader LightUBO block (gfxcoopa/toyengine/blendy) "
    "byte-matches this prefix; see the point_lights/sky_zenith comment above.");
static_assert(offsetof(LightUBO, spot_lights) == offsetof(LightUBO, spot_light_space_matrix) + sizeof(glm::mat4) + sizeof(glm::vec4),
    "LightUBO::spot_lights must immediately follow spot_light_space_matrix/spot_shadow_params.");
static_assert(offsetof(LightUBO, contact_soft_params) == offsetof(LightUBO, contact_params) + sizeof(glm::vec4),
    "LightUBO::contact_soft_params must immediately follow contact_params.");
static_assert(offsetof(LightUBO, dir_cascade_matrix) == offsetof(LightUBO, contact_soft_params) + sizeof(glm::vec4),
    "LightUBO's cascade block must immediately follow contact_soft_params -- appended last per the "
    "append-only rule, so every shorter shader prefix still byte-matches.");
static_assert(offsetof(LightUBO, dir_cascade_info) ==
                  offsetof(LightUBO, dir_cascade_matrix) + sizeof(glm::mat4) * MAX_DIR_CASCADES + sizeof(glm::vec4) * 3,
    "LightUBO's cascade block is packed tighter or looser than the std140 GLSL block in "
    "light_ubo_body.glsl expects (mat4[4] has a 64-byte stride there too).");

static_assert(offsetof(LightUBO, sky_sun) == offsetof(LightUBO, local_shadows) + sizeof(LocalShadowBlock),
    "LightUBO's physical-sky block must immediately follow local_shadows (append-only rule).");
static_assert(offsetof(LightUBO, fog_color) == offsetof(LightUBO, sky_extra) + sizeof(glm::vec4),
    "LightUBO's fog block must immediately follow sky_extra (append-only rule).");
static_assert(offsetof(LightUBO, cloud_shadow) == offsetof(LightUBO, fog_water) + sizeof(glm::vec4),
    "LightUBO's cloud shadow block must immediately follow the fog block (append-only rule).");
static_assert(sizeof(LocalShadowBlock) % 16 == 0, "LocalShadowBlock must keep std140's 16-byte stride.");
static_assert(offsetof(LightUBO, dir_level_matrix) == offsetof(LightUBO, toon_params1) + sizeof(glm::vec4),
    "LightUBO's extended directional-level block must immediately follow toon_params1 -- appended "
    "last per the append-only rule, so every shorter shader prefix still byte-matches.");
static_assert(offsetof(LightUBO, dir_level_info) ==
                  offsetof(LightUBO, dir_level_matrix) + sizeof(glm::mat4) * MAX_DIR_SHADOW_LEVELS +
                      sizeof(glm::vec4) * (MAX_DIR_SHADOW_LEVELS / 4) * 4,
    "LightUBO's extended level block is packed tighter or looser than the std140 GLSL block in "
    "light_ubo_body.glsl expects (mat4[8] has a 64-byte stride there too).");

/**
 * @class LightData
 * @brief Manages the per-frame directional light uniform buffer.
 *
 * Holds a host-visible uniform buffer containing LightUBO data.
 * Call upload() once per frame after modifying the light parameters.
 */
class LightData {
public:
    /**
     * @brief Creates the light uniform buffer.
     * @param device    Logical device.
     * @param allocator VMA allocator.
     */
    LightData(core::Device& device, memory::Allocator& allocator)
        : buffer_(memory::Buffer::uniform(device, allocator, sizeof(LightUBO)))
    {
        // Upload default light on creation.
        upload();
    }

    LightData(const LightData&) = delete;
    LightData& operator=(const LightData&) = delete;

    /**
     * @brief Returns the mutable light UBO data (modify then call upload()).
     */
    LightUBO& data() { return data_; }

    /**
     * @brief Returns the immutable light UBO data.
     */
    const LightUBO& data() const { return data_; }

    /**
     * @brief Uploads the current LightUBO to the GPU buffer.
     *
     * Call this once per frame (or whenever the light parameters change).
     */
    void upload() {
        buffer_.upload(&data_, sizeof(LightUBO));
    }

    /**
     * @brief Returns the underlying uniform buffer for descriptor binding.
     */
    const memory::Buffer& buffer() const { return buffer_; }

private:
    LightUBO       data_;   /**< Host-side light parameters. */
    memory::Buffer buffer_; /**< GPU-side uniform buffer. */
};

} // namespace data
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_ENGINE_LIGHT_DATA_H

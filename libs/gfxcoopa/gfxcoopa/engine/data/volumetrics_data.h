/**
 * @file volumetrics_data.h
 * @brief Per-frame uniform buffer for the raymarched local volume pass.
 *
 * Self-contained (own inv_view_proj/camera_pos rather than extending CameraData
 * in camera_ubo.h): adding it never touches the
 * std140 layout ~10 existing shaders already share via CameraUBO/LightData.
 *
 * There is deliberately NO global term here. Fog (LightUBO's fog block) is the global,
 * analytic, config-driven atmosphere; everything in this buffer is a BOUNDED,
 * scene-placed volume. That split is why a "layer" and a "volume" are the same
 * object in this design -- each volume carries its own complete field
 * description alongside its bounds, rather than referring to a separate global
 * layer array.
 */

#ifndef GFXCOOPA_ENGINE_DATA_VOLUMETRICS_DATA_H
#define GFXCOOPA_ENGINE_DATA_VOLUMETRICS_DATA_H

#include <glm/glm.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
// For MAX_DIR_CASCADES: the march's sun term samples the SAME directional shadow atlas the
// surface lighting does, so the cascade count is one shared constant, not a second copy.
#include <gfxcoopa/engine/data/light_data.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace data {

/// Maximum simultaneous local volumes. A plain UBO array (not an SSBO)
/// deliberately -- see VolumetricsPass's doc for why this stays a uniform binding.
static constexpr uint32_t MAX_VOLUMES = 8;

/// Maximum point/spot lights that in-scatter into the march. Small on purpose:
/// each one costs a distance/falloff evaluation per march step per pixel.
static constexpr uint32_t MAX_SCATTER_LIGHTS = 4;

/**
 * @struct ScatterLightGPU
 * @brief std140-aligned point or spot light that in-scatters into local volumes.
 *
 * A trimmed copy of LightUBO's per-light data (light_data.h) carrying only what
 * the march needs, so the volumetrics UBO stays self-contained (same reasoning
 * as VolumetricsUBO's own inv_view_proj) and the caller controls which of the
 * scene's lights are worth a per-step cost.
 */
struct alignas(16) ScatterLightGPU {
    glm::vec4 position_range  = glm::vec4(0.0f); /**< xyz = world position, w = range. */
    glm::vec4 color_intensity = glm::vec4(0.0f); /**< rgb = colour, w = intensity (LightUBO's units). */
    glm::vec4 direction_cone  = glm::vec4(0.0f); /**< xyz = spot direction, w = cos(outer half-angle).
                                                   Ignored for point lights (params.z = 0). */
    glm::vec4 params          = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f); /**< x = falloff sharpness (same curve as the
                                                   lighting pass's point loop), y = cos(inner half-angle),
                                                   z = 1 spot / 0 point, w = the light's shadow slot in
                                                   local_shadows, 1-based (0 = unshadowed). */
};

/**
 * @struct VolumeGPU
 * @brief std140-aligned local volume: bounds plus a complete field description.
 *
 * `mode_params.x` selects the density function (see VolumeKind in
 * components/volume.h and gfx/volumetrics.glsl): 0 uniform, 1 ridged ribbons,
 * 2 smooth billows. Every other field means the same thing for all three, which
 * is what lets one march serve all of them.
 */
struct alignas(16) VolumeGPU {
    glm::mat4 inv_world       = glm::mat4(1.0f); /**< World -> volume local space (TRS inverse). */
    glm::vec4 extent_shape    = glm::vec4(1.0f); /**< xyz = local half-extent (Sphere uses .x as radius),
                                                   w = 0 Box / 1 Sphere. */
    glm::vec4 direction_speed = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f); /**< xyz = NORMALIZED advection direction,
                                                   w = speed in world units per second. */
    glm::vec4 field_params    = glm::vec4(0.0f); /**< x = noise scale (for ridged noise this sets feature
                                                   THICKNESS as well as spacing), y = streak (elongation
                                                   along the direction; 1 = isotropic), z = coverage
                                                   (fraction of the volume with no density),
                                                   w = fbm amplitude gain per octave. */
    glm::vec4 shape_params    = glm::vec4(0.0f); /**< x = peak density (extinction per world unit),
                                                   y = height base (world Z; engine is Z-up),
                                                   z = height falloff in world units (<= 0 disables),
                                                   w = fbm octave count. */
    glm::vec4 flow_params     = glm::vec4(0.0f); /**< x = meander amplitude in WORLD UNITS (0 disables the
                                                   domain warp), y = meander frequency, z = ridge sharpness,
                                                   w = sparsity-gate frequency (<= 0 disables the gate). */
    glm::vec4 color_occlusion = glm::vec4(0.0f); /**< rgb = scatter colour, w = occlusion scale: how much of
                                                   the extinction actually veils the scene behind it.
                                                   0 = pure additive glow, 1 = a full participating medium. */
    glm::vec4 mode_params     = glm::vec4(0.0f); /**< x = kind (0 Fog / 1 Wind / 2 Haze),
                                                   y = Henyey-Greenstein sun in-scatter strength,
                                                   z = edge softness, a fraction of a dimensionless [0,1]
                                                   surface metric (0 = hard, 1 = maximally soft). w unused. */
};

/**
 * @struct VolumetricsUBO
 * @brief std140-aligned shared march state plus up to MAX_VOLUMES local volumes.
 *
 * Must match gfx/volumetrics_ubo.glsl's VolumetricsUBO block field for field. Scalars are
 * packed into vec4s alongside related fields, matching LightUBO.
 */
struct alignas(16) VolumetricsUBO {
    glm::mat4 inv_view_proj = glm::mat4(1.0f); /**< Clip -> world, for reconstructing per-pixel view rays.
                                                 Built from the UNJITTERED projection: these fields are
                                                 sampled in world space, so TAA jitter here would make them
                                                 swim against the pixel grid on top of their own motion. */
    glm::vec4 camera_pos    = glm::vec4(0.0f); /**< xyz = world camera position, w = debug view flag
                                                 (> 0.5 outputs accumulated density alone). */
    glm::vec4 sun_direction = glm::vec4(0.0f); /**< xyz = direction the light travels (matches LightUBO). */
    glm::vec4 sun_color     = glm::vec4(0.0f); /**< rgb = sun colour * intensity, for the HG in-scatter tint. */
    glm::vec4 march_params  = glm::vec4(0.0f); /**< x = raymarch step count, y = max march distance,
                                                 z = max opacity, w = HG anisotropy g. Anisotropy is SHARED
                                                 rather than per-volume: it is a property of the light's
                                                 phase function, not of which medium a sample sits in, and
                                                 hoisting it keeps the phase evaluated once per pixel. */
    glm::vec4 time_params   = glm::vec4(0.0f); /**< x = elapsed time (seconds; drives advection),
                                                 y = delta time, z = frame index (dither shift). w unused. */
    glm::vec4 counts        = glm::vec4(0.0f); /**< x = active volume count, y = active scatter light
                                                 count, z = light-scatter strength (0 disables the
                                                 scatter-light loop entirely). w unused. */
    VolumeGPU volumes[MAX_VOLUMES];            /**< Only the first counts.x are read. */

    // Shadowing for the march's in-scatter terms. Appended after `volumes` so no
    // pre-existing field moves -- the same std140 matching-prefix rule LightUBO
    // documents (light_data.h).
    glm::mat4 dir_light_space_matrix  = glm::mat4(1.0f); /**< World -> directional shadow clip (LightUBO's copy). */
    glm::mat4 spot_light_space_matrix = glm::mat4(1.0f); /**< World -> spot shadow clip (LightUBO's copy). */
    glm::vec4 shadow_params = glm::vec4(0.0f, 1.0f, 0.002f, 0.002f); /**< x = 1 to shadow the sun term (dir map bound,
                                                 shadows on), y = shadow strength (dir shadow_intensity *
                                                 the volumetric-shadow config strength), z = dir depth
                                                 bias, w = spot depth bias. */
    ScatterLightGPU scatter_lights[MAX_SCATTER_LIGHTS]; /**< Only the first counts.y are read. */

    // Directional shadow cascades, appended last per the matching-prefix rule above. The
    // directional shadow map is an atlas of per-slice tiles (see ShadowMapTarget), so a
    // march point past the near cascade has to be projected with ITS cascade's matrix and
    // remapped into ITS tile -- dir_light_space_matrix above is only cascade 0's. Copied
    // verbatim from LightUBO's fields of the same name, so the shaft and the surface
    // shadow agree by construction.
    glm::mat4 dir_cascade_matrix[MAX_DIR_CASCADES] = {
        glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)
    };
    glm::vec4 dir_cascade_info = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f); /**< x = cascade count,
                                                 y = tiles per atlas row, z = selection inset in
                                                 tile uv, w = dither band (unused here -- the
                                                 march has no TAA behind it to resolve a dither). */

    // Froxel mode (FroxelVolumetricsPass), appended last per the matching-prefix rule. The
    // raymarcher ignores all three.
    glm::mat4 prev_view_proj = glm::mat4(1.0f); /**< LAST frame's unjittered view-projection, for
                                                 reprojecting a froxel into the previous grid. */
    glm::vec4 froxel_grid   = glm::vec4(0.0f); /**< x = W, y = H (froxels per slice), z = D (slices),
                                                 w = slices per atlas row. */
    glm::vec4 froxel_params = glm::vec4(0.0f); /**< x = grid near distance, y = grid far distance,
                                                 z = temporal history weight, w = 1 when last frame's
                                                 grid is valid history. */
    glm::vec4 prev_camera_pos = glm::vec4(0.0f); /**< xyz = LAST frame's camera position (a froxel's
                                                 previous slice is its distance from there). */
    glm::vec4 froxel_params2 = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f); /**< x = samples per froxel where
                                                 there is no history, y = composite lookup jitter in
                                                 froxels / slices (0 = off; only useful under TAA). */
    /** Point/spot shadows -- LightUBO::local_shadows' copy, so a scatter light whose slot is set
     *  is shadowed from the same local-light atlas the lighting pass samples. */
    LocalShadowBlock local_shadows;
    /** An app's cloud shadow map over the sun's in-scatter -- LightUBO::cloud_shadow /
     *  cloud_shadow_layer's copy (toyengine's cloud_shadow.glsl). cloud_shadow.w = 0: off. */
    glm::vec4 cloud_shadow       = glm::vec4(0.0f);
    glm::vec4 cloud_shadow_layer = glm::vec4(0.0f);
};

/**
 * @class VolumetricsData
 * @brief Manages the per-frame volumetrics uniform buffer.
 *
 * A host-visible, persistently-mapped uniform buffer,
 * re-uploaded once per frame. Fill data()'s fields directly, then upload().
 *
 * CAVEAT: SINGLE-buffered (one UBO, not one
 * per frame in flight), so inv_view_proj can be one frame stale under fast camera
 * motion. VolumetricsPass binds its descriptor set once at construction, so making
 * this per-slot needs an additive API change for a hazard a low-frequency
 * volumetric does not visibly exercise.
 */
class VolumetricsData {
public:
    /**
     * @brief Creates the volumetrics uniform buffer.
     * @param device    Logical device.
     * @param allocator VMA allocator.
     */
    VolumetricsData(core::Device& device, memory::Allocator& allocator)
        : buffer_(memory::Buffer::uniform(device, allocator, sizeof(VolumetricsUBO)))
    {
        buffer_.upload(&data_, sizeof(VolumetricsUBO));
    }

    VolumetricsData(const VolumetricsData&) = delete;
    VolumetricsData& operator=(const VolumetricsData&) = delete;

    /** @brief Mutable access to the host-side data, filled by the caller before upload(). */
    VolumetricsUBO& data() { return data_; }

    /** @brief Uploads the current host-side data to the GPU. Call once per frame. */
    void upload() { buffer_.upload(&data_, sizeof(VolumetricsUBO)); }

    /** @brief Returns the underlying uniform buffer for descriptor binding. */
    const memory::Buffer& buffer() const { return buffer_; }

private:
    VolumetricsUBO data_;
    memory::Buffer buffer_;
};

} // namespace data
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_DATA_VOLUMETRICS_DATA_H

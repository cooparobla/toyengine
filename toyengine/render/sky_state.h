/**
 * @file sky_state.h
 * @brief What the physical sky (render `sky_model: physical`) needs from outside the renderer
 *        each frame: the atmosphere's media, the sun and moon, and the light levels the CPU
 *        atmosphere model (toyengine/weather/atmosphere_model.h) worked out for them.
 *
 * Plain data, like WaterFrameState / SurfaceFrameState: Engine fills it every frame
 * (Engine::sync_sky_render_state_()) and hands it over with PixelRenderPipeline::set_sky_state(),
 * so render/ never depends on the weather module.
 */

#ifndef TOYENGINE_RENDER_SKY_STATE_H
#define TOYENGINE_RENDER_SKY_STATE_H

#include <glm/glm.hpp>

namespace toy {
namespace render {

/**
 * @brief The atmosphere's scattering media, in kilometres (Hillaire 2020's Earth defaults).
 *
 * Shared by the GPU LUT passes (as SkyAtmospherePass::AtmosphereGpu) and the CPU model, so the
 * two can never describe different skies.
 */
struct SkyAtmosphereMedia {
    glm::vec3 rayleigh_scattering{5.802e-3f, 13.558e-3f, 33.1e-3f};  ///< 1/km at sea level.
    float     rayleigh_scale_height = 8.0f;                          ///< km.
    glm::vec3 mie_scattering{3.996e-3f};                             ///< 1/km at sea level.
    glm::vec3 mie_extinction{4.40e-3f};                              ///< 1/km at sea level.
    float     mie_scale_height = 1.2f;                               ///< km.
    float     mie_g = 0.8f;                                          ///< Forward-scattering asymmetry.
    glm::vec3 ozone_absorption{0.650e-3f, 1.881e-3f, 0.085e-3f};     ///< 1/km at the layer's peak (25 km).
    float     bottom_radius = 6360.0f;                               ///< Planet radius, km.
    float     top_radius = 6460.0f;                                  ///< Top of the atmosphere, km.
    glm::vec3 ground_albedo{0.3f};

    /**
     * @brief The Earth defaults with render `atmosphere_density` (haze: multiplies the aerosols,
     *        and a little of the air) and `ozone` (multiplies the ozone layer) applied.
     */
    static SkyAtmosphereMedia earth(float density, float ozone) {
        SkyAtmosphereMedia m;
        const float d = glm::clamp(density, 0.0f, 20.0f);
        m.mie_scattering *= d;
        m.mie_extinction *= d;
        // Thick haze also scatters more broadly (a whiter, lower-contrast sky).
        m.mie_g = glm::mix(0.8f, 0.7f, glm::clamp((d - 1.0f) / 9.0f, 0.0f, 1.0f));
        m.rayleigh_scattering *= glm::mix(1.0f, glm::clamp(d, 0.5f, 2.0f), 0.25f);
        m.ozone_absorption *= glm::clamp(ozone, 0.0f, 10.0f);
        return m;
    }

    bool operator==(const SkyAtmosphereMedia& o) const {
        return rayleigh_scattering == o.rayleigh_scattering && rayleigh_scale_height == o.rayleigh_scale_height &&
               mie_scattering == o.mie_scattering && mie_extinction == o.mie_extinction &&
               mie_scale_height == o.mie_scale_height && mie_g == o.mie_g && ozone_absorption == o.ozone_absorption &&
               bottom_radius == o.bottom_radius && top_radius == o.top_radius && ground_albedo == o.ground_albedo;
    }
    bool operator!=(const SkyAtmosphereMedia& o) const { return !(*this == o); }
};

/**
 * @brief One frame of the physical sky. `active` false (the default) draws the gradient sky.
 */
struct SkyFrameState {
    bool active = false;              ///< The physical sky is on and these values are this frame's.
    bool clouds = false;              ///< Draw and composite the cloud layer.
    SkyAtmosphereMedia media;

    glm::vec3 sun_to{0.0f, 0.0f, 1.0f};    ///< Unit direction TO the sun.
    glm::vec3 moon_to{0.0f, 0.0f, -1.0f};  ///< Unit direction TO the moon (the sun's opposite).
    float sky_illuminance = 1.0f;     ///< Sun illuminance the sky-view LUT is scaled by (engine units).
    float moon_ratio = 0.0f;          ///< Moon illuminance / sun illuminance, for the LUT's moonlit sky.
    float sun_disc_radiance = 0.0f;   ///< The sun disc's brightness above the atmosphere.
    float moon_disc_radiance = 0.0f;  ///< The moon disc's brightness above the atmosphere.
    float sun_disc_cos = 1.0f;        ///< cos of the sun disc's angular radius.
    float moon_disc_cos = 1.0f;       ///< cos of the moon disc's angular radius.
    float star_visibility = 0.0f;     ///< 0 daylight / overcast .. 1 a clear night.
    float night_floor = 0.0f;         ///< Airglow floor so a moonless night sky is not pure black.

    /// Multiplies the directional light's colour wherever the renderer reads it (lighting, fog,
    /// volumetrics, water): the air's transmittance toward the light, and the cloud cover's dimming.
    glm::vec3 light_tint{1.0f};

    // --- Cloud layer ---
    float cloud_coverage = 0.0f;      ///< 0..1.
    float cloud_altitude = 1500.0f;   ///< m, base of the layer.
    float cloud_thickness = 1200.0f;  ///< m.
    float cloud_density = 1.0f;
    glm::vec2 cloud_offset{0.0f};     ///< Accumulated wind drift (m).
    glm::vec3 cloud_light_to{0.0f, 0.0f, 1.0f};  ///< The light the clouds are lit by (sun, or the moon at night).
    glm::vec3 cloud_light_color{0.0f};           ///< Its illuminance above the atmosphere (engine units).
    float time = 0.0f;                ///< Seconds; cloud evolution and star twinkle.
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_SKY_STATE_H

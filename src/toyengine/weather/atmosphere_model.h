/**
 * @file atmosphere_model.h
 * @brief The physical sky on the CPU: the same atmosphere the GPU draws (Hillaire 2020 -- see
 *        assets/shaders/sky_atmosphere.glsl), evaluated for the few numbers the rest of the
 *        renderer needs to agree with it.
 *
 * With render `sky_model: physical` the lighting pass draws the sky from GPU tables, but many
 * consumers still read the three-colour sky gradient (render sky_zenith / sky_horizon /
 * sky_ground): ambient and indirect specular, the SSR fallback, fog's sky blend, transparent and
 * water shading, particles. The engine fills those colours from this model every frame, so they
 * are the sky actually drawn -- and the sun's colour comes from the air's transmittance toward it,
 * so a sunset sky lights the scene orange (Engine::sync_sky_render_state_()).
 *
 * Same media, same parameterisations and the same integration as the shaders: a transmittance
 * table (64 x 16, Bruneton's mapping) and a multiple-scattering table (16 x 16), rebuilt when the
 * media change (a few milliseconds), then single + multiple scattering along a view ray from the
 * sun and the moon. Pure math: no scene, no GPU.
 *
 * Units: kilometres inside, engine Z-up; radiance is per unit of sun illuminance (callers scale).
 */

#ifndef TOYENGINE_WEATHER_ATMOSPHERE_MODEL_H
#define TOYENGINE_WEATHER_ATMOSPHERE_MODEL_H

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/render/sky_state.h>

namespace toy {
namespace weather {

/** @brief The gradient colours a sky reduces to, per unit of sun illuminance. */
struct SkyGradient {
    glm::vec3 zenith{0.0f};
    glm::vec3 horizon{0.0f};
    glm::vec3 ground{0.0f};
};

/**
 * @class AtmosphereModel
 * @brief See the file doc.
 */
class AtmosphereModel {
public:
    static constexpr float kPi = 3.14159265358979f;
    static constexpr int kTW = 64, kTH = 16;   ///< Transmittance table.
    static constexpr int kMS = 16;             ///< Multi-scatter table (square).
    static constexpr float kPlanetOffset = 0.01f;

    AtmosphereModel() { set_media(render::SkyAtmosphereMedia{}); }

    /** @brief Uses these media, rebuilding the tables if they changed. */
    void set_media(const render::SkyAtmosphereMedia& m);
    const render::SkyAtmosphereMedia& media() const { return media_; }
    /** @brief How often the tables were rebuilt (tests). */
    int builds() const { return builds_; }

    /** @brief Radius (km) of a viewer at world height `z_metres` -- sky_view_radius() in GLSL. */
    float view_radius(float z_metres) const { return media_.bottom_radius + std::max(z_metres, 0.0f) * 0.001f + kPlanetOffset; }

    /** @brief Transmittance from radius r toward cos-zenith mu, to the top of the atmosphere (no planet test). */
    glm::vec3 transmittance(float r, float mu) const;

    /**
     * @brief Transmittance from a viewer at `view_r` toward `dir`, with the planet blocking it
     *        (zero for a direction into the ground). The sun's colour at the viewer is this times
     *        the sun's colour above the atmosphere.
     */
    glm::vec3 transmittance_toward(float view_r, const glm::vec3& dir) const;

    /**
     * @brief Sky radiance toward `dir` from a viewer at `view_r`, lit by the sun at `sun_to` and
     *        the moon at -sun_to with `moon_ratio` of its illuminance -- what the sky-view table
     *        holds (per unit of sun illuminance).
     */
    glm::vec3 radiance(const glm::vec3& dir_in, const glm::vec3& sun_to, float moon_ratio, float view_r, int steps = 30) const;

    /**
     * @brief The sky reduced to the renderer's three gradient colours (per unit of sun
     *        illuminance): straight up; the average a few degrees above the horizon all the way
     *        round; and the average looking 25 degrees down (the lit ground through the haze).
     */
    SkyGradient gradient(const glm::vec3& sun_to, float moon_ratio, float view_r) const;

    /** @brief The multiple-scattering contribution at radius r for a sun at cos-zenith mu_s. */
    glm::vec3 multiscatter(float r, float mu_s) const;

private:
    struct Medium { glm::vec3 scattering, extinction, rayleigh, mie; };

    Medium medium_(float r) const;

    static float ray_sphere_(const glm::vec3& ro, const glm::vec3& rd, float radius);
    float earth_shadow_(const glm::vec3& p, const glm::vec3& l) const {
        return ray_sphere_(p, l, media_.bottom_radius) >= 0.0f ? 0.0f : 1.0f;
    }
    static float rayleigh_phase_(float c) { return 3.0f / (16.0f * kPi) * (1.0f + c * c); }
    static float mie_phase_(float g, float c);

    static float sub_uv_(float u, int res) { return (u + 0.5f / res) * (static_cast<float>(res) / (res + 1.0f)); }
    static float unit_uv_(float u, int res) { return (u - 0.5f / res) * (static_cast<float>(res) / (res - 1.0f)); }

    /** @brief Bilinear sample of a w x h table at uv in [0, 1] (texel centres at (i + 0.5) / w). */
    static glm::vec3 sample_(const std::vector<glm::vec3>& t, int w, int h, float u, float v);

    glm::vec2 transmittance_uv_(float r, float mu) const;

    void build_transmittance_();

    void build_multiscatter_();

    render::SkyAtmosphereMedia media_;
    std::vector<glm::vec3> trans_;
    std::vector<glm::vec3> ms_;
    bool built_ = false;
    int builds_ = 0;
};

} // namespace weather
} // namespace toy

#endif // TOYENGINE_WEATHER_ATMOSPHERE_MODEL_H

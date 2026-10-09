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
    void set_media(const render::SkyAtmosphereMedia& m) {
        if (built_ && m == media_) return;
        media_ = m;
        build_transmittance_();
        build_multiscatter_();
        built_ = true;
        ++builds_;
    }
    const render::SkyAtmosphereMedia& media() const { return media_; }
    /** @brief How often the tables were rebuilt (tests). */
    int builds() const { return builds_; }

    /** @brief Radius (km) of a viewer at world height `z_metres` -- sky_view_radius() in GLSL. */
    float view_radius(float z_metres) const { return media_.bottom_radius + std::max(z_metres, 0.0f) * 0.001f + kPlanetOffset; }

    /** @brief Transmittance from radius r toward cos-zenith mu, to the top of the atmosphere (no planet test). */
    glm::vec3 transmittance(float r, float mu) const {
        glm::vec2 uv = transmittance_uv_(r, mu);
        return sample_(trans_, kTW, kTH, sub_uv_(uv.x, kTW), sub_uv_(uv.y, kTH));
    }

    /**
     * @brief Transmittance from a viewer at `view_r` toward `dir`, with the planet blocking it
     *        (zero for a direction into the ground). The sun's colour at the viewer is this times
     *        the sun's colour above the atmosphere.
     */
    glm::vec3 transmittance_toward(float view_r, const glm::vec3& dir) const {
        if (ray_sphere_(glm::vec3(0, 0, view_r), dir, media_.bottom_radius) >= 0.0f) return glm::vec3(0.0f);
        return transmittance(view_r, dir.z);
    }

    /**
     * @brief Sky radiance toward `dir` from a viewer at `view_r`, lit by the sun at `sun_to` and
     *        the moon at -sun_to with `moon_ratio` of its illuminance -- what the sky-view table
     *        holds (per unit of sun illuminance).
     */
    glm::vec3 radiance(const glm::vec3& dir_in, const glm::vec3& sun_to, float moon_ratio, float view_r, int steps = 30) const {
        const glm::vec3 dir = glm::normalize(dir_in);
        const glm::vec3 pos(0.0f, 0.0f, view_r);
        const glm::vec3 sun = glm::normalize(sun_to), moon = -sun;
        const float t_ground = ray_sphere_(pos, dir, media_.bottom_radius);
        const float t_top = ray_sphere_(pos, dir, media_.top_radius);
        const float t_max = t_ground >= 0.0f ? t_ground : std::max(t_top, 0.0f);
        const float g = media_.mie_g;
        const float cs = glm::dot(dir, sun), cm = glm::dot(dir, moon);
        const float pr_s = rayleigh_phase_(cs), pm_s = mie_phase_(g, cs);
        const float pr_m = rayleigh_phase_(cm), pm_m = mie_phase_(g, cm);

        glm::vec3 lum(0.0f), thr(1.0f);
        float t_prev = 0.0f;
        for (int i = 0; i < steps; ++i) {
            const float f = (static_cast<float>(i) + 1.0f) / static_cast<float>(steps);
            const float t_next = t_max * f * f;
            const float dt = t_next - t_prev;
            const float t = t_prev + dt * 0.3f;
            t_prev = t_next;
            const glm::vec3 p = pos + dir * t;
            const float r = glm::length(p);
            const glm::vec3 up = p / r;
            const Medium m = medium_(r);
            const glm::vec3 st = glm::exp(-m.extinction * dt);
            const float mu_s = glm::dot(sun, up);
            const glm::vec3 t_sun = transmittance(r, mu_s) * earth_shadow_(p, sun);
            glm::vec3 s = t_sun * (m.rayleigh * pr_s + m.mie * pm_s) + multiscatter(r, mu_s) * m.scattering;
            if (moon_ratio > 0.0f) {
                const glm::vec3 t_moon = transmittance(r, -mu_s) * earth_shadow_(p, moon);
                s += moon_ratio * (t_moon * (m.rayleigh * pr_m + m.mie * pm_m) + multiscatter(r, -mu_s) * m.scattering);
            }
            const glm::vec3 ext = glm::max(m.extinction, glm::vec3(1e-7f));
            lum += thr * (s - s * st) / ext;
            thr *= st;
        }
        if (t_ground >= 0.0f) {
            const glm::vec3 p = pos + dir * t_ground;
            const glm::vec3 up = glm::normalize(p);
            const float mu_s = glm::dot(sun, up);
            const glm::vec3 lit = transmittance(glm::length(p), mu_s) * std::clamp(mu_s, 0.0f, 1.0f) +
                                  moon_ratio * transmittance(glm::length(p), -mu_s) * std::clamp(-mu_s, 0.0f, 1.0f);
            lum += thr * lit * media_.ground_albedo / kPi;
        }
        return lum;
    }

    /**
     * @brief The sky reduced to the renderer's three gradient colours (per unit of sun
     *        illuminance): straight up; the average a few degrees above the horizon all the way
     *        round; and the average looking 25 degrees down (the lit ground through the haze).
     */
    SkyGradient gradient(const glm::vec3& sun_to, float moon_ratio, float view_r) const {
        SkyGradient out;
        out.zenith = radiance(glm::vec3(0, 0, 1), sun_to, moon_ratio, view_r);
        constexpr int kAz = 8;
        const float he = std::sin(glm::radians(4.0f)), hc = std::cos(glm::radians(4.0f));
        const float ge = -std::sin(glm::radians(25.0f)), gc = std::cos(glm::radians(25.0f));
        for (int i = 0; i < kAz; ++i) {
            const float a = 2.0f * kPi * (static_cast<float>(i) + 0.5f) / kAz;
            out.horizon += radiance(glm::vec3(hc * std::cos(a), hc * std::sin(a), he), sun_to, moon_ratio, view_r, 24);
            if (i % 2 == 0) out.ground += radiance(glm::vec3(gc * std::cos(a), gc * std::sin(a), ge), sun_to, moon_ratio, view_r, 16);
        }
        out.horizon /= static_cast<float>(kAz);
        out.ground /= static_cast<float>(kAz / 2);
        return out;
    }

    /** @brief The multiple-scattering contribution at radius r for a sun at cos-zenith mu_s. */
    glm::vec3 multiscatter(float r, float mu_s) const {
        const float u = std::clamp(mu_s * 0.5f + 0.5f, 0.0f, 1.0f);
        const float v = std::clamp((r - media_.bottom_radius) / (media_.top_radius - media_.bottom_radius), 0.0f, 1.0f);
        return sample_(ms_, kMS, kMS, sub_uv_(u, kMS), sub_uv_(v, kMS));
    }

private:
    struct Medium { glm::vec3 scattering, extinction, rayleigh, mie; };

    Medium medium_(float r) const {
        const float h = std::max(r - media_.bottom_radius, 0.0f);
        const float dr = std::exp(-h / media_.rayleigh_scale_height);
        const float dm = std::exp(-h / media_.mie_scale_height);
        const float doz = std::clamp(h < 25.0f ? h / 15.0f - 2.0f / 3.0f : -h / 15.0f + 8.0f / 3.0f, 0.0f, 1.0f);
        Medium m;
        m.rayleigh = media_.rayleigh_scattering * dr;
        m.mie = media_.mie_scattering * dm;
        m.scattering = m.rayleigh + m.mie;
        m.extinction = m.rayleigh + media_.mie_extinction * dm + media_.ozone_absorption * doz;
        return m;
    }

    static float ray_sphere_(const glm::vec3& ro, const glm::vec3& rd, float radius) {
        const float b = glm::dot(ro, rd);
        const float c = glm::dot(ro, ro) - radius * radius;
        const float disc = b * b - c;
        if (disc < 0.0f) return -1.0f;
        const float s = std::sqrt(disc);
        if (-b - s >= 0.0f) return -b - s;
        if (-b + s >= 0.0f) return -b + s;
        return -1.0f;
    }
    float earth_shadow_(const glm::vec3& p, const glm::vec3& l) const {
        return ray_sphere_(p, l, media_.bottom_radius) >= 0.0f ? 0.0f : 1.0f;
    }
    static float rayleigh_phase_(float c) { return 3.0f / (16.0f * kPi) * (1.0f + c * c); }
    static float mie_phase_(float g, float c) {
        const float k = 3.0f / (8.0f * kPi) * (1.0f - g * g) / (2.0f + g * g);
        return k * (1.0f + c * c) / std::pow(std::max(1.0f + g * g - 2.0f * g * c, 1e-4f), 1.5f);
    }

    static float sub_uv_(float u, int res) { return (u + 0.5f / res) * (static_cast<float>(res) / (res + 1.0f)); }
    static float unit_uv_(float u, int res) { return (u - 0.5f / res) * (static_cast<float>(res) / (res - 1.0f)); }

    /** @brief Bilinear sample of a w x h table at uv in [0, 1] (texel centres at (i + 0.5) / w). */
    static glm::vec3 sample_(const std::vector<glm::vec3>& t, int w, int h, float u, float v) {
        const float x = std::clamp(u * w - 0.5f, 0.0f, static_cast<float>(w - 1));
        const float y = std::clamp(v * h - 0.5f, 0.0f, static_cast<float>(h - 1));
        const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
        const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
        const float fx = x - x0, fy = y - y0;
        const glm::vec3 a = glm::mix(t[y0 * w + x0], t[y0 * w + x1], fx);
        const glm::vec3 b = glm::mix(t[y1 * w + x0], t[y1 * w + x1], fx);
        return glm::mix(a, b, fy);
    }

    glm::vec2 transmittance_uv_(float r, float mu) const {
        const float bottom = media_.bottom_radius, top = media_.top_radius;
        const float H = std::sqrt(std::max(top * top - bottom * bottom, 0.0f));
        const float rho = std::sqrt(std::max(r * r - bottom * bottom, 0.0f));
        const float disc = r * r * (mu * mu - 1.0f) + top * top;
        const float d = std::max(0.0f, -r * mu + std::sqrt(std::max(disc, 0.0f)));
        const float d_min = top - r, d_max = rho + H;
        return {(d - d_min) / std::max(d_max - d_min, 1e-6f), rho / H};
    }

    void build_transmittance_() {
        trans_.assign(kTW * kTH, glm::vec3(1.0f));
        const float bottom = media_.bottom_radius, top = media_.top_radius;
        const float H = std::sqrt(top * top - bottom * bottom);
        for (int y = 0; y < kTH; ++y) {
            for (int x = 0; x < kTW; ++x) {
                const float ux = std::clamp(unit_uv_((x + 0.5f) / kTW, kTW), 0.0f, 1.0f);
                const float uy = std::clamp(unit_uv_((y + 0.5f) / kTH, kTH), 0.0f, 1.0f);
                const float rho = H * uy;
                const float r = std::sqrt(rho * rho + bottom * bottom);
                const float d_min = top - r, d_max = rho + H;
                const float d = d_min + ux * (d_max - d_min);
                float mu = d == 0.0f ? 1.0f : (H * H - rho * rho - d * d) / (2.0f * r * d);
                mu = std::clamp(mu, -1.0f, 1.0f);
                const glm::vec3 pos(0.0f, 0.0f, r), dir(std::sqrt(std::max(1.0f - mu * mu, 0.0f)), 0.0f, mu);
                const float t_max = std::max(ray_sphere_(pos, dir, top), 0.0f);
                constexpr int kSteps = 40;
                const float dt = t_max / kSteps;
                glm::vec3 depth(0.0f);
                for (int i = 0; i < kSteps; ++i) depth += medium_(glm::length(pos + dir * ((i + 0.5f) * dt))).extinction * dt;
                trans_[y * kTW + x] = glm::exp(-depth);
            }
        }
    }

    void build_multiscatter_() {
        ms_.assign(kMS * kMS, glm::vec3(0.0f));
        const float bottom = media_.bottom_radius, top = media_.top_radius;
        constexpr int kSqrtDirs = 8, kSteps = 20;
        const float iso = 1.0f / (4.0f * kPi);
        for (int y = 0; y < kMS; ++y) {
            for (int x = 0; x < kMS; ++x) {
                const float ux = std::clamp(unit_uv_((x + 0.5f) / kMS, kMS), 0.0f, 1.0f);
                const float uy = std::clamp(unit_uv_((y + 0.5f) / kMS, kMS), 0.0f, 1.0f);
                const float sun_mu = ux * 2.0f - 1.0f;
                const float r = bottom + uy * (top - bottom - kPlanetOffset) + kPlanetOffset;
                const glm::vec3 pos(0.0f, 0.0f, r), sun(std::sqrt(std::max(1.0f - sun_mu * sun_mu, 0.0f)), 0.0f, sun_mu);
                glm::vec3 lum_sum(0.0f), f_sum(0.0f);
                for (int iy = 0; iy < kSqrtDirs; ++iy) {
                    for (int ix = 0; ix < kSqrtDirs; ++ix) {
                        const float u = (ix + 0.5f) / kSqrtDirs, v = (iy + 0.5f) / kSqrtDirs;
                        const float ct = 1.0f - 2.0f * v, stt = std::sqrt(std::max(1.0f - ct * ct, 0.0f));
                        const float phi = 2.0f * kPi * u;
                        const glm::vec3 dir(stt * std::cos(phi), stt * std::sin(phi), ct);
                        const float t_ground = ray_sphere_(pos, dir, bottom);
                        const float t_top = ray_sphere_(pos, dir, top);
                        const float t_max = t_ground >= 0.0f ? t_ground : std::max(t_top, 0.0f);
                        const float dt = t_max / kSteps;
                        glm::vec3 lum(0.0f), fms(0.0f), thr(1.0f);
                        for (int i = 0; i < kSteps; ++i) {
                            const glm::vec3 p = pos + dir * ((i + 0.3f) * dt);
                            const float pr = glm::length(p);
                            const Medium m = medium_(pr);
                            const glm::vec3 st = glm::exp(-m.extinction * dt);
                            const float mu_s = glm::dot(sun, p / pr);
                            const glm::vec3 t_sun = transmittance(pr, mu_s) * earth_shadow_(p, sun);
                            const glm::vec3 s = m.scattering * iso * t_sun;
                            const glm::vec3 ext = glm::max(m.extinction, glm::vec3(1e-7f));
                            lum += thr * (s - s * st) / ext;
                            fms += thr * (m.scattering - m.scattering * st) / ext;
                            thr *= st;
                        }
                        if (t_ground >= 0.0f) {
                            const glm::vec3 p = pos + dir * t_ground;
                            const float pr = glm::length(p);
                            const float mu_s = glm::dot(sun, p / pr);
                            lum += thr * transmittance(pr, mu_s) * std::clamp(mu_s, 0.0f, 1.0f) * media_.ground_albedo / kPi;
                        }
                        lum_sum += lum;
                        f_sum += fms;
                    }
                }
                const float inv = 1.0f / (kSqrtDirs * kSqrtDirs);
                ms_[y * kMS + x] = (lum_sum * inv) / glm::max(glm::vec3(1.0f) - f_sum * inv, glm::vec3(1e-3f));
            }
        }
    }

    render::SkyAtmosphereMedia media_;
    std::vector<glm::vec3> trans_;
    std::vector<glm::vec3> ms_;
    bool built_ = false;
    int builds_ = 0;
};

} // namespace weather
} // namespace toy

#endif // TOYENGINE_WEATHER_ATMOSPHERE_MODEL_H

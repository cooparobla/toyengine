#version 450

// Physical sky, LUT 3 of 3: the sky's radiance in every direction from the camera's height
// (Hillaire 2020's sky-view LUT, 192 x 108). x = azimuth measured from the sun (resolution
// gathered toward it), y = view zenith compressed toward the horizon -- see sky_view_uv(). Lit by
// the sun AND the moon: the moon is always the sun's opposite point (toyengine/weather/
// sky_model.h), so it shares the sun's vertical plane and the LUT's symmetry. Rendered every
// frame the physical sky is on; values are per unit of sun illuminance (the lighting pass scales
// them by LightUBO::sky_params.w).

#include "sky_atmosphere.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_transmittance;
layout(set = 0, binding = 1) uniform sampler2D u_multiscatter;

layout(push_constant) uniform Params {
    SkyAtmosphere atmo;
    vec4 sun;    // xyz = unit direction TO the sun (world), w = moon illuminance / sun illuminance
    vec4 view;   // x = camera radius (km), y = steps, z = moon intensity of the ground bounce, w unused
} pc;

void main() {
    vec2 uv = vec2(sky_from_sub_uv_to_unit(in_uv.x, SKY_VIEW_RES.x),
                   sky_from_sub_uv_to_unit(in_uv.y, SKY_VIEW_RES.y));
    uv = clamp(uv, 0.0, 1.0);
    float bottom = pc.atmo.ozone.w, top = pc.atmo.ground.w;
    float view_r = pc.view.x;

    // Inverse of sky_view_uv().
    float v_horizon = sqrt(max(view_r * view_r - bottom * bottom, 0.0));
    float beta = acos(clamp(v_horizon / view_r, -1.0, 1.0));
    float zenith_horizon = SKY_PI - beta;
    float view_zenith;
    if (uv.y < 0.5) {
        float c = 2.0 * uv.y;
        c = 1.0 - c;
        c *= c;
        c = 1.0 - c;
        view_zenith = zenith_horizon * c;
    } else {
        float c = uv.y * 2.0 - 1.0;
        c *= c;
        view_zenith = zenith_horizon + beta * c;
    }
    float light_view_cos = -((uv.x * uv.x) * 2.0 - 1.0);
    float light_view_sin = sqrt(max(1.0 - light_view_cos * light_view_cos, 0.0));

    // Local frame: the sun's azimuth along +X.
    float sun_mu = clamp(pc.sun.z, -1.0, 1.0);
    vec3 sun = vec3(sqrt(max(1.0 - sun_mu * sun_mu, 0.0)), 0.0, sun_mu);
    vec3 moon = -sun;
    float sin_vz = sin(view_zenith);
    vec3 dir = vec3(sin_vz * light_view_cos, sin_vz * light_view_sin, cos(view_zenith));
    vec3 pos = vec3(0.0, 0.0, view_r);

    float t_ground = sky_ray_sphere(pos, dir, bottom);
    float t_top = sky_ray_sphere(pos, dir, top);
    float t_max = t_ground >= 0.0 ? t_ground : max(t_top, 0.0);

    float g = pc.atmo.mie_ext.w;
    float cs = dot(dir, sun), cm = dot(dir, moon);
    float pr_s = sky_rayleigh_phase(cs), pm_s = sky_mie_phase(g, cs);
    float pr_m = sky_rayleigh_phase(cm), pm_m = sky_mie_phase(g, cm);
    float moon_k = pc.sun.w;

    int steps = int(pc.view.y);
    vec3 lum = vec3(0.0), thr = vec3(1.0);
    float t_prev = 0.0;
    for (int i = 0; i < steps; ++i) {
        // Quadratic distribution: dense near the camera, where the air is thickest.
        float f = (float(i) + 1.0) / float(steps);
        float t_next = t_max * f * f;
        float dt = t_next - t_prev;
        float t = t_prev + dt * 0.3;
        t_prev = t_next;
        vec3 p = pos + dir * t;
        float r = length(p);
        vec3 up = p / r;
        SkyMedium m = sky_sample_medium(pc.atmo, r);
        vec3 sample_t = exp(-m.extinction * dt);

        float mu_s = dot(sun, up);
        vec3 t_sun = sky_transmittance(u_transmittance, pc.atmo, r, mu_s) * sky_earth_shadow(pc.atmo, p, sun);
        vec3 ms_sun = sky_multiscatter(u_multiscatter, pc.atmo, r, mu_s);
        vec3 s = t_sun * (m.rayleigh_scattering * pr_s + m.mie_scattering * pm_s) + ms_sun * m.scattering;

        if (moon_k > 0.0) {
            float mu_m = -mu_s;
            vec3 t_moon = sky_transmittance(u_transmittance, pc.atmo, r, mu_m) * sky_earth_shadow(pc.atmo, p, moon);
            vec3 ms_moon = sky_multiscatter(u_multiscatter, pc.atmo, r, mu_m);
            s += moon_k * (t_moon * (m.rayleigh_scattering * pr_m + m.mie_scattering * pm_m) + ms_moon * m.scattering);
        }

        vec3 ext = max(m.extinction, vec3(1e-7));
        lum += thr * (s - s * sample_t) / ext;
        thr *= sample_t;
    }
    // Rays that end on the planet see its sunlit (and moonlit) ground through the air between.
    if (t_ground >= 0.0) {
        vec3 p = pos + dir * t_ground;
        float r = length(p);
        vec3 up = p / r;
        float mu_s = dot(sun, up);
        vec3 lit = sky_transmittance(u_transmittance, pc.atmo, r, mu_s) * clamp(mu_s, 0.0, 1.0)
                 + moon_k * sky_transmittance(u_transmittance, pc.atmo, r, -mu_s) * clamp(-mu_s, 0.0, 1.0);
        lum += thr * lit * pc.atmo.ground.rgb / SKY_PI;
    }
    out_color = vec4(lum, 1.0);
}

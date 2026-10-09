#version 450

// Physical sky, LUT 2 of 3: the multiple-scattering contribution (Hillaire 2020, section 5.5) --
// for a height and a sun zenith angle, the luminance that light scattered twice or more adds per
// unit of scattering coefficient, assuming an isotropic phase from the second bounce on.
// 32 x 32; rendered with the transmittance LUT whenever the atmosphere's parameters change.

#include "sky_atmosphere.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_transmittance;

layout(push_constant) uniform Params {
    SkyAtmosphere atmo;
} pc;

void main() {
    vec2 uv = vec2(sky_from_sub_uv_to_unit(in_uv.x, SKY_MULTISCATTER_RES.x),
                   sky_from_sub_uv_to_unit(in_uv.y, SKY_MULTISCATTER_RES.y));
    uv = clamp(uv, 0.0, 1.0);
    float bottom = pc.atmo.ozone.w, top = pc.atmo.ground.w;
    float sun_mu = uv.x * 2.0 - 1.0;
    float r = bottom + uv.y * (top - bottom - SKY_PLANET_OFFSET) + SKY_PLANET_OFFSET;
    vec3 pos = vec3(0.0, 0.0, r);
    vec3 sun = vec3(sqrt(max(1.0 - sun_mu * sun_mu, 0.0)), 0.0, sun_mu);

    // 8 x 8 directions over the sphere, 20 steps each.
    const int SQRT_DIRS = 8;
    const int STEPS = 20;
    const float iso = 1.0 / (4.0 * SKY_PI);
    vec3 lum_sum = vec3(0.0);
    vec3 f_ms_sum = vec3(0.0);
    for (int iy = 0; iy < SQRT_DIRS; ++iy) {
        for (int ix = 0; ix < SQRT_DIRS; ++ix) {
            float u = (float(ix) + 0.5) / float(SQRT_DIRS);
            float v = (float(iy) + 0.5) / float(SQRT_DIRS);
            float cos_t = 1.0 - 2.0 * v;
            float sin_t = sqrt(max(1.0 - cos_t * cos_t, 0.0));
            float phi = 2.0 * SKY_PI * u;
            vec3 dir = vec3(sin_t * cos(phi), sin_t * sin(phi), cos_t);

            float t_ground = sky_ray_sphere(pos, dir, bottom);
            float t_top = sky_ray_sphere(pos, dir, top);
            float t_max = t_ground >= 0.0 ? t_ground : max(t_top, 0.0);
            float dt = t_max / float(STEPS);

            vec3 lum = vec3(0.0), f_ms = vec3(0.0), thr = vec3(1.0);
            for (int i = 0; i < STEPS; ++i) {
                vec3 p = pos + dir * ((float(i) + 0.3) * dt);
                float pr = length(p);
                SkyMedium m = sky_sample_medium(pc.atmo, pr);
                vec3 sample_t = exp(-m.extinction * dt);
                vec3 up = p / pr;
                float mu_s = dot(sun, up);
                vec3 t_sun = sky_transmittance(u_transmittance, pc.atmo, pr, mu_s) * sky_earth_shadow(pc.atmo, p, sun);
                vec3 s = m.scattering * iso * t_sun;
                vec3 ext = max(m.extinction, vec3(1e-7));
                vec3 s_int = (s - s * sample_t) / ext;
                vec3 ms_int = (m.scattering - m.scattering * sample_t) / ext;
                lum += thr * s_int;
                f_ms += thr * ms_int;
                thr *= sample_t;
            }
            if (t_ground >= 0.0) {
                vec3 p = pos + dir * t_ground;
                float pr = length(p);
                vec3 up = p / pr;
                float mu_s = dot(sun, up);
                vec3 t_sun = sky_transmittance(u_transmittance, pc.atmo, pr, mu_s);
                lum += thr * t_sun * clamp(mu_s, 0.0, 1.0) * pc.atmo.ground.rgb / SKY_PI;
            }
            lum_sum += lum;
            f_ms_sum += f_ms;
        }
    }
    const float inv = 1.0 / float(SQRT_DIRS * SQRT_DIRS);
    // Uniform sphere integration: each direction carries 4pi / N of solid angle, and the
    // isotropic phase 1/(4pi) cancels it.
    vec3 l2 = lum_sum * inv;
    vec3 f = f_ms_sum * inv;
    out_color = vec4(l2 / max(vec3(1.0) - f, vec3(1e-3)), 1.0);
}

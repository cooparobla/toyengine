#ifndef TOY_SKY_ATMOSPHERE_GLSL
#define TOY_SKY_ATMOSPHERE_GLSL

// sky_atmosphere.glsl -- the physical sky's atmosphere model (Hillaire 2020, "A Scalable and
// Production Ready Sky and Atmosphere Rendering Technique"), shared by the LUT passes
// (sky_transmittance.frag, sky_multiscatter.frag, sky_view.frag), the cloud march
// (sky_clouds.frag) and the lighting pass's sky branch (sky_physical.glsl).
//
// The CPU twin is toyengine/weather/atmosphere_model.h: the same media, the same LUT
// parameterisations and the same integration, so the sky colours it hands the gradient
// consumers (ambient, SSR fallback, fog, forward shading) match what this draws. Change one,
// change both.
//
// Units: kilometres, engine Z-up. The planet's centre is straight below the camera; a world
// height h (metres) sits at radius bottom_radius + h / 1000.
//
// Declares no uniforms or samplers (gfx/noise.glsl's rule): every input is a parameter, so
// each pass binds its own sets wherever it likes.

const float SKY_PI = 3.14159265358979;

/// The atmosphere's media, as the passes' push constants carry it (80 bytes).
/// C++: toy::render::passes::SkyAtmospherePass::AtmosphereGpu.
struct SkyAtmosphere {
    vec4 rayleigh;   // rgb = scattering (1/km), w = scale height (km)
    vec4 mie;        // rgb = scattering (1/km), w = scale height (km)
    vec4 mie_ext;    // rgb = extinction (1/km), w = phase asymmetry g
    vec4 ozone;      // rgb = absorption (1/km), w = bottom (planet) radius (km)
    vec4 ground;     // rgb = ground albedo, w = top-of-atmosphere radius (km)
};

const vec2 SKY_TRANSMITTANCE_RES = vec2(256.0, 64.0);
const vec2 SKY_MULTISCATTER_RES  = vec2(32.0, 32.0);
const vec2 SKY_VIEW_RES          = vec2(192.0, 108.0);

// Keeps a sample a hair above the planet's surface, where the LUT parameterisations are singular.
const float SKY_PLANET_OFFSET = 0.01;

/// Nearest non-negative hit of a ray with a sphere centred at the origin, or -1.
float sky_ray_sphere(vec3 ro, vec3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    float s = sqrt(disc);
    float t0 = -b - s;
    float t1 = -b + s;
    if (t0 >= 0.0) return t0;
    if (t1 >= 0.0) return t1;
    return -1.0;
}

struct SkyMedium {
    vec3 scattering;
    vec3 extinction;
    vec3 rayleigh_scattering;
    vec3 mie_scattering;
};

SkyMedium sky_sample_medium(SkyAtmosphere a, float radius) {
    float h = max(radius - a.ozone.w, 0.0);
    float dr = exp(-h / a.rayleigh.w);
    float dm = exp(-h / a.mie.w);
    // Ozone: a tent peaking at 25 km, 30 km either side (Bruneton's two linear layers).
    float doz = clamp(h < 25.0 ? h / 15.0 - 2.0 / 3.0 : -h / 15.0 + 8.0 / 3.0, 0.0, 1.0);
    SkyMedium m;
    m.rayleigh_scattering = a.rayleigh.rgb * dr;
    m.mie_scattering = a.mie.rgb * dm;
    m.scattering = m.rayleigh_scattering + m.mie_scattering;
    m.extinction = m.rayleigh_scattering + a.mie_ext.rgb * dm + a.ozone.rgb * doz;
    return m;
}

float sky_from_unit_to_sub_uv(float u, float res) { return (u + 0.5 / res) * (res / (res + 1.0)); }
float sky_from_sub_uv_to_unit(float u, float res) { return (u - 0.5 / res) * (res / (res - 1.0)); }

// --- Transmittance LUT (Bruneton's parameterisation: x = distance to the top, y = radius) ---

vec2 sky_transmittance_uv(SkyAtmosphere a, float r, float mu) {
    float bottom = a.ozone.w, top = a.ground.w;
    float H = sqrt(max(top * top - bottom * bottom, 0.0));
    float rho = sqrt(max(r * r - bottom * bottom, 0.0));
    float disc = r * r * (mu * mu - 1.0) + top * top;
    float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
    float d_min = top - r;
    float d_max = rho + H;
    float x_mu = (d - d_min) / max(d_max - d_min, 1e-6);
    float x_r = rho / H;
    return vec2(x_mu, x_r);
}

void sky_transmittance_params(SkyAtmosphere a, vec2 uv, out float r, out float mu) {
    float bottom = a.ozone.w, top = a.ground.w;
    float H = sqrt(max(top * top - bottom * bottom, 0.0));
    float rho = H * uv.y;
    r = sqrt(rho * rho + bottom * bottom);
    float d_min = top - r;
    float d_max = rho + H;
    float d = d_min + uv.x * (d_max - d_min);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

/// Transmittance from radius r along a direction at cos-zenith mu to the top of the atmosphere
/// (the planet is NOT tested here -- callers that care do so themselves, see sky_earth_shadow()).
vec3 sky_transmittance(sampler2D lut, SkyAtmosphere a, float r, float mu) {
    vec2 uv = sky_transmittance_uv(a, r, mu);
    uv = vec2(sky_from_unit_to_sub_uv(uv.x, SKY_TRANSMITTANCE_RES.x),
              sky_from_unit_to_sub_uv(uv.y, SKY_TRANSMITTANCE_RES.y));
    return textureLod(lut, uv, 0.0).rgb;
}

/// 0 when the planet blocks the light from `pos` (relative to the planet's centre), else 1.
float sky_earth_shadow(SkyAtmosphere a, vec3 pos, vec3 to_light) {
    return sky_ray_sphere(pos, to_light, a.ozone.w) >= 0.0 ? 0.0 : 1.0;
}

// --- Phase functions ---

float sky_rayleigh_phase(float c) {
    return 3.0 / (16.0 * SKY_PI) * (1.0 + c * c);
}
/// Cornette-Shanks, as in Hillaire's reference.
float sky_mie_phase(float g, float c) {
    float k = 3.0 / (8.0 * SKY_PI) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + c * c) / pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5);
}

// --- Multi-scatter LUT (x = sun cos-zenith in [-1, 1], y = height in the atmosphere) ---

vec3 sky_multiscatter(sampler2D lut, SkyAtmosphere a, float r, float sun_mu) {
    float bottom = a.ozone.w, top = a.ground.w;
    vec2 uv = vec2(clamp(sun_mu * 0.5 + 0.5, 0.0, 1.0),
                   clamp((r - bottom) / (top - bottom), 0.0, 1.0));
    uv = vec2(sky_from_unit_to_sub_uv(uv.x, SKY_MULTISCATTER_RES.x),
              sky_from_unit_to_sub_uv(uv.y, SKY_MULTISCATTER_RES.y));
    return textureLod(lut, uv, 0.0).rgb;
}

// --- Sky-view LUT (x = azimuth from the sun, y = view zenith, horizon-compressed) ---

vec2 sky_view_uv(SkyAtmosphere a, float view_r, float view_mu, float light_view_cos) {
    float bottom = a.ozone.w;
    float v_horizon = sqrt(max(view_r * view_r - bottom * bottom, 0.0));
    float cos_beta = v_horizon / view_r;
    float beta = acos(clamp(cos_beta, -1.0, 1.0));
    float zenith_horizon = SKY_PI - beta;
    float view_zenith = acos(clamp(view_mu, -1.0, 1.0));
    vec2 uv;
    if (view_zenith < zenith_horizon) {
        float c = view_zenith / zenith_horizon;
        c = 1.0 - c;
        c = sqrt(max(c, 0.0));
        c = 1.0 - c;
        uv.y = c * 0.5;
    } else {
        float c = (view_zenith - zenith_horizon) / max(beta, 1e-5);
        c = sqrt(clamp(c, 0.0, 1.0));
        uv.y = c * 0.5 + 0.5;
    }
    uv.x = sqrt(clamp(-light_view_cos * 0.5 + 0.5, 0.0, 1.0));
    return vec2(sky_from_unit_to_sub_uv(uv.x, SKY_VIEW_RES.x), sky_from_unit_to_sub_uv(uv.y, SKY_VIEW_RES.y));
}

/// Radius (km from the planet's centre) of a camera at world height `z_metres`.
float sky_view_radius(SkyAtmosphere a, float z_metres) {
    return a.ozone.w + max(z_metres * 0.001, 0.0) + SKY_PLANET_OFFSET;
}

/// Sky-view LUT lookup for a world direction, given the sun's direction.
vec3 sky_view_lookup(sampler2D lut, SkyAtmosphere a, float view_r, vec3 dir, vec3 sun_to) {
    vec2 dh = dir.xy;
    vec2 sh = sun_to.xy;
    float ld = dot(dh, dh), ls = dot(sh, sh);
    float light_view_cos = (ld > 1e-8 && ls > 1e-8) ? dot(dh, sh) * inversesqrt(ld * ls) : 1.0;
    return textureLod(lut, sky_view_uv(a, view_r, dir.z, light_view_cos), 0.0).rgb;
}

#endif // TOY_SKY_ATMOSPHERE_GLSL

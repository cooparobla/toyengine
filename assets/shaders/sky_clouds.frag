#version 450

// The physical sky's cloud layer: the raymarch (SkyCloudPass). Each frame traces ONE pixel of
// every 2x2 block of the cloud region (the half-resolution target's top-left `scale` fraction;
// the block's traced pixel cycles through all four over four frames), into a quarter-size trace
// target. sky_cloud_resolve.frag then reconstructs the full region by reprojecting the
// accumulated history -- the Unreal / Horizon Zero Dawn scheme: four times the steps for the same
// cost, and a temporally converged result instead of per-frame noise.
//
// Output (sky_cloud_common.glsl): rgb = light the clouds scatter toward the camera, a = packed
// transmittance + distance to the clouds (what the reconstruction reprojects by). A texel whose
// full-resolution 2x2 block holds no sky writes a = -1 and is skipped.
//
// The layer is a spherical shell over the planet (cloud_altitude .. + cloud_thickness), so it
// curves down to the horizon and fades into the haze with distance. Density (Schneider 2015 /
// Hillaire 2016, as in Unreal's volumetric clouds): a tiling weather map places the clouds
// (coverage field, cloud type, low-frequency variation), the type picks a height profile
// (stratus .. cumulus), a 128^3 Perlin-Worley volume gives the base shape, eroded at the edges by
// a 64^3 Worley detail volume (wisps at the base, billows at the top). Every texture is
// mipmapped and sampled at the mip of the sample's screen footprint, so distant clouds are
// pre-filtered instead of aliasing. Lighting: the scene's sun (or moon) through a shadow march
// -- Beer's law with Wrenninge's multiple-scattering octaves, a dual-lobe Henyey-Greenstein
// phase plus a narrow silver-lining lobe, a powder term, and the deck's statistical
// self-shadowing at a low sun -- plus the sky's ambient.

#include <gfx/spot_light.glsl>
#include "sky_atmosphere.glsl"

#define CLOUD_UBO_SET 2
#define CLOUD_UBO_BINDING 5
#include "sky_cloud_common.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

#include "light_ubo_body.glsl"

layout(set = 2, binding = 0) uniform sampler2D u_transmittance;
layout(set = 2, binding = 1) uniform sampler2D u_weather;   // 512^2 weather map, mipmapped, repeat
layout(set = 2, binding = 2) uniform sampler2D g_normal;    // nearest
layout(set = 2, binding = 3) uniform sampler3D u_shape;     // 128^3 base shape, mipmapped, repeat
layout(set = 2, binding = 4) uniform sampler3D u_detail;    // 64^3 detail, mipmapped, repeat

const float PLANET_R = 6360000.0;        // metres; the atmosphere's bottom radius
const float WEATHER_PERIOD = 13000.0;    // metres the weather map spans before it repeats
const float SHAPE_PERIOD = 5200.0;       // metres the base-shape volume spans
const float DETAIL_PERIOD = 820.0;       // metres the detail volume spans
const float MAX_MARCH = 16000.0;         // metres of layer marched along one ray at most
const float FADE_DIST = 45000.0;         // e-folding distance (m) of the haze fading far clouds
const float FAR_DEPTH = 60000.0;         // reprojection distance of a ray that met no cloud

float remap(float v, float lo, float hi, float nlo, float nhi) {
    return nlo + (v - lo) / max(hi - lo, 1e-5) * (nhi - nlo);
}

// Ray vs sphere centred at c: both roots (t0 <= t1), false on a miss.
bool sphere_hits(vec3 ro, vec3 rd, vec3 c, float r, out float t0, out float t1) {
    vec3 oc = ro - c;
    float b = dot(oc, rd);
    float q = dot(oc, oc) - r * r;
    float disc = b * b - q;
    if (disc < 0.0) { t0 = t1 = -1.0; return false; }
    float s = sqrt(disc);
    t0 = -b - s;
    t1 = -b + s;
    return true;
}

float hg(float g, float c) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * SKY_PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

vec3 g_center;   // the planet's centre (below the camera)

// Height in the layer, 0 at the base .. 1 at the top.
float layer_height(vec3 p) {
    return (length(p - g_center) - PLANET_R - cf.slab.x) / cf.slab.y;
}

// The weather at a (wind-shifted) position: x = local coverage 0..1 (how strongly this column
// is a cloud), y = cloud type, z = the base's lift (fraction of the layer).
vec3 weather_at(vec2 xy, float lod) {
    // Two reads -- the second rotated, ~2.4x larger and offset -- blended and re-stretched, so
    // neither period shows as rows of identical clouds toward the horizon.
    vec4 m1 = textureLod(u_weather, xy / WEATHER_PERIOD, lod);
    vec2 xy2 = mat2(0.6, 0.8, -0.8, 0.6) * xy / (WEATHER_PERIOD * 2.37) + vec2(0.61, 0.17);
    vec4 m2 = textureLod(u_weather, xy2, max(lod - 1.25, 0.0));
    float field = clamp((mix(m1.r, m2.r, 0.4) - 0.5) * 1.45 + 0.5, 0.0, 1.0);
    // The larger read's low-frequency channel varies the coverage across the sky (~15 km).
    float big = m2.b;
    float cov = clamp(cf.slab.z + (big - 0.5) * min(cf.slab.z, 1.0 - cf.slab.z), 0.0, 1.0);
    // Capped below 1 (so the shape noise carves even the densest column into lumps) until the
    // sky closes over: past ~70% coverage every column fills toward an overcast deck.
    float overcast = smoothstep(0.7, 1.0, cov);
    float wc = clamp((field - (1.0 - cov)) * 2.2 + 0.12 * step(0.001, cov) + overcast * 0.9, 0.0, 1.0);
    wc *= mix(0.78, 1.0, overcast) * smoothstep(0.0, 0.02, cov);
    float type = clamp(mix(m1.g, m2.g, 0.35) + (cov - 0.5) * 0.4, 0.0, 1.0);
    return vec3(wc, type, clamp(big * 0.22 - 0.05, 0.0, 0.18));
}

// The vertical density profile of a cloud type: stratus (thin, low) .. stratocumulus .. cumulus
// (tall, rounded top).
float height_profile(float h, float type) {
    float st = smoothstep(0.0, 0.06, h) * (1.0 - smoothstep(0.14, 0.3, h));
    float sc = smoothstep(0.0, 0.1, h) * (1.0 - smoothstep(0.32, 0.6, h));
    float cu = smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(0.5, 0.95, h));
    return type < 0.5 ? mix(st, sc, type * 2.0) : mix(sc, cu, type * 2.0 - 1.0);
}

// Cloud density at p (in units of the layer's extinction scale). `foot` is the sample's
// footprint in metres (screen footprint, or step length); it picks every texture's mip. detail:
// erode with the detail volume (the view march; the shadow march skips it). `empty` reports a
// column the weather leaves clear, which the march crosses in long strides.
float cloud_density(vec3 p, float h, float foot, float step_len, bool detail, out bool empty) {
    empty = false;
    if (h < 0.0 || h > 1.0) return 0.0;
    vec2 xy = p.xy + cf.wind.xy;
    vec3 w = weather_at(xy, log2(max(foot / (WEATHER_PERIOD / 512.0), 1.0)));
    if (w.x <= 0.0) { empty = true; return 0.0; }
    // Base height varies cloud to cloud, so the deck's bottoms don't line up.
    float hl = (h - w.z) / (1.0 - w.z);
    float profile = height_profile(hl, w.y);
    if (profile <= 0.0) return 0.0;

    float z = h * cf.slab.y;
    vec3 sp = vec3(xy, z) / SHAPE_PERIOD + vec3(0.0, 0.0, cf.wind.z * 2e-5);
    vec4 s = textureLod(u_shape, sp, log2(max(foot / (SHAPE_PERIOD / 128.0), 1.0)));
    float low = s.g * 0.625 + s.b * 0.25 + s.a * 0.125;
    float base = clamp(remap(s.r, low - 1.0, 1.0, 0.0, 1.0), 0.0, 1.0);
    base = clamp((base - 0.55) / 0.45, 0.0, 1.0);   // its spread (~0.57..0.99) stretched to [0, 1]
    // Coverage carves the shape: the threshold rises toward the top, rounding each cloud into a
    // dome; full coverage keeps the core solid.
    float thr = 1.0 - w.x * mix(1.0, 0.6, hl);
    float d = clamp(remap(base * profile, thr, 1.0, 0.0, 1.0), 0.0, 1.0);
    if (d <= 0.0) return 0.0;
    // The remapped field rises slowly from the threshold; the contrast below turns it into a
    // defined surface a few tens of metres deep instead of a haze hundreds of metres thick.
    // Along the ray, the surface is pre-filtered to the step: a step much longer than the
    // surface is deep would land on it or miss it depending on the jitter (shimmering edges),
    // so long steps see a softer surface -- the ray-direction counterpart of a texture mip.
    float CONTRAST = mix(3.0, 1.0, clamp((step_len - 40.0) / 160.0, 0.0, 1.0));

    if (detail) {
        float lod = log2(max(foot / (DETAIL_PERIOD / 64.0), 1.0));
        float fade = 1.0 - smoothstep(3.0, 5.0, lod);   // too small on screen to show: skip
        if (fade > 0.0) {
            // A little swirl: the shape's own channels push the detail around.
            vec3 dp = vec3(xy, z * 1.3) / DETAIL_PERIOD + (s.gba - 0.5) * 0.35;
            vec4 dn = textureLod(u_detail, dp, lod);
            float hf = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
            hf = mix(hf, 1.0 - hf, clamp(hl * 4.0, 0.0, 1.0));   // wisps at the base, billows above
            d = clamp(remap(d, hf * 0.45 * fade, 1.0, 0.0, 1.0), 0.0, 1.0);
        }
    }
    return min(d * CONTRAST, 1.0) * cf.slab.w;
}

void main() {
    // This frame's traced pixel of the 2x2 block this invocation covers.
    ivec2 q = ivec2(gl_FragCoord.xy);
    ivec2 hp = q * 2 + ivec2(cf.trace.xy);
    vec2 region = cf.trace.zw;
    if (any(greaterThanEqual(hp, ivec2(region)))) { out_color = vec4(0.0, 0.0, 0.0, -1.0); return; }
    vec2 uv = (vec2(hp) + 0.5) / region;

    // Skip a block the full-resolution G-buffer fills with geometry.
    vec4 nx = textureGather(g_normal, uv, 0);
    vec4 ny = textureGather(g_normal, uv, 1);
    vec4 nz = textureGather(g_normal, uv, 2);
    vec4 n2 = nx * nx + ny * ny + nz * nz;
    if (!any(lessThan(n2, vec4(0.001)))) {
        out_color = vec4(0.0, 0.0, 0.0, -1.0);
        return;
    }

    vec3 ndc = vec3(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0);
    vec4 world = cf.inv_view_proj * vec4(ndc, 1.0);
    vec3 ro = cf.camera.xyz;
    vec3 rd = normalize(world.xyz / world.w - ro);
    g_center = vec3(ro.xy, -PLANET_R);

    // The part of the ray inside the layer's shell (camera below, inside or above it).
    float r_in = PLANET_R + cf.slab.x, r_out = r_in + cf.slab.y;
    float a0, a1, b0, b1, g0, g1;
    bool hit_out = sphere_hits(ro, rd, g_center, r_out, b0, b1);
    const vec4 CLEAR = vec4(0.0, 0.0, 0.0, cloud_pack(FAR_DEPTH, 1.0));
    if (!hit_out || b1 <= 0.0) { out_color = CLEAR; return; }
    bool hit_in = sphere_hits(ro, rd, g_center, r_in, a0, a1);
    float cam_r = length(ro - g_center);
    float t_start, t_end;
    if (cam_r < r_in) {
        // Below: from leaving the inner sphere to leaving the outer one -- unless the ground
        // is in the way.
        if (sphere_hits(ro, rd, g_center, PLANET_R, g0, g1) && g0 > 0.0) { out_color = CLEAR; return; }
        t_start = a1;
        t_end = b1;
    } else if (cam_r < r_out) {
        t_start = 0.0;
        t_end = (hit_in && a0 > 0.0) ? a0 : b1;
    } else {
        t_start = max(b0, 0.0);
        t_end = (hit_in && a0 > 0.0) ? a0 : b1;
    }
    t_end = min(t_end, t_start + MAX_MARCH);
    if (t_end <= t_start) { out_color = CLEAR; return; }

    // Steps: the segment over the tier's count, clamped to 25..400 m, growing slowly with
    // distance (far clouds are small on screen and fade into the haze). Clear columns (the
    // weather map says no cloud) are crossed in 3x strides, backing off on entry so a cloud's
    // edge is still sampled finely. The iteration budget is 2x the tier's count.
    int base_steps = int(cf.light_dir.w);
    int steps = base_steps * 2;
    int light_steps = int(cf.light_color.w);
    float dt0 = clamp((t_end - t_start) / float(base_steps), 25.0, 400.0);
    // The start offset: interleaved gradient noise across the screen (neighbours differ, so the
    // reconstruction's spatial average sees well-spread offsets), advanced by the golden ratio
    // each time this pixel is traced again (every 4th frame), so its own offsets over time are a
    // low-discrepancy sequence and the temporal average converges smoothly.
    float cycle = floor(cf.wind.w / 4.0);
    float ign = fract(52.9829189 * fract(dot(vec2(hp), vec2(0.06711056, 0.00583715))));
    float jitter = fract(ign + 0.61803399 * mod(cycle, 1024.0));
    float pix_angle = cf.history.w;

    vec3 L = cf.light_dir.xyz;
    float cos_t = dot(rd, L);
    // Phase: a soft forward lobe plus some back-scatter for the cloud body, and a narrow
    // (g = 0.85) silver-lining lobe that only shows within ~20 degrees of the sun and only
    // where the cloud is thin toward it (it rides the single-scatter exp(-od) term alone).
    float phase = mix(hg(0.3, cos_t), hg(-0.2, cos_t), 0.3);
    float silver = min(0.12 * hg(0.85, cos_t), 0.6);
    float phase2 = mix(hg(0.2, cos_t), hg(-0.1, cos_t), 0.35);
    float phase3 = 1.0 / (4.0 * SKY_PI);

    // Sunlight arriving at the layer: through the atmosphere from the layer's mid height.
    SkyAtmosphere atmo;
    atmo.ozone.w = PLANET_R * 0.001;
    atmo.ground.w = atmo.ozone.w + 100.0;
    vec3 mid = ro + rd * (0.5 * (t_start + t_end));
    vec3 up = normalize(mid - g_center);
    float mid_r = atmo.ozone.w + (cf.slab.x + 0.5 * cf.slab.y) * 0.001;
    vec3 sun_light = cf.light_color.rgb * sky_transmittance(u_transmittance, atmo, mid_r, dot(up, L));
    if (dot(up, L) < -0.2) sun_light = vec3(0.0);
    // A low sun crosses the deck sideways, through many other clouds the shadow march never
    // reaches: attenuate it by the deck's expected transmittance along that slanted path
    // (coverage x path length in layer thicknesses). Without it a sunset deck is lit as if
    // each cloud stood alone -- uniformly glowing, and bright enough to crush the exposure.
    float slant = max(1.0 / max(dot(up, L), 0.03) - 1.5, 0.0);
    sun_light *= exp(-cf.slab.z * cf.slab.w * 0.3 * slant);
    // Ambient: the sky dome above (the CPU's matched zenith colour) and darker light from below.
    vec3 amb_top = lights.sky_zenith.rgb * 0.9 + lights.sky_horizon.rgb * 0.3;
    vec3 amb_bottom = lights.sky_horizon.rgb * 0.2 + lights.sky_ground.rgb * 0.6;

    const float SIGMA = 0.02;    // extinction (1/m) at density 1
    float light_len = cf.slab.y * 0.9;
    float seg = light_len / float((1 << light_steps) - 1);
    vec3 scatter = vec3(0.0);
    float trans = 1.0;
    float t_weighted = 0.0, w_sum = 0.0;
    float t = t_start + jitter * dt0;
    float stride = dt0;
    float fine_until = -1.0;   // after a back-off, fine steps up to here even through gaps
    for (int i = 0; i < steps && t < t_end; ++i) {
        float dt = dt0 * (1.0 + (t - t_start) / 6000.0);
        vec3 p = ro + rd * t;
        float h = layer_height(p);
        float foot = t * pix_angle;
        bool empty;
        float d = cloud_density(p, h, foot, dt, true, empty);
        if (empty) {
            stride = t < fine_until ? dt : dt * 3.0;
            t += stride;
            continue;
        }
        if (stride > dt * 1.01) {
            // Entered from a long stride: step back so the cloud's edge is sampled finely.
            fine_until = t;
            t -= stride - dt;
            stride = dt;
            continue;
        }
        stride = dt;
        if (d <= 0.0) { t += dt; continue; }

        // Shadow march toward the light, steps growing geometrically; no detail erosion (its
        // effect on the optical depth over hundreds of metres is noise).
        float od = 0.0;
        float tl = 0.0;
        for (int j = 0; j < light_steps; ++j) {
            float sl = seg * float(1 << j);
            vec3 lp = p + L * (tl + sl * 0.5);
            bool e;
            od += cloud_density(lp, layer_height(lp), max(foot, sl * 0.25), sl, false, e) * sl;
            tl += sl;
        }
        od *= SIGMA * 0.7;   // light leaks through more than a single-scattering march says
        // Wrenninge's octaves (Hillaire 2016's a = b = 0.5): each successive scattering order
        // carries half the energy through half the optical depth, with a softer phase.
        float body = exp(-od) * phase + 0.5 * exp(-od * 0.5) * phase2 + 0.25 * exp(-od * 0.25) * phase3;
        // Powder darkens the cloud's thin fringe; the silver lining is exempt (it IS the
        // thin fringe, seen against the sun).
        float powder = 1.0 - exp(-d * SIGMA * 240.0);
        float beer = body * mix(powder, 1.0, 0.35) + exp(-od) * silver;
        vec3 direct = sun_light * beer * (4.0 * SKY_PI * 0.6);
        vec3 ambient = mix(amb_bottom, amb_top, clamp(h * 1.4, 0.0, 1.0)) * mix(0.55, 1.0, powder);
        vec3 s = (direct + ambient) * 0.9;   // 0.9: cloud single-scattering albedo

        // Energy-conserving integration of the step (Hillaire 2015): exact for constant
        // in-scatter over the step, so thin steps and thick ones agree.
        float ext = max(d * SIGMA, 1e-6);
        float step_t = exp(-ext * dt);
        scatter += trans * s * (1.0 - step_t);
        t_weighted += t * trans * (1.0 - step_t);
        w_sum += trans * (1.0 - step_t);
        trans *= step_t;
        if (trans < 0.01) { trans = 0.0; break; }
        t += dt;
    }

    // Far clouds dissolve into the haze (the sky behind already carries the air's in-scatter).
    float dist = w_sum > 1e-4 ? t_weighted / w_sum : t_start + 0.5 * (t_end - t_start);
    float fade = exp(-max(dist - 4000.0, 0.0) / FADE_DIST);
    scatter *= fade;
    trans = mix(1.0, trans, fade);
    out_color = vec4(scatter, cloud_pack(dist, trans));
}

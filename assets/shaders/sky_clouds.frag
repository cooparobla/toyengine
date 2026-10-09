#version 450

// The physical sky's cloud layer, raymarched into the top-left `scale` fraction of a
// half-resolution target (SkyCloudPass; the scale follows sky_quality). Output:
// rgb = light the clouds scatter toward the camera, a = how much of the sky behind still shows
// (transmittance); the lighting pass's sky branch composites sky * a + rgb. A 2x2 block of the
// full-resolution G-buffer with no sky pixel in it writes a = -1 and is skipped, both here and by
// the lighting pass's upsample (so silhouettes never pick up cloud from behind the geometry).
//
// The layer is a spherical shell over the planet (cloud_altitude .. + cloud_thickness), so it
// curves down to the horizon and fades into the haze with distance. Density: the tiling shape
// map (sky_cloud_noise.frag, read twice at different scales) thresholded by the coverage, shaped
// by a height profile (with a varying base) and eroded by a baked tiling 3D noise
// (sky_cloud_noise3d.frag). Lighting: the scene's sun (or moon) through a short shadow march --
// Beer's law with Wrenninge's multiple-scattering octaves, a dual-lobe Henyey-Greenstein phase
// plus a narrow silver-lining lobe, a powder term, and the deck's statistical self-shadowing at
// a low sun -- plus the sky's ambient. Steps start at a per-pixel interleaved-gradient offset
// that rotates per frame under TAA, which resolves the banding into a smooth result.

#include <gfx/spot_light.glsl>
#include <gfx/noise.glsl>
#include "sky_atmosphere.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

#include "light_ubo_body.glsl"

layout(set = 2, binding = 0) uniform sampler2D u_transmittance;
layout(set = 2, binding = 1) uniform sampler2D u_sky_view;
layout(set = 2, binding = 2) uniform sampler2D u_cloud_noise;   // repeating sampler
layout(set = 2, binding = 3) uniform sampler2D g_normal;        // nearest
layout(set = 2, binding = 4) uniform sampler2D u_noise3d;       // 64^3 atlas, linear clamp

layout(push_constant) uniform CloudParams {
    mat4 inv_view_proj;   // unjittered-or-jittered inverse(proj * view) of this frame
    vec4 slab;            // x = base altitude (m), y = thickness (m), z = coverage 0..1, w = density
    vec4 wind;            // xy = accumulated wind offset (m), z = time (s), w = jitter frame (< 0: none)
    vec4 light_dir;       // xyz = unit direction TO the light (sun or moon), w = view steps
    vec4 light_color;     // rgb = the light's illuminance above the atmosphere, w = shadow steps
} pc;

const float PLANET_R = 6360000.0;        // metres; the atmosphere's bottom radius
const float MAP_PERIOD = 13000.0;        // metres the shape map spans before it repeats
const float MAX_MARCH = 16000.0;         // metres of layer marched along one ray at most
const float FADE_DIST = 45000.0;         // e-folding distance (m) of the haze fading far clouds

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
    return (length(p - g_center) - PLANET_R - pc.slab.x) / pc.slab.y;
}

// The baked 3D noise (sky_cloud_noise3d.frag): c in periods (any range, it wraps). Bilinear
// inside a 64 x 64 slice of the 8 x 8 atlas, a manual lerp between neighbouring slices. Texel 63
// of every axis repeats texel 0, so clamping inside the slice still tiles seamlessly.
vec2 cloud_noise3d(vec3 c) {
    vec3 t = fract(c) * 63.0;
    float z0 = floor(t.z);
    float fz = t.z - z0;
    vec2 inner = t.xy + 0.5;
    vec2 a = (vec2(mod(z0, 8.0), floor(z0 / 8.0)) * 64.0 + inner) / 512.0;
    float z1 = z0 + 1.0;
    vec2 b = (vec2(mod(z1, 8.0), floor(z1 / 8.0)) * 64.0 + inner) / 512.0;
    return mix(textureLod(u_noise3d, a, 0.0).rg, textureLod(u_noise3d, b, 0.0).rg, fz);
}

// Cloud density at p (in units of the layer's extinction scale). lod 0: the full field (2D map +
// 3D lumps + fine 3D erosion), 1: no erosion, 2: the 2D map alone (the shadow march's cheap read:
// two texture fetches). `margin` returns how far below the cloud's threshold p is (> 0 outside),
// which the march uses to take longer strides through empty sky.
float cloud_density(vec3 p, float h, int lod, out float margin) {
    margin = 1.0;
    if (h < 0.0 || h > 1.0) return 0.0;
    vec2 xy = p.xy + pc.wind.xy;
    // Two reads of the shape map -- the second rotated, ~2.4x larger and offset -- blended and
    // re-stretched, so neither period shows as rows of identical clouds toward the horizon.
    vec4 m1 = textureLod(u_cloud_noise, xy / MAP_PERIOD, 0.0);
    vec2 xy2 = mat2(0.6, 0.8, -0.8, 0.6) * xy / (MAP_PERIOD * 2.37) + vec2(0.61, 0.17);
    vec4 m2 = textureLod(u_cloud_noise, xy2, 0.0);
    float field = clamp((mix(m1.r, m2.r, 0.4) - 0.5) * 1.45 + 0.5, 0.0, 1.0);
    // The larger read's low-frequency channel varies the coverage across the sky (~15 km).
    float big = m2.b;
    float cov = clamp(pc.slab.z + (big - 0.5) * 0.5 * min(pc.slab.z, 1.0 - pc.slab.z) * 2.0, 0.0, 1.0);
    // Base height varies cloud to cloud (and across the sky), so the deck's bottoms don't line up.
    // Only ever raised (the march's shell cuts at the layer base), tops still reach the top.
    float base = clamp(big * 0.15 + m1.b * 0.25 - 0.08, 0.0, 0.35);
    h = (h - base) / (1.0 - base);
    // The coverage threshold rises with height (and a little toward the base), so a cloud's
    // footprint shrinks as it climbs: every blob of the map becomes a dome whose height follows
    // how far the map exceeds the threshold, with a softly rounded base.
    float lo = 1.0 - cov * 0.75;
    float rise = max(h, 0.0) * 1.5;
    float sink = min(max(0.12 - h, 0.0) / 0.12, 3.0);
    float thr = lo + (1.0 - lo) * (rise * mix(1.0, 0.55, cov * cov) + 0.35 * sink * sink);
    // Signed distance-like margin above the threshold (> 0 inside). The 3D noises move the
    // threshold itself, so the cloud's SURFACE becomes lumpy in all three dimensions.
    float span = max(cov, 0.08);
    float shape = field - thr;
    margin = -shape / span - 0.5;
    if (margin > 0.0) return 0.0;                      // no noise can reach it
    if (lod == 2) return clamp((shape + 0.1 * span) / (0.3 * span), 0.0, 1.0) * pc.slab.w;
    vec3 q = vec3(xy, p.z * 1.6) / 900.0 + vec3(0.0, 0.0, pc.wind.z * 0.002);
    vec2 n = cloud_noise3d(q / 8.0);                     // r: lumps (period 8 lattice cells)
    shape += (n.r - 0.5) * 0.75 * span;
    // The erosion moves the surface by at most 0.11 span: skip it where it cannot change the
    // result (well outside, or deep enough inside that the density stays saturated).
    if (lod == 0 && abs(shape - 0.11 * span) < 0.22 * span) {
        // Billows toward the top, wisps toward the base.
        float f = cloud_noise3d(q * (3.3 / 4.0) + 0.37).g;
        f = clamp((f - 0.5) * 1.6 + 0.5, 0.0, 1.0);    // the baked fbm's spread is narrower
        shape += (mix(f, 1.0 - f, clamp(h * 2.5, 0.0, 1.0)) - 0.5) * 0.22 * span;
    }
    float d = clamp(shape / (0.22 * span), 0.0, 1.0);
    return d * pc.slab.w;
}
float cloud_density(vec3 p, float h, int lod) {
    float m;
    return cloud_density(p, h, lod, m);
}

void main() {
    // Skip a block the full-resolution G-buffer fills with geometry.
    vec4 nx = textureGather(g_normal, in_uv, 0);
    vec4 ny = textureGather(g_normal, in_uv, 1);
    vec4 nz = textureGather(g_normal, in_uv, 2);
    vec4 n2 = nx * nx + ny * ny + nz * nz;
    if (!any(lessThan(n2, vec4(0.001)))) {
        out_color = vec4(0.0, 0.0, 0.0, -1.0);
        return;
    }

    vec3 ndc = vec3(in_uv.x * 2.0 - 1.0, 1.0 - in_uv.y * 2.0, 1.0);
    vec4 world = pc.inv_view_proj * vec4(ndc, 1.0);
    vec3 ro = camera.camera_pos;
    vec3 rd = normalize(world.xyz / world.w - ro);
    g_center = vec3(ro.xy, -PLANET_R);

    // The part of the ray inside the layer's shell (camera below, inside or above it).
    float r_in = PLANET_R + pc.slab.x, r_out = r_in + pc.slab.y;
    float a0, a1, b0, b1, g0, g1;
    bool hit_out = sphere_hits(ro, rd, g_center, r_out, b0, b1);
    if (!hit_out || b1 <= 0.0) { out_color = vec4(0.0, 0.0, 0.0, 1.0); return; }
    bool hit_in = sphere_hits(ro, rd, g_center, r_in, a0, a1);
    float cam_r = length(ro - g_center);
    float t_start, t_end;
    if (cam_r < r_in) {
        // Below: from leaving the inner sphere to leaving the outer one -- unless the ground
        // is in the way.
        if (sphere_hits(ro, rd, g_center, PLANET_R, g0, g1) && g0 > 0.0) { out_color = vec4(0.0, 0.0, 0.0, 1.0); return; }
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
    if (t_end <= t_start) { out_color = vec4(0.0, 0.0, 0.0, 1.0); return; }

    // Steps inside cloud are the layer's span over the tier's count, but no longer than ~150 m
    // (the noise's scale) near the camera, growing with distance (far clouds are small on
    // screen and fade into the haze), so grazing views keep their detail instead of slicing
    // into bands. Empty sky is crossed in strides up to 4x as long (backing off on entry); the
    // iteration budget is 2x the tier's count.
    int base_steps = int(pc.light_dir.w);
    int steps = base_steps * 2;
    int light_steps = int(pc.light_color.w);
    float dt0 = min((t_end - t_start) / float(base_steps), 150.0);
    float dt = dt0;
    // Interleaved gradient noise, rotated per frame when TAA is there to resolve it.
    vec2 px = gl_FragCoord.xy;
    if (pc.wind.w >= 0.0) px += 5.588238 * mod(pc.wind.w, 64.0);
    float jitter = fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));

    vec3 L = pc.light_dir.xyz;
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
    float mid_r = atmo.ozone.w + (pc.slab.x + 0.5 * pc.slab.y) * 0.001;
    vec3 sun_light = pc.light_color.rgb * sky_transmittance(u_transmittance, atmo, mid_r, dot(up, L));
    if (dot(up, L) < -0.2) sun_light = vec3(0.0);
    // A low sun crosses the deck sideways, through many other clouds the short shadow march
    // never reaches: attenuate it by the deck's expected transmittance along that slanted path
    // (coverage x path length in layer thicknesses). Without it a sunset deck is lit as if
    // each cloud stood alone -- uniformly glowing, and bright enough to crush the exposure.
    float slant = max(1.0 / max(dot(up, L), 0.03) - 1.5, 0.0);
    sun_light *= exp(-pc.slab.z * pc.slab.w * 0.3 * slant);
    // Ambient: the sky dome above (the CPU's matched zenith colour) and darker light from below.
    vec3 amb_top = lights.sky_zenith.rgb * 0.9 + lights.sky_horizon.rgb * 0.3;
    vec3 amb_bottom = lights.sky_horizon.rgb * 0.2 + lights.sky_ground.rgb * 0.6;

    const float SIGMA = 0.02;    // extinction (1/m) at density 1
    float light_len = pc.slab.y * 0.9;
    float seg = light_len / float((1 << light_steps) - 1);
    vec3 scatter = vec3(0.0);
    float trans = 1.0;
    float t_weighted = 0.0, w_sum = 0.0;
    float t = t_start + jitter * dt;
    float stride = dt;
    float od = 0.0;            // optical depth toward the light at the last lit step
    int lit_steps = 0;
    float fine_until = -1.0;   // after a back-off, fine steps up to here even through gaps
    for (int i = 0; i < steps && t < t_end; ++i) {
        dt = dt0 * (1.0 + (t - t_start) / 3000.0);
        vec3 p = ro + rd * t;
        float h = layer_height(p);
        float margin;
        float d = cloud_density(p, h, t - t_start < 10000.0 ? 0 : 1, margin);   // far: no erosion
        if (margin > 0.0) {
            // Empty: the further below the threshold the map is, the longer the stride.
            stride = t < fine_until ? dt : dt * clamp(1.0 + margin * 4.0, 1.0, 4.0);
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
        if (d <= 0.0) { t += dt; continue; }

        // Shadow march toward the light, steps growing geometrically -- on every other lit
        // step only: the optical depth changes slowly along the view ray, so the step between
        // reuses it (halves the march's texture reads).
        if ((lit_steps++ & 1) == 0) {
            od = 0.0;
            float tl = 0.0;
            for (int j = 0; j < light_steps; ++j) {
                float s = seg * float(1 << j);
                vec3 q = p + L * (tl + s * 0.5);
                od += cloud_density(q, layer_height(q), 2) * s;
                tl += s;
            }
            od *= SIGMA * 0.7;   // light leaks through more than a single-scattering march says
        }
        // Wrenninge's octaves: each successive order sees a thinner medium and a softer phase.
        float body = exp(-od) * phase + 0.5 * exp(-od * 0.25) * phase2 + 0.18 * exp(-od * 0.08) * phase3;
        // Powder darkens the cloud's thin fringe; the silver lining is exempt (it IS the
        // thin fringe, seen against the sun).
        float powder = 1.0 - exp(-d * SIGMA * 240.0);
        float beer = body * mix(powder, 1.0, 0.35) + exp(-od) * silver;
        vec3 direct = sun_light * beer * (4.0 * SKY_PI * 0.6);
        vec3 ambient = mix(amb_bottom, amb_top, clamp(h * 1.4, 0.0, 1.0)) * mix(0.55, 1.0, powder);
        vec3 s = (direct + ambient) * 0.9;   // 0.9: cloud single-scattering albedo

        float ext = d * SIGMA;
        float step_t = exp(-ext * dt);
        scatter += trans * s * (1.0 - step_t);
        t_weighted += t * trans * (1.0 - step_t);
        w_sum += trans * (1.0 - step_t);
        trans *= step_t;
        if (trans < 0.01) { trans = 0.0; break; }
        t += dt;
    }

    // Far clouds dissolve into the haze (the sky behind already carries the air's in-scatter).
    float dist = w_sum > 0.0 ? t_weighted / w_sum : t_start;
    float fade = exp(-max(dist - 4000.0, 0.0) / FADE_DIST);
    scatter *= fade;
    trans = mix(1.0, trans, fade);
    out_color = vec4(scatter, trans);
}

#version 450

// The volumetric cloud layer: the raymarch (SkyCloudPass), with either sky model. Each frame
// traces ONE pixel of every 2x2 block of the cloud region (the half-resolution target's top-left
// `scale` fraction; the block's traced pixel cycles through all four over four frames), into a
// quarter-size trace target. sky_cloud_resolve.frag then reconstructs the full region by
// reprojecting the accumulated history -- the Unreal / Horizon Zero Dawn scheme: four times the
// steps for the same cost, and a temporally converged result instead of per-frame noise.
//
// Output (sky_cloud_common.glsl): rgb = light the clouds scatter toward the camera, a = packed
// transmittance + distance to the clouds (what the reconstruction reprojects by, and what the
// composite over geometry depth-tests).
//
// The layer is a spherical shell over the planet (cloud_altitude .. + cloud_thickness), so it
// curves down to the horizon and fades into the haze with distance, marched in cloud space (world
// / cloud_scale) so a small scale makes small, low clouds with the same look. Every ray stops at
// the scene: the farthest surface of its full-resolution block (sky = no limit), so the clouds
// cover the ground below a high camera and hide behind mountains in front of them. The density
// field is sky_cloud_density.glsl. Lighting: the scene's sun (or moon) through a shadow march --
// Beer's law with Wrenninge's multiple-scattering octaves, a dual-lobe Henyey-Greenstein phase
// plus a narrow silver-lining lobe, a powder term, and the deck's statistical self-shadowing at a
// low sun -- plus the sky's ambient (the gradient's colours, or the physical sky's matched ones).

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
layout(set = 2, binding = 6) uniform sampler2D g_position;  // nearest: world position

#include "sky_cloud_density.glsl"

const float MAX_MARCH = 16000.0;         // metres of layer marched along one ray at most
const float FADE_DIST = 45000.0;         // e-folding distance (m) of the haze fading far clouds
const float FAR_DEPTH = 60000.0;         // reprojection distance of a ray that met no cloud

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

void main() {
    // This frame's traced pixel of the 2x2 block this invocation covers.
    ivec2 q = ivec2(gl_FragCoord.xy);
    ivec2 hp = q * 2 + ivec2(cf.trace.xy);
    vec2 region = cf.trace.zw;
    if (any(greaterThanEqual(hp, ivec2(region)))) { out_color = vec4(0.0, 0.0, 0.0, -1.0); return; }
    vec2 uv = (vec2(hp) + 0.5) / region;

    // The farthest surface of the full-resolution block (cloud space; sky pixels: no limit).
    // The farthest, so a block on a silhouette marches the clouds behind it too; the composite
    // over geometry depth-tests each pixel against the clouds' distance.
    float scale = cf.look.x;
    vec4 nx = textureGather(g_normal, uv, 0);
    vec4 ny = textureGather(g_normal, uv, 1);
    vec4 nz = textureGather(g_normal, uv, 2);
    vec4 n2 = nx * nx + ny * ny + nz * nz;
    float t_scene = 1e30;
    if (!any(lessThan(n2, vec4(0.001)))) {
        vec4 px = textureGather(g_position, uv, 0) - cf.camera.x;
        vec4 py = textureGather(g_position, uv, 1) - cf.camera.y;
        vec4 pz = textureGather(g_position, uv, 2) - cf.camera.z;
        vec4 d2 = px * px + py * py + pz * pz;
        t_scene = sqrt(max(max(d2.x, d2.y), max(d2.z, d2.w))) / scale;
    }

    vec3 ndc = vec3(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0);
    vec4 world = cf.inv_view_proj * vec4(ndc, 1.0);
    vec3 rd = normalize(world.xyz / world.w - cf.camera.xyz);
    vec3 ro = cf.camera.xyz / scale;
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
    t_end = min(t_end, t_scene);
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
    // The layer's real height (world metres) in the atmosphere, whatever its scale.
    float mid_r = atmo.ozone.w + (cf.slab.x + 0.5 * cf.slab.y) * scale * 0.001;
    // The physical sky colours the light above the atmosphere by the air below the layer; under
    // the gradient sky light_color is the scene's light as it already reaches the ground.
    vec3 sun_light = cf.light_color.rgb;
    if (cf.look.z > 0.5) sun_light *= sky_transmittance(u_transmittance, atmo, mid_r, dot(up, L));
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

    const float SIGMA = CLOUD_SIGMA;
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
    // The camera fade (render cloud_camera_fade) thins the whole layer the same way.
    float fade = exp(-max(dist - 4000.0, 0.0) / FADE_DIST) * cf.look.y;
    scatter *= fade;
    trans = mix(1.0, trans, fade);
    out_color = vec4(scatter, cloud_pack(dist, trans));
}

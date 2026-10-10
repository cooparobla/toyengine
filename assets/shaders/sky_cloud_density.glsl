#ifndef TOY_SKY_CLOUD_DENSITY_GLSL
#define TOY_SKY_CLOUD_DENSITY_GLSL

// The volumetric clouds' density field, shared by the view march (sky_clouds.frag) and the
// cloud shadow map (cloud_shadow_volumetric.frag), so the shadows are exactly the clouds seen.
// All lengths are cloud space (sky_cloud_common.glsl).
//
// Density (Schneider 2015 / Hillaire 2016, as in Unreal's volumetric clouds): a tiling weather
// map places the clouds (coverage field, cloud type, low-frequency variation), the type picks a
// height profile (stratus .. cumulus), a 128^3 Perlin-Worley volume gives the base shape, eroded
// at the edges by a 64^3 Worley detail volume (wisps at the base, billows at the top). Every
// texture is mipmapped and sampled at the mip of the sample's footprint, so distant clouds are
// pre-filtered instead of aliasing.
//
// REQUIRED BEFORE INCLUDE: the CloudFrame block `cf` (sky_cloud_common.glsl with CLOUD_UBO_SET),
// and the samplers u_weather (2D), u_shape (3D) and u_detail (3D). The caller sets g_center (the
// planet's centre below the camera) before calling layer_height() / cloud_density().

const float PLANET_R = 6360000.0;        // metres; the atmosphere's bottom radius
const float WEATHER_PERIOD = 13000.0;    // metres the weather map spans before it repeats
const float SHAPE_PERIOD = 5200.0;       // metres the base-shape volume spans
const float DETAIL_PERIOD = 820.0;       // metres the detail volume spans
const float CLOUD_SIGMA = 0.02;          // extinction (1/m) at density 1

float remap(float v, float lo, float hi, float nlo, float nhi) {
    return nlo + (v - lo) / max(hi - lo, 1e-5) * (nhi - nlo);
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
// erode with the detail volume (the view march; the shadow marches skip it). `empty` reports a
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

#endif // TOY_SKY_CLOUD_DENSITY_GLSL

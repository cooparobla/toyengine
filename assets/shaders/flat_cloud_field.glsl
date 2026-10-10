#ifndef TOY_FLAT_CLOUD_FIELD_GLSL
#define TOY_FLAT_CLOUD_FIELD_GLSL

// The flat (toon) cloud layer's field (render cloud_type: flat), shared by its draw
// (flat_clouds.frag) and its shadow map (cloud_shadow_flat.frag), so the shadows are exactly the
// clouds seen.
//
// A signed margin over the ground plane: > 0 inside a cloud (how far the field rises above the
// coverage threshold), < 0 in clear sky; zero is a cloud's edge. Cloud groups come from the
// volumetric clouds' weather map (an even spread over [0, 1], so about `coverage` of the ground
// exceeds 1 - coverage), their edges broken into puffs by two octaves of the Perlin-Worley shape
// volume. What makes it look blown by the wind rather than slid along it:
//   * a domain warp: the whole field is pushed around by a slow, low-frequency flow that drifts
//     at a fraction of the wind's speed, so shapes stretch, bend and roll as they travel;
//   * the two puff octaves drift at different speeds (the upper, finer one faster), so the
//     clouds' edges churn against their bodies;
//   * every puff slowly morphs (the shape volume's z slice advances with time).
// Every drift offset arrives already wrapped to its own read's period (CloudOverlayPass), so the
// field never jumps however long the wind blows.
//
// REQUIRED BEFORE INCLUDE: FLAT_UBO_SET / FLAT_UBO_BINDING, and the samplers u_weather (2D) and
// u_shape (3D), both repeating.

layout(set = FLAT_UBO_SET, binding = FLAT_UBO_BINDING) uniform FlatCloudFrame {
    vec4 layer;   // x = base height (world z), y = puff height (m), z = puff size (m), w = opacity
    vec4 drift0;  // xy = the groups' drift offset (m, wrapped), zw = the warp's drift offset
    vec4 drift1;  // xy = the first puff octave's drift offset, zw = the second octave's
    vec4 look;    // x = coverage 0..1, y = light bands, z = outline 0..1, w = camera fade 0..1
    vec4 flow;    // x = turbulence, y = the puffs' evolution phase (0..1, wraps), z = time (s)
} fc;

// The period (in puff sizes) of each read -- CloudOverlayPass wraps the drift offsets to these.
const float FLAT_GROUP_PERIOD = 18.0;
const float FLAT_WARP_PERIOD = 14.0;
const float FLAT_PUFF0_PERIOD = 3.2;
const float FLAT_PUFF1_PERIOD = 1.7;

float flat_margin(vec2 xy) {
    float cov = fc.look.x;
    if (cov < 0.005) return -1.0;
    float size = fc.layer.z;
    // The warp: a smooth flow field -- the weather map's two low-frequency channels (cloud type
    // and variation), sampled at a long wavelength -- displacing the field by up to ~0.6 puffs.
    // Gentle on purpose: a warp whose displacement changes faster than the space it moves
    // through folds the field over itself and tears the clouds into shreds.
    vec2 wn = textureLod(u_weather, (xy + fc.drift0.zw) / (size * FLAT_WARP_PERIOD) + vec2(0.31, 0.77), 0.0).gb;
    vec2 warp = (wn - 0.5) * 1.2 * size * fc.flow.x;
    vec2 p = xy + warp;
    float groups = textureLod(u_weather, (p + fc.drift0.xy) / (size * FLAT_GROUP_PERIOD), 0.0).r;
    float puff0 = textureLod(u_shape, vec3((p + fc.drift1.xy) / (size * FLAT_PUFF0_PERIOD), 0.37 + fc.flow.y), 0.0).r;
    float puff1 = textureLod(u_shape, vec3((p + fc.drift1.zw) / (size * FLAT_PUFF1_PERIOD) + vec2(0.53, 0.21),
                                           0.71 + fc.flow.y), 0.0).r;
    float puffs = mix(puff0, puff1, 0.35);
    return groups + (puffs - 0.75) * 0.5 - (1.0 - cov);   // 0.75: the puffs' mean
}

// The puff's top above the layer's base for a margin: a dome rising over the cloud's edge band.
float flat_top_of(float m) { return sqrt(clamp(m / 0.3, 0.0, 1.0)) * fc.layer.y; }

#endif // TOY_FLAT_CLOUD_FIELD_GLSL

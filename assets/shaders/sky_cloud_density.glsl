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
// planet's centre below the camera), g_radius, g_spin and g_march_origin before calling
// layer_height() / cloud_density().

// The atmosphere transmittance table's bottom radius (metres). This is a property of the
// ATMOSPHERE MODEL, not of the world's geometry: sky_clouds.frag feeds it to sky_transmittance()
// as `ozone.w` in km. A toy planet's own radius must never be substituted here -- the LUT is
// built for an Earth-sized air column, and a 122-unit radius would index it far off its domain.
// The geometric radius is g_radius below, and on a flat world the two happen to coincide.
const float ATMO_BOTTOM_R = 6360000.0;
const float WEATHER_PERIOD = 13000.0;    // metres the weather map spans before it repeats
const float SHAPE_PERIOD = 5200.0;       // metres the base-shape volume spans
const float DETAIL_PERIOD = 820.0;       // metres the detail volume spans
const float CLOUD_SIGMA = 0.02;          // extinction (1/m) at density 1

float remap(float v, float lo, float hi, float nlo, float nhi) {
    return nlo + (v - lo) / max(hi - lo, 1e-5) * (nhi - nlo);
}

// Set by the caller (sky_clouds.frag / cloud_shadow_volumetric.frag) before any call below.
vec3  g_center;   // the planet's centre: the real one, or a fake one under the camera when flat
float g_radius;   // the GEOMETRIC radius height 0 lies at -- ATMO_BOTTOM_R when flat
vec4  g_spin;     // xyz = the deck's rotation axis, w = angle (rad); w == 0 and/or flat: no spin
/// Where the march's own origin sits in the WORLD, in cloud space.
///
/// A march whose rays start at the camera (sky_clouds.frag, whose matrices are rebased there so
/// that large coordinates never reach them) sets the camera's position; one whose points are
/// already world positions (cloud_shadow_volumetric.frag) sets zero. The field is a property of
/// the WORLD, and this is what carries a sample out of the march's own frame and into one: a
/// domain left in a camera-relative frame would travel with the camera, sliding the whole cloud
/// pattern across the world as the eye moves.
vec3  g_march_origin;

/// How far toward `cf.albedo_deep` the last sample's colour lies, 0..1. Written by
/// cloud_density(), read by the march one line later.
///
/// Two sources, and which one is live is the world's own answer to "what makes these clouds
/// coloured". A BANDED world takes it from the belt the sample sits in, so a band is one colour
/// end to end -- which is what a gas giant looks like. Anywhere else it is the sample's own
/// density, so thin edges stay bright and thick cores take the deeper tint, which is what a
/// water cloud looks like. At the default albedos the two are the same colour and neither shows.
float g_tint;

/// The point the cloud field is evaluated at: `p` turned backwards by the deck's rotation.
///
/// A flat world slides the field across the world by adding `cf.wind.xy` (done at the call
/// site, which keeps its double-wrapped offset); a planet instead turns the whole deck rigidly
/// about the planet's pole, because a translation has no meaning on a sphere -- it would stretch
/// at one pole and pile up at the other. One rotation here serves the weather, shape and detail
/// lookups alike, so all three stay locked together as the deck moves.
vec3 cloud_domain(vec3 p) {
    if (g_spin.w == 0.0) return p;
    vec3 d = p - g_center;
    float angle = -g_spin.w;
    // ZONAL JETS. A rigid turn gives one deck moving as a plate; a real atmosphere does not --
    // Jupiter's belts and zones run past one another at different speeds, and that shear is what
    // draws them out into bands in the first place. The angle is scaled by latitude rather than
    // the rate being, so the field stays a function of position: a sample's total turn depends
    // only on where it is, and nothing has to be integrated per band.
    //
    // `bands.x` sets how many belts alternate between prograde and retrograde, and the
    // equator-weighted `1 - lat^2` keeps the poles near the deck's own rate (a polar vortex is a
    // storm, not a jet). At `bands.w` 0 this is exactly the rigid turn it replaced.
    if (cf.bands.w != 0.0 && cf.bands.x > 0.0) {
        float len = max(length(d), 1e-5);
        float lat = clamp(dot(d / len, g_spin.xyz), -1.0, 1.0);
        angle *= 1.0 + cf.bands.w * cos(lat * cf.bands.x * 3.14159265) * (1.0 - lat * lat);
    }
    // Rodrigues, by -angle: the field turns one way so the clouds appear to turn the other.
    float c = cos(angle), s_ = sin(angle);
    vec3  k = g_spin.xyz;
    return g_center + d * c + cross(k, d) * s_ + k * (dot(k, d) * (1.0 - c));
}

/// How much a belt at this point raises or lowers the coverage, and which band it is in.
///
/// Latitude about the deck's own pole, banded into alternating belts and zones. `phase` comes
/// back as 0 in the middle of a zone and 1 in the middle of a belt, which is what carries a
/// band's COLOUR (the march blends the two albedos by it) -- so the light bands and the dark
/// ones are the same two numbers seen from either end.
///
/// Returns 1 and a phase of 0 when `bands.x` is zero, which is every earthlike sky.
float cloud_band(vec3 q_centred, out float phase) {
    phase = 0.0;
    if (cf.bands.x <= 0.0) return 1.0;
    float len = max(length(q_centred), 1e-5);
    float lat = clamp(dot(q_centred / len, g_spin.xyz), -1.0, 1.0);
    float wave = sin(lat * cf.bands.x * 3.14159265);
    // Sharpness squares the belts off (>1) or softens them into a swell (<1). Signed power, so
    // the shaping is symmetric about the band edge rather than biased to one side.
    float sharp = max(cf.bands.z, 0.05);
    float shaped = sign(wave) * pow(abs(wave), 1.0 / sharp);
    phase = shaped * 0.5 + 0.5;
    return clamp(1.0 + cf.bands.y * shaped, 0.0, 2.0);
}

/// A cyclone's effect at one point: swirls `q` about the storm's own axis and reports how much
/// the storm raises the coverage there.
///
/// The same four numbers are a hurricane on an earthlike world and a Great Red Spot on a giant:
/// a swirl that falls off from the wall, a raised wall, and a cleared eye. Costs one dot per
/// storm outside its disc, which is almost every sample.
float cloud_storms(inout vec3 q_centred) {
    int n = int(cf.storms.x);
    if (n <= 0) return 0.0;
    float len = max(length(q_centred), 1e-5);
    vec3 dir = q_centred / len;
    float lift = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (i >= n) break;
        float cd = dot(dir, cf.storm[i].xyz);
        if (cd <= cf.storm[i].w) continue;            // outside this storm's disc
        // 0 at the rim .. 1 at the eye, in the cosine the test already computed.
        float t = (cd - cf.storm[i].w) / max(1.0 - cf.storm[i].w, 1e-5);
        vec4 sp = cf.storm_spin[i];
        // The swirl: strongest just inside the rim and winding tighter toward the middle, which
        // is what draws the field out into the spiral arms a cyclone reads by.
        float angle = sp.x * t * t;
        if (angle != 0.0) {
            vec3 k = cf.storm[i].xyz;
            float c = cos(angle), s_ = sin(angle);
            q_centred = q_centred * c + cross(k, q_centred) * s_ + k * (dot(k, q_centred) * (1.0 - c));
        }
        // A RING, NOT A DISC: the wall stands just outside a cleared eye, which is the one
        // feature that reads as a cyclone from any distance. `t` runs 0 at the rim to 1 at the
        // middle, so the eye is the innermost `sp.z` of the radius.
        float inner = 1.0 - clamp(sp.z, 0.0, 0.9);
        float in_eye = smoothstep(inner, inner + 0.04, t);
        float wall = smoothstep(inner - 0.25, inner, t) * (1.0 - in_eye);
        // Negative inside the eye: the storm CLEARS it rather than merely not filling it, so a
        // cyclone over an overcast deck still has a hole in the middle.
        lift = max(lift, (sp.y * t + sp.w * wall) * (1.0 - in_eye) - in_eye);
    }
    return lift;
}

// Height in the layer, 0 at the base .. 1 at the top.
float layer_height(vec3 p) {
    return (length(p - g_center) - g_radius - cf.slab.x) / cf.slab.y;
}

/// The two weather reads at one planar coordinate: the second rotated, ~2.4x larger and
/// offset, so neither period shows as rows of identical clouds toward the horizon.
void weather_reads(vec2 xy, float lod, out vec4 m1, out vec4 m2) {
    m1 = textureLod(u_weather, xy / WEATHER_PERIOD, lod);
    vec2 xy2 = mat2(0.6, 0.8, -0.8, 0.6) * xy / (WEATHER_PERIOD * 2.37) + vec2(0.61, 0.17);
    m2 = textureLod(u_weather, xy2, max(lod - 1.25, 0.0));
}

// The weather at a (wind-shifted) position: x = local coverage 0..1 (how strongly this column
// is a cloud), y = cloud type, z = the base's lift (fraction of the layer).
//
// `q` is the domain point in the field's own frame -- on a planet that frame is centred on it --
// and `planar` is the flat-world projection.
// On a FLAT world the map is read at `planar` exactly as it always was. On a PLANET the same
// tiling map is folded over the sphere triplanar-wise -- three reads along the cardinal planes,
// weighted by the sharpened radial direction. A direct xy projection cannot be used there: it
// maps the two poles to one point and sends both hemispheres the same column, and a cube-face
// parametrisation (the engine's own PlanetLayout convention) would be uniform but discontinuous
// at all twelve cube edges, drawing straight seams in the cover right round the globe. Folding
// a TILING map has neither failure: it is seam-free and pole-free, and every constant below
// stays as tuned. The sharpened weights leave one projection dominant almost everywhere, so
// the usual cost is one or two reads' worth rather than three.
vec3 weather_at(vec3 q, vec2 planar, float lod) {
    vec4 m1, m2;
    if (cf.world_frame.w > 0.0) {
        vec3 n = normalize(q);                // the field's frame is the planet's, so q is radial
        vec3 w = n * n;                       // already non-negative and sums to 1
        w = w * w;                            // sharpen: one plane dominates away from the diagonals
        w /= max(w.x + w.y + w.z, 1e-5);
        m1 = vec4(0.0);
        m2 = vec4(0.0);
        // Each plane reads the two coordinates that lie IN it, so the fold is continuous.
        if (w.z > 0.01) { vec4 a, b; weather_reads(q.xy, lod, a, b); m1 += w.z * a; m2 += w.z * b; }
        if (w.y > 0.01) { vec4 a, b; weather_reads(q.xz, lod, a, b); m1 += w.y * a; m2 += w.y * b; }
        if (w.x > 0.01) { vec4 a, b; weather_reads(q.yz, lod, a, b); m1 += w.x * a; m2 += w.x * b; }
    } else {
        weather_reads(planar, lod, m1, m2);
    }
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
    // The domain the field is read in, anchored to the world however the march itself is framed
    // (g_march_origin). A planet reads it in its OWN centred frame -- the frame the deck turns in,
    // and the one that leaves the field independent of where the planet was placed and of where
    // the camera is. A flat world reads it at the absolute position, slid by the wrapped wind
    // offset kept in double precision on the CPU so it never jumps; a planet turns it instead
    // (cloud_domain).
    vec3 q  = cf.world_frame.w > 0.0 ? cloud_domain(p) - g_center
                                     : cloud_domain(p) + g_march_origin;
    // The cyclones warp the domain before anything reads it, so the shape volume and the
    // weather map spiral together rather than one sliding over the other.
    float storm_lift = cf.world_frame.w > 0.0 ? cloud_storms(q) : 0.0;
    float band_phase = 0.0;
    float band = cf.world_frame.w > 0.0 ? cloud_band(q, band_phase) : 1.0;
    g_tint = cf.bands.x > 0.0 ? band_phase : 0.0;
    vec2 xy = q.xy + cf.wind.xy;
    vec3 w = weather_at(q, xy, log2(max(foot / (WEATHER_PERIOD / 512.0), 1.0)));
    // The belts and the storms act on the COVERAGE the weather left, not on the map itself:
    // one tiling map still places the clouds, and these say how much of it survives here.
    w.x = clamp(w.x * band + storm_lift, 0.0, 1.0);
    if (w.x <= 0.0) { empty = true; return 0.0; }
    // Base height varies cloud to cloud, so the deck's bottoms don't line up.
    float hl = (h - w.z) / (1.0 - w.z);
    float profile = height_profile(hl, w.y);
    if (profile <= 0.0) return 0.0;

    float z = h * cf.slab.y;
    // On a PLANET the shape volume is read at the real 3D domain point. u_shape is a repeating
    // 128^3 volume (see cloud_noise_texture.h), so a 3D position tiles it seamlessly in every
    // direction -- no spherical parametrisation, no poles, no seams, and the layer's own radial
    // `h` still drives the height profile above. A flat world keeps the planar construction.
    vec3 sp = (cf.world_frame.w > 0.0 ? q / SHAPE_PERIOD : vec3(xy, z) / SHAPE_PERIOD)
            + vec3(0.0, 0.0, cf.wind.z * 2e-5);
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
            vec3 dp = (cf.world_frame.w > 0.0 ? q / DETAIL_PERIOD : vec3(xy, z * 1.3) / DETAIL_PERIOD)
                    + (s.gba - 0.5) * 0.35;
            vec4 dn = textureLod(u_detail, dp, lod);
            float hf = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
            hf = mix(hf, 1.0 - hf, clamp(hl * 4.0, 0.0, 1.0));   // wisps at the base, billows above
            d = clamp(remap(d, hf * 0.45 * fade, 1.0, 0.0, 1.0), 0.0, 1.0);
        }
    }
    float dens = min(d * CONTRAST, 1.0) * cf.slab.w;
    // Where there are no bands the tint comes from the cloud's own body instead: thin edges keep
    // the bright albedo and thick cores take the deeper one. `albedo.w` is how quickly that
    // happens, and at 0 it never does -- which is every sky that has not asked for it.
    if (cf.bands.x <= 0.0) g_tint = clamp(dens * cf.albedo.w, 0.0, 1.0);
    return dens;
}

#endif // TOY_SKY_CLOUD_DENSITY_GLSL

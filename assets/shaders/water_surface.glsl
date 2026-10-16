// water_surface.glsl -- the "water" derived transparent shader: Gerstner waves, flow-advected
// ripples, depth-based colour/opacity, shoreline and contact foam, whitewater on rapids and
// behind obstacles. water.vert/water.frag are thin includers of this file
// over the transparent vertex/fragment backbones.
//
// Everything is driven by toy::water::WaterSystem (toyengine/water/water_system.h), which
// bakes the mesh and writes the material:
//
//   per vertex   uv      = (water depth below the vertex in m, turbulence 0..1)
//                tangent = (object-space surface current m/s, 2 + wave scale) -- w > 1.5 marks a
//                          baked water vertex and w - 2 is how tall its waves stand (1 = fully,
//                          lower where a surface must calm down without the body-wide wave change
//                          that would pull a vertex it SHARES with the body next to it apart --
//                          a river meeting its lake, toy::world::TerrainParams::river_wave_scale).
//                          Any other mesh (w == +-1, a plain tangent) is read as still, deep
//                          water waving fully, so a YAML-only `shader: water` mesh still draws.
//   gfx_params        = base wave: amplitude, wavelength, direction (radians), steepness
//   gfx_params_ext0   = foam colour rgb, foam amount           (forward pass only)
//   gfx_params_ext1   = shore foam depth, edge fade depth, ripple strength, ripple scale
//   (water_planet, compiled with WATER_LOCAL_FRAME: a local-frame body -- a planet's water. Its
//    waves come from the world position through the three axis planes (triplanar) and lift the
//    surface along its normal, so the many tilted pieces of a planet's sea meet without seams,
//    and the per-pixel detail uses the world plane facing the surface most squarely. Without
//    the define -- the "water" shader -- none of that code exists.)
//   gfx_time.w        = the water clock (WaterSystem::render_time()) -- waves animate on it, NOT
//                       gfx_time.x, so the drawn surface and buoyancy's CPU queries share a phase
//   refraction thickness (pushed) = clarity: depth in m at which the water reads ~63% opaque.
//                          The hook replaces it with the MEASURED depth for Beer-Lambert.
//
// The vertex hook writes custom = (flow_ws.xy, turbulence, crest) for the fragment hook
// (GFX_SURFACE_CUSTOM_VARYING). The fragment hook reads the opaque scene depth (u_hiz_map,
// mip 0) to measure how much water lies under each pixel: that one number drives the
// shallow-to-deep colour, the soft contact edge, and the foam band along shores and around
// anything that pierces the surface -- static rocks and floating crates alike.
//
// Ripple rings from moving bodies arrive in forward_globals (water_ripple_rings()), and the
// surface is two-sided: from below it shows Snell's window and total internal reflection.
//
// Distance and quality (toy::water::WaterSettings, water_quality in config.yaml). The vertex
// stage fades each derived wave out with camera distance (water_waves.glsl). The fragment stage
// reads forward_globals.water_ripple_info.yzw = (flow-ripple layers, detail distance, ring range):
// past the detail distance the ripples and foam noise are skipped for a rougher surface, and
// rings are only looped over within the ring range.
//
// No shadow entry points: water is BLEND, and BLEND materials only cast shadows at full
// opacity (see record_directional_shadow_'s doc) -- water never reaches the shadow pass.

#include "water_waves.glsl"

// ---------------------------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------------------------

/**
 * The vertex spacing a BAKED (untessellated) water tile is drawn at, as metres per metre of
 * distance -- the shape water_wave_mesh_fade() wants.
 *
 * The renderer picks a tile's LOD by its projected size, so whatever the distance a tile is
 * drawn with edges of roughly a fixed number of render pixels (toy::water::k_pixel_lod_factor,
 * water_tiles.h, is that rule). Neighbouring tiles sit on either side of a switch and land on
 * DIFFERENT levels, so a wave only a few spacings long is sampled differently on each side and
 * the water breaks into a patchwork of tiles meeting along their shared edges. Handing those
 * waves to the fragment stage -- the vertex stage leaves them out of the mesh, the fragment adds
 * their slope back per pixel -- is what the tessellated path already does, and it makes the
 * drawn surface a function of distance alone, the same on both sides of every seam.
 *
 * The constant is the LOD rule's ~8 px, with room for the coarser tiers' lod_bias.
 */
const float WATER_BAKED_EDGE_PIXELS = 12.0;
vec2 water_baked_lod(float pixel_scale) {
    return vec2(WATER_BAKED_EDGE_PIXELS / max(pixel_scale, 1.0), 1.0e9);
}

float water_hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float water_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = water_hash(i);
    float b = water_hash(i + vec2(1.0, 0.0));
    float c = water_hash(i + vec2(0.0, 1.0));
    float d = water_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// water_noise() and its analytic gradient in one evaluation: .x = value, .yz = d/dp. One noise
// instead of the three a finite difference needs.
vec3 water_noise_d(vec2 p) {
    vec2 i  = floor(p);
    vec2 f  = fract(p);
    vec2 u  = f * f * (3.0 - 2.0 * f);
    vec2 du = 6.0 * f * (1.0 - f);
    float a = water_hash(i);
    float b = water_hash(i + vec2(1.0, 0.0));
    float c = water_hash(i + vec2(0.0, 1.0));
    float d = water_hash(i + vec2(1.0, 1.0));
    float k1 = b - a, k2 = c - a, k3 = a - b - c + d;
    return vec3(a + k1 * u.x + k2 * u.y + k3 * u.x * u.y,
                du * vec2(k1 + k3 * u.y, k2 + k3 * u.x));
}

#ifdef WATER_LOCAL_FRAME
// The world plane the per-pixel detail runs in: the axis pair facing the surface normal most
// squarely (0: yz, 1: xz, 2: xy).
int water_frame_plane(vec3 n) {
    vec3 a = abs(n);
    return (a.x >= a.y && a.x >= a.z) ? 0 : (a.y >= a.z ? 1 : 2);
}
vec2 water_plane_coords(vec3 p, int plane) { return plane == 0 ? p.yz : (plane == 1 ? p.xz : p.xy); }
vec3 water_plane_lift(vec2 d, int plane) {
    return plane == 0 ? vec3(0.0, d) : (plane == 1 ? vec3(d.x, 0.0, d.y) : vec3(d, 0.0));
}
#endif

// ---------------------------------------------------------------------------------------------
// Vertex: waves, flow hand-off
// ---------------------------------------------------------------------------------------------

#ifdef GFX_SURFACE_VERTEX
void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    bool  baked      = v.tangent_os.w > 1.5;
    float depth      = baked ? v.uv.x : 1000.0;
    float turbulence = baked ? v.uv.y : 0.0;
    vec3  flow_ws    = baked ? mat3(v.model) * v.tangent_os.xyz : vec3(0.0);
    // How tall this vertex's waves stand (see the file doc). A function of the corner alone, so
    // two bodies sharing it displace it identically and no gap can open between them.
    float wave_scale = baked ? clamp(v.tangent_os.w - 2.0, 0.0, 1.0) : 1.0;
    vec4  wave_base  = vec4(gfx_params.x * wave_scale, gfx_params.yzw);

#ifdef WATER_LOCAL_FRAME
    {
        // The waves of the world position, through the three axis planes blended by how
        // squarely each faces the surface, lifting it along its normal only: a function of the
        // world position alone, so every piece of a planet's sea moves a shared vertex alike.
        vec3  n0    = normalize(v.normal_ws);
        vec3  blend = n0 * n0;
        blend /= max(blend.x + blend.y + blend.z, 1e-4);
        float atten = water_depth_attenuation(depth, gfx_params.y);
        float view  = distance(camera.camera_pos, v.position_ws);
        float h = 0.0, crest = 0.0;
        vec3  tilt = vec3(0.0);
        for (int k = 0; k < 3; ++k) {
            if (blend[k] < 0.02) continue;
            WaterWave w = water_gerstner(wave_base, water_plane_coords(v.position_ws, k), gfx_time.w, atten, view, gfx_tess_lod);
            h     += blend[k] * w.displacement.z;
            tilt  += blend[k] * water_plane_lift(w.normal.xy, k);
            crest += blend[k] * w.crest;
        }
        v.position_ws += n0 * h;
        v.normal_ws = normalize(n0 + tilt - n0 * dot(tilt, n0));
        vec3 ref = abs(v.normal_ws.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
        v.tangent_ws = normalize(ref - v.normal_ws * dot(ref, v.normal_ws));
        v.custom = vec4(water_plane_coords(flow_ws, water_frame_plane(v.normal_ws)), wave_scale, crest);
        return;
    }
#endif

    // The waves the mesh is too coarse to hold at this distance are left out and shaded back in
    // per pixel by the fragment stage: gfx_tess_lod while tessellating, water_baked_lod() on a
    // baked tile. Either way what the mesh carries is a function of distance alone, so the
    // hand-over from one LOD (or tile, or body) to the next is a gradient, not an aliased edge.
    vec2 mesh_lod = gfx_tess_lod.y > 0.0 ? gfx_tess_lod : water_baked_lod(camera.jitter_ndc.z);
    WaterWave w = water_gerstner(wave_base, v.position_ws.xy, gfx_time.w,
                                 water_depth_attenuation(depth, gfx_params.y),
                                 distance(camera.camera_pos, v.position_ws), mesh_lod);
    v.position_ws += w.displacement;

    // Tilt the mesh's own normal (not +Z: a river surface slopes) by the wave slope.
    v.normal_ws = normalize(v.normal_ws + vec3(w.normal.xy, w.normal.z - 1.0));

    // The mesh tangent slot carried the flow, not a tangent: rebuild a valid one (water has
    // no normal map, so only its orthogonality to N matters).
    vec3 ref = abs(v.normal_ws.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    v.tangent_ws = normalize(ref - v.normal_ws * dot(ref, v.normal_ws));

    // custom.z carries the wave scale, not the turbulence: the fragment stage reads turbulence
    // straight from its own interpolated uv.y (the same per-vertex channel), and needs the scale
    // to shade back exactly the waves this vertex left out of the mesh.
    v.custom = vec4(flow_ws.xy, wave_scale, w.crest);
}
#endif // GFX_SURFACE_VERTEX

// ---------------------------------------------------------------------------------------------
// Fragment: ripples, depth, foam
// ---------------------------------------------------------------------------------------------

#ifdef GFX_SURFACE_FRAGMENT

// Slope of the ripple height field noise(p) * 0.6 + noise(p * 2.31 + offset) * 0.4, analytic.
vec2 water_ripple_slope(vec2 p) {
    return water_noise_d(p).yz * 0.6 + water_noise_d(p * 2.31 + vec2(17.3, 5.1)).yz * (0.4 * 2.31);
}

// Two-phase flow-map advection (Vlachos, "Water Flow in Portal 2"): two copies of a pattern
// are dragged along the flow, each reset every period while the other is at full weight, so a
// spatially varying current never smears the pattern indefinitely.
const float WATER_FLOW_PERIOD = 1.6;

vec2 water_flow_ripples(vec2 pos, vec2 flow, float t, float scale) {
    float ph0 = fract(t / WATER_FLOW_PERIOD);
    float ph1 = fract(t / WATER_FLOW_PERIOD + 0.5);
    vec2  s0  = water_ripple_slope((pos - flow * ph0 * WATER_FLOW_PERIOD) * scale);
    vec2  s1  = water_ripple_slope((pos - flow * ph1 * WATER_FLOW_PERIOD) * scale + vec2(0.37, 0.71));
    float w1  = abs(1.0 - 2.0 * ph0); // 1 when copy 0 resets, 0 at its midpoint
    return mix(s0, s1, w1);
}

float water_flow_noise(vec2 pos, vec2 flow, float t, float scale) {
    float ph0 = fract(t / WATER_FLOW_PERIOD);
    float ph1 = fract(t / WATER_FLOW_PERIOD + 0.5);
    float n0  = water_noise((pos - flow * ph0 * WATER_FLOW_PERIOD) * scale);
    float n1  = water_noise((pos - flow * ph1 * WATER_FLOW_PERIOD) * scale + vec2(3.7, 9.1));
    return mix(n0, n1, abs(1.0 - 2.0 * ph0));
}

// Expanding rings from moving and splashing bodies (toy::water::WaterSystem's ripples, handed
// over per frame in forward_globals). Each ring is a short wave packet travelling outward from
// its start radius, widening and decaying as it ages; young strong rings carry a foam crest.
// Adds the packets' radial slope to `slope` and their foam to `foam`.
const float WATER_RIPPLE_SPEED = 1.4;   // m/s outward
const float WATER_RIPPLE_K     = 10.0;  // rad/m -- ~0.6 m between crests

void water_ripple_rings(vec2 pos, inout vec2 slope, inout float foam) {
    int n = min(int(forward_globals.water_ripple_info.x), WATER_MAX_RIPPLES);
    for (int i = 0; i < n; ++i) {
        vec4  a     = forward_globals.water_ripples[2 * i];
        float r0    = forward_globals.water_ripples[2 * i + 1].x;
        float age   = a.z;
        vec2  d     = pos - a.xy;
        float dist  = length(d);
        float sigma = 0.18 + age * 0.22;
        float x     = dist - (r0 + age * WATER_RIPPLE_SPEED);
        if (abs(x) > 3.0 * sigma || dist < r0 * 0.5) continue;
        float env = exp(-x * x / (2.0 * sigma * sigma)) * a.w * exp(-age * 0.9);
        slope += d / max(dist, 1e-3) * env * cos(WATER_RIPPLE_K * x) * 2.0;
        foam = max(foam, step(0.5, env * (0.6 + 0.4 * sin(WATER_RIPPLE_K * x))) * step(age, 1.2));
    }
}

void gfx_surface_fragment(inout GfxTransparentSurface s) {
    float t     = gfx_time.w;
#ifdef WATER_LOCAL_FRAME
    int   plane = water_frame_plane(s.normal_ws);
    vec2  pos   = water_plane_coords(s.position_ws, plane);
#else
    vec2  pos   = s.position_ws.xy;
#endif
    vec2  flow  = s.custom.xy;
    float turb  = s.uv.y;        // the baked turbulence (see the file doc's vertex packing)
    float wave_scale = s.custom.z;
    float crest = s.custom.w;

    vec3  foam_color      = gfx_params_ext0.rgb;
    float foam_amount     = gfx_params_ext0.a;
    float shore_depth     = max(gfx_params_ext1.x, 1e-3);
    float edge_depth      = max(gfx_params_ext1.y, 1e-3);
    float ripple_strength = gfx_params_ext1.z;
    float ripple_scale    = max(gfx_params_ext1.w, 1e-3);
    // water_quality (toy::water::WaterSettings), via forward_globals.water_ripple_info.
    int   ripple_layers   = int(forward_globals.water_ripple_info.y + 0.5);
    float detail_distance = max(forward_globals.water_ripple_info.z, 1.0);
    float ring_range      = forward_globals.water_ripple_info.w;

    // Detail falls off with distance: past detail_distance the per-pixel ripples and foam noise
    // are below what a pixel resolves (they would only sparkle), so they are skipped and the
    // surface turns rougher instead -- the filtered look of the ripples it skips.
    float view_dist = distance(camera.camera_pos, s.position_ws);
    float detail    = 1.0 - smoothstep(0.6 * detail_distance, detail_distance, view_dist);

    // Still water still drifts a little with the wind (the base wave's direction).
    vec2 wind = vec2(cos(gfx_params.z), sin(gfx_params.z)) * 0.25;
    vec2 drift = flow + wind;

    // --- Ripples: advected along the current, stronger where the water is churned up ---
    vec2  slope = vec2(0.0);
    float ring_foam = 0.0;
    if (detail > 0.0) {
        if (ripple_layers >= 2) {
            slope = water_flow_ripples(pos, drift, t, ripple_scale) * 0.6
                  + water_flow_ripples(pos * 1.9 + vec2(4.3, 1.7), drift * 1.3, t + 0.4, ripple_scale) * 0.4;
        } else {
            slope = water_flow_ripples(pos, drift, t, ripple_scale) * 0.85;
        }
        float strength = ripple_strength * (1.0 + turb * 1.5);
        slope *= strength * detail;
#ifndef WATER_LOCAL_FRAME
        if (length(camera.camera_pos.xy - pos) <= ring_range) water_ripple_rings(pos, slope, ring_foam);
#endif
    }
    // The slope of the waves the vertex stage left out of the mesh (water_wave_mesh_fade), per
    // pixel: a tessellated draw's own params (packed in refraction_ior_flags.zw), or the baked
    // tile LOD rule. Shading them here rather than displacing them is what keeps a tile's
    // surface the same whichever LOD it is drawn at -- see water_baked_lod().
#ifndef WATER_LOCAL_FRAME
    {
        uvec2 tess_bits = floatBitsToUint(material.refraction_ior_flags.zw);
        vec2 mesh_lod = tess_bits.x != 0u
                      ? vec2(max(unpackHalf2x16(tess_bits.x).x, 0.5) / max(camera.jitter_ndc.z, 1.0),
                             unpackHalf2x16(tess_bits.y).x)
                      : water_baked_lod(camera.jitter_ndc.z);
        slope += water_gerstner_residual_slope(vec4(gfx_params.x * wave_scale, gfx_params.yzw), pos,
                                               gfx_time.w, view_dist, mesh_lod);
    }
#endif
#ifdef WATER_LOCAL_FRAME
    s.normal_ws = normalize(s.normal_ws - water_plane_lift(slope, plane));
#else
    s.normal_ws = normalize(s.normal_ws - vec3(slope, 0.0));
#endif
    s.roughness = mix(max(s.roughness, 0.2), s.roughness, detail);

    // --- The underside, seen from below (water is two-sided; the backbone has already turned
    // the normal to face the viewer). Light leaving water refracts with the inverse IOR, so
    // inside Snell's window (~48.6 degrees from straight up) the world above shows through, bent;
    // outside it, total internal reflection turns the surface into a dark mirror of the water
    // below. The depth-based shore/contact terms are meaningless from this side (what lies
    // "behind" is above the water) -- UnderwaterPass fogs this surface by its in-water distance.
    // Only from BELOW, though: seen from above, a back face is a steep crest folding over on
    // itself (tessellated water resolves those), which should read as more water, not as a dark
    // underside mirror.
#ifdef WATER_LOCAL_FRAME
    if (!gl_FrontFacing) {   // a planet's water has no world "below": its back faces are its underside
#else
    if (!gl_FrontFacing && camera.camera_pos.z < s.position_ws.z) {
#endif
        vec3  Vu     = normalize(camera.camera_pos - s.position_ws);
        float cos_v  = dot(Vu, s.normal_ws);
        float window = smoothstep(0.6, 0.72, cos_v);       // critical angle: cos = 0.66
        s.ior        = 1.0 / max(s.ior, 1.0001);
        s.alpha      = mix(0.97, 0.12, window);
        s.thickness  = 0.0;
        s.roughness  = mix(0.35, s.roughness, window);
        float f = clamp(ring_foam * foam_amount, 0.0, 1.0);
        s.albedo = mix(s.albedo, foam_color, f);
        s.alpha  = mix(s.alpha, 1.0, f * 0.8);
        return;
    }

    // --- Whitewater: rapids and obstacle wakes (turbulence), wave crests ---
    // Far away the broken foam patterns are replaced by their expected coverage (the fraction of
    // the noise range each threshold passes), blended in across the detail band -- so distant
    // rapids read as partly white, not as a solid sheet or as sparkle.
    float foam = max(0.7 * turb * step(0.05, turb),
                     clamp((crest - 0.47) / 0.35, 0.0, 1.0) * step(0.35, crest));
    if (detail > 0.0) {
        float n_turb  = water_flow_noise(pos, flow, t, 2.2);
        float n_crest = water_noise(pos * 1.7 + wind * t * 2.0);
        float broken  = max(step(1.0 - turb * 0.7, n_turb) * step(0.05, turb),
                            step(0.82, crest + n_crest * 0.35) * step(0.35, crest));
        foam = mix(foam, broken, detail);
    }

    // --- Water depth under this pixel, from the opaque scene depth ---
    vec4  view_pos = camera.view * vec4(s.position_ws, 1.0);
    vec4  clip     = camera.proj * view_pos;
    vec2  suv      = ssr_ndc_to_uv(clip.xy / clip.w);
    // Linearized directly (perspective: view z = -P32 / (ndc z + P22)) -- no per-pixel inverse().
    float ndc_z    = textureLod(u_hiz_map, clamp(suv, 0.0, 1.0), 0.0).r;
    float scene_z  = -camera.proj[3][2] / (ndc_z + camera.proj[2][2]);
    float ratio    = scene_z / min(view_pos.z, -1e-4);            // >= 1 when the scene is behind
    float ray_len  = length(view_pos.xyz) * max(ratio - 1.0, 0.0); // water traversed by the view ray
    vec3  V        = normalize(camera.camera_pos - s.position_ws);
#ifdef WATER_LOCAL_FRAME
    float depth    = ray_len * abs(dot(V, s.normal_ws));          // ~depth along the surface's up
#else
    float depth    = ray_len * abs(V.z);                          // ~vertical water depth
#endif

    // Shallow-to-deep: Beer-Lambert over the real depth, and opacity rising toward the
    // material's deep colour. `clarity` arrives in the (otherwise overwritten) thickness slot.
    float clarity = max(s.thickness, 1e-3);
    float deep    = 1.0 - exp(-depth / clarity);
    float edge    = smoothstep(0.0, edge_depth, depth);
    s.alpha       = mix(s.alpha, 0.94, deep) * edge;
    s.thickness   = min(depth, clarity * 2.0) * edge;

    // --- Shore and contact foam: a broken band hugging every intersection ---
    float band     = 1.0 - clamp(depth / shore_depth, 0.0, 1.0);
    float contact  = step(depth, edge_depth * 1.5);
    // Expected coverage far away (see the whitewater note), the broken band up close.
    float shore    = max(clamp(band * band * 1.15, 0.0, 1.0), 0.75 * contact);
    if (detail > 0.0) {
        float n_shore = water_flow_noise(pos, drift * 0.5, t, 2.6);
        float lines   = step(0.86, sin(depth / shore_depth * 12.566 - t * 2.2) * 0.5 + 0.5 + n_shore * 0.25);
        float broken  = max(step(n_shore, band * band * 1.15), lines * band);
        broken        = max(broken, contact * step(0.25, n_shore)); // contact line
        shore = mix(shore, broken, detail);
    }
    foam = max(foam, shore * step(1e-3, band));

    foam = max(foam, ring_foam);
    foam = clamp(foam * foam_amount, 0.0, 1.0);
    s.albedo    = mix(s.albedo, foam_color, foam);
    s.alpha     = mix(s.alpha, 1.0, foam * 0.9);
    s.roughness = mix(s.roughness, 0.85, foam);
}
#endif // GFX_SURFACE_FRAGMENT

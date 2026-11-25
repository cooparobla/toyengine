#ifndef TOY_SKY_CLOUD_COMMON_GLSL
#define TOY_SKY_CLOUD_COMMON_GLSL

// Shared by the volumetric cloud march (sky_clouds.frag), its temporal reconstruction
// (sky_cloud_resolve.frag), its shadow map (cloud_shadow_volumetric.frag) and every reader of the
// reconstructed layer (sky_physical.glsl's upsample, cloud_composite.frag).
//
// The march runs in CLOUD SPACE: world metres divided by the layer's scale (render cloud_scale),
// so every tuned length of the cloud field (noise periods, step clamps, extinction) holds at any
// size. Depths below are cloud-space metres; multiply by the scale for world metres.
//
// A cloud texel is vec4(rgb, a): rgb = light the clouds scatter toward the camera, a packs the
// transmittance (how much of what is behind still shows) with the distance to the clouds:
//   a = depth code (integer, 0..4095: log2 of the distance over 50 m, 256 codes per octave)
//       + 4096 when the texel's full-resolution block holds sky -- equivalently, when nothing
//         stopped its ray short of the whole slab, since a block with no sky in it is exactly
//         the one the march clamps at the surface (sky_clouds.frag). Carried by the trace, the
//         reconstruction and so the history alike.
//       + transmittance * 0.999 (the fraction)
// a < 0 marks a texel outside the marched region (never composited). The cloud targets are
// RGBA32F, so the fraction keeps ~11 bits beside the 13-bit integer part.

float cloud_pack(float depth_m, float trans) {
    float code = clamp(floor(log2(max(depth_m, 50.0) / 50.0) * 256.0 + 0.5), 0.0, 4095.0);
    return code + clamp(trans, 0.0, 1.0) * 0.999;
}
float cloud_pack_sky(float depth_m, float trans, bool sky) { return cloud_pack(depth_m, trans) + (sky ? 4096.0 : 0.0); }
float cloud_trans(float a) { return min(fract(a) / 0.999, 1.0); }
float cloud_depth(float a) { return 50.0 * exp2(mod(floor(a), 4096.0) / 256.0); }
bool  cloud_sky(float a) { return a >= 4096.0; }

#ifdef CLOUD_UBO_SET
// The cloud pass's per-frame parameters (SkyCloudPass::FrameData, one buffer per frame slot).
// Lengths are in cloud space unless noted.
layout(set = CLOUD_UBO_SET, binding = CLOUD_UBO_BINDING) uniform CloudFrame {
    mat4 inv_view_proj;   // unjittered inverse(proj * view), this frame (world)
    mat4 prev_view_proj;  // unjittered proj * view, previous frame (world)
    vec4 slab;            // x = base altitude, y = thickness, z = coverage 0..1, w = density
    vec4 wind;            // xy = accumulated wind offset (wrapped), z = time (s), w = frame index
    vec4 light_dir;       // xyz = unit direction TO the light (sun or moon), w = view steps
    vec4 light_color;     // rgb = the light's illuminance, w = shadow steps
    vec4 trace;           // xy = this frame's traced pixel in each 2x2 block, zw = the marched region (px)
    vec4 history;         // xy = the clouds' drift this frame (world m), z = history valid, w = radians per region px
    vec4 camera;          // xyz = camera position (world m), ABSOLUTE: the view march's rays are
                          // rebased onto it, so it is both what g_position is measured against
                          // and where that march's origin sits in the world.
                          // w = relative lighting change since last frame
    vec4 look;            // x = cloud scale (world m per cloud m), y = camera fade 0..1,
                          // z = 1: light_color is above the atmosphere (the physical sky's
                          //     transmittance table colours it) / 0: use it as is,
                          // w = this frame's spin step (rad), planet only
    vec4 world_frame;     // xyz = the centre the world curves about, RELATIVE TO THE CAMERA
                          // (world m), w = its radius.
                          // w <= 0 is a FLAT world: the march pins a fake planet under the
                          // camera as it always has, and `spin` below is unused.
    vec4 spin;            // xyz = the axis the deck turns about (unit), w = total angle (rad).
                          // A planet's wind ROTATES the deck; a flat world's slides the noise
                          // domain by wind.xy instead. Only one is ever live.
    vec4 day_fill;        // rgb = the sky's colour at noon: the deck's ambient on the day side.
                          // w = the parallax rate (region px per frame): the camera's
                          // translation over its distance to the layer's near edge. 0 on a
                          // cut or the first frame.
    vec4 night_fill;      // rgb = its night-side ambient, already scaled by the moon. Both are
                          // zero on a flat world, where the scene's sky_zenith family is used
                          // exactly as it always was.

    // --- What the clouds are MADE of, rather than where they are ---------------------------
    //
    // Everything below is neutral at its default and a scene that sets none of it draws exactly
    // what it drew before these existed: albedo vec3(0.9) is the scalar the march used to carry,
    // and a zero in `bands.x` or `storms.x` switches its whole term off at the first compare.

    vec4 albedo;      // rgb = single-scattering albedo where a cloud is thin and lit.
                      // w = the blend's onset: how far the deep tint has taken over, 0 = never.
    vec4 albedo_deep; // rgb = the albedo the blend reaches -- a band's own colour on a giant, a
                      // thick base's grey on an earthlike world. w unused.
    vec4 bands;       // x = bands from pole to pole (0 = none, and the whole term is skipped).
                      // y = contrast: how much a band's coverage differs from a zone's, 0..1.
                      // z = sharpness: >1 squares the belts off, <1 softens them.
                      // w = jet shear: how much faster a belt turns than the deck as a whole.
    vec4 storms;      // x = how many of the four below are live. yzw unused.
    // A cyclone: a swirl in the field plus a raised wall and a cleared eye. The same four
    // numbers are a hurricane on an earthlike world and a Great Red Spot on a giant.
    vec4 storm[4];      // xyz = unit direction from the world's centre to the eye, w = cos(angular radius)
    vec4 storm_spin[4]; // x = swirl at the wall (rad), y = coverage lift, z = eye fraction of the
                        // radius, w = how hard the wall itself is raised over the lift
} cf;
#endif

#endif // TOY_SKY_CLOUD_COMMON_GLSL

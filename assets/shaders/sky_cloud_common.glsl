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
//       + 4096 when the texel's full-resolution block holds sky (reconstructed layer only)
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
    vec4 camera;          // xyz = camera position (world m), w = relative lighting change since last frame
    vec4 look;            // x = cloud scale (world m per cloud m), y = camera fade 0..1,
                          // z = 1: light_color is above the atmosphere (the physical sky's
                          //     transmittance table colours it) / 0: use it as is, w = unused
} cf;
#endif

#endif // TOY_SKY_CLOUD_COMMON_GLSL

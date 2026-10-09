// Shared by the cloud march (sky_clouds.frag) and its temporal reconstruction
// (sky_cloud_resolve.frag).
//
// A traced cloud texel is vec4(rgb, a): rgb = light the clouds scatter toward the camera, a packs the
// transmittance (how much of the sky behind still shows) with the distance to the clouds the
// reconstruction reprojects by:
//   a = depth code (integer, 0..4095: log2 of the distance over 50 m, 256 codes per octave)
//       + transmittance * 0.999 (the fraction)
// a < 0 marks a texel whose full-resolution block holds no sky (skipped, never composited).
// The trace target is RGBA32F, so the fraction keeps ~11 bits beside a 12-bit code.

float cloud_pack(float depth_m, float trans) {
    float code = clamp(floor(log2(max(depth_m, 50.0) / 50.0) * 256.0 + 0.5), 0.0, 4095.0);
    return code + clamp(trans, 0.0, 1.0) * 0.999;
}
float cloud_trans(float a) { return min(fract(a) / 0.999, 1.0); }
float cloud_depth(float a) { return 50.0 * exp2(floor(a) / 256.0); }

#ifdef CLOUD_UBO_SET
// The cloud pass's per-frame parameters (SkyCloudPass::FrameData, one buffer per frame slot).
layout(set = CLOUD_UBO_SET, binding = CLOUD_UBO_BINDING) uniform CloudFrame {
    mat4 inv_view_proj;   // unjittered inverse(proj * view), this frame
    mat4 prev_view_proj;  // unjittered proj * view, previous frame
    vec4 slab;            // x = base altitude (m), y = thickness (m), z = coverage 0..1, w = density
    vec4 wind;            // xy = accumulated wind offset (m), z = time (s), w = frame index
    vec4 light_dir;       // xyz = unit direction TO the light (sun or moon), w = view steps
    vec4 light_color;     // rgb = the light's illuminance above the atmosphere, w = shadow steps
    vec4 trace;           // xy = this frame's traced pixel in each 2x2 block, zw = the marched region (px)
    vec4 history;         // xy = the clouds' drift this frame (m), z = history valid, w = radians per region px
    vec4 camera;          // xyz = camera position (m), w = relative lighting change since last frame
} cf;
#endif

#version 450

// Stylize overlay: optional bloom composite -> exposure+ACES tonemap ->
// depth/normal-discontinuity outline (alpha-blended over the input color) ->
// ordered (Bayer) dither -> palette quantization, in that order. Bloom runs
// first (and in linear HDR) since it's an additive light contribution, not
// a stylistic filter over the final image the way the other four are.
//
// The tonemap step is optional: a full-PBR renderer with its own tonemap/AA
// chain (e.g. blendy) leaves `exposure` <= 0 and feeds this shader
// already-tonemapped LDR input, so nothing double-tonemaps. A consumer with
// no tonemap step of its own (e.g. toyengine) sets `exposure` > 0 and feeds
// raw HDR scene color instead.
//
// All five effects are independently no-ops at their "off" value
// (bloom_intensity <= 0, exposure <= 0, outline_thickness <= 0,
// dither_strength <= 0, palette_count <= 0), so a consumer can leave this
// pass always-constructed and always-executed and simply zero the fields it
// doesn't want.

layout(location = 0) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D scene_color;  // already tonemapped LDR
layout(set = 0, binding = 1) uniform sampler2D scene_depth;  // gbuffer depth
layout(set = 0, binding = 2) uniform sampler2D scene_normal; // gbuffer world normal (+ metallic in .a)
layout(set = 0, binding = 3) uniform sampler2D palette_lut;  // Nx1 RGBA8, NEAREST
// Finished bloom image from BloomPass: already bright-pass filtered and blurred
// through a downsample/upsample pyramid, at HALF this pass's resolution. Sampled
// with a plain texture() through a LINEAR sampler -- the bilinear upsample to full
// resolution is free and is exactly what a soft additive glow wants.
layout(set = 0, binding = 4) uniform sampler2D bloom_tex;
// 1x1 adapting exposure multiplier from ExposurePass, or a 1x1 texture of 1.0
// when auto-exposure is off -- so the read needs no separate flag.
layout(set = 0, binding = 5) uniform sampler2D exposure_tex;
// Colour-grading strip LUT (see GradingLut / gfx/grading.glsl); grading_size <= 1
// disables the lookup, and the binding then points at a 1x1 dummy.
layout(set = 0, binding = 6) uniform sampler2D grading_lut;

#include <gfx/grading.glsl>

layout(push_constant) uniform StylizePushConstants {
    vec4  outline_color;      // listed first: std430 would otherwise pad around a mid-struct vec4
    vec2  inv_render_size;
    float outline_thickness;  // in texels; <= 0 disables
    float depth_threshold;
    float normal_threshold;
    float dither_strength;    // <= 0 disables
    float palette_count;      // <= 0 disables palette quantization
    float camera_near;
    float camera_far;
    float camera_is_perspective; // >= 0.5 => perspective, else orthographic
    float exposure;              // <= 0 disables the tonemap step (input is already LDR)
    float bloom_intensity;       // <= 0 disables bloom
    float auto_exposure;         // != 0 -> multiply `exposure` by exposure_tex's adapting value
    float grading_size;          // colour-grading LUT cube side; <= 1 disables the lookup
} params;

layout(location = 0) out vec4 out_color;

// 8x8 Bayer ordered-dither matrix, values 0..63.
const float BAYER8[64] = float[64](
     0,32, 8,40, 2,34,10,42,
    48,16,56,24,50,18,58,26,
    12,44, 4,36,14,46, 6,38,
    60,28,52,20,62,30,54,22,
     3,35,11,43, 1,33, 9,41,
    51,19,59,27,49,17,57,25,
    15,47, 7,39,13,45, 5,37,
    63,31,55,23,61,29,53,21
);

// True view-space distance from the camera, from raw Vulkan [0,1]
// post-projection depth. Perspective depth is hyperbolic (glm::perspective ->
// perspectiveRH_ZO); orthographic depth is already linear in the raw value
// (glm::ortho -> orthoRH_ZO), hence the branch.
float linear_depth(float d) {
    if (params.camera_is_perspective < 0.5) {
        return mix(params.camera_near, params.camera_far, d);
    }
    return params.camera_near * params.camera_far /
           (params.camera_far - d * (params.camera_far - params.camera_near));
}

bool is_background(vec3 n) { return dot(n, n) < 0.001; }

// True if uv sits on the near side of a depth or normal discontinuity, at
// n0 = the (already-normalized) normal sampled at uv.
bool is_outline_at(vec2 uv, vec3 n0, vec2 texel) {
    vec2 dx = vec2(texel.x, 0.0);
    vec2 dy = vec2(0.0, texel.y);

    vec3 nx  = texture(scene_normal, uv + dx).rgb;
    vec3 ny  = texture(scene_normal, uv + dy).rgb;
    vec3 nxn = texture(scene_normal, uv - dx).rgb;
    vec3 nyn = texture(scene_normal, uv - dy).rgb;

    // A background neighbour is an unconditional silhouette edge: there is no
    // depth or normal on that side to compare against, so if it were reachable
    // by the depth test at all, the sky sits at the far plane where hyperbolic
    // depth precision is at its worst.
    if (is_background(nx) || is_background(ny) || is_background(nxn) || is_background(nyn)) {
        return true;
    }

    float z0  = linear_depth(texture(scene_depth, uv).r);
    float zx  = linear_depth(texture(scene_depth, uv + dx).r);
    float zy  = linear_depth(texture(scene_depth, uv + dy).r);
    float zxn = linear_depth(texture(scene_depth, uv - dx).r);
    float zyn = linear_depth(texture(scene_depth, uv - dy).r);

    // 1/z is affine in screen space over any plane, however steeply it
    // recedes -- so on a plane the centre sits exactly halfway between its
    // two opposite neighbours in inverse depth, and this is identically
    // zero. Only a genuine step in Z (not just a grazing viewing angle)
    // makes it non-zero. The *z0 turns the raw 1/z gap (~Delta z / z^2) into
    // a relative step (~Delta z / z), so one depth_threshold works at any
    // distance. The signed (not abs) form keeps the line one-sided: it
    // fires on the texel that pops toward the camera, not on the surface
    // behind it.
    float rel_x = (1.0 / z0 - 0.5 * (1.0 / zx + 1.0 / zxn)) * z0;
    float rel_y = (1.0 / z0 - 0.5 * (1.0 / zy + 1.0 / zyn)) * z0;
    if (max(rel_x, rel_y) > params.depth_threshold) return true;

    float min_dot = min(min(dot(n0, normalize(nx)), dot(n0, normalize(ny))),
                         min(dot(n0, normalize(nxn)), dot(n0, normalize(nyn))));
    return min_dot < params.normal_threshold;
}

bool is_outline(vec2 uv, vec3 n0) {
    int steps = max(int(round(params.outline_thickness)), 0);
    for (int i = 1; i <= steps; ++i) {
        vec2 texel = float(i) * params.inv_render_size;
        if (is_outline_at(uv, n0, texel)) return true;
    }
    return false;
}

// ACES fitted curve (Narkowicz).
vec3 aces_film(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Exact inverse of upscale.frag's srgb_decode() -- see this pass's main() for why it runs
// here, right after the tonemap curve.
vec3 srgb_encode(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 quantize_to_palette(vec3 color) {
    int count = int(params.palette_count);
    if (count <= 0) return color;

    vec3 best = color;
    float best_dist = 1.0 / 0.0; // +inf
    for (int i = 0; i < count; ++i) {
        float u = (float(i) + 0.5) / params.palette_count;
        vec3 entry = texture(palette_lut, vec2(u, 0.5)).rgb;
        vec3 diff = entry - color;
        float dist = dot(diff, diff);
        if (dist < best_dist) {
            best_dist = dist;
            best = entry;
        }
    }
    return best;
}

void main() {
    vec3 color = texture(scene_color, in_uv).rgb;

    // Bloom: additive soft glow in linear HDR, before the tonemap, since it is a light
    // contribution rather than a stylistic filter over the final image. Thresholding already
    // happened once in bloom_prefilter.frag, per source texel and before any blurring --
    // deliberately NOT repeated here: subtracting a threshold from an already-blurred image
    // would eat the halo falloff the pyramid exists to produce, and would put a hard
    // C0 cutoff back on a value that drifts under camera motion (the shimmer this pass used
    // to have when bloom was a single reused mip lookup).
    if (params.bloom_intensity > 0.0) {
        color += texture(bloom_tex, in_uv).rgb * params.bloom_intensity;
    }

    // Tonemap, then encode to sRGB so the 8-bit UNORM write below quantizes in perceptual
    // space instead of linear -- linear quantization wastes almost all 256 codes on the top
    // half of the range and leaves darks (exactly where a bloom falloff or a sky gradient
    // lives) with a handful of codes each, which is what was producing visible banding
    // through this pass's low-slope regions. Everything from here down (outline_color, the
    // dither, palette_lut) is authored/stored display-referred, so this is also the one place
    // in the pass where the switch has to happen. Gated on the same exposure>0 branch as the
    // tonemap itself: a consumer with its own tonemap chain (exposure<=0, see this file's
    // PixelStylizePass doc) feeds already-tonemapped, already-encoded LDR and must not have
    // this reapplied.
    if (params.exposure > 0.0) {
        // Auto-exposure multiplies the configured exposure rather than replacing it,
        // so the config value keeps its meaning as the scene's baseline stop and the
        // metered term is a relative adaptation on top (HDRP's own split between
        // Fixed exposure and Exposure Compensation).
        float exposure = params.exposure;
        if (params.auto_exposure != 0.0) {
            exposure *= texture(exposure_tex, vec2(0.5)).r;
        }
        color = aces_film(color * exposure);
        color = srgb_encode(color);
    }

    // Colour grading, on DISPLAY-REFERRED colour: after the tonemap and sRGB encode
    // (a LUT authored in an image editor expects the values it would see there) and
    // before outline/dither/palette, which are stylization the grade must not touch --
    // an outline is a fixed authored colour, and quantizing to a palette after grading
    // would grade the palette entries themselves off-palette.
    color = gfx_apply_grading_lut(grading_lut, color, params.grading_size);

    vec3 n0 = texture(scene_normal, in_uv).rgb;
    if (params.outline_thickness > 0.0 && !is_background(n0) &&
        is_outline(in_uv, normalize(n0))) {
        color = mix(color, params.outline_color.rgb, params.outline_color.a);
    }

    if (params.dither_strength > 0.0) {
        ivec2 texel = ivec2(gl_FragCoord.xy);
        float t = (BAYER8[(texel.y & 7) * 8 + (texel.x & 7)] / 63.0 - 0.5) * params.dither_strength;
        color += vec3(t);
    }

    color = quantize_to_palette(clamp(color, 0.0, 1.0));

    out_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}

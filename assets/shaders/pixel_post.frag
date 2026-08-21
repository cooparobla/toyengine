#version 450

// Pixel-art post-process: outline (depth/normal discontinuity) -> exposure ->
// ordered (Bayer) dither -> palette quantization, in that order, all at the
// low internal resolution so every effect upscales as crisp blocks instead
// of shimmering. Runs as its own render pass writing into a fresh LDR
// target (post_target_) that UpscalePass then samples -- see
// PixelRenderPipeline for why this couldn't just be folded into
// pixel_lighting.frag's render pass (a second target is simpler than
// re-opening one, which pipeline::RenderPass's hardcoded LOAD_OP_CLEAR
// forbids anyway).

layout(location = 0) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D scene_color;  // offscreen_target_'s lit+sky output
layout(set = 0, binding = 1) uniform sampler2D scene_depth;  // gbuffer depth
layout(set = 0, binding = 2) uniform sampler2D scene_normal; // gbuffer G1 (world normal + metallic)
layout(set = 0, binding = 3) uniform sampler2D palette_lut;  // Nx1 RGBA8, NEAREST

layout(push_constant) uniform PostParams {
    vec2  inv_render_size;
    float exposure;
    float outline_thickness;  // in low-res texels
    vec4  outline_color;
    float depth_threshold;
    float normal_threshold;
    float dither_strength;
    float palette_count;      // 0.0 disables palette quantization
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

float linearize_depth(float d) {
    // Depth here is whatever the G-buffer's D32_SFLOAT wrote -- already in
    // [0,1] post-projection, not a true view-space distance. Good enough for
    // a discontinuity edge detector, which only cares about relative jumps.
    return d;
}

bool is_outline(vec2 uv) {
    vec2 texel = params.outline_thickness * params.inv_render_size;

    float d0 = linearize_depth(texture(scene_depth, uv).r);
    float dx = linearize_depth(texture(scene_depth, uv + vec2(texel.x, 0.0)).r);
    float dy = linearize_depth(texture(scene_depth, uv + vec2(0.0, texel.y)).r);
    float dxn = linearize_depth(texture(scene_depth, uv - vec2(texel.x, 0.0)).r);
    float dyn = linearize_depth(texture(scene_depth, uv - vec2(0.0, texel.y)).r);

    // Scale the threshold by the fragment's own depth so distant geometry
    // (whose depth buffer values compress non-linearly) doesn't outline
    // every triangle -- a fixed absolute threshold is too tight up close and
    // too loose far away.
    float scaled_threshold = params.depth_threshold * max(d0, 0.05);
    float max_delta = max(max(abs(dx - d0), abs(dy - d0)), max(abs(dxn - d0), abs(dyn - d0)));
    if (max_delta > scaled_threshold) return true;

    vec3 n0  = normalize(texture(scene_normal, uv).rgb);
    vec3 nx  = texture(scene_normal, uv + vec2(texel.x, 0.0)).rgb;
    vec3 ny  = texture(scene_normal, uv + vec2(0.0, texel.y)).rgb;
    vec3 nxn = texture(scene_normal, uv - vec2(texel.x, 0.0)).rgb;
    vec3 nyn = texture(scene_normal, uv - vec2(0.0, texel.y)).rgb;

    float min_dot = 1.0;
    if (dot(nx, nx) > 0.001)  min_dot = min(min_dot, dot(n0, normalize(nx)));
    if (dot(ny, ny) > 0.001)  min_dot = min(min_dot, dot(n0, normalize(ny)));
    if (dot(nxn, nxn) > 0.001) min_dot = min(min_dot, dot(n0, normalize(nxn)));
    if (dot(nyn, nyn) > 0.001) min_dot = min(min_dot, dot(n0, normalize(nyn)));

    return min_dot < params.normal_threshold;
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

    bool has_geometry = dot(texture(scene_normal, in_uv).rgb, texture(scene_normal, in_uv).rgb) > 0.001;
    if (has_geometry && is_outline(in_uv)) {
        color = params.outline_color.rgb;
    } else {
        color *= params.exposure;
    }

    if (params.dither_strength > 0.0) {
        ivec2 texel = ivec2(gl_FragCoord.xy);
        float t = (BAYER8[(texel.y & 7) * 8 + (texel.x & 7)] / 63.0 - 0.5) * params.dither_strength;
        color += vec3(t);
    }

    color = quantize_to_palette(clamp(color, 0.0, 1.0));

    out_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}

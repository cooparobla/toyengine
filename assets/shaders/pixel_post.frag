#version 450

// Pixel-art post-process: exposure -> ACES tonemap -> outline (depth/normal
// discontinuity, alpha-blended over the tonemapped color) -> ordered (Bayer)
// dither -> palette quantization, in that order, all at the low internal
// resolution so every effect upscales as crisp blocks instead of shimmering.
// Runs as its own render pass writing into a fresh LDR target (post_target_)
// that UpscalePass then samples -- see PixelRenderPipeline for why this
// couldn't just be folded into pixel_lighting.frag's render pass (a second
// target is simpler than re-opening one, which pipeline::RenderPass's
// hardcoded LOAD_OP_CLEAR forbids anyway). scene_color is HDR
// (offscreen_target_/ssr_pass_'s output are both R16G16B16A16_SFLOAT); the
// tonemap is what brings it back to LDR before outline/dither/palette
// quantize it -- outline runs after tonemap (not before, as it used to) so
// outline_color.a blends in the same LDR space as the rest of the image
// instead of hard-replacing it.

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
    float camera_near;
    float camera_far;
    float camera_is_perspective; // >= 0.5 => perspective, else orthographic
    float _pad0;
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
    // depth or normal on that side to compare against (the sky writes no
    // G-buffer data at all -- see skybox.frag), so if it were reachable by
    // the depth test at all, the sky sits at the far plane where hyperbolic
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

// ACES fitted curve (Narkowicz), ported from blendy/assets/shaders/tonemapping.frag.
// Always applied -- pixel_lighting.frag's sky-based indirect lighting can exceed 1.0
// regardless of which toggles are set, so this pass always brings HDR scene_color back
// into range before dither/palette quantize it.
vec3 aces_film(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
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
    vec3 color = texture(scene_color, in_uv).rgb * params.exposure;
    color = aces_film(color);

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

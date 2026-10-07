#ifndef GFX_BILATERAL_UPSAMPLE_GLSL
#define GFX_BILATERAL_UPSAMPLE_GLSL

// gfx/bilateral_upsample.glsl -- depth/normal-aware 2x2 upsample of a HALF-resolution image
// to the full-resolution G-buffer, shared by the SSR composite (gfx_ssr_fetch) and the
// half-res SSAO's upsample stage (ssao_upsample.frag). Declares nothing; every input is a
// parameter.
//
// Convention: half-res texel hc was produced from full-res G-buffer texel 2*hc + 1 -- what a
// half-res fragment's centre UV (hc + 0.5) / half_size lands on when scaled by the full size
// (ssr.frag's origin snapping, ssao.frag's px lookup). So each tap's surface is looked up
// exactly rather than guessed, and weighted by bilinear x plane-distance x normal agreement.
//
// @param lo        The half-res image (texelFetch'd; sampled bilinearly only for the fallback).
// @param lo_size   Its size in texels.
// @param full_size The full-res G-buffer size.
// @param P, N      This full-res pixel's surface (world position, unit normal).
// @param px_world  World size of one full-res texel at this pixel (ssr_texel_world_size).
vec4 gfx_bilateral_upsample(sampler2D lo, sampler2D g_position, sampler2D g_normal,
                            vec2 uv, vec2 lo_size, vec2 full_size,
                            vec3 P, vec3 N, float px_world) {
    vec2  f    = uv * lo_size - 0.5;
    ivec2 base = ivec2(floor(f));
    vec2  frac = f - vec2(base);

    vec4  sum  = vec4(0.0);
    float wsum = 0.0;

    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            ivec2 hc = clamp(base + ivec2(dx, dy), ivec2(0), ivec2(lo_size) - 1);
            ivec2 fc = clamp(hc * 2 + 1, ivec2(0), ivec2(full_size) - 1);

            vec3 Pt = texelFetch(g_position, fc, 0).rgb;
            vec3 Nt = texelFetch(g_normal,   fc, 0).rgb;

            float bw = (dx == 0 ? 1.0 - frac.x : frac.x) * (dy == 0 ? 1.0 - frac.y : frac.y);

            // Plane distance rather than raw position distance, so a tap sliding along this
            // pixel's own surface is not penalised. Normalised by a texel's world size, which
            // makes the falloff scale-free.
            float dw = exp(-abs(dot(Pt - P, N)) / max(2.0 * px_world, 1e-6));
            float nw = pow(max(dot(normalize(Nt + vec3(1e-6)), N), 0.0), 8.0);

            float w = bw * dw * nw;
            sum  += texelFetch(lo, hc, 0) * w;
            wsum += w;
        }
    }

    // Every tap rejected (a lone pixel of thin geometry, where no half-res sample shares this
    // surface): fall back to plain bilinear rather than to a hole.
    return (wsum > 1e-5) ? sum / wsum : texture(lo, uv);
}

#endif // GFX_BILATERAL_UPSAMPLE_GLSL

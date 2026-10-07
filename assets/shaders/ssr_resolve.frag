#version 450

#include <gfx/ssr_common.glsl>
#include <gfx/ycocg.glsl>

// Temporal resolve for the SSR raymarch and the traced-SSGI bounce (rgb = confidence-
// premultiplied hit radiance, a = confidence). One shader serves both chains -- SsrPass runs the
// same pipeline twice with different descriptor sets and accumulation depths.
//
// Continuous stochastic accumulation, the scheme Unity HDRP's PBR-accumulation SSR and its
// ray-tracing denoisers use: the trace re-jitters its ray every frame, and each pixel here
// maintains a running AVERAGE of those draws, blending frame N in at 1/N. The count N comes from
// the shared buffer TemporalHistoryPass publishes -- one depth-based disocclusion answer for
// every temporally accumulated effect in the frame -- so this shader needs no disocclusion test
// of its own: a rejected pixel arrives here with count 1, which weights the current frame whole.
//
// A fixed-rate exponential blend cannot do this. At a 0.85 history weight, a re-jittered
// input leaves sqrt(0.15/1.85) ~= 28% of the single-ray noise standing forever, no matter how
// long the camera holds still -- so the trace's jitter would have to be frozen at rest to get
// a static image, baking one frame's grain in. A converging average is
// static at rest AND quiet in motion, because the moving image and the resting image are the
// same many-frame estimate.
//
// Reprojection goes through the rasterized DEPTH and a clip-to-clip matrix, never through the
// G-buffer's world position: that buffer is RGBA16F, whose quantization step at world
// coordinates of a few hundred units is 0.06-0.25 wu, and a float product of two world-scale
// view-projections cancels catastrophically. Either error is whole pixels of reprojection drift,
// which makes the accumulated reflection slide and boil against the geometry in motion. Same
// correction ssao_resolve.frag documents for the identical bug.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex_current;   // NEAREST -- this frame's trace
layout(set = 0, binding = 1) uniform sampler2D tex_history;   // LINEAR  -- reprojected UV
layout(set = 0, binding = 2) uniform sampler2D u_depth;       // NEAREST -- rasterized scene depth
// Shared accumulation count (TemporalHistoryPass::kFormat, .g = count), or that pass's 1x1
// neutral texture when it never ran -- see the fallback branch in main().
layout(set = 0, binding = 3) uniform sampler2D u_count;
// Trace hit distance (world units, 0 = miss; R16F at trace resolution -- the 1x1 zero image for
// the SSGI chain), the G-buffer velocity (G4), and G-buffer normal / position-roughness. All
// NEAREST; the G-buffer ones are full resolution and tapped at in_uv.
layout(set = 0, binding = 4) uniform sampler2D u_hit_dist;
layout(set = 0, binding = 5) uniform sampler2D u_velocity;
layout(set = 0, binding = 6) uniform sampler2D g_normal_metallic;
layout(set = 0, binding = 7) uniform sampler2D g_position_roughness;

// Set 1: camera, for the virtual-point reprojection's view direction and its projection.
layout(set = 1, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

layout(push_constant) uniform PushConstants {
    // Current clip space -> previous frame's clip space, composed in double precision on the CPU.
    mat4  reproject;
    // TRACE resolution (half of the screen under ssr_half_res). Flattened vec2, matching the
    // house rule in SsaoPass's own push blocks.
    float resolution_x;
    float resolution_y;
    // Accumulation cap for THIS chain: the count is clamped to it here, so the specular and
    // diffuse chains can average over different depths while sharing one count buffer. 0
    // degenerates this pass into a passthrough of the current frame (how temporal_enabled ==
    // false is implemented).
    float max_accum;
    // Fallback history weight, used only where the shared count buffer is unavailable (count
    // channel <= 0, i.e. the 1x1 neutral texture). Reproduces the old fixed-rate blend.
    float blend_factor;
    int   history_valid;   // 0 until both a history image and a previous matrix exist
    float gamma;           // variance-clipping width, in std deviations (ssr_temporal_gamma)
    // Nonzero once the camera has been still long enough for the average to top up: accepted
    // history is then held verbatim, which is what makes a resting image byte-static without
    // ever converging onto a single noisy draw.
    int   frozen;
    // 1: reproject the surface through u_velocity, so a MOVING reflector keeps its history
    // (the camera matrix alone only knows where static geometry went).
    int   use_velocity;
    // Share of the virtual-point reprojection for mirror-like surfaces (scaled down by roughness
    // below). 0 = reproject by the reflecting surface only, the right answer for a diffuse
    // bounce but not for a sharp reflection, which moves with the reflected object's parallax.
    float virtual_blend;
} pc;

/// Slack added to the variance band before clipping. The band is `mean +/- gamma*sigma`, and on a
/// perfectly flat region sigma is 0 -- an unfloored band there collapses to a point and rejects
/// history that is correct to the last bit, stalling the accumulation exactly where it should be
/// cheapest. Scaled by the neighbourhood mean's own magnitude so it stays meaningful across the
/// HDR range this buffer carries (a floor in absolute radiance units would be enormous in a dim
/// scene and negligible in a bright one).
const float kSigmaFloor = 0.02;

/// Catmull-Rom resample of the reprojected history (9 bilinear taps, the standard 5x5-as-9
/// factorization). A plain bilinear history read is a low-pass filter applied once per frame, and
/// under sustained camera motion that repeated subpixel resampling erodes the pixel-scale detail
/// the accumulator exists to preserve -- most visibly on a sharp reflection of a thin object.
/// Negative-lobe overshoot is bounded by the variance clip in main().
vec4 sample_history_catmull_rom(vec2 uv, vec2 res) {
    vec2 sample_pos = uv * res;
    vec2 tex_pos1   = floor(sample_pos - 0.5) + 0.5;
    vec2 f  = sample_pos - tex_pos1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;
    vec2 p0  = (tex_pos1 - 1.0) / res;
    vec2 p3  = (tex_pos1 + 2.0) / res;
    vec2 p12 = (tex_pos1 + offset12) / res;
    return texture(tex_history, vec2(p0.x,  p0.y )) * (w0.x  * w0.y )
         + texture(tex_history, vec2(p12.x, p0.y )) * (w12.x * w0.y )
         + texture(tex_history, vec2(p3.x,  p0.y )) * (w3.x  * w0.y )
         + texture(tex_history, vec2(p0.x,  p12.y)) * (w0.x  * w12.y)
         + texture(tex_history, vec2(p12.x, p12.y)) * (w12.x * w12.y)
         + texture(tex_history, vec2(p3.x,  p12.y)) * (w3.x  * w12.y)
         + texture(tex_history, vec2(p0.x,  p3.y )) * (w0.x  * w3.y )
         + texture(tex_history, vec2(p12.x, p3.y )) * (w12.x * w3.y )
         + texture(tex_history, vec2(p3.x,  p3.y )) * (w3.x  * w3.y );
}

void main() {
    vec4 current = texture(tex_current, in_uv);

    // 3x3 neighbourhood of the CURRENT frame's trace. Feeds both the variance band below and the
    // no-history fallback: a pixel with no usable history -- which under camera motion means the
    // fresh disocclusions that open up along every silhouette every frame -- would otherwise show
    // one stochastic ray at the estimator's full variance. Averaging nine cuts that amplitude by
    // ~3x for eight extra taps, and the spatial denoise still runs after this.
    vec2 texel_size = 1.0 / vec2(pc.resolution_x, pc.resolution_y);
    vec4 s0 = texture(tex_current, in_uv + vec2(-1.0,  1.0) * texel_size);
    vec4 s1 = texture(tex_current, in_uv + vec2( 0.0,  1.0) * texel_size);
    vec4 s2 = texture(tex_current, in_uv + vec2( 1.0,  1.0) * texel_size);
    vec4 s3 = texture(tex_current, in_uv + vec2(-1.0,  0.0) * texel_size);
    vec4 s4 = current;
    vec4 s5 = texture(tex_current, in_uv + vec2( 1.0,  0.0) * texel_size);
    vec4 s6 = texture(tex_current, in_uv + vec2(-1.0, -1.0) * texel_size);
    vec4 s7 = texture(tex_current, in_uv + vec2( 0.0, -1.0) * texel_size);
    vec4 s8 = texture(tex_current, in_uv + vec2( 1.0, -1.0) * texel_size);

    vec4 spatial_fallback = (s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8) / 9.0;

    if (pc.history_valid == 0 || pc.max_accum <= 0.0) {
        out_color = (pc.max_accum <= 0.0) ? current : spatial_fallback;
        return;
    }

    // This pixel's exact clip position: NDC from the fragment's own UV (pixel centre, matching
    // rasterization) and the rasterized depth. u_depth is full-resolution while this pass can run
    // at half -- in_uv is normalized, so the tap scales either way, and the sampler is NEAREST so
    // it stays a point lookup when the resolutions match (ssr_half_res off).
    float depth    = texture(u_depth, in_uv).r;
    vec2  ndc      = vec2(in_uv.x * 2.0 - 1.0, -(in_uv.y * 2.0 - 1.0));
    vec4  prev_clip = pc.reproject * vec4(ndc, depth, 1.0);

    // Behind the previous frame's eye: no history exists for this point at all. The shared count
    // buffer rejects this case too, but it is cheaper to answer here than to reproject into a
    // meaningless UV first.
    if (prev_clip.w <= 0.0) {
        out_color = spatial_fallback;
        return;
    }

    vec2 prev_uv = ssr_ndc_to_uv(prev_clip.xy / prev_clip.w);

    if (pc.use_velocity != 0 && depth < 1.0) {
        vec4 vel = texture(u_velocity, in_uv);
        // No previous pose (newly spawned, or behind last frame's eye): no history at all.
        if (vel.z <= 0.0) {
            out_color = spatial_fallback;
            return;
        }
        // Sub-0.05 px motion is fp32 re-projection noise, not motion (as ssao_resolve.frag).
        vec2 gsize = vec2(textureSize(u_velocity, 0));
        vec2 v = vel.xy;
        if (length(v * gsize) < 0.05) v = vec2(0.0);
        vec2 surface_prev_uv = in_uv - v;
        // The surface moved only with the camera when both reprojections agree.
        bool surface_static = length((surface_prev_uv - prev_uv) * gsize) < 0.5;
        prev_uv = surface_prev_uv;

        // Reflection parallax (Unreal's SSR temporal reprojection by the reflected point): a
        // mirror's reflection is an image `travel` BEHIND the surface along the view ray, and
        // under camera motion it moves like that virtual point, not like the mirror. Reprojecting
        // by the surface smears a sharp reflection along the motion; by the virtual point it stays
        // attached to what it reflects. Rough reflections are a blur with no single image, so
        // they keep the surface reprojection. Only for a static reflector -- a moving one's
        // virtual point would need the reflected object's own motion too.
        if (pc.virtual_blend > 0.0 && surface_static) {
            float travel = texture(u_hit_dist, in_uv).r;
            vec4  pr     = texture(g_position_roughness, in_uv);
            float wv     = pc.virtual_blend * (1.0 - smoothstep(0.1, 0.4, pr.a));
            if (travel > 0.0 && wv > 0.0) {
                // Clip-space arithmetic relative to the surface, so no world-scale position ever
                // enters a projection: clip_surf rebuilt from NDC and the surface's own clip w
                // (its linear depth under perspective, 1 under orthographic), plus the projected
                // view-ray offset, which carries only the view's rotation.
                vec3  Vdir      = normalize(pr.xyz - camera.camera_pos);
                float w_surf    = (camera.proj[3][3] > 0.5) ? 1.0 : vel.w;
                vec4  clip_surf = vec4(ndc, depth, 1.0) * w_surf;
                vec4  clip_virt = clip_surf + camera.proj * (camera.view * vec4(Vdir * travel, 0.0));
                vec4  prev_virt = pc.reproject * clip_virt;
                if (prev_virt.w > 0.0) {
                    vec2 virt_uv = ssr_ndc_to_uv(prev_virt.xy / prev_virt.w);
                    prev_uv = mix(prev_uv, virt_uv, wv);
                }
            }
        }
    }

    if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
        out_color = spatial_fallback;
        return;
    }

    // Shared per-pixel accumulation count. A pixel whose history the depth test rejected arrives
    // with count 1, which makes the weight below 1.0 -- the current frame taken whole. Count 0 is
    // the 1x1 neutral texture (that pass never ran): fall back to the fixed-rate blend.
    float n = texture(u_count, in_uv).g;
    bool  have_count = n > 0.0;
    n = min(max(n, 1.0), pc.max_accum);

    // Still camera, average topped up: hold the accumulated value verbatim. texelFetch, not the
    // Catmull-Rom tap: with a still camera the reprojected UV sits an epsilon off the texel
    // centre, and a filtered read there plus the RGBA16F round-trip oscillates the held value by
    // one ulp per frame -- sub-level dither that never lets the image reach byte-static. The
    // exact fetch copies the texel bit-for-bit, so the held image is the many-frame average that
    // was on screen while moving, frozen in place. Guarded on the count: a pixel with no accepted
    // history has nothing worth holding.
    if (pc.frozen != 0 && have_count && n > 1.0) {
        out_color = texelFetch(tex_history, ivec2(gl_FragCoord.xy), 0);
        return;
    }

    // Variance clipping (Salvi 2016 / Marrs et al.) rather than a raw min/max AABB. A box over
    // nine samples is maximally sensitive to a single outlier -- exactly what a stochastic ray
    // produces at a silhouette, where a jittered ray hits or misses independently of its
    // neighbours even when the underlying surface has not changed. Clipping to mean +/- gamma
    // standard deviations accepts that per-pixel noise as normal variation and rejects only the
    // genuine ghost, which is what lets the average below integrate the jitter into a soft edge
    // instead of discarding history every frame.
    //
    // Statistics in YCoCg for the colour (see gfx/ycocg.glsl) -- a luma/chroma box is tighter
    // around the same samples than an RGB box, so the clip catches a ghost sooner without eating
    // a real change in reflected colour.
    vec3 y0 = gfx_rgb_to_ycocg(s0.rgb), y1 = gfx_rgb_to_ycocg(s1.rgb), y2 = gfx_rgb_to_ycocg(s2.rgb);
    vec3 y3 = gfx_rgb_to_ycocg(s3.rgb), y4 = gfx_rgb_to_ycocg(s4.rgb), y5 = gfx_rgb_to_ycocg(s5.rgb);
    vec3 y6 = gfx_rgb_to_ycocg(s6.rgb), y7 = gfx_rgb_to_ycocg(s7.rgb), y8 = gfx_rgb_to_ycocg(s8.rgb);

    vec3 ysum  = y0 + y1 + y2 + y3 + y4 + y5 + y6 + y7 + y8;
    vec3 ysum2 = y0*y0 + y1*y1 + y2*y2 + y3*y3 + y4*y4 + y5*y5 + y6*y6 + y7*y7 + y8*y8;
    vec3 ymean = ysum / 9.0;
    vec3 ysigma = sqrt(max(ysum2 / 9.0 - ymean * ymean, vec3(0.0)));

    float asum  = s0.a + s1.a + s2.a + s3.a + s4.a + s5.a + s6.a + s7.a + s8.a;
    float asum2 = s0.a*s0.a + s1.a*s1.a + s2.a*s2.a + s3.a*s3.a + s4.a*s4.a
                + s5.a*s5.a + s6.a*s6.a + s7.a*s7.a + s8.a*s8.a;
    float amean  = asum / 9.0;
    float asigma = sqrt(max(asum2 / 9.0 - amean * amean, 0.0));

    // Epsilon floor on the band half-width -- see kSigmaFloor.
    vec3  yband = pc.gamma * ysigma + kSigmaFloor * (abs(ymean) + 1e-4);
    float aband = pc.gamma * asigma + kSigmaFloor;

    vec4 history = sample_history_catmull_rom(prev_uv, vec2(pc.resolution_x, pc.resolution_y));

    vec3 hist_rgb = gfx_ycocg_to_rgb(clamp(gfx_rgb_to_ycocg(history.rgb),
                                           ymean - yband, ymean + yband));
    // Confidence is clipped two-sided like the colour, not one-sided down as it once was. The old
    // "never pull history above what this frame supports" rule was written for a fixed-rate blend
    // that could not recover: with a converging average and a shared disocclusion test, a
    // neighbourhood that momentarily misses is just variance, and forcing confidence down on
    // every such frame is itself the pulse the jitter exists to avoid.
    float hist_a = clamp(history.a, amean - aband, amean + aband);

    // Running average by stored count. The 0.25 ceiling caps how much one frame can inject into
    // barely-accumulated pixels (counts 2-4, i.e. the fresh bands a panning camera opens every
    // frame): their noise amplitude drops in exchange for the reflection fading in over ~4 frames
    // instead of ~2 -- a soft lag that reads far quieter than sparkle. Count 1 is excluded: that
    // pixel has no accepted history at all, so its honest weight is 1.0, and capping it would
    // keep three quarters of a value belonging to a different surface.
    float w = have_count
        ? ((n <= 1.0) ? 1.0 : min(1.0 / n, 0.25))
        : (1.0 - pc.blend_factor);

    out_color = vec4(mix(hist_rgb, current.rgb, w), mix(hist_a, current.a, w));
}

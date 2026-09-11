#ifndef TOY_REFRACTION_GLSL
#define TOY_REFRACTION_GLSL

// refraction.glsl -- screen-space refraction for BLEND MESH objects only.
//
// Deliberately NOT part of pixel_forward_shading.glsl: that file's
// gfx_pixel_forward_shade() is shared byte-for-byte with sdf_forward.frag, and
// SDF glass is excluded from refraction (see the refraction plan's "Scope"
// section for why). This file is included by transparent.frag alone, and
// operates on the vec4 gfx_pixel_forward_shade() already returned rather than
// reaching into its internals -- so the shared shading body needs no changes
// and the SDF forward path is untouched.
//
// The includer must, BEFORE including this file, already have declared (with
// these exact names):
//   - `u_scene_color`  -- sampler2D, the prefiltered scene-colour mip chain
//                         (see SceneColorMipPass; LINEAR/MIPMAP_MODE_LINEAR,
//                         maxLod = mip_levels_, so a fractional LOD works).
//   - `u_hiz_map`      -- sampler2D, opaque G-buffer depth (mip 0 is the raw
//                         depth attachment; NEAREST filter -- see HiZPass),
//                         for gfx_refraction_apply()'s occluder-validity walk.
//   - #include <gfx/brdf.glsl> (fresnel_schlick), <gfx/ssr_common.glsl>
//     (ssr_ndc_to_uv, ssr_texel_world_size), <gfx/ssr_trace_body.glsl>
//     (gfx_ssr_get_view_z), and "pixel_forward_shading.glsl"
//     (gfx_forward_silhouette_fade) -- all already included by
//     transparent.frag ahead of this file.

/// Per-object refraction inputs -- see TransparentRefractionPushConstants
/// (pixel_render_pipeline.h) for the exact push-constant bytes this is filled
/// from.
struct GfxRefractionMaterial {
    bool  enabled;
    float ior;
    float thickness;
    vec3  tint;
};

/// Per-frame refraction tuning -- byte-for-byte ForwardGlobals' refract0/refract1
/// (forward_globals.h).
struct GfxRefractionParams {
    bool  enabled;         // master gate (config_.refraction_enabled)
    float strength;        // multiplier on the screen-space UV offset
    float max_offset;      // clamp in UV units
    float chromatic;       // RGB IOR split fraction; 0 = single tap
    float blur;            // roughness -> scene-colour mip scale
    float density;         // Beer-Lambert absorption strength
    bool  fresnel_enabled; // dim transmission at grazing angles
};

// Occluder-validity walk-back tuning for gfx_refraction_apply() below -- file-local
// constants, not config knobs, since they're a numerical-robustness detail (how many NEAREST
// depth taps to spend, how much bias to give the comparison) rather than an authored look.
const int   GFX_REFRACTION_DEPTH_STEPS       = 4;   // halvings; 4 -> worst case shrinks duv to 1/16th
const float GFX_REFRACTION_DEPTH_BIAS_TEXELS = 2.0; // mirrors ssr_bias_texels' role in ssr_trace_body.glsl

/// Opaque view-space depth (negative; RH + DEPTH_ZERO_TO_ONE, more negative = farther) at
/// `uv`. Hi-Z mip 0 is the G-buffer depth attachment verbatim and the sampler is NEAREST
/// (see HiZPass), so this is an exact per-texel read, not a filtered blend across an
/// occluder edge. Background texels clear to depth 1.0 (gbuffer_target.h) and therefore
/// linearise to the far plane -- "behind everything" -- so the occluder-validity check below
/// needs no separate background case.
float gfx_refraction_scene_view_z(vec2 uv, mat4 inv_proj) {
    return gfx_ssr_get_view_z(textureLod(u_hiz_map, clamp(uv, 0.0, 1.0), 0.0).r, inv_proj);
}

/// Samples u_scene_color at `uv` with chromatic aberration split by `chromatic`
/// around `duv`'s direction, at mip level `lod`. A single tap when chromatic <= 0.
vec3 gfx_refraction_sample(vec2 uv, vec2 duv, float chromatic, float lod) {
    if (chromatic <= 0.0) {
        return textureLod(u_scene_color, clamp(uv, 0.0, 1.0), lod).rgb;
    }
    vec2 uv_r = clamp(uv + duv * chromatic, 0.0, 1.0);
    vec2 uv_b = clamp(uv - duv * chromatic, 0.0, 1.0);
    float r = textureLod(u_scene_color, uv_r, lod).r;
    float g = textureLod(u_scene_color, uv, lod).g;
    float b = textureLod(u_scene_color, uv_b, lod).b;
    return vec3(r, g, b);
}

/// Composites screen-space refraction onto `shaded` (gfx_pixel_forward_shade()'s
/// return value: rgb = surface colour, a = material alpha) and returns the final
/// pixel colour with alpha forced to 1.0 -- the shader itself blends transmission
/// with the surface colour here, since the destination image already holds the
/// background BEFORE this draw (relying on fixed-function src-alpha-over on top
/// of that would double-count it). When refraction is inactive, returns `shaded`
/// unchanged so the fixed-function blend behaves exactly as it did before this
/// feature existed.
///
/// @param shaded       gfx_pixel_forward_shade()'s result for this fragment.
/// @param world_pos    Shaded surface point, world space (same as passed to
///                     gfx_pixel_forward_shade()).
/// @param N            Shading normal, world space, already facing the viewer.
/// @param V             normalize(camera_pos - world_pos).
/// @param roughness    Surface roughness, drives the blur LOD.
/// @param F0           Base reflectance (mix(0.04, albedo, metallic)), for the
///                     optional Fresnel transmission falloff.
/// @param view, proj   Camera view/projection, for projecting the transmitted ray.
/// @param inv_proj     inverse(proj) -- caller-supplied (not computed here) so the
///                     occluder-validity walk below, which re-evaluates it per step,
///                     doesn't each call inverse() itself.
/// @param mat          Per-object refraction inputs.
/// @param p            Per-frame refraction tuning.
/// @param max_color_mip Highest valid LOD into u_scene_color (ForwardGlobals::ssr_mip.x).
vec4 gfx_refraction_apply(vec4 shaded, vec3 world_pos, vec3 N, vec3 V, float roughness, vec3 F0,
                          mat4 view, mat4 proj, mat4 inv_proj, GfxRefractionMaterial mat,
                          GfxRefractionParams p, int max_color_mip) {
    if (!p.enabled || !mat.enabled || mat.ior <= 1.0) return shaded;

    // Transmitted ray direction (Snell's law, air -> material) and the point it reaches after
    // travelling `thickness` through the object -- projecting both world_pos and that point
    // gives the UV delta a straight screen-space normal offset can't get right under
    // perspective or camera roll.
    vec3 T = refract(-V, N, 1.0 / mat.ior);
    if (dot(T, T) < 1e-8) return shaded; // total internal reflection: no transmitted ray

    vec4 clip_a = proj * view * vec4(world_pos, 1.0);
    vec4 clip_b = proj * view * vec4(world_pos + T * mat.thickness, 1.0);
    if (clip_a.w <= 0.0 || clip_b.w <= 0.0) return shaded;

    vec2 uv_a = ssr_ndc_to_uv(clip_a.xy / clip_a.w);
    vec2 uv_b = ssr_ndc_to_uv(clip_b.xy / clip_b.w);
    vec2 duv  = (uv_b - uv_a) * p.strength;

    float offset_len = length(duv);
    float max_offset = max(p.max_offset, 0.0);
    if (offset_len > max_offset && offset_len > 0.0) {
        duv *= max_offset / offset_len;
    }
    // Screen-space refraction can only show what u_scene_color already holds at the offset
    // texel -- and nothing stops that texel belonging to geometry NEARER the camera than
    // this surface. It did: the pixel_demo water plane sampled ~22 screen texels "into"
    // cube.000, which stands well in front of it, and drew a ghost of the cube's top face
    // inside the pond. Halving the offset until the tap lands on something actually behind
    // this fragment degrades smoothly to zero offset near a foreground occluder, instead of
    // a hard cutoff, which would read as a seam. i == "keep the full-length duv" is always a
    // candidate; the loop only ever shrinks it, never grows it back once shortened.
    float frag_view_z = (view * vec4(world_pos, 1.0)).z; // negative; RH + DEPTH_ZERO_TO_ONE
    float depth_bias = GFX_REFRACTION_DEPTH_BIAS_TEXELS *
        ssr_texel_world_size(frag_view_z, proj[1][1], float(textureSize(u_hiz_map, 0).y));
    for (int i = 0; i < GFX_REFRACTION_DEPTH_STEPS; ++i) {
        // More negative == farther, so "behind me" is scene_view_z <= my view_z (+ bias).
        if (gfx_refraction_scene_view_z(uv_a + duv, inv_proj) <= frag_view_z + depth_bias) break;
        duv *= 0.5;
    }
    vec2 uv = uv_a + duv;

    float lod = clamp(roughness * p.blur * float(max_color_mip), 0.0, float(max_color_mip));
    vec3 transmitted = gfx_refraction_sample(uv, duv, max(p.chromatic, 0.0), lod);

    // Beer-Lambert absorption: (1 - tint) is how much of each channel the material eats per
    // unit thickness*density; tint == white passes everything through unabsorbed.
    vec3 absorption = exp(-(vec3(1.0) - clamp(mat.tint, 0.0, 1.0)) * max(p.density, 0.0) * max(mat.thickness, 0.0));
    transmitted *= absorption;

    if (p.fresnel_enabled) {
        // Dimming transmission by (1-F) keeps this from double-counting the reflected
        // energy already present in `shaded` via the SSR/sky term gfx_pixel_forward_shade()
        // computed -- but that bookkeeping is only honest where the reflection term is
        // actually live. Toward the silhouette it is not: the forward SSR/SSGI terms fade
        // themselves out over gfx_forward_silhouette_fade()'s NdotV band (see that
        // function's doc for the texel math), and the sky-gradient fallback for a
        // down-facing limb normal is the dark ground half of the gradient. Meanwhile
        // Schlick's pow5 spike crosses F ~0.35 -> ~1.0 entirely inside the last ~0.2 of
        // cos_theta -- a sub-texel band of a curved silhouette at this engine's internal
        // resolution. Unweighted, a limb texel multiplies its transmission by ~0 with no
        // compensating reflection, collapsing to alpha * (dark shaded) with no background
        // at all: a black outline stippled along every refracting silhouette. Fading the
        // dimming with the SAME curve the reflection terms fade with keeps the two energy
        // ledgers in step: full fresnel dimming where the reflection is live, none where
        // it was never rendered.
        float cos_theta = max(dot(N, V), 0.0);
        vec3  F = fresnel_schlick(cos_theta, F0);
        transmitted *= mix(vec3(1.0), vec3(1.0) - F, gfx_forward_silhouette_fade(cos_theta));
    }

    vec3 result = shaded.rgb * shaded.a + transmitted * (1.0 - shaded.a);
    return vec4(result, 1.0);
}

#endif // TOY_REFRACTION_GLSL

#ifndef GFX_SSR_COMPOSITE_BODY_GLSL
#define GFX_SSR_COMPOSITE_BODY_GLSL

// gfx/ssr_composite_body.glsl -- shared main() for the SSR composite.
//
// REQUIRED BEFORE INCLUDE (name-for-name; set indices are the includer's
// choice, but every name below must resolve to exactly this type):
//   uniform CameraUBO { mat4 view; mat4 proj; vec3 camera_pos; } camera;
//   sampler2D g_albedo_ao, g_normal_metallic, g_position_roughness, g_ssao;
//   sampler2D u_ssr_map;
//   sampler2D u_scene_color, u_scene_color_mips;
//   #include <gfx/ssr_composite_pc.glsl>  (the `pc` push-constant block)
//   Hooks: hook_env_brdf, hook_env_specular (see gfx/indirect_specular.glsl)
//
// Also requires <gfx/sky.glsl>, <gfx/ssr_common.glsl> and
// <gfx/indirect_specular.glsl> to have been included first.

#include <gfx/ao_composite.glsl>
#include <gfx/bilateral_upsample.glsl>

/// Depth/normal-aware upsample of the (possibly half-resolution) resolved
/// SSR buffer. Degenerates to a single texel fetch when pc.half_res == 0,
/// which is every consumer that doesn't implement half-res tracing.
vec4 gfx_ssr_fetch(vec2 uv, vec3 P, vec3 N, float px_world) {
    if (pc.half_res == 0) {
        return texture(u_ssr_map, uv);
    }

    // The shared depth/normal-aware 2x2 upsample (gfx/bilateral_upsample.glsl): half-res
    // texel hc was traced from full-res texel 2*hc + 1, ssr.frag's origin snapping.
    return gfx_bilateral_upsample(u_ssr_map, g_position_roughness, g_normal_metallic, uv,
                                  pc.ssr_resolution, pc.screen_resolution, P, N, px_world);
}

void main() {
    vec3 scene_color = texture(u_scene_color, in_uv).rgb;

    ivec2 gcoord = ivec2(gl_FragCoord.xy);
    vec4 norm_met = texelFetch(g_normal_metallic, gcoord, 0);
    vec3 N = norm_met.rgb;
    if (dot(N, N) < 0.001) {
        out_color = vec4(scene_color, 1.0);
        return;
    }
    N = normalize(N);
    float metallic = norm_met.a;

    vec4 albedo_ao = texelFetch(g_albedo_ao, gcoord, 0);
    vec3 albedo = albedo_ao.rgb;
    float ao    = albedo_ao.a;
    // texture()+normalized UV, NOT texelFetch(gcoord): gcoord is a full-render-resolution
    // integer pixel coordinate, only valid for texelFetch when g_ssao is bound to the real
    // SSAO output. With SSAO disabled the binding points at a 1x1 neutral fallback texture,
    // and texelFetch at an out-of-bounds coordinate into a 1x1 image is undefined behaviour
    // (texelFetch does not apply CLAMP_TO_EDGE the way normalized texture() sampling does) --
    // it reads back ~0 in practice, collapsing the occlusion factors below to zero.
    float ssao = texture(g_ssao, in_uv).r;

    vec4 pos_rough = texelFetch(g_position_roughness, gcoord, 0);
    vec3 P = pos_rough.rgb;
    float roughness = pos_rough.a;

    float view_z   = (camera.view * vec4(P, 1.0)).z;
    float px_world = ssr_texel_world_size(view_z, abs(camera.proj[1][1]), pc.screen_resolution.y);

    vec4 ssr_sample = gfx_ssr_fetch(in_uv, P, N, px_world);
    vec3 ssr_color = ssr_sample.rgb;
    float confidence = ssr_sample.a;

    vec3 V = normalize(camera.camera_pos - P);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // The one call that guarantees this cancels against the lighting pass -- see
    // gfx/indirect_specular.glsl.
    GfxIndirectSpecular ind = gfx_indirect_specular(P, N, V, F0, roughness, pc.sky_intensity,
                                                    pc.sky_zenith.rgb, pc.sky_horizon.rgb, pc.sky_ground.rgb);

    // ind.value is already premultiplied by ssr_color's own confidence... no -- ssr_color
    // (u_ssr_map.rgb) is premultiplied by confidence (ssr.frag); ind.value (env/sky specular)
    // is not, so the confidence weight applies only to the delta, not to scene_color itself.
    vec3 ssr_specular = ssr_color * (ind.F * ind.brdf.x + ind.brdf.y);
    // Occlusion factors mirror pixel_lighting.frag's composite exactly (gfx/ao_composite.glsl):
    // min-combined material/screen AO, a specular occlusion cone tinted by F0 for the ind.value
    // subtraction, and multi-bounce diffuse occlusion for the SSGI bounce below. The
    // subtraction cancels the ind.value term lighting added, so any drift between the two
    // factors shows up as a halo or double-darkening at reflective pixels.
    const GfxAoTerms aot = gfx_ao_terms(ao, ssao, albedo, F0, max(dot(N, V), 0.0), roughness, 0.0);
    const float occlusion = aot.occlusion;
    const vec3  ao_spec   = aot.specular;
    // Clamp: a false-positive SSR hit against nearby dark geometry can leave ssr_specular
    // near zero, making the unclamped delta go negative -- a visible black speckle rather
    // than "no reflection here".
    vec3 color = max(scene_color + (ssr_specular - confidence * ind.value) * ao_spec, 0.0);

    // Screen-space diffuse bounce ("SSGI"), two tiers behind one intensity dial. In the
    // body, not a hook: ssgi_intensity 0 (blendy's default) makes both a no-op, so a
    // consumer with no SSGI concept of its own gets the option for free with zero extra
    // shader text.
    if (pc.ssgi_intensity > 0.0) {
        vec3 kD = (vec3(1.0) - ind.F) * (1.0 - metallic);
        if (pc.ssgi_traced > 0.5) {
            // Traced tier: u_ssgi_map holds the temporally-resolved cosine-hemisphere
            // trace (ssgi.frag). Its rgb is hit radiance premultiplied by the trace's own
            // confidence (screen-edge/distance fades included), and cosine-weighted
            // sampling makes E[radiance] the irradiance over pi -- so albedo * rgb IS the
            // Lambertian bounce, correctly normalized with no extra factors.
            //
            // A plain bilinear tap, NOT gfx_ssr_fetch's depth/normal-aware upsample: under
            // half-res tracing that costs the bounce a little bleed across silhouettes,
            // which is invisible on a term this low-frequency (it has already been through
            // a wide bilateral denoise) and not worth four extra G-buffer fetches per pixel.
            vec3 gi = texture(u_ssgi_map, in_uv).rgb;
            color += kD * albedo * gi
                   * pc.ssgi_intensity * gfx_gtao_multi_bounce(occlusion, albedo);
        } else {
            // Fallback tier: one blurry scene-colour mip tap at a point offset along the
            // surface normal, weighted by the same SSR confidence (so it only contributes
            // where a valid on-screen reflector was found) and the same screen-edge fade
            // the raymarch itself uses.
            vec4 bounce_clip = camera.proj * camera.view * vec4(P + N * pc.ssgi_distance, 1.0);
            if (bounce_clip.w > 0.0) {
                vec2 bounce_uv = ssr_ndc_to_uv(bounce_clip.xy / bounce_clip.w);
                vec2 edge = smoothstep(vec2(0.0), vec2(0.08), bounce_uv)
                          * smoothstep(vec2(1.0), vec2(0.92), bounce_uv);
                vec3 bounce = textureLod(u_scene_color_mips, clamp(bounce_uv, 0.0, 1.0),
                                         float(pc.max_color_mip)).rgb;
                color += kD * albedo * bounce * (edge.x * edge.y) * confidence
                       * pc.ssgi_intensity * gfx_gtao_multi_bounce(occlusion, albedo);
            }
        }
    }

    out_color = vec4(color, 1.0);
}

#endif // GFX_SSR_COMPOSITE_BODY_GLSL

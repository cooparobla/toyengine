#version 450

// SSR composite: swaps the sky-based indirect specular pixel_lighting.frag
// already wrote for the confidence-weighted SSR hit (mirroring
// blendy/assets/shaders/ssr_composite.frag, with its set-4 reflection-probe/
// GI descriptor set collapsed away entirely -- toyengine has no probes), and
// adds a screen-space diffuse bounce term ("SSGI"): a single tap of the
// prefiltered scene-colour mip chain along the surface normal, weighted by
// the same SSR confidence and screen-edge fade so it only ever contributes
// where the specular trace already found a valid on-screen surface to
// bounce off of.
//
// No half-res tracing (unlike blendy) -- toyengine already renders at the
// pipeline's low internal resolution, so SsrPass traces at full render
// resolution and this shader needs no bilateral upsample.

#include "sky.glsl"
#include "ssr_common.glsl"
#include "brdf.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

// Set 0: Camera UBO
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: G-Buffer + screen-space AO
layout(set = 1, binding = 0) uniform sampler2D g_albedo_ao;
layout(set = 1, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 1, binding = 2) uniform sampler2D g_position_roughness;
layout(set = 1, binding = 3) uniform sampler2D g_ssao;

// Set 2: Resolved SSR map (temporal-resolved raymarch output)
layout(set = 2, binding = 0) uniform sampler2D u_ssr_map;

// Set 3: Deferred-lit scene colour (raw, for compositing into) and its
// prefiltered mip chain (blurry, for the SSGI diffuse-bounce tap).
layout(set = 3, binding = 0) uniform sampler2D u_scene_color;
layout(set = 3, binding = 1) uniform sampler2D u_scene_color_mips;

layout(push_constant) uniform CompositePushConstants {
    vec2  screen_resolution;
    float sky_intensity;    // must match pixel_lighting.frag's params.sky_intensity
    float ssgi_intensity;   // 0 disables the diffuse-bounce term
    float ssgi_distance;    // world-space offset along N for the bounce sample point
    int   max_color_mip;    // top mip of u_scene_color_mips
} pc;

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
    // texture()+normalized UV, not texelFetch(gcoord) -- gcoord is a full-render-resolution
    // integer pixel coordinate, which is only valid for texelFetch when g_ssao is bound to
    // SsaoPass::output_view() (matches render_extent_). When ssao_enabled is false,
    // PixelRenderPipeline binds SsaoPass::neutral_view() instead -- a 1x1 fallback meant to
    // always read 1.0 -- and texelFetch at an out-of-bounds texel coordinate into a 1x1 texture
    // is undefined behavior (texelFetch does not apply the sampler's CLAMP_TO_EDGE addressing
    // the way normalized texture() sampling does), which in practice read back ~0 instead of
    // the intended 1.0 and collapsed the whole ao * ssao term below. texture(in_uv) is exactly
    // as cheap here (both are single-tap, point-filtered reads) and correct for both cases --
    // matches how pixel_lighting.frag already reads this same texture.
    float ssao  = texture(g_ssao, in_uv).r;

    vec4 pos_rough = texelFetch(g_position_roughness, gcoord, 0);
    vec3 P = pos_rough.rgb;
    float roughness = pos_rough.a;

    vec4 ssr_sample = texture(u_ssr_map, in_uv);
    vec3 ssr_color = ssr_sample.rgb;
    float confidence = ssr_sample.a;

    vec3 V = normalize(camera.camera_pos - P);
    float NdotV = max(dot(N, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    // Must be the identical expression pixel_lighting.frag uses for
    // F_ind/brdf_ind, or the subtraction below does not cancel.
    vec3 F = fresnel_schlick_roughness(NdotV, F0, roughness);
    vec2 brdf = env_brdf_approx(NdotV, roughness);

    vec3 sky_specular = sky_gradient(reflect(-V, N)) * (F * brdf.x + brdf.y) * pc.sky_intensity;
    // u_ssr_map.rgb is already premultiplied by confidence (ssr.frag).
    vec3 ssr_specular = ssr_color * (F * brdf.x + brdf.y);
    // Clamp: a false-positive SSR hit against nearby dark geometry can leave ssr_specular
    // near zero, making the unclamped delta go negative -- a visible black speckle rather
    // than "no reflection here".
    vec3 color = max(scene_color + (ssr_specular - confidence * sky_specular) * ao * ssao, 0.0);

    // Screen-space diffuse bounce ("SSGI"): sample the blurriest scene-colour
    // mip at a point offset along the surface normal, and add it as a
    // Lambertian bounce weighted by the same SSR confidence (so it only
    // contributes where a valid on-screen reflector was actually found) and
    // the same screen-edge fade the raymarch itself uses.
    if (pc.ssgi_intensity > 0.0) {
        vec4 bounce_clip = camera.proj * camera.view * vec4(P + N * pc.ssgi_distance, 1.0);
        if (bounce_clip.w > 0.0) {
            vec2 bounce_uv = ssr_ndc_to_uv(bounce_clip.xy / bounce_clip.w);
            vec2 edge = smoothstep(vec2(0.0), vec2(0.08), bounce_uv)
                      * smoothstep(vec2(1.0), vec2(0.92), bounce_uv);
            vec3 bounce = textureLod(u_scene_color_mips, clamp(bounce_uv, 0.0, 1.0),
                                     float(pc.max_color_mip)).rgb;
            vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
            color += kD * albedo * bounce * (edge.x * edge.y) * confidence
                   * pc.ssgi_intensity * ao * ssao;
        }
    }

    out_color = vec4(color, 1.0);
}

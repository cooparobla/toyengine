#version 450

// TransparentPreviewPass's fragment stage -- see toyengine/render/passes/transparent_preview_pass.h.
// A BLEND material in the editor's Solid / Material Preview / Wireframe shading: the same
// look as the opaque surfaces (editor_shading.glsl), with the material's alpha, hidden behind
// opaque geometry by a compare against the G-buffer depth.

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_world_normal;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in mat3 frag_TBN;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

layout(set = 1, binding = 0) uniform sampler2D u_scene_depth;

// MaterialTextureCache's layout (binding 0, the alpha mask, unused: BLEND never alpha-tests).
layout(set = 2, binding = 1) uniform sampler2D u_albedo_map;
layout(set = 2, binding = 3) uniform sampler2D u_metallic_roughness_map;

layout(push_constant) uniform PreviewPC {
    vec4  albedo;        // rgb, a = alpha
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;
    vec4  gfx_time;
    vec4  gfx_params;
    vec4  view;          // x: mode (1 solid, 2 material preview, 3 wireframe), yz: 1 / target size
    vec4  emissive;
} pc;

layout(location = 0) out vec4 out_color;

#include "editor_shading.glsl"

void main() {
    // Behind opaque geometry? (Standard [0,1] depth, Less; transparent surfaces are never in
    // the G-buffer, so a tiny slack is all that's needed.)
    float scene_z = texture(u_scene_depth, gl_FragCoord.xy * pc.view.yz).r;
    if (gl_FragCoord.z > scene_z + 1e-6) discard;

    vec4 albedo_tex = texture(u_albedo_map, frag_uv);
    vec2 mr = texture(u_metallic_roughness_map, frag_uv).bg;
    vec3 albedo = pc.albedo.rgb * albedo_tex.rgb;
    float alpha = clamp(pc.albedo.a * albedo_tex.a, 0.0, 1.0);
    vec3 N = normalize(frag_world_normal);
    if (!gl_FrontFacing) N = -N;

    int mode = int(pc.view.x + 0.5);
    vec3 col;
    // Forward surfaces get no screen-space AO and no direct-light AO -- same as the renderer's
    // forward path (toy_forward_shading.glsl).
    const float ndotv = max(dot(N, normalize(camera.camera_pos - frag_world_pos)), 0.0);
    if (mode == 3) {
        col = vec3(0.12);                 // wireframe: a faint film, the edges come from overlays
        alpha *= 0.25;
    } else if (mode == 2) {
        const vec3 F0 = mix(vec3(0.04), albedo, clamp(pc.metallic * mr.x, 0.0, 1.0));
        const GfxAoTerms aot = gfx_ao_terms(pc.ao, 1.0, albedo, F0, ndotv, clamp(pc.roughness * mr.y, 0.04, 1.0), 0.0);
        col = editor_material_preview(N, frag_world_pos, camera.camera_pos, albedo, pc.metallic * mr.x,
                                      pc.roughness * mr.y, aot, pc.emissive.rgb);
    } else {
        const GfxAoTerms aot = gfx_ao_terms(1.0, 1.0, editor_solid_base(albedo), vec3(0.04), ndotv, 0.5, 0.0);
        col = editor_solid(N, frag_world_pos, camera.camera_pos, albedo, aot);
    }
    out_color = vec4(col, alpha);
}

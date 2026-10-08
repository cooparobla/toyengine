#ifndef GFX_SURFACE_TRANSPARENT_TES_GLSL
#define GFX_SURFACE_TRANSPARENT_TES_GLSL

// gfx/surface/transparent_tes.glsl -- the forward transparent pass's TESSELLATION EVALUATION
// backbone: gfx/surface/transparent_vs.glsl per generated vertex (water's Gerstner waves on a
// camera-adaptive grid). Same outputs, GFX_SURFACE_CUSTOM_VARYING included, so every transparent
// fragment shader runs unchanged behind it. The tessellation params ride in the refraction
// block's ior_flags.zw as raw bits (TransparentRefractionPushConstants); the pixel scale in the
// camera UBO's jitter_ndc.z.
//
// Include order: #define GFX_SURFACE_VERTEX (and GFX_SURFACE_CUSTOM_VARYING), this file, then
// the surface file with the hook.

layout(triangles, fractional_odd_spacing, cw) in;

layout(location = 0) in vec3 tc_position_os[];
layout(location = 1) in vec3 tc_normal_os[];
layout(location = 2) in vec2 tc_uv[];
layout(location = 3) in vec4 tc_tangent_os[];
layout(location = 4) patch in mat4 tc_model;
layout(location = 8) patch in mat4 tc_prev_model;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
    mat4 prev_view;
    mat4 prev_proj;
    vec4 jitter_ndc;   // z: pixel scale
} camera;

layout(push_constant) uniform PushConstants {
    vec4  albedo;
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;
    vec4  gfx_time;
    vec4  gfx_params;
    vec4  refraction_tint_thickness;
    vec4  refraction_ior_flags;   // zw: packed tessellation params (raw bits)
} material;

vec4 gfx_time   = material.gfx_time;
vec4 gfx_params = material.gfx_params;

#include <gfx/surface/tess_common.glsl>

layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec3 frag_world_normal;
layout(location = 2) out vec2 frag_uv;
layout(location = 3) out mat3 frag_TBN;
#ifdef GFX_SURFACE_CUSTOM_VARYING
layout(location = 6) out vec4 frag_custom;
#endif

struct GfxSurfaceVertex {
    vec3 position_os;
    vec3 normal_os;
    vec4 tangent_os;
    vec2 uv;
    mat4 model;
    mat3 normal_matrix;
    vec3 position_ws;
    vec3 normal_ws;
    vec3 tangent_ws;
    vec4 custom;
};

// The tessellated mesh's resolution, for a hook's level of detail: x = metres of vertex spacing
// the tessellator aims for per metre of camera distance, y = the tessellation range (m). Set
// before the hook runs; vec2(0) in the untessellated backbone (transparent_vs.glsl).
vec2 gfx_tess_lod = vec2(0.0);

#ifdef GFX_SURFACE_VERTEX
void gfx_surface_vertex(inout GfxSurfaceVertex v);
#else
void gfx_surface_vertex(inout GfxSurfaceVertex v) {}
#endif

void main() {
    vec3 b = gl_TessCoord;
    precise vec3 pos_os = tc_position_os[0] * b.x + tc_position_os[1] * b.y + tc_position_os[2] * b.z;
    vec3 n_os = normalize(tc_normal_os[0] * b.x + tc_normal_os[1] * b.y + tc_normal_os[2] * b.z);
    vec4 t_os = tc_tangent_os[0] * b.x + tc_tangent_os[1] * b.y + tc_tangent_os[2] * b.z;
    vec2 uv = tc_uv[0] * b.x + tc_uv[1] * b.y + tc_uv[2] * b.z;

    GfxSurfaceVertex v;
    v.position_os   = pos_os;
    v.normal_os     = n_os;
    // tangent.w is NOT always a handedness here: water packs w == 2 as a "baked water vertex"
    // flag (water_system.h) -- interpolation of equal values keeps it exact.
    v.tangent_os    = vec4(t_os.xyz, t_os.w);
    v.uv            = uv;
    v.model         = tc_model;
    v.normal_matrix = transpose(inverse(mat3(tc_model)));
    v.position_ws   = (tc_model * vec4(pos_os, 1.0)).xyz;
    v.normal_ws     = normalize(v.normal_matrix * n_os);
    v.tangent_ws    = normalize(v.normal_matrix * (t_os.xyz + vec3(1e-6)));
    v.custom        = vec4(0.0);

    GfxTessParams tp = gfx_unpack_tess(floatBitsToUint(material.refraction_ior_flags.z),
                                       floatBitsToUint(material.refraction_ior_flags.w));
    gfx_tess_lod = vec2(tp.edge_pixels / max(camera.jitter_ndc.z, 1.0), tp.max_distance);

    gfx_surface_vertex(v);

    vec3 T = normalize(v.tangent_ws - dot(v.tangent_ws, v.normal_ws) * v.normal_ws);
    vec3 B = cross(v.normal_ws, T) * (v.tangent_os.w < 0.0 ? -1.0 : 1.0);

    frag_world_pos    = v.position_ws;
    frag_world_normal = v.normal_ws;
    frag_uv           = v.uv;
    frag_TBN          = mat3(T, B, v.normal_ws);
#ifdef GFX_SURFACE_CUSTOM_VARYING
    frag_custom       = v.custom;
#endif
    gl_Position = camera.proj * camera.view * vec4(v.position_ws, 1.0);
}

#endif // GFX_SURFACE_TRANSPARENT_TES_GLSL

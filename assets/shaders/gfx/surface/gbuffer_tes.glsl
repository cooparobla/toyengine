#ifndef GFX_SURFACE_GBUFFER_TES_GLSL
#define GFX_SURFACE_GBUFFER_TES_GLSL

// gfx/surface/gbuffer_tes.glsl -- the G-buffer TESSELLATION EVALUATION backbone: what
// gbuffer_vs.glsl does per authored vertex, done per GENERATED vertex. It interpolates the
// patch's object-space attributes, transforms them to world space, pushes the vertex out along
// its normal by the material's displacement map, runs the shader's own displacement hook
// (gfx_surface_vertex -- water's waves, foliage, deep snow) and writes exactly gbuffer_vs.glsl's
// outputs, so every G-buffer fragment shader runs unchanged behind it.
//
// Include order mirrors gbuffer_vs.glsl's:
//
//   #version 450
//   #define GFX_SURFACE_VERTEX
//   #include <gfx/surface/gbuffer_tes.glsl>
//   #include "my_surface.glsl"   // defines gfx_surface_vertex()

layout(triangles, fractional_odd_spacing, cw) in;

layout(location = 0) in vec3 tc_position_os[];
layout(location = 1) in vec3 tc_normal_os[];
layout(location = 2) in vec2 tc_uv[];
layout(location = 3) in vec4 tc_tangent_os[];
layout(location = 4) patch in mat4 tc_model;
layout(location = 8) patch in mat4 tc_prev_model;
layout(location = 12) patch in mat4 tc_snow_anchor;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
    mat4 prev_view;
    mat4 prev_proj;
    vec4 jitter_ndc;
} camera;

layout(push_constant) uniform PushConstants {
    vec4  albedo;
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;
    vec4  emissive;
    vec4  gfx_time;
    vec4  gfx_params;
    uvec4 surface_ext;
} material;

vec4 gfx_time   = material.gfx_time;
vec4 gfx_params = material.gfx_params;

// Set 1, binding 4: the material's displacement (height) map -- black when it has none.
layout(set = 1, binding = 4) uniform sampler2D u_displacement;

#define GFX_WORLD_SET 2
#include <gfx/surface/world.glsl>
#include <gfx/surface/tess_common.glsl>

layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec3 frag_world_normal;
layout(location = 2) out vec2 frag_uv;
layout(location = 3) out mat3 frag_TBN;
layout(location = 11) out vec3 frag_prev_world_pos;
layout(location = 12) out vec3 frag_snow_pos;        // see gfx/surface/gbuffer_vs.glsl
layout(location = 13) flat out vec3 frag_snow_up;

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
};

#ifdef GFX_SURFACE_VERTEX
void gfx_surface_vertex(inout GfxSurfaceVertex v);
#else
void gfx_surface_vertex(inout GfxSurfaceVertex v) {}
#endif

// The displaced surface's normal: the patch's world-space dP/du, dP/dv (solved from its three
// corners -- constant over a flat patch, so neighbours sharing an edge agree up to their UV
// mapping) bent by the height map's central-difference gradient.
vec3 gfx_displaced_normal_(vec3 n, vec2 uv, float scale) {
    vec3 p0 = (tc_model * vec4(tc_position_os[0], 1.0)).xyz;
    vec3 e1 = (tc_model * vec4(tc_position_os[1], 1.0)).xyz - p0;
    vec3 e2 = (tc_model * vec4(tc_position_os[2], 1.0)).xyz - p0;
    vec2 d1 = tc_uv[1] - tc_uv[0];
    vec2 d2 = tc_uv[2] - tc_uv[0];
    float det = d1.x * d2.y - d2.x * d1.y;
    if (abs(det) < 1e-12) return n;
    float r = 1.0 / det;
    vec3 dpdu = (e1 * d2.y - e2 * d1.y) * r;
    vec3 dpdv = (e2 * d1.x - e1 * d2.x) * r;
    // Central differences a fixed 5 cm apart on the surface (not a texel apart: the step then
    // means the same thing whatever the map's resolution and the patch's UV scale).
    const float step_m = 0.05;
    float lu = max(length(dpdu), 1e-6), lv = max(length(dpdv), 1e-6);
    vec2 du = vec2(step_m / lu, 0.0), dv = vec2(0.0, step_m / lv);
    float hu = textureLod(u_displacement, uv + du, 0.0).r - textureLod(u_displacement, uv - du, 0.0).r;
    float hv = textureLod(u_displacement, uv + dv, 0.0).r - textureLod(u_displacement, uv - dv, 0.0).r;
    vec3 tu = dpdu / lu + n * (hu * scale / (2.0 * step_m));
    vec3 tv = dpdv / lv + n * (hv * scale / (2.0 * step_m));
    vec3 dn = normalize(cross(tu, tv));
    return dot(dn, n) < 0.0 ? -dn : dn;
}

void main() {
    vec3 b = gl_TessCoord;
    // `precise`: a vertex on a shared edge must come out bit-identical from both patches (its
    // weights are the same pair there), or a hairline crack shows between them.
    precise vec3 pos_os = tc_position_os[0] * b.x + tc_position_os[1] * b.y + tc_position_os[2] * b.z;
    vec3 n_os = normalize(tc_normal_os[0] * b.x + tc_normal_os[1] * b.y + tc_normal_os[2] * b.z);
    vec4 t_os = tc_tangent_os[0] * b.x + tc_tangent_os[1] * b.y + tc_tangent_os[2] * b.z;
    vec2 uv = tc_uv[0] * b.x + tc_uv[1] * b.y + tc_uv[2] * b.z;

    GfxSurfaceVertex v;
    v.position_os   = pos_os;
    v.normal_os     = n_os;
    v.tangent_os    = vec4(normalize(t_os.xyz + vec3(1e-6)), t_os.w < 0.0 ? -1.0 : 1.0);
    v.uv            = uv;
    v.model         = tc_model;
    v.normal_matrix = transpose(inverse(mat3(tc_model)));

    vec4 world_pos = tc_model * vec4(pos_os, 1.0);
    v.position_ws = world_pos.xyz;
    v.normal_ws   = normalize(v.normal_matrix * n_os);
    v.tangent_ws  = normalize(v.normal_matrix * v.tangent_os.xyz);

    // The material's displacement map, along the (world) normal.
    GfxTessParams tp = gfx_unpack_tess(material.surface_ext.x, material.surface_ext.y);
    if (tp.displacement != 0.0) {
        v.position_ws += v.normal_ws * (textureLod(u_displacement, uv, 0.0).r * tp.displacement);
        v.normal_ws = gfx_displaced_normal_(v.normal_ws, uv, tp.displacement);
        v.tangent_ws = normalize(v.tangent_ws - dot(v.tangent_ws, v.normal_ws) * v.normal_ws + vec3(1e-6));
    }

    gfx_surface_vertex(v);

    vec3 disp = v.position_ws - world_pos.xyz;
    frag_prev_world_pos = (tc_prev_model * vec4(pos_os, 1.0)).xyz + disp;
    frag_snow_pos = (tc_snow_anchor * vec4(pos_os, 1.0)).xyz + disp;
    frag_snow_up  = mat3(tc_snow_anchor) * (transpose(v.normal_matrix) * vec3(0.0, 0.0, 1.0));

    vec3 T = normalize(v.tangent_ws - dot(v.tangent_ws, v.normal_ws) * v.normal_ws);
    vec3 B = cross(v.normal_ws, T) * v.tangent_os.w;

    frag_world_pos    = v.position_ws;
    frag_world_normal = v.normal_ws;
    frag_uv           = v.uv;
    frag_TBN          = mat3(T, B, v.normal_ws);

    gl_Position = camera.proj * camera.view * vec4(v.position_ws, 1.0);
}

#endif // GFX_SURFACE_GBUFFER_TES_GLSL

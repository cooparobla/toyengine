#ifndef GFX_SURFACE_SHADOW_TES_GLSL
#define GFX_SURFACE_SHADOW_TES_GLSL

// gfx/surface/shadow_tes.glsl -- the directional / spot shadow pass's TESSELLATION EVALUATION
// backbone: gfx/surface/shadow_vs.glsl per generated vertex. The control stage split the patch by
// the MAIN camera's view (shadow_depth.tesc), so the caster is subdivided -- and displaced --
// exactly as the camera sees it, and its shadow matches its surface. Unlike shadow_vs.glsl this
// backbone has the real normal and tangent (the tessellated pipelines stream the whole vertex),
// which the displacement map needs; a hook gets them too.
//
// Include order: #define GFX_SURFACE_VERTEX, this file, then the surface file with the hook.

layout(triangles, fractional_odd_spacing, cw) in;

layout(location = 0) in vec3 tc_position_os[];
layout(location = 1) in vec3 tc_normal_os[];
layout(location = 2) in vec2 tc_uv[];
layout(location = 3) in vec4 tc_tangent_os[];
layout(location = 4) patch in mat4 tc_model;
layout(location = 8) patch in mat4 tc_prev_model;

#ifdef GFX_SHADOW_CUBE
layout(push_constant) uniform CubeShadowPC {
    mat4  light_space_matrix;
    vec4  light_pos_range;
    float alpha;
    float alpha_cutoff;
    uint  tess_a;
    uint  tess_b;
    vec4  gfx_time;
    vec4  gfx_params;
} pc;
layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec2 frag_uv;
#else
layout(push_constant) uniform ShadowPC {
    mat4  light_space_matrix;
    float alpha;
    float alpha_cutoff;
    uint  tess_a;
    uint  tess_b;
    vec4  gfx_time;
    vec4  gfx_params;
} pc;
layout(location = 0) out vec2 frag_uv;
#endif

vec4 gfx_time   = pc.gfx_time;
vec4 gfx_params = pc.gfx_params;

layout(set = 0, binding = 4) uniform sampler2D u_displacement;   // the material set's height map

#define GFX_WORLD_SET 1
#include <gfx/surface/world.glsl>
#include <gfx/surface/tess_common.glsl>

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

void main() {
    vec3 b = gl_TessCoord;
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
    v.position_ws   = (tc_model * vec4(pos_os, 1.0)).xyz;
    v.normal_ws     = normalize(v.normal_matrix * n_os);
    v.tangent_ws    = normalize(v.normal_matrix * v.tangent_os.xyz);

    GfxTessParams tp = gfx_unpack_tess(pc.tess_a, pc.tess_b);
    if (tp.displacement != 0.0) {
        v.position_ws += v.normal_ws * (textureLod(u_displacement, uv, 0.0).r * tp.displacement);
    }

    gfx_surface_vertex(v);

#ifdef GFX_SHADOW_CUBE
    frag_world_pos = v.position_ws;
#endif
    frag_uv     = v.uv;
    gl_Position = pc.light_space_matrix * vec4(v.position_ws, 1.0);
}

#endif // GFX_SURFACE_SHADOW_TES_GLSL

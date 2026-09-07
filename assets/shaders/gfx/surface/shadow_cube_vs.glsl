#ifndef GFX_SURFACE_SHADOW_CUBE_VS_GLSL
#define GFX_SURFACE_SHADOW_CUBE_VS_GLSL

// gfx/surface/shadow_cube_vs.glsl -- point-light cubemap-face shadow-depth
// vertex backbone. See shadow_vs.glsl for why this lives in toyengine and
// the GfxSurfaceVertex placeholder-field contract -- identical here.

layout(location = 0) in vec3 in_position;
layout(location = 2) in vec2 in_uv;      // only meaningful when ShadowPipeline was built
                                          // with a material_layout (CUTOUT support)
layout(location = 4) in mat4 in_model;   // per-instance (locations 4-7)

layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec2 frag_uv;

layout(push_constant) uniform CubeShadowPC {
    mat4 light_space_matrix;
    vec4 light_pos_range; // xyz = light pos, w = range
    float alpha;
    float alpha_cutoff; // 0.0 disables shadow_cube_fs.glsl's alpha-mask discard
    vec4  gfx_time;
    vec4  gfx_params;
} pc;

// See gfx/surface/gbuffer_vs.glsl's identical aliases.
vec4 gfx_time   = pc.gfx_time;
vec4 gfx_params = pc.gfx_params;

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
    GfxSurfaceVertex v;
    v.position_os   = in_position;
    v.normal_os     = vec3(0.0);
    v.tangent_os    = vec4(0.0);
    v.uv            = in_uv;
    v.model         = in_model;
    v.normal_matrix = mat3(1.0);

    vec4 world_pos = in_model * vec4(in_position, 1.0);
    v.position_ws = world_pos.xyz;
    v.normal_ws   = vec3(0.0, 1.0, 0.0);
    v.tangent_ws  = vec3(1.0, 0.0, 0.0);

    gfx_surface_vertex(v);

    frag_world_pos = v.position_ws;
    frag_uv        = v.uv;
    gl_Position    = pc.light_space_matrix * vec4(v.position_ws, 1.0);
}

#endif // GFX_SURFACE_SHADOW_CUBE_VS_GLSL

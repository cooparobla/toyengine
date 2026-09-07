#ifndef GFX_SURFACE_SHADOW_VS_GLSL
#define GFX_SURFACE_SHADOW_VS_GLSL

// gfx/surface/shadow_vs.glsl -- directional-light shadow-depth vertex
// backbone.
//
// Lives in toyengine, not gfxcoopa's base library: toyengine's
// shadow_depth.vert/.frag already fork from gfxcoopa's base (they add the
// CUTOUT alpha-mask sampler + uv attribute gfxcoopa's base shadow pass
// doesn't have -- see MaterialTextureCache), so this backbone extracts
// *that* fork, the one derived shaders actually build on. gfxcoopa's own
// base shadow_depth.vert is untouched and still resolves for anyone (e.g.
// blendy) who doesn't override it.
//
// A derived shader MUST include this backbone (not just gbuffer_vs.glsl)
// in its own shadow entry point and define the SAME gfx_surface_vertex()
// body there -- see foliage_surface.glsl. A caster whose shadow doesn't
// move with its mesh is a visible bug, not a missing feature, so this is
// the file that keeps that in sync.
//
// Include-order contract identical to gfx/surface/gbuffer_vs.glsl:
//   #version 450
//   #define GFX_SURFACE_VERTEX
//   #include <gfx/surface/shadow_vs.glsl>
//   #include "my_surface.glsl"
//
// GfxSurfaceVertex here has the same field names as gbuffer_vs.glsl's so
// one gfx_surface_vertex() definition compiles against both backbones
// unchanged -- but normal_os/tangent_os/normal_ws/tangent_ws are
// placeholders (the shadow vertex layout binds only position + uv, see
// ShadowPipeline's ctor comment on locations 1/3 staying unconsumed), so a
// hook that reads them here gets zeros/defaults, not real bindings. Every
// hook shipped so far (wind sway) only needs position and gfx_time, which
// is why this placeholder is sufficient rather than widening the shadow
// vertex layout.

layout(location = 0) in vec3 in_position;
layout(location = 2) in vec2 in_uv;      // only meaningful when ShadowPipeline was built
                                          // with a material_layout (CUTOUT support)
layout(location = 4) in mat4 in_model;   // per-instance (locations 4-7)

layout(location = 0) out vec2 frag_uv;

layout(push_constant) uniform ShadowPC {
    mat4 light_space_matrix;
    float alpha;
    float alpha_cutoff; // 0.0 disables shadow_fs.glsl's alpha-mask discard
    vec4  gfx_time;     // x=time, y=delta_time, z=frame_index, w=spare
    vec4  gfx_params;   // must match the caster's G-Buffer surface block exactly
} pc;

// See gfx/surface/gbuffer_vs.glsl's identical aliases: a surface file's
// gfx_surface_vertex() reads gfx_time/gfx_params without knowing this backbone's push
// block is named `pc` rather than `material` (gbuffer_vs.glsl's name).
vec4 gfx_time   = pc.gfx_time;
vec4 gfx_params = pc.gfx_params;

struct GfxSurfaceVertex {
    vec3 position_os;
    vec3 normal_os;   // placeholder -- see file doc above
    vec4 tangent_os;  // placeholder
    vec2 uv;
    mat4 model;
    mat3 normal_matrix; // placeholder (identity)
    vec3 position_ws;
    vec3 normal_ws;   // placeholder
    vec3 tangent_ws;  // placeholder
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

    frag_uv     = v.uv;
    gl_Position = pc.light_space_matrix * vec4(v.position_ws, 1.0);
}

#endif // GFX_SURFACE_SHADOW_VS_GLSL

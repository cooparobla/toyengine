#ifndef GFX_SURFACE_TRANSPARENT_VS_GLSL
#define GFX_SURFACE_TRANSPARENT_VS_GLSL

// gfx/surface/transparent_vs.glsl -- forward BLEND transparent vertex backbone.
//
// Lives in toyengine, not gfxcoopa's base library: this backbone's push-constant block
// must match TransparentPass::PushConstants (64 bytes: material + gfx_time/gfx_params) --
// gfxcoopa's own pbr.vert declares no push_constant block at all (nothing in the stock
// forward path reads one in the vertex stage), so a derived shader's displacement hook
// needs a version of pbr.vert that actually declares it. The math below is otherwise
// identical to pbr.vert/gfx/surface/gbuffer_vs.glsl.
//
// The SAME entry point compiled from this backbone is used by BOTH TransparentPass (the
// visible draw) and TransparentCapturePass (the SSR-secondary-source capture) -- see
// TransparentCapturePass::add_variant()'s doc for why: SSR must reflect the same displaced
// geometry the visible draw shows.
//
// Include-order contract identical to gfx/surface/gbuffer_vs.glsl:
//   #version 450
//   #define GFX_SURFACE_VERTEX
//   #include <gfx/surface/transparent_vs.glsl>
//   #include "my_surface.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent; // xyz = tangent, w = handedness
layout(location = 4) in mat4 in_model;   // per-instance (locations 4-7)

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Must stay byte-identical to TransparentPass::PushConstants (see that struct's own doc).
// metallic/roughness/ao/alpha_cutoff are unread here (fragment-stage-only) but must be
// declared to match that stage's block exactly (shared VkPushConstantRange).
layout(push_constant) uniform PushConstants {
    vec4  albedo;
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;
    vec4  gfx_time;    // x=time, y=delta_time, z=frame_index, w=spare
    vec4  gfx_params;  // four author-defined floats; see the surface shader's own doc
} material;

// See gfx/surface/gbuffer_vs.glsl's identical aliases.
vec4 gfx_time   = material.gfx_time;
vec4 gfx_params = material.gfx_params;

layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec3 frag_world_normal;
layout(location = 2) out vec2 frag_uv;
layout(location = 3) out mat3 frag_TBN;

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
    v.position_os = in_position;
    v.normal_os   = in_normal;
    v.tangent_os  = in_tangent;
    v.uv          = in_uv;
    v.model       = in_model;
    v.normal_matrix = transpose(inverse(mat3(in_model)));

    vec4 world_pos = in_model * vec4(in_position, 1.0);
    v.position_ws = world_pos.xyz;
    v.normal_ws   = normalize(v.normal_matrix * in_normal);
    v.tangent_ws  = normalize(v.normal_matrix * in_tangent.xyz);

    gfx_surface_vertex(v);

    vec3 T = normalize(v.tangent_ws - dot(v.tangent_ws, v.normal_ws) * v.normal_ws);
    vec3 B = cross(v.normal_ws, T) * in_tangent.w;

    frag_world_pos    = v.position_ws;
    frag_world_normal = v.normal_ws;
    frag_uv           = v.uv;
    frag_TBN          = mat3(T, B, v.normal_ws);

    gl_Position = camera.proj * camera.view * vec4(v.position_ws, 1.0);
}

#endif // GFX_SURFACE_TRANSPARENT_VS_GLSL

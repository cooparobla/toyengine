#ifndef GFX_SURFACE_GBUFFER_VS_GLSL
#define GFX_SURFACE_GBUFFER_VS_GLSL

// gfx/surface/gbuffer_vs.glsl -- opaque/mask G-Buffer vertex backbone.
//
// A derived shader (e.g. toyengine's foliage) overrides displacement by
// following this include order exactly (see gbuffer.vert for the canonical
// no-override example):
//
//   #version 450
//   #define GFX_SURFACE_VERTEX
//   #include <gfx/surface/gbuffer_vs.glsl>
//   #include "my_surface.glsl"   // defines gfx_surface_vertex()
//
// GFX_SURFACE_VERTEX must be #defined BEFORE this include -- that's what
// turns the hook below from a no-op default into a forward declaration the
// includer must satisfy. The file defining gfx_surface_vertex() must be
// #included AFTER this one, since it needs GfxSurfaceVertex in scope.
// Getting either backwards is a glslc redefinition/undeclared-identifier
// error at compile time, never a silent miscompile.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent; // xyz = tangent, w = handedness
layout(location = 4) in mat4 in_model;       // per-instance (locations 4-7)
layout(location = 8) in mat4 in_prev_model;  // per-instance (locations 8-11): last frame's model
layout(location = 12) in mat4 in_snow_anchor; // per-instance (locations 12-15): the snow pattern's pose
// Per-instance foliage sway (data::InstanceData::foliage). Zero for every instance that is
// not a procedural graph's leaf card; see assets/shaders/foliage_surface.glsl.
layout(location = 16) in vec4 in_foliage;
// Plain-named alias, like gfx_time/gfx_params below: a surface hook reads it without knowing
// which backbone it was compiled against.
vec4 gfx_instance_foliage = in_foliage;
// The second half of it (data::InstanceData::foliage1).
layout(location = 17) in vec4 in_foliage1;
vec4 gfx_instance_foliage1 = in_foliage1;

// Set 0: Camera UBO. The trailing reprojection members (data::CameraData) are declared here
// and in gbuffer_fs.glsl only; every other shader keeps the three-member block.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
    mat4 prev_view;
    mat4 prev_proj;
    vec4 jitter_ndc;
} camera;

// Shared with gbuffer_fs.glsl -- see that file for the fragment-stage-only
// fields (albedo/metallic/.../emissive are unread here but must stay
// byte-identical to the fragment stage's declaration; they share one
// VkPushConstantRange). gfx_time/gfx_params are the standard trailing
// "surface" block every surface backbone appends -- see
// the push-constant budget table in the layered-shaders plan for why it's
// fixed at 32 bytes (the point-light cube shadow pass is the tightest fit).
layout(push_constant) uniform PushConstants {
    vec4  albedo;
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;
    vec4  emissive;
    vec4  gfx_time;    // x=time, y=delta_time, z=frame_index, w=the water clock (WaterFrameState::time)
    vec4  gfx_params;  // four author-defined floats; see the surface shader's own doc
    uvec4 surface_ext; // x/y packed tessellation params, z flags (bit 0: no snow), w toon code
} material;

// Plain-named aliases so a surface file's gfx_surface_vertex()/gfx_surface_fragment() can
// read gfx_time/gfx_params without knowing which backbone it was compiled against -- the
// push block's own instance name differs per backbone (`material` here, `pc` in
// gfx/surface/shadow_vs.glsl and shadow_cube_vs.glsl), but every backbone assigns these two
// globals from it, so a hook body is portable across all of them. GLSL evaluates
// non-constant global initializers once per shader invocation, before main() runs, so this
// correctly reads each draw's own push-constant values rather than some shared/stale copy.
vec4 gfx_time   = material.gfx_time;
vec4 gfx_params = material.gfx_params;

// Set 2: the surface world (camera tessellation view, snow, the precipitation occlusion map and
// the snow trench field) -- see gfx/surface/world.glsl. A hook may read it (the deep-snow shader).
#define GFX_WORLD_SET 2
#include <gfx/surface/world.glsl>

// Outputs to G-Buffer fragment shader
layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec3 frag_world_normal;
layout(location = 2) out vec2 frag_uv;
layout(location = 3) out mat3 frag_TBN;          // occupies locations 3-5
// Where this vertex was LAST frame, in world space, for the velocity attachment
// gbuffer_fs.glsl writes. Location 11: derived shaders own 6-10 for their own varyings
// (triplanar.vert), and this must be declared by BOTH backbones so every vert/frag pair
// built on them matches -- an unmatched extra varying is the MoltenVK instability
// editor_paint.vert records.
layout(location = 11) out vec3 frag_prev_world_pos;
// The snow cover's pattern space (gfx/surface/snow.glsl): this point as it sits in the instance's
// snow anchor pose (data::InstanceData::snow_anchor), and the world's up direction in that pose.
// For anything that has not moved both are just world space; a moved object carries its snow.
layout(location = 12) out vec3 frag_snow_pos;
layout(location = 13) flat out vec3 frag_snow_up;

/// What a vertex-displacement hook receives and may edit. The _os fields are
/// as-authored (pre-model-matrix); everything else is already in world
/// space by the time the hook runs, since most displacement (wind, waves)
/// is easiest to reason about in world space. Edit position_ws/normal_ws/
/// tangent_ws in place; the backbone re-derives the TBN and clip position
/// from whatever the hook leaves behind.
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

    // normal_matrix is derived here from the per-instance model matrix rather than
    // streamed from the CPU; scenes use non-uniform scale (e.g. Cornell box walls),
    // so mat3(in_model) alone would be wrong.
    v.normal_matrix = transpose(inverse(mat3(in_model)));

    vec4 world_pos = in_model * vec4(in_position, 1.0);
    v.position_ws = world_pos.xyz;
    v.normal_ws   = normalize(v.normal_matrix * in_normal);
    v.tangent_ws  = normalize(v.normal_matrix * in_tangent.xyz);

    gfx_surface_vertex(v);

    // THIS frame's hook displacement, world space -- used below by the snow anchor (pattern
    // stability, not velocity: it must stay keyed to the CURRENT displacement regardless of
    // which path frag_prev_world_pos takes).
    vec3 disp = v.position_ws - world_pos.xyz;

#ifdef GFX_SURFACE_PREV_POSE
    // A TRUE previous-frame position: the SAME hook, re-run on last frame's model matrix and
    // last frame's clocks (gfx_prev_pose_, world.glsl's gfx_world_time()) -- exactly what Unreal
    // does for a World Position Offset material opted into motion vectors (re-evaluating WPO at
    // PrevFrameGameTime). Opted into per shader (foliage.vert, hair.vert #define this) because it
    // runs the hook twice per vertex; a rigid shader's cheap #else path below is already exact.
    //
    // Limitation: only the two clocks rewind. gfx_world.wind.xyz, gfx_env_turbulence_at() and
    // every gfx_world.env[] source's position/strength stay CURRENT frame, so a MOVING
    // EnvInfluence leaves a residual velocity error bounded by one frame's change in the wind
    // field -- zero for a static gust source, and far below taa.frag's 0.05px dead zone for one
    // that is itself moving. Carrying a previous env[] snapshot would double EnvSourceGpu's UBO
    // footprint; not worth it here.
    GfxSurfaceVertex pv;
    pv.position_os   = in_position;
    pv.normal_os     = in_normal;
    pv.tangent_os    = in_tangent;
    pv.uv            = in_uv;
    pv.model         = in_prev_model;
    pv.normal_matrix = transpose(inverse(mat3(in_prev_model)));
    pv.position_ws   = (in_prev_model * vec4(in_position, 1.0)).xyz;
    pv.normal_ws     = normalize(pv.normal_matrix * in_normal);
    pv.tangent_ws    = normalize(pv.normal_matrix * in_tangent.xyz);
    gfx_prev_pose_ = 1;
    gfx_surface_vertex(pv);
    gfx_prev_pose_ = 0;
    frag_prev_world_pos = pv.position_ws;
#else
    // Last frame's model matrix, plus THIS frame's hook displacement applied to both poses.
    // Rigid object motion is carried exactly this way; a time-animated hook needs
    // GFX_SURFACE_PREV_POSE above instead, or its own animation reads as zero velocity.
    frag_prev_world_pos = (in_prev_model * vec4(in_position, 1.0)).xyz + disp;
#endif
    // inverse(mat3(model)) == transpose(normal_matrix): world up into the object, then out into
    // the anchor pose.
    frag_snow_pos = (in_snow_anchor * vec4(in_position, 1.0)).xyz + disp;
    frag_snow_up  = mat3(in_snow_anchor) * (transpose(v.normal_matrix) * vec3(0.0, 0.0, 1.0));

    vec3 T = normalize(v.tangent_ws - dot(v.tangent_ws, v.normal_ws) * v.normal_ws);
    vec3 B = cross(v.normal_ws, T) * in_tangent.w;

    frag_world_pos    = v.position_ws;
    frag_world_normal = v.normal_ws;
    frag_uv           = v.uv;
    frag_TBN          = mat3(T, B, v.normal_ws);

    gl_Position = camera.proj * camera.view * vec4(v.position_ws, 1.0);
}

#endif // GFX_SURFACE_GBUFFER_VS_GLSL

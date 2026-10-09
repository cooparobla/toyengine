#ifndef GFX_SURFACE_GBUFFER_FS_GLSL
#define GFX_SURFACE_GBUFFER_FS_GLSL

// gfx/surface/gbuffer_fs.glsl -- opaque/mask G-Buffer fragment backbone.
// See gbuffer_vs.glsl for the include-order contract; GFX_SURFACE_FRAGMENT
// gates gfx_surface_fragment() the same way GFX_SURFACE_VERTEX gates
// gfx_surface_vertex() there. The backbone -- not the hook -- owns the
// CUTOUT alpha test and the MRT writeout, so every derived shader keeps
// exactly one alpha-test policy and one G-Buffer layout. Also flips the
// normal on a back face (see main()) so a double-sided material
// (PBRMaterial::cull_backfaces == false, or a CullMode::None shader variant
// like foliage) shades its interior with an outward-relative-to-viewer
// normal instead of the mesh's fixed geometric one -- same fix
// transparent_fs.glsl already has for the BLEND path.

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_world_normal;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in mat3 frag_TBN;            // locations 3-5
layout(location = 11) in vec3 frag_prev_world_pos; // see gbuffer_vs.glsl
layout(location = 12) in vec3 frag_snow_pos;       // the snow pattern's space -- see gbuffer_vs.glsl
layout(location = 13) flat in vec3 frag_snow_up;

// Set 0: Camera UBO, with the trailing reprojection members (data::CameraData) the velocity
// attachment below needs. Same binding every other pass reads its three-member block from.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
    mat4 prev_view;
    mat4 prev_proj;
    vec4 jitter_ndc;
} camera;

layout(push_constant) uniform PushConstants {
    vec4  albedo;     // xyz = albedo, w = alpha
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff; // 0.0 disables the alpha test below
    vec4  emissive;     // xyz = pre-multiplied emissive radiance, w reserved
    vec4  gfx_time;     // x=time, y=delta_time, z=frame_index, w=spare
    vec4  gfx_params;   // four author-defined floats; see the surface shader's own doc
    uvec4 surface_ext; // x/y packed tessellation params, z flags (bit 0: no snow), w reserved
} material;

// See gbuffer_vs.glsl's identical aliases for why these exist: a surface file's
// gfx_surface_fragment() reads gfx_time/gfx_params without knowing this backbone's push
// block is named `material` rather than `pc` (shadow_fs.glsl/shadow_cube_fs.glsl's name).
vec4 gfx_time   = material.gfx_time;
vec4 gfx_params = material.gfx_params;

// Set 2: the surface world -- see gfx/surface/world.glsl. The snow cover layer below reads it.
#define GFX_WORLD_SET 2
#include <gfx/surface/world.glsl>
#include <gfx/surface/snow.glsl>

// Set 1: material textures. A consumer that passes GBufferPipeline a non-null material_layout
// must bind all four combined samplers here (see engine::util::MaterialTextureCache, which
// binds a neutral fallback -- white for alpha_mask/albedo/metallic_roughness, flat-up for
// normal -- for every slot a material doesn't use, so an untextured material's math below
// collapses back to exactly the constant-only result). A consumer that never passes a
// material_layout never reaches set 1 at all.
layout(set = 1, binding = 0) uniform sampler2D u_alpha_mask;
layout(set = 1, binding = 1) uniform sampler2D u_albedo_map;
layout(set = 1, binding = 2) uniform sampler2D u_normal_map;
layout(set = 1, binding = 3) uniform sampler2D u_metallic_roughness_map;

// G-Buffer Render Targets
layout(location = 0) out vec4 out_albedo_ao;          // RGB = Albedo, A = AO
layout(location = 1) out vec4 out_normal_metallic;    // RGB = World Normal, A = Metallic
layout(location = 2) out vec4 out_position_roughness; // RGB = World Pos, A = Roughness
layout(location = 3) out vec4 out_emissive;           // RGB = emissive radiance (HDR), A = unused
// G4, per-object motion vectors: XY = this surface's screen motion since last frame in UV
// units (current unjittered position minus previous), Z = its linear view depth LAST frame
// (-1 when it was behind the eye: no history exists for it), W = its linear view depth now.
// The temporal passes (SSAO's resolve, TAA) reproject their history through XY and compare
// the depth their history stored against Z to detect a different surface.
layout(location = 4) out vec4 out_velocity;

// NDC xy -> UV, matching gfx/ssr_common.glsl's ssr_ndc_to_uv (Y flips between the spaces).
vec2 gfx_gbuffer_ndc_to_uv(vec2 ndc) { return vec2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5); }

/// What a fragment-shading hook receives and may edit, seeded from the
/// material push block and the interpolated vertex outputs. Editing
/// normal_ws re-lights the surface (e.g. animated ripple normals); the
/// backbone still runs the cutout test before this struct is built and
/// owns the MRT writeout after the hook returns.
struct GfxSurface {
    vec3  albedo;
    float metallic;
    float roughness;
    float ao;
    vec3  emissive;
    vec3  normal_ws;
    vec3  position_ws;
    vec2  uv;
    mat3  tbn; // world-space tangent/bitangent/normal frame -- see frag_TBN. Seeded before the
               // hook runs, so a hook that wants to perturb normal_ws in tangent space (e.g. its
               // own secondary detail map) can do so without recomputing the frame itself.
};

#ifdef GFX_SURFACE_FRAGMENT
void gfx_surface_fragment(inout GfxSurface s);
#else
void gfx_surface_fragment(inout GfxSurface s) {}
#endif

// Material-texture sample hook: an including shader may define
// GFX_SURFACE_SAMPLE(tex, uv) BEFORE including this backbone to wrap every
// material-map fetch -- e.g. toyengine's gbuffer.frag routes it through
// gfx_texel_aa_uv() (gfx/texel_aa.glsl) so its pixel-art atlas keeps hard
// texels at rest but stops snapping them under camera motion. Default is a
// plain fetch, so no other consumer changes.
#ifndef GFX_SURFACE_SAMPLE
#define GFX_SURFACE_SAMPLE(tex, uv) texture(tex, uv)
#endif
// The normal map has its own hook: GFX_SURFACE_SAMPLE above is commonly redefined for
// albedo-style sampling (e.g. toyengine's texel-AA gbuffer.frag), which a normal map
// should not inherit. A variant that remaps UVs (toyengine's terrain.frag) defines both.
#ifndef GFX_SURFACE_SAMPLE_NORMAL
#define GFX_SURFACE_SAMPLE_NORMAL(tex, uv) texture(tex, uv)
#endif

void main() {
    vec4 albedo_tex = GFX_SURFACE_SAMPLE(u_albedo_map, frag_uv);

    // CUTOUT/MASK materials: alpha_cutoff > 0 arms the test. albedo.a (the constant per-material
    // alpha) multiplied by the sampled mask's alpha and the albedo map's own alpha (glTF
    // convention: a base color texture's alpha channel participates in the alpha test same as
    // a dedicated mask) gives a real per-texel silhouette test when either is authored, and
    // collapses to the old constant-only test when neither is (both fallbacks are opaque white).
    float alpha = material.albedo.a * GFX_SURFACE_SAMPLE(u_alpha_mask, frag_uv).a * albedo_tex.a;
    if (material.alpha_cutoff > 0.0 && alpha < material.alpha_cutoff) discard;

    // glTF packing: metallic in B, roughness in G (R and A unused/reserved). Fallback is opaque
    // white, so mr == vec2(1.0, 1.0) when no metallic_roughness map is authored.
    vec2 mr = GFX_SURFACE_SAMPLE(u_metallic_roughness_map, frag_uv).bg;

    GfxSurface s;
    s.albedo      = material.albedo.rgb * albedo_tex.rgb;
    s.metallic    = material.metallic  * mr.x;
    s.roughness   = max(material.roughness * mr.y, 0.045);
    s.ao          = material.ao;
    s.emissive    = material.emissive.rgb;
    // Normal map is decoded from [0,1] to [-1,1] and rotated into world space by the
    // interpolated TBN; frag_TBN's third column IS frag_world_normal (see gbuffer_vs.glsl), so
    // an exact (0,0,1) tangent-space normal would round-trip to frag_world_normal exactly. The
    // fallback texel (128,128,255) is one integer off that exact midpoint (127.5), so an
    // untextured material's normal_ws is off by ~0.32 degrees rather than bit-identical -- see
    // engine::util::MaterialTextureCache's doc, which makes the same call for the same reason.
    s.tbn         = frag_TBN;
    s.normal_ws   = normalize(s.tbn * (GFX_SURFACE_SAMPLE_NORMAL(u_normal_map, frag_uv).xyz * 2.0 - 1.0));
    if (!gl_FrontFacing) s.normal_ws = -s.normal_ws; // correct for double-sided materials
                                                      // (PBRMaterial::cull_backfaces == false)
                                                      // and any CullMode::None shader variant
                                                      // (e.g. foliage) -- see gbuffer_fs.glsl's
                                                      // file doc and transparent_fs.glsl's
                                                      // identical fix for the BLEND path.
    s.position_ws = frag_world_pos;
    s.uv          = frag_uv;

    gfx_surface_fragment(s);

    // Lying snow on the open, up-facing parts (gfx/surface/snow.glsl), over whatever the hook
    // made -- unless the material (surface_ext.z bit 0) or the shader opts out.
#ifndef GFX_SURFACE_NO_SNOW
    if ((material.surface_ext.z & 1u) == 0u && gfx_world.snow.x > 0.0) {
        vec3 geo_n = normalize(gl_FrontFacing ? frag_world_normal : -frag_world_normal);
        gfx_snow_apply(s, geo_n, gfx_snow_pattern_xy(frag_snow_pos, frag_snow_up));
    }
#endif

    out_albedo_ao          = vec4(s.albedo, s.ao);
    out_normal_metallic    = vec4(s.normal_ws, s.metallic);
    out_position_roughness = vec4(s.position_ws, s.roughness);
    out_emissive            = vec4(s.emissive, 0.0);

    // Velocity. Both positions are re-projected here from the perspective-correct interpolated
    // world positions (frag_world_pos is the displaced one, so cur_clip reproduces this
    // fragment's gl_Position to round-off); a static surface under a still camera gets
    // bit-identical inputs on both sides, hence an exactly zero velocity. The current
    // position has this frame's TAA jitter removed (the NDC shift the jittered proj applied,
    // see TaaPass/apply_taa_jitter_), the previous projection never had any, so the motion is
    // unjittered-to-unjittered: a still camera measures zero rather than the jitter sequence.
    {
        vec4  cur_clip   = camera.proj * camera.view * vec4(frag_world_pos, 1.0);
        vec4  prev_clip  = camera.prev_proj * camera.prev_view * vec4(frag_prev_world_pos, 1.0);
        vec2  cur_uv     = gfx_gbuffer_ndc_to_uv(cur_clip.xy / cur_clip.w - camera.jitter_ndc.xy);
        float cur_depth  = -(camera.view      * vec4(frag_world_pos, 1.0)).z;
        float prev_depth = -(camera.prev_view * vec4(frag_prev_world_pos, 1.0)).z;
        if (prev_clip.w <= 0.0) {
            out_velocity = vec4(0.0, 0.0, -1.0, cur_depth);
        } else {
            vec2 prev_uv = gfx_gbuffer_ndc_to_uv(prev_clip.xy / prev_clip.w);
            out_velocity = vec4(cur_uv - prev_uv, prev_depth, cur_depth);
        }
    }
}

#endif // GFX_SURFACE_GBUFFER_FS_GLSL
